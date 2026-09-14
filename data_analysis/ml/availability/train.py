"""Train the availability forecaster: one ordinal distribution model per predicted hour.

    python -m data_analysis.ml.availability.train --output data_analysis/outputs/ml_avail_run1

Only the rows the export marks TRAIN are fitted, VALIDATION calibrates the interval level, and
TEST is touched once at the end for the numbers that go into ``model_metadata.json``.  A
``--holdout-city`` run additionally trains without that city at all and scores it as if it were
a station that had just opened, which is the cold-start evidence the load-only run cannot give.

Two flags exist for the questions "a training pass is an hour long, what if it dies?" and "did you
actually train enough?":

* ``--resume`` keeps the bundles already saved in ``--output`` and retrains only the missing ones.
  A saved bundle is reused only when it says it came from this same recipe - same batch, manifest,
  feature version, seed, model version, steps and round budget - and anything that does not match
  is refused rather than overwritten.  ``train_summary.json`` marks a finished, quotable run, so a
  directory holding one is never resumable.
* ``--rounds-multiplier 5`` fits each hour model with five times the boosting rounds and five times
  the early-stopping patience, and stamps the bundles ``-r5`` so a stretched model cannot be
  mistaken for the published one.  ``1.0`` reproduces the shipped recipe exactly.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np
import pandas as pd

from data_analysis.ml.availability.model import OrdinalForecastModel, scaled_rounds
from data_analysis.ml.common import artifacts, forecaster, metrics
from data_analysis.ml.common.data_io import DEFAULT_EXPORT
from data_analysis.ml.common.tasks import AVAILABILITY

MODEL_VERSION = "0.2.0"
#: copied into every bundle's ``training_report.json`` by :func:`main` (see ``build_hierarchical``)
RUN_CONTEXT: dict = {}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export", default=str(DEFAULT_EXPORT))
    parser.add_argument("--output", required=True)
    parser.add_argument("--horizons", type=int, nargs="+", default=[1, 6, 24], choices=[1, 6, 24])
    parser.add_argument("--seed", type=int, default=20260913)
    parser.add_argument("--nominal-coverage", type=float, default=0.80)
    parser.add_argument("--holdout-city", action="append", default=[], help="train without this city and score it as new")
    parser.add_argument("--limit-steps", type=int, default=0, help="debug: only train the first N steps")
    parser.add_argument("--rounds-multiplier", type=float, default=1.0,
                        help="fit each hour model with this many times the published 250 boosting rounds "
                             "(early-stopping patience scales with it); 1.0 is the shipped recipe")
    parser.add_argument("--resume", action="store_true",
                        help="continue an interrupted run in --output instead of starting a new one: reuse "
                             "every saved bundle whose recorded recipe matches this command")
    arguments = parser.parse_args(argv)

    if not arguments.rounds_multiplier > 0:
        parser.error(f"--rounds-multiplier must be positive, got {arguments.rounds_multiplier}")

    started = time.time()
    RUN_CONTEXT.update({"command": artifacts.invocation(__name__, argv)})
    output = Path(arguments.output)
    # A published run is evidence, not a build artefact, so this entry refuses to rewrite one.
    # ``--resume`` relaxes the bundle half of that check, never the summary half: once
    # ``train_summary.json`` exists the numbers in it have been quoted somewhere.
    occupied = published_content(output)
    if "train_summary.json" in occupied or (occupied and not arguments.resume):
        raise SystemExit(f"{output} already holds saved bundles or a train summary; pass a new --output "
                         f"directory instead of rewriting a published run")
    frame = forecaster.build_frame(Path(arguments.export))
    data = frame.data
    columns = frame.feature_columns
    print(f"[frame] {len(data)} rows x {len(columns)} features; batch {frame.export.published_batch_id}")
    print(f"[splits] {forecaster.split_summary(frame)}")

    summary = {
        "task": AVAILABILITY.key,
        "datasetId": frame.export.dataset_id,
        "trainingPublishedBatchId": frame.export.published_batch_id,
        "sourceManifestSha256": frame.export.source_manifest_sha256,
        "servingManifestSha256": frame.export.manifest_sha256,
        "featureVersion": frame.export.feature_version,
        "modelVersion": f"{MODEL_VERSION}{_variant(arguments.rounds_multiplier)}",
        "seed": arguments.seed,
        "reproducibleCommand": RUN_CONTEXT.get("command"),
        "nominalCoverage": arguments.nominal_coverage,
        "roundsMultiplier": arguments.rounds_multiplier,
        "boostingRoundsBudget": scaled_rounds(arguments.rounds_multiplier)["max_iter"],
        "featureColumns": columns,
        "run": {},
        "holdout": {},
    }

    for horizon in arguments.horizons:
        summary["run"][f"h{horizon:02d}"] = _train_horizon(
            frame, horizon, output / f"h{horizon:02d}", seed=arguments.seed,
            coverage=arguments.nominal_coverage, steps=arguments.limit_steps,
            rounds_multiplier=arguments.rounds_multiplier, resume=arguments.resume,
        )
        for city in arguments.holdout_city:
            summary["holdout"].setdefault(city, {})[f"h{horizon:02d}"] = _train_horizon(
                frame, horizon, output / f"coldstart_{city}" / f"h{horizon:02d}",
                seed=arguments.seed, coverage=arguments.nominal_coverage,
                exclude_city=city, steps=arguments.limit_steps,
                rounds_multiplier=arguments.rounds_multiplier, resume=arguments.resume,
            )

    trained = list(summary["run"].values()) + [entry for per_city in summary["holdout"].values()
                                               for entry in per_city.values()]
    summary["resumedBundles"] = [entry["directory"] for entry in trained if entry.get("reused")]
    if summary["resumedBundles"]:
        # Stated rather than left to whoever reads the summary: the recorded command re-runs the
        # fitted half of this run, and only ``--resume`` against this directory reproduces the rest.
        summary["reproducibleCommandNote"] = (
            f"{len(summary['resumedBundles'])} of {len(trained)} bundles were loaded from {output} by --resume "
            f"instead of being fitted by this command; they are listed in resumedBundles")

    summary["elapsedSeconds"] = round(time.time() - started, 1)
    (output / "train_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"[done] {output} in {summary['elapsedSeconds']}s")
    return 0


def _train_horizon(frame, horizon: int, directory: Path, *, seed: int, coverage: float,
                   exclude_city: str | None = None, steps: int = 0,
                   rounds_multiplier: float = 1.0, resume: bool = False) -> dict:
    data = frame.data
    columns = frame.feature_columns
    split_column = frame.export.split_column(horizon)
    available_steps = list(range(1, (steps or horizon) + 1))

    if resume:
        reused = _reuse(directory, frame, horizon=horizon, seed=seed, exclude_city=exclude_city,
                        available_steps=available_steps, rounds_multiplier=rounds_multiplier)
        if reused is not None:
            return reused

    keep = pd.Series(True, index=data.index)
    if exclude_city:
        keep = data["city_id"] != exclude_city
    train_rows = keep & (data[split_column] == "TRAIN")
    validation_rows = keep & (data[split_column] == "VALIDATION")
    scored_rows = ~keep & (data[split_column] == "TEST") if exclude_city else keep & (data[split_column] == "TEST")
    if train_rows.sum() < 1000 or validation_rows.sum() < 100 or scored_rows.sum() < 100:
        raise ValueError(f"h{horizon:02d}: not enough rows in one of the splits")

    x_train = data.loc[train_rows, columns]
    classes = np.arange(0, int(data["capacity"].max()) + 1)
    model = OrdinalForecastModel(
        classes=classes, steps={}, feature_columns=list(columns),
        capacity=int(data["capacity"].max()), nominal_coverage=coverage,
    )
    validation_labels: dict[int, np.ndarray] = {}
    per_step: dict[int, dict] = {}
    budget = scaled_rounds(rounds_multiplier)
    for step in available_steps:
        label = AVAILABILITY.label_column(step)
        y_train = data.loc[train_rows, label].astype(int)
        model.fit_step(step, x_train, y_train, seed=seed, rounds_multiplier=rounds_multiplier)
        validation_labels[step] = data.loc[validation_rows, label].to_numpy(dtype=float)
        x_test = data.loc[scored_rows, columns]
        y_test = data.loc[scored_rows, label].to_numpy(dtype=float)
        per_step[step] = score_step(model, step, x_test, y_test, data.loc[scored_rows])
        # What early stopping actually spent, so "train longer" is answerable from the artefact
        # instead of from the budget the command asked for.
        per_step[step]["boostingRounds"] = int(model.steps[step].n_iter_)
        per_step[step]["maxBoostingRounds"] = int(budget["max_iter"])

    calibration = model.calibrate(data.loc[validation_rows, columns], validation_labels)
    # Re-score with the calibrated levels so the reported coverage is the shipped behaviour.
    for step in available_steps:
        label = AVAILABILITY.label_column(step)
        per_step[step].update(score_step(model, step, data.loc[scored_rows, columns],
                                         data.loc[scored_rows, label].to_numpy(dtype=float),
                                         data.loc[scored_rows]))
        per_step[step]["calibration"] = calibration[step]

    pooled = pool_metrics(per_step)
    version = f"{MODEL_VERSION}{_variant(rounds_multiplier)}"
    model_id = (f"{AVAILABILITY.id_prefix}-ord-h{horizon:02d}{_variant(rounds_multiplier)}"
                + (f"-cold{exclude_city}" if exclude_city else ""))
    bundle = {
        "model": model,
        "target": AVAILABILITY.target,
        "horizonHours": horizon,
        "modelId": model_id,
        "modelVersion": version,
        "featureVersion": frame.export.feature_version,
        "featureColumns": list(columns),
        "cities": frame.cities,
        "calendar": frame.calendar,
        "stations": frame.stations,
        "datasetId": frame.export.dataset_id,
        "publishedBatchId": frame.export.published_batch_id,
        "excludeCity": exclude_city,
        "capacity": model.capacity,
        "roundsMultiplier": rounds_multiplier,
    }
    metadata = artifacts.build_metadata(
        model_id=model_id,
        model_version=version,
        target=AVAILABILITY.target,
        feature_version=frame.export.feature_version,
        dataset_id=frame.export.dataset_id,
        source_manifest_sha256=frame.export.source_manifest_sha256,
        training_published_batch_id=frame.export.published_batch_id,
        splits=frame.export.ml_splits,
        feature_columns=columns,
        artifact_file=artifacts.ARTIFACT_NAME,
        artifact_sha256="0" * 64,
        supported_horizons=[horizon],
        metrics={"mae": round(pooled["mae"], 4), "rmse": round(pooled["rmse"], 4),
                 "testSamples": int(pooled["points"]), "unit": AVAILABILITY.unit},
    )
    report = {
        "modelId": model_id,
        "modelVersion": version,
        "horizonHours": horizon,
        "holdoutCity": exclude_city,
        "seed": seed,
        "reproducibleCommand": RUN_CONTEXT.get("command"),
        "roundsMultiplier": rounds_multiplier,
        "estimatorParams": dict(budget),
        "splits": {
            "train": int(train_rows.sum()),
            "validation": int(validation_rows.sum()),
            "scored": int(scored_rows.sum()),
        },
        "pointValue": "distribution median (integer chargers); expectation reported separately",
        "pooledTest": {key: (round(value, 4) if isinstance(value, float) else value) for key, value in pooled.items()},
        "perStep": per_step,
        "preparedAt": artifacts.stamp(),
    }
    artifacts.save_bundle(directory, bundle, metadata, report)
    spent = [entry["boostingRounds"] for entry in per_step.values()]
    print(f"[h{horizon:02d}{'' if not exclude_city else f' cold:{exclude_city}'}] "
          f"TEST MAE {pooled['mae']:.3f} RMSE {pooled['rmse']:.3f} coverage@{coverage:.0%} "
          f"{pooled['coverage']:.3f} depletionAUC {pooled['depletionAUC']:.3f} n={pooled['points']} "
          f"rounds {min(spent)}-{max(spent)}/{budget['max_iter']}")
    return {"modelId": model_id, "directory": str(directory), "pooled": report["pooledTest"],
            "reused": False, "boostingRounds": {"min": min(spent), "max": max(spent),
                                                "budget": int(budget["max_iter"])}}


def published_content(output: Path) -> list[str]:
    """What is already sitting in a run directory that makes it evidence somebody may have quoted.

    Only two things count: saved bundles and ``train_summary.json``.  ``prepare_data`` writes
    ``data_profile.json`` into the same directory before training starts, and a directory that does
    not exist yet is the normal case - ``--output`` names a path this command is about to create, so
    the existence check happens here rather than letting :func:`artifacts.bundle_directories` raise.
    """
    found = []
    if (output / "train_summary.json").exists():
        found.append("train_summary.json")
    if output.is_dir():
        found += [directory.name for directory in artifacts.bundle_directories(output)]
    return found


def _variant(rounds_multiplier: float) -> str:
    """Name tag that keeps a stretched round budget from claiming the published ``modelId``."""
    return "" if rounds_multiplier == 1.0 else f"-r{rounds_multiplier:g}"


def _expected_recipe(frame, horizon: int, *, seed: int, exclude_city: str | None,
                     available_steps: list[int], rounds_multiplier: float) -> dict:
    """What a bundle sitting in ``--output`` has to record to be reusable by this command."""
    export = frame.export
    return {
        "modelVersion": f"{MODEL_VERSION}{_variant(rounds_multiplier)}",
        "featureVersion": export.feature_version,
        "datasetId": export.dataset_id,
        "trainingPublishedBatchId": export.published_batch_id,
        "sourceManifestSha256": export.source_manifest_sha256,
        "featureColumns": list(frame.feature_columns),
        "seed": seed,
        "horizonHours": horizon,
        "holdoutCity": exclude_city,
        "roundsMultiplier": rounds_multiplier,
        "steps": sorted(str(step) for step in available_steps),
    }


def _saved_recipe(directory: Path) -> tuple[dict, dict, str | None]:
    """The recipe a saved bundle records about itself, the report, and why it cannot be trusted.

    Read from the two JSON sidecars rather than from the pickle, so a bundle whose ``model.joblib``
    is corrupt is caught by the hash check and never loaded.
    """
    try:
        metadata = json.loads((directory / artifacts.METADATA_NAME).read_text(encoding="utf-8"))
        report = json.loads((directory / artifacts.REPORT_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return {}, {}, (f"{artifacts.METADATA_NAME} or {artifacts.REPORT_NAME} cannot be read ({error}); "
                        f"this looks like an interrupted save")
    artifact = directory / metadata.get("artifactFile", artifacts.ARTIFACT_NAME)
    if not artifact.is_file():
        return {}, {}, f"{artifact.name} is missing"
    if artifacts.sha256_file(artifact) != metadata.get("artifactSha256"):
        return {}, {}, f"{artifact.name} does not match the hash recorded in {artifacts.METADATA_NAME}"
    saved = {
        "modelVersion": metadata.get("modelVersion"),
        "featureVersion": metadata.get("featureVersion"),
        "datasetId": metadata.get("datasetId"),
        "trainingPublishedBatchId": metadata.get("trainingPublishedBatchId"),
        "sourceManifestSha256": metadata.get("sourceManifestSha256"),
        "featureColumns": metadata.get("featureColumns"),
        "seed": report.get("seed"),
        "horizonHours": report.get("horizonHours"),
        "holdoutCity": report.get("holdoutCity"),
        "roundsMultiplier": report.get("roundsMultiplier", 1.0),
        "steps": sorted(str(step) for step in report.get("perStep", {})),
    }
    if saved["seed"] is None:
        # A bundle that does not say which seed made it cannot be claimed as this command's output.
        saved["seed"] = "<unrecorded>"
    return saved, report, None


def _reuse(directory: Path, frame, *, horizon: int, seed: int, exclude_city: str | None,
           available_steps: list[int], rounds_multiplier: float) -> dict | None:
    """The summary entry for a saved bundle this command may load instead of retraining, if any.

    Returns ``None`` when nothing is saved here yet, and refuses outright when something is saved
    but is not this model: silently replacing a half-finished run's artefacts with different ones is
    exactly what the no-overwrite rule exists to stop.
    """
    if not (directory / artifacts.METADATA_NAME).exists():
        return None
    saved, report, problem = _saved_recipe(directory)
    label = f"h{horizon:02d}" + ("" if not exclude_city else f" cold:{exclude_city}")
    if problem:
        raise SystemExit(f"[resume] {directory} cannot be reused: {problem}. {label} will not be "
                         f"overwritten in place - delete that directory or pass a new --output")
    expected = _expected_recipe(frame, horizon, seed=seed, exclude_city=exclude_city,
                                available_steps=available_steps, rounds_multiplier=rounds_multiplier)
    differences = [(key, saved.get(key), value) for key, value in expected.items() if saved.get(key) != value]
    if differences:
        detail = "; ".join(f"{key} saved={old!r} this command={new!r}" for key, old, new in differences)
        raise SystemExit(f"[resume] {directory} was fitted by a different recipe, so --resume will not "
                         f"overwrite it: {detail}")
    pooled = report.get("pooledTest", {})
    per_step = report.get("perStep", {})
    recorded = [value for value in (entry.get("boostingRounds") for entry in per_step.values())
                if value is not None]
    budget = (report.get("estimatorParams") or {}).get("max_iter")
    score = f"TEST MAE {pooled['mae']:.4f}, " if "mae" in pooled else "no recorded metrics, "
    print(f"[resume] {label} reusing {directory} ({score}{len(per_step)} hour-model(s), unchanged on disk)")
    # Same shape as the fitted path's entry, so a summary can be read without knowing which bundles
    # came from disk; ``None`` says the reused bundle predates round recording.
    return {"modelId": report.get("modelId"), "directory": str(directory), "pooled": pooled,
            "reused": True, "savedAt": report.get("preparedAt"),
            "boostingRounds": {"min": min(recorded) if recorded else None,
                               "max": max(recorded) if recorded else None, "budget": budget}}


def score_step(model, step: int, x, y: np.ndarray, rows: pd.DataFrame,
               keys: pd.DataFrame | None = None) -> dict:
    """TEST-split numbers for one predicted hour; ``keys`` is only needed by the hierarchical model."""
    median = model.median(x, step, keys).astype(float)
    expectation = model.expectation(x, step, keys)
    probability = model.distribution_for(x, step, keys)
    low, high = model.interval_for(probability, model.interval_levels.get(step, model.nominal_coverage))
    depletion_truth = (y == 0).astype(float)
    return {
        "mae": metrics.mae(y, median),
        "maeExpectation": metrics.mae(y, expectation),
        "rmse": metrics.rmse(y, expectation),
        "baselinePersistenceMae": metrics.mae(y, rows["lag_available_h01"].to_numpy(dtype=float)),
        "legality": metrics.legality(median, 0.0, float(model.capacity)),
        "coverage": metrics.coverage(y, low, high),
        "intervalWidth": metrics.interval_width(low, high),
        "winkler": metrics.winkler(y, low, high, 1.0 - model.nominal_coverage),
        "depletionBaseRate": float(np.mean(depletion_truth)),
        "depletionAUC": metrics.roc_auc(depletion_truth, model.p_depletion(x, step, keys)),
        "points": int(len(y)),
    }


def pool_metrics(per_step: dict) -> dict:
    points = sum(entry["points"] for entry in per_step.values())
    weights = np.array([entry["points"] for entry in per_step.values()], dtype=float)
    weights /= weights.sum()

    def average(key):
        return float(np.average([entry[key] for entry in per_step.values()], weights=weights))
    return {
        "points": points,
        "mae": average("mae"),
        "maeExpectation": average("maeExpectation"),
        "rmse": average("rmse"),
        "coverage": average("coverage"),
        "intervalWidth": average("intervalWidth"),
        "winkler": average("winkler"),
        "legality": average("legality"),
        "depletionAUC": average("depletionAUC"),
        "depletionBaseRate": average("depletionBaseRate"),
        "baselinePersistenceMae": average("baselinePersistenceMae"),
    }


if __name__ == "__main__":
    raise SystemExit(main())
