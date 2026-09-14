"""Ship each trained classifier wrapped in the station x hour prior, as a new run.

    python -m data_analysis.ml.availability.build_hierarchical \
        --source-run data_analysis/outputs/ml_avail_run5 \
        --output data_analysis/outputs/ml_avail_run6

No gradient boosting is redone: the estimators are loaded from ``--source-run`` unchanged, the
station x hour empirical prior is fitted on the same TRAIN rows those estimators saw, and the only
free parameters - one pseudo-count, one interval level and one point rule per predicted hour - are
chosen on VALIDATION.  TEST is touched once at the end, which is what ``model_metadata.json``
reports, so a run2 bundle is servable exactly like a run1 one and the audit script measures the
artefact rather than a re-implementation of it.

The output directory must be empty: published runs are never rewritten.  ``--resume`` is the one
relaxation, for a run that died halfway - a saved bundle is kept only when it names this same base
bundle, seed and batch, and both artefacts still match their recorded hashes.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np
import pandas as pd

from data_analysis.ml.availability import train as base_train
from data_analysis.ml.availability.model import HierarchicalForecastModel, point_value
from data_analysis.ml.availability.prior import (
    KEY_LEVELS,
    MIN_LEVEL_ROWS,
    NO_PRIOR,
    HourCellPrior,
    add_site_key,
    site_key_of,
)
from data_analysis.ml.common import artifacts, forecaster, metrics
from data_analysis.ml.common.data_io import DEFAULT_EXPORT
from data_analysis.ml.common.tasks import AVAILABILITY

MODEL_VERSION = "0.3.0"
KEY_COLUMNS = ["station_id", "hour_of_day", "site_key", "city_id"]
PRIOR_LEVELS = [level for level, _ in KEY_LEVELS]
CAVEAT = "全部指标为模拟数据测试结果（第二阶段发布批次），不代表真实运营数据表现。"
#: filled in by :func:`main` and copied into every bundle's ``training_report.json``, so a published
#: run states the exact command and seed that produced it instead of relying on someone's shell history
RUN_CONTEXT: dict = {}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export", default=str(DEFAULT_EXPORT))
    parser.add_argument("--source-run", default=None, help="trained run whose estimators are reused")
    parser.add_argument("--output", required=True)
    parser.add_argument("--seed", type=int, default=20260913)
    parser.add_argument("--only", default=None, help="substring filter on the bundle path, for smoke runs")
    parser.add_argument("--resume", action="store_true",
                        help="continue an interrupted run in --output: reuse a saved bundle whose recorded "
                             "source run, seed and batch match this command instead of wrapping it again")
    parser.add_argument("--render", action="store_true",
                        help="re-write hierarchical_report.md from the JSON already sitting in --output; "
                             "touches no model and no score, it only re-renders a finished run")
    arguments = parser.parse_args(argv)

    output = Path(arguments.output)
    if arguments.render:
        report = output / "hierarchical_report.json"
        payload = json.loads(report.read_text(encoding="utf-8"))
        (output / "hierarchical_report.md").write_text(_markdown(payload), encoding="utf-8")
        print(f"[render] {output / 'hierarchical_report.md'} from {report}")
        return 0
    if not arguments.source_run:
        parser.error("--source-run is required unless --render is given")
    source = Path(arguments.source_run)
    RUN_CONTEXT.update({"seed": arguments.seed, "command": artifacts.invocation(__name__, argv),
                        "sourceRun": str(source)})

    # As in ``train.py``: a finished report is a quotable claim, so it is never rewritten; bundles
    # without one are an interrupted run, and ``--resume`` may continue it from what is on disk.
    if output.exists() and any(output.iterdir()):
        if (output / "hierarchical_report.json").exists() or not arguments.resume:
            raise SystemExit(f"{output} already has content; pick a new run directory - published runs are not rewritten")
    directories = artifacts.bundle_directories(source)
    if arguments.only:
        directories = [entry for entry in directories if arguments.only in entry.relative_to(source).as_posix()]
    if not directories:
        raise SystemExit(f"no trained bundles found under {source}"
                         + (f" matching --only {arguments.only}" if arguments.only else ""))

    started = time.time()
    frame = forecaster.build_frame(Path(arguments.export))
    data = add_site_key(frame.data)
    payload = {
        "task": AVAILABILITY.key,
        "datasetId": frame.export.dataset_id,
        "trainingPublishedBatchId": frame.export.published_batch_id,
        "sourceManifestSha256": frame.export.source_manifest_sha256,
        "servingManifestSha256": frame.export.manifest_sha256,
        "featureVersion": frame.export.feature_version,
        "modelVersion": MODEL_VERSION,
        "seed": arguments.seed,
        "reproducibleCommand": RUN_CONTEXT.get("command"),
        "reusedEstimatorFrom": str(source),
        "priorLevels": PRIOR_LEVELS,
        "note": "boosted classifiers are loaded from the source run unchanged; only k, the interval "
                "level and the point rule are chosen here, on VALIDATION",
        "bundles": {},
        "reusedBundles": [],
    }
    for directory in sorted(directories):
        relative = directory.relative_to(source).as_posix()
        target = output / relative
        entry = _reuse(target, base_bundle=directory, source=source, frame=frame,
                       seed=arguments.seed) if arguments.resume else None
        if entry is None:
            entry = _build(frame, data, directory, source, target, relative)
        else:
            payload["reusedBundles"].append(relative)
        payload["bundles"][relative] = entry
    payload["elapsedSeconds"] = round(time.time() - started, 1)
    if payload["reusedBundles"]:
        payload["reproducibleCommandNote"] = (
            f"{len(payload['reusedBundles'])} of {len(payload['bundles'])} bundles were loaded from {output} by "
            f"--resume instead of being wrapped by this command; they are listed in reusedBundles")
    _write(output / "hierarchical_report.json", payload)
    (output / "hierarchical_report.md").write_text(_markdown(payload), encoding="utf-8")
    print(f"[done] {output} in {payload['elapsedSeconds']}s")
    return 0


def _reuse(target: Path, *, base_bundle: Path, source: Path, frame, seed: int) -> dict | None:
    """The report entry for an already-wrapped bundle this command would reproduce, if any.

    ``None`` means nothing is saved at this path yet.  A saved bundle is only kept when it names the
    same base bundle, seed, round budget, batch and manifest as this command, and when both its own
    artefact and the base artefact still match their recorded hashes - the wrapping step is cheap to
    redo but a silently mixed provenance is not detectable afterwards.
    """
    if not (target / artifacts.METADATA_NAME).exists():
        return None
    try:
        metadata = json.loads((target / artifacts.METADATA_NAME).read_text(encoding="utf-8"))
        report = json.loads((target / artifacts.REPORT_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise SystemExit(f"[resume] {target} cannot be reused: {artifacts.METADATA_NAME} or "
                         f"{artifacts.REPORT_NAME} cannot be read ({error}); it looks like an interrupted save")
    artifact = target / metadata.get("artifactFile", artifacts.ARTIFACT_NAME)
    if not artifact.is_file():
        raise SystemExit(f"[resume] {target} cannot be reused: {artifact.name} is missing")
    if artifacts.sha256_file(artifact) != metadata.get("artifactSha256"):
        raise SystemExit(f"[resume] {target} cannot be reused: {artifact.name} does not match the hash "
                         f"recorded in {artifacts.METADATA_NAME}")
    base, base_metadata = artifacts.load_bundle(base_bundle)
    forecaster.validate_bundle_frame(base, base_metadata, frame)
    relative = base_bundle.relative_to(source).as_posix()
    _, version, rounds = _naming(base)
    expected = {
        "modelVersion": version,
        "featureVersion": frame.export.feature_version,
        "datasetId": frame.export.dataset_id,
        "trainingPublishedBatchId": frame.export.published_batch_id,
        "sourceManifestSha256": frame.export.source_manifest_sha256,
        "seed": seed,
        "roundsMultiplier": rounds,
        "reusedEstimatorFrom": f"{source.name}/{relative}",
        "sourceModelId": base_metadata.get("modelId"),
        "horizonHours": int(base["horizonHours"]),
        "holdoutCity": base["excludeCity"],
        "steps": sorted(str(int(step)) for step in base["model"].steps),
    }
    saved = {
        "modelVersion": metadata.get("modelVersion"),
        "featureVersion": metadata.get("featureVersion"),
        "datasetId": metadata.get("datasetId"),
        "trainingPublishedBatchId": metadata.get("trainingPublishedBatchId"),
        "sourceManifestSha256": metadata.get("sourceManifestSha256"),
        "seed": report.get("seed"),
        "roundsMultiplier": report.get("roundsMultiplier", 1.0),
        "reusedEstimatorFrom": report.get("reusedEstimatorFrom"),
        "sourceModelId": report.get("sourceModelId"),
        "horizonHours": report.get("horizonHours"),
        "holdoutCity": report.get("holdoutCity"),
        "steps": sorted(str(int(step)) for step in report.get("perStep", {})),
    }
    differences = [(key, saved.get(key), value) for key, value in expected.items() if saved.get(key) != value]
    if differences:
        detail = "; ".join(f"{key} saved={old!r} this command={new!r}" for key, old, new in differences)
        raise SystemExit(f"[resume] {target} was wrapped from something else, so --resume will not overwrite "
                         f"it: {detail}")
    pooled = report.get("pooledTest") or {}
    score = f"TEST MAE {pooled['mae']:.4f}, " if "mae" in pooled else "no recorded metrics, "
    entry = report.get("payloadEntry")
    if not isinstance(entry, dict):
        # Bundles saved before the wrapper wrote its own row down - the published 0.3.0 run - are still
        # resumable, since every column can be rebuilt from the full-precision per-hour records that
        # report keeps.  The stored row is preferred anyway: it is what the finished run published.
        entry = _entry_from_report(report, relative=relative)
        note = ", entry rebuilt from the stored metrics"
    else:
        note = ""
    print(f"[resume] {relative} reusing {target} ({score}{len(saved['steps'])} hour models, "
          f"unchanged on disk{note})")
    return dict(entry, reused=True)


def _naming(base: dict) -> tuple[str, str, float]:
    """``(modelId, modelVersion, roundsMultiplier)`` for the bundle that wraps a given base estimator.

    A base fitted with a stretched round budget is a different model, so the wrapped artefact carries
    the same ``-r5`` tag ``train`` gave it - two models must not sit in the registry under one
    ``modelId``.  Bundles saved before ``--rounds-multiplier`` existed record no multiplier.
    """
    rounds = float(base.get("roundsMultiplier") or 1.0)
    variant = base_train._variant(rounds)
    horizon, holdout = int(base["horizonHours"]), base["excludeCity"]
    model_id = (f"{AVAILABILITY.id_prefix}-hier-h{horizon:02d}{variant}"
                + (f"-cold{holdout}" if holdout else ""))
    return model_id, f"{MODEL_VERSION}{variant}", rounds


def _gain(new: float, reference: float) -> float | None:
    """Percent MAE reduction of ``new`` over ``reference`` - one definition, used by both paths.

    Both the wrapping run and a resume pass the full-precision pooled MAE, which the per-hour records
    in a saved report carry, so a bundle's cell reads the same whether it was built by this command or
    read back from disk by ``--resume``.
    """
    return round(100.0 * (1.0 - new / reference), 2) if reference else None


def _entry_from_report(report: dict, *, relative: str) -> dict:
    """Rebuild, from a saved bundle's own report, the entry a full run puts in its payload.

    Every number here is one the finished run wrote down; nothing is re-derived, so a resumed run's
    ``hierarchical_report.md`` carries the same table as one that wrapped all twelve bundles in one
    sitting.  Bundles that recorded their own entry (:func:`_build`'s ``payloadEntry``) do not need
    this and are read back verbatim.
    """
    pooled, alone = report["pooledTest"], report["pooledBaseTest"]
    steps = report["perStep"]
    # ``pooledTest`` keeps four decimals, which is enough to quote but not to divide by without moving
    # a percentage's last digit, so the gain columns are recomputed from the full-precision per-hour
    # MAEs the same report keeps, averaged exactly the way the wrapping run averaged its own.
    full = base_train.pool_metrics(steps)["mae"]
    full_base = base_train.pool_metrics({step: dict(entry, mae=entry["baseMae"])
                                         for step, entry in steps.items()})["mae"]
    step_weights = np.array([entry["points"] for entry in steps.values()], dtype=float)
    full_rule = float(np.average([entry["baseRuleMae"] for entry in steps.values()], weights=step_weights))
    windows = report["calibration"]
    first = windows[min(windows, key=lambda key: int(key))]
    return {
        "modelId": report["modelId"],
        "directory": relative,
        "reusedEstimatorFrom": report["reusedEstimatorFrom"],
        "holdoutCity": report.get("holdoutCity"),
        "horizonHours": report["horizonHours"],
        "splits": report["splits"],
        "pointRule": first["pointRule"],
        "pseudoCount": {step: window["pseudoCount"] for step, window in windows.items()},
        "priorWeightMean": round(float(np.mean([window["priorWeightMean"] for window in windows.values()])), 4),
        "zeroSupportShare": round(float(np.mean([window["zeroSupportShare"] for window in windows.values()])), 4),
        "test": pooled,
        "testModelAlone": alone,
        "testModelWithChosenRule": round(full_rule, 4),
        "maeChangePct": _gain(full, full_base),
        "maeChangeFromPriorPct": _gain(full, full_rule),
        "validation": windows,
    }


def _build(frame, data: pd.DataFrame, directory: Path, source: Path, target: Path, relative: str) -> dict:
    bundle, metadata = artifacts.load_bundle(directory)
    forecaster.validate_bundle_frame(bundle, metadata, frame)
    base_model = bundle["model"]
    horizon = int(bundle["horizonHours"])
    holdout = bundle["excludeCity"]
    columns = bundle["featureColumns"]
    split_column = frame.export.split_column(horizon)

    keep = pd.Series(True, index=data.index)
    if holdout:
        keep = data["city_id"] != holdout
    train_rows = keep & (data[split_column] == "TRAIN")
    validation_rows = keep & (data[split_column] == "VALIDATION")
    scored_rows = (~keep & (data[split_column] == "TEST")) if holdout else (keep & (data[split_column] == "TEST"))

    steps = sorted(base_model.steps)
    prior = HourCellPrior.fit(data.loc[train_rows], steps=steps, classes=base_model.classes)
    model = HierarchicalForecastModel(base=base_model, prior=prior)
    # A cold-start bundle is deployed on a city it never saw, so it is calibrated on that city's
    # VALIDATION rows - the in-city rows would resolve to station cells and measure nothing about
    # the borrowed-level regime it actually serves.  TEST is still never touched before the end.
    tuning_rows = ((data["city_id"] == holdout) & (data[split_column] == "VALIDATION")
                   if holdout else validation_rows)
    tuning_labels = {step: data.loc[tuning_rows, AVAILABILITY.label_column(step)].to_numpy(dtype=float)
                     for step in steps}
    calibration = model.calibrate(data.loc[tuning_rows, columns],
                                  data.loc[tuning_rows, KEY_COLUMNS],
                                  tuning_labels)

    per_step: dict[int, dict] = {}
    base_step: dict[int, dict] = {}
    base_rule: dict[int, dict] = {}
    for step in steps:
        label = AVAILABILITY.label_column(step)
        truth = data.loc[scored_rows, label].to_numpy(dtype=float)
        x_test = data.loc[scored_rows, columns]
        per_step[step] = base_train.score_step(model, step, x_test, truth,
                                               data.loc[scored_rows], data.loc[scored_rows, KEY_COLUMNS])
        base_step[step] = base_train.score_step(base_model, step, x_test, truth, data.loc[scored_rows])
        # The same estimator judged with the rule this bundle chose, to separate "the prior helped"
        # from "the 0.2.0 bundles were reporting the mode while calling it the median".
        base_rule[step] = {"mae": metrics.mae(
            truth, point_value(model.classes, base_model.distribution_for(x_test, step), model.point_rule)),
            "points": int(len(truth))}
        per_step[step]["calibration"] = calibration[step]
        per_step[step]["baseMae"] = base_step[step]["mae"]
        per_step[step]["baseRuleMae"] = base_rule[step]["mae"]

    pooled = base_train.pool_metrics(per_step)
    pooled_base = base_train.pool_metrics(base_step)
    rule_weights = np.array([entry["points"] for entry in base_rule.values()], dtype=float)
    pooled_rule = float(np.average([entry["mae"] for entry in base_rule.values()], weights=rule_weights))
    model_id, version, rounds = _naming(bundle)
    shipped = dict(bundle)
    shipped.update({
        "model": model,
        "modelId": model_id,
        "modelVersion": version,
        "hierarchical": True,
        "pointRule": model.point_rule,
        "pseudoCount": {str(step): model.pseudo_count[step] for step in steps},
        "priorLevels": PRIOR_LEVELS,
        "inheritedFrom": f"{source.name}/{directory.relative_to(source).as_posix()}",
        # The site level's key per station, recorded so a served request does not have to re-derive
        # it.  It deliberately does NOT go inside ``stations``: that profile is the numeric one-hot
        # block the online feature builder reads, and a ``site_``-prefixed string in it is parsed as
        # a one-hot by anything that scans the prefix.
        "stationSiteKeys": {station_id: site_key_of(profile)
                            for station_id, profile in bundle["stations"].items()},
    })
    rebuilt = artifacts.build_metadata(
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
    # The four-decimal figures the report and the run's table share, computed once: the entry the
    # payload shows and the metrics the bundle stores are then the same numbers by construction.
    test_rounded = {key: round(value, 4) if isinstance(value, float) else value for key, value in pooled.items()}
    alone_rounded = {key: round(value, 4) if isinstance(value, float) else value
                     for key, value in pooled_base.items()}
    rule_rounded = round(pooled_rule, 4)
    entry = {
        "modelId": model_id,
        "directory": relative,
        "reusedEstimatorFrom": shipped["inheritedFrom"],
        "holdoutCity": holdout,
        "horizonHours": horizon,
        "splits": {"train": int(train_rows.sum()), "validation": int(validation_rows.sum()),
                   "tuning": int(tuning_rows.sum()), "scored": int(scored_rows.sum())},
        "pointRule": model.point_rule,
        "pseudoCount": {str(step): dict(model.pseudo_count[step]) for step in steps},
        "priorWeightMean": round(float(np.mean([calibration[step]["priorWeightMean"] for step in steps])), 4),
        "zeroSupportShare": round(float(np.mean([calibration[step]["zeroSupportShare"] for step in steps])), 4),
        "test": test_rounded,
        "testModelAlone": alone_rounded,
        "testModelWithChosenRule": rule_rounded,
        "maeChangePct": _gain(pooled["mae"], pooled_base["mae"]),
        "maeChangeFromPriorPct": _gain(pooled["mae"], pooled_rule),
        "validation": {str(step): calibration[step] for step in steps},
    }
    report = {
        "modelId": model_id,
        "modelVersion": version,
        "horizonHours": horizon,
        "holdoutCity": holdout,
        "seed": RUN_CONTEXT.get("seed"),
        "reproducibleCommand": RUN_CONTEXT.get("command"),
        "roundsMultiplier": rounds,
        "reusedEstimatorFrom": shipped["inheritedFrom"],
        "sourceModelId": metadata.get("modelId"),
        "sourceMetrics": metadata.get("metrics"),
        "splits": entry["splits"],
        "pointValue": f"distribution {model.point_rule} of the shrunk mixture (integer chargers); "
                      "the rule was chosen on VALIDATION",
        "pooledTest": entry["test"],
        "pooledBaseTest": entry["testModelAlone"],
        "pooledBaseWithChosenRuleTest": {"mae": entry["testModelWithChosenRule"],
                                         "points": int(pooled_base["points"])},
        "calibration": {str(step): calibration[step] for step in steps},
        "perStep": per_step,
        # The row this bundle contributes to ``hierarchical_report.md``, saved with the bundle so a
        # resumed run reads the published table verbatim instead of rebuilding the row from metrics.
        "payloadEntry": entry,
        "preparedAt": artifacts.stamp(),
    }
    artifacts.save_bundle(target, shipped, rebuilt, report)
    print(f"[{relative} -> {model_id}] TEST MAE {pooled['mae']:.4f} "
          f"(model alone {pooled_base['mae']:.4f}, same model with the chosen rule {pooled_rule:.4f}) "
          f"coverage@{model.nominal_coverage:.0%} {pooled['coverage']:.3f} rule={model.point_rule} "
          f"n={pooled['points']}")
    return dict(entry, reused=False)


def _write(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _markdown(payload: dict) -> str:
    lines = [
        "# 空闲桩数预测 · 层级收缩（模型 + 站点×小时先验）报告",
        "",
        f"- 复用估计器来源：`{payload['reusedEstimatorFrom']}`（未重新拟合梯度提升）",
        f"- 数据集 / 发布批次：`{payload['datasetId']}` / `{payload['trainingPublishedBatchId']}`",
        f"- 原始数据清单 sha256：`{payload['sourceManifestSha256']}`",
        f"- 先验层级：{' → '.join(payload['priorLevels'])} → 全局，键为**被预测小时**（`hour_of_day + step - 1`）",
        "- 自由参数只有三类：点值规则 1 个、每一层伪计数 k（每步每层）、区间水平 1 个；全部在 VALIDATION 上选取，TEST 只在最后评分一次",
        f"- 模型版本：`{payload['modelVersion']}`；生成耗时 {payload['elapsedSeconds']} 秒",
        "",
        f"> {CAVEAT}",
        "",
        "## 1. TEST 区间：混合模型 vs 单独的提升模型",
        "",
        "| 来源 | 跨度 | 留出城市 | 点数 | 0.2.0 模型 MAE | 同模型改判规则 | 混合 MAE | 规则修正 | 先验贡献 | 点值规则 | 平均先验权重 | 覆盖率 | 区间宽度 | 合法率 | 风险 AUC |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for name in sorted(payload["bundles"]):
        entry = payload["bundles"][name]
        test, alone = entry["test"], entry["testModelAlone"]
        with_rule = entry["testModelWithChosenRule"]
        lines.append(
            f"| `{name}` | h{entry['horizonHours']:02d} | {entry['holdoutCity'] or '-'} | {test['points']} "
            f"| {alone['mae']:.4f} | {with_rule:.4f} | {test['mae']:.4f} "
            f"| {100.0 * (1.0 - with_rule / alone['mae']):+.2f}% | {entry['maeChangeFromPriorPct']:+.2f}% "
            f"| {entry['pointRule']} "
            f"| {entry['priorWeightMean']:.3f} | {test['coverage']:.4f} | {test['intervalWidth']:.3f} "
            f"| {test['legality']:.4f} | {test['depletionAUC']:.4f} |")
    lines += [
        "",
        "两列变化分开看：`规则修正` 只把 0.2.0 的提升分布按选定的点值规则重新判一次（0.2.0 产物存的是众数，",
        "却把那一列写成 median），`先验贡献` 才是在这一基础上再加站点×小时先验带来的 MAE 下降；两者相加约等于",
        "`0.2.0 模型 MAE` → `混合 MAE` 的总变化。为正表示 MAE 下降。",
        "`平均先验权重` 是 `n/(n+k)` 在 VALIDATION 行之上的均值，",
        "`n` 是该行所属格子里的 TRAIN 小时数；`n = 0` 的行（训练区间里从未出现过的站点/城市）无论 k 取多少都退回模型本身。",
        "",
        "## 2. 每一层单独选出的收缩常数 k（VALIDATION，TEST 未参与）",
        "",
        "| 来源 | 层级 | VALIDATION 行数（各步均值） | k 取值 | 该层仅模型 MAE | 该层混合 MAE | 该层仅先验 MAE |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for name in sorted(payload["bundles"]):
        entry = payload["bundles"][name]
        windows = list(entry["validation"].values())
        for level in PRIOR_LEVELS:
            rows = [window["validationRowsByLevel"].get(level, 0) for window in windows]
            if not any(rows):
                continue
            picks = [float(window["pseudoCount"].get(level, NO_PRIOR)) for window in windows]
            finite = sorted({pick for pick in picks if pick < NO_PRIOR})
            thin = [level in window.get("thinLevels", [])
                    for window, pick in zip(windows, picks) if pick >= NO_PRIOR]
            label = f"{finite[0]:g}" if len(finite) == 1 else (
                f"{finite[0]:g}…{finite[-1]:g}" if finite else "关闭")
            if len(finite) > 1:
                label += f"（{len(finite)} 档）"
            if thin:
                reason = ("行数不足" if all(thin) else
                          ("验证显示借用不划算" if not any(thin) else "部分步行数不足"))
                label += (f"（{reason}）" if not finite
                          else f"，{len(thin)}/{len(picks)} 步关闭（{reason}）")

            def mean(key: str) -> float:
                return float(np.mean([window[key].get(level, window["validationMae"]) for window in windows]))

            lines.append(f"| `{name}` | {level} | {int(np.mean(rows))} | {label} "
                         f"| {mean('modelMaeByLevel'):.4f} | {mean('validationMaeByLevel'):.4f} "
                         f"| {mean('priorOnlyMaeByLevel'):.4f} |")
    in_domain = [entry for entry in payload["bundles"].values() if not entry["holdoutCity"]]
    cold = [entry for entry in payload["bundles"].values() if entry["holdoutCity"]]

    def mean_prior_gain(entries: list[dict]) -> float | None:
        values = [entry["maeChangeFromPriorPct"] for entry in entries if entry["maeChangeFromPriorPct"] is not None]
        return float(np.mean(values)) if values else None

    def mean_rule_gain(entries: list[dict]) -> float | None:
        if not entries:
            return None
        return float(np.mean([100.0 * (1.0 - entry["testModelWithChosenRule"] / entry["testModelAlone"]["mae"])
                              for entry in entries]))

    def percent(value: float | None) -> str:
        """A partial run (``--only`` / ``--resume``) may legitimately hold no bundles of one kind."""
        return "不适用（本次运行没有该类 bundle）" if value is None else f"{value:+.2f}%"

    def mean_k(entries: list[dict]) -> float:
        values = [float(count) for entry in entries for window in entry["validation"].values()
                  for level, count in window["pseudoCount"].items() if level != "global" and count < NO_PRIOR]
        return float(np.mean(values)) if values else float("nan")

    trend = "、".join(f"h{entry['horizonHours']:02d} 平均 k≈{mean_k([entry]):.0f}"
                     for entry in sorted(in_domain, key=lambda item: item["horizonHours"]))

    domain_line = (f"- 域内包（{len(in_domain)} 个）：点值规则修正平均带来 "
                   f"{percent(mean_rule_gain(in_domain))} 的 MAE 下降，"
                   f"再加上站点×小时先验又多 {percent(mean_prior_gain(in_domain))}。先验只在 `station` 层被采信，"
                   f"且跨度越长采信得越多（{trend}，k 越小越信查表）。"
                   if in_domain else
                   "- 域内包（0 个）：这一运行不含域内 bundle，第 1、2 节的域内结论对它不作任何断言。")
    cold_line = (f"- 冷启动包（{len(cold)} 个）：改善全部来自点值规则（{percent(mean_rule_gain(cold))}），"
                 f"先验只贡献 {percent(mean_prior_gain(cold))}（负号是在 TEST 上略微亏掉的部分）—— "
                 "这些城市的站点在 TRAIN 里没有任何 station 格子，"
                 "唯一能借的是 `site` 层，而验证显示借它比只信模型还差（第 2 节里“该层混合 MAE”高于“该层仅模型 MAE”的行），"
                 "于是多数步数把它关掉，退回纯模型。这不是失败，是收缩机制应有的行为：没有本地历史就不该假装查表有用。"
                 if cold else
                 "- 冷启动包（0 个）：这一运行不含留出城市（`--only` 或 `--resume` 的部分运行只列出它"
                 "实际写出的 bundle），所以本节的冷启动结论对它不作任何断言。")

    lines += [
        "",
        "读法：`k = 0` 表示完全采信查表，`k` 越大越偏向模型；`关闭` 表示该层按 VALIDATION 选了 "
        f"{NO_PRIOR:g}，等于退回纯模型，原因分两种——行数不足 {MIN_LEVEL_ROWS} 无从估计，或估计出来借用不划算。",
        "三个 MAE 列给出收缩的两端与端点之间的取值，用来核对收缩方向没有反：`关闭` 行的“该层混合 MAE”"
        "必须与“该层仅模型 MAE”一字不差。",
        "",
        "## 3. 结论",
        "",
        domain_line,
        cold_line,
        "- `0.2.0` 报告里“冷启动退回纯模型”那句话当时是反的：`_shrink` 被当成了模型的权重，"
        "`n = 0` 的行拿到的其实是查表中位数；本版把权重方向、每层独立的 k、以及可选项“关闭”一起写进产物，"
        "并由 `test_ml_contract.PriorShrinkageTest` 钉住。",
        "",
        "## 4. 口径说明",
        "",
        "- 指标单位为空闲充电桩数量（0–3 桩），不是百分比。",
        "- 点值规则在中位数/众数之间按 VALIDATION MAE 择优选出并写入产物，`model_metadata.json` 只保留 mae/rmse/testSamples/unit 四项。",
        f"- {CAVEAT}",
    ]
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    raise SystemExit(main())
