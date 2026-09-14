"""Train the availability forecaster: one ordinal distribution model per predicted hour.

    python -m data_analysis.ml.availability.train --output data_analysis/outputs/ml_avail_run1

Only the rows the export marks TRAIN are fitted, VALIDATION calibrates the interval level, and
TEST is touched once at the end for the numbers that go into ``model_metadata.json``.  A
``--holdout-city`` run additionally trains without that city at all and scores it as if it were
a station that had just opened, which is the cold-start evidence the load-only run cannot give.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np
import pandas as pd

from data_analysis.ml.availability.model import OrdinalForecastModel
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
    arguments = parser.parse_args(argv)

    started = time.time()
    RUN_CONTEXT.update({"command": artifacts.invocation(__name__, argv)})
    output = Path(arguments.output)
    # A published run is evidence, not a build artefact, so this entry refuses to rewrite one.  The
    # test is for bundles rather than for a non-empty directory because ``prepare_data`` is allowed
    # to have written ``data_profile.json`` here first - that is the documented order of operations.
    if artifacts.bundle_directories(output) or (output / "train_summary.json").exists():
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
        "modelVersion": MODEL_VERSION,
        "seed": arguments.seed,
        "reproducibleCommand": RUN_CONTEXT.get("command"),
        "nominalCoverage": arguments.nominal_coverage,
        "featureColumns": columns,
        "run": {},
        "holdout": {},
    }

    for horizon in arguments.horizons:
        summary["run"][f"h{horizon:02d}"] = _train_horizon(
            frame, horizon, output / f"h{horizon:02d}", seed=arguments.seed,
            coverage=arguments.nominal_coverage, steps=arguments.limit_steps,
        )
        for city in arguments.holdout_city:
            summary["holdout"].setdefault(city, {})[f"h{horizon:02d}"] = _train_horizon(
                frame, horizon, output / f"coldstart_{city}" / f"h{horizon:02d}",
                seed=arguments.seed, coverage=arguments.nominal_coverage,
                exclude_city=city, steps=arguments.limit_steps,
            )

    summary["elapsedSeconds"] = round(time.time() - started, 1)
    (output / "train_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"[done] {output} in {summary['elapsedSeconds']}s")
    return 0


def _train_horizon(frame, horizon: int, directory: Path, *, seed: int, coverage: float,
                   exclude_city: str | None = None, steps: int = 0) -> dict:
    data = frame.data
    columns = frame.feature_columns
    split_column = frame.export.split_column(horizon)
    available_steps = range(1, (steps or horizon) + 1)

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
    for step in available_steps:
        label = AVAILABILITY.label_column(step)
        y_train = data.loc[train_rows, label].astype(int)
        model.fit_step(step, x_train, y_train, seed=seed)
        validation_labels[step] = data.loc[validation_rows, label].to_numpy(dtype=float)
        x_test = data.loc[scored_rows, columns]
        y_test = data.loc[scored_rows, label].to_numpy(dtype=float)
        per_step[step] = score_step(model, step, x_test, y_test, data.loc[scored_rows])

    calibration = model.calibrate(data.loc[validation_rows, columns], validation_labels)
    # Re-score with the calibrated levels so the reported coverage is the shipped behaviour.
    for step in available_steps:
        label = AVAILABILITY.label_column(step)
        per_step[step].update(score_step(model, step, data.loc[scored_rows, columns],
                                         data.loc[scored_rows, label].to_numpy(dtype=float),
                                         data.loc[scored_rows]))
        per_step[step]["calibration"] = calibration[step]

    pooled = pool_metrics(per_step)
    model_id = f"{AVAILABILITY.id_prefix}-ord-h{horizon:02d}" + (f"-cold{exclude_city}" if exclude_city else "")
    bundle = {
        "model": model,
        "target": AVAILABILITY.target,
        "horizonHours": horizon,
        "modelId": model_id,
        "modelVersion": MODEL_VERSION,
        "featureVersion": frame.export.feature_version,
        "featureColumns": list(columns),
        "cities": frame.cities,
        "calendar": frame.calendar,
        "stations": frame.stations,
        "datasetId": frame.export.dataset_id,
        "publishedBatchId": frame.export.published_batch_id,
        "excludeCity": exclude_city,
        "capacity": model.capacity,
    }
    metadata = artifacts.build_metadata(
        model_id=model_id,
        model_version=MODEL_VERSION,
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
        "horizonHours": horizon,
        "holdoutCity": exclude_city,
        "seed": seed,
        "reproducibleCommand": RUN_CONTEXT.get("command"),
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
    print(f"[h{horizon:02d}{'' if not exclude_city else f' cold:{exclude_city}'}] "
          f"TEST MAE {pooled['mae']:.3f} RMSE {pooled['rmse']:.3f} coverage@{coverage:.0%} "
          f"{pooled['coverage']:.3f} depletionAUC {pooled['depletionAUC']:.3f} n={pooled['points']}")
    return {"modelId": model_id, "directory": str(directory), "pooled": report["pooledTest"]}


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
