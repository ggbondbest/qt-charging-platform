"""Screen stronger models for the availability line.  Selection reads VALIDATION only.

    python -m data_analysis.ml.availability.model_search --horizon 6

Why this script exists: the shipped 0.3.0 forecaster (``build_hierarchical``) reaches its accuracy
by blending a boosted model with a station x hour lookup table, and the shipped 75 features
**never name the station** - the model sees lagged power and availability, calendar, city and site
type, but nothing that separates ``ST-BJ-01`` from ``ST-BJ-02`` except through their own history.
That is a plausible reason the model alone is no better than the table it gets blended with, so the
first thing to try on this dataset is not a fancier estimator, it is telling the estimator which
station it is forecasting.

Configurations screened here (all share the same TRAIN/VALIDATION rows and the same median rule):

``control``             shipped 75 features, shipped ``HistGradientBoostingClassifier`` settings.
``cell_features``       + the station -> site -> city answer-hour cell as *input columns*
                        (pmf over 0..3, its median, its support, which level answered).
``cell_features_deep``  ``cell_features`` with a deeper, slower-tuned booster.
``cell_features_ordinal`` ``cell_features`` with cumulative-hazard fits (P(y>=1),P(y>=2),P(y>=3)),
                        which respects the ordinal scale instead of treating 0..3 as nominal.
``cell_features_et``    ``cell_features`` with Extremely Randomized Trees.
``cell_plus_prior``     ``cell_features``'s model, then shrunk towards its own cell with the same
                        ``n/(n+k)`` rule the shipped 0.3.0 uses, ``k`` picked per step from
                        :data:`prior.SUPPORT_GRID`.  This is the stacking question: the cell is now
                        both an input *and* the thing the output is blended with, so it answers
                        whether the shipped prior still adds anything once the model can see it.
                        Its ``k`` is chosen on the same rows it is scored on, so its number is
                        optimistic by the amount any tuned weight is.
``table_only``          the cell distribution itself, no model (the honest floor to beat).
``blend_grid``          per-step weight between ``control`` and ``table_only``, chosen on
                        VALIDATION - i.e. what the shipped empirical-Bayes shrinkage approximates.

Nothing here touches TEST.  A winner has to be confirmed on TEST separately (``--confirm-test``, one
readout, and it prints a warning that this is a confirmation, not a selection), and shipping it
means a real retrain + build + evaluate, which is the lead's call, not this script's.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np
import pandas as pd
from sklearn.ensemble import ExtraTreesClassifier, HistGradientBoostingClassifier

from data_analysis.ml.availability import prior
from data_analysis.ml.common import artifacts, forecaster, metrics
from data_analysis.ml.common.data_io import DEFAULT_EXPORT
from data_analysis.ml.common.tasks import AVAILABILITY

#: shipped ``model.OrdinalForecastModel.fit_step`` settings, copied so the control is comparable
CONTROL_PARAMS = {"max_iter": 250, "learning_rate": 0.06, "max_leaf_nodes": 31,
                  "min_samples_leaf": 40, "early_stopping": True, "validation_fraction": 0.1,
                  "n_iter_no_change": 15}
DEEP_PARAMS = {**CONTROL_PARAMS, "max_iter": 500, "learning_rate": 0.03, "max_leaf_nodes": 63,
               "min_samples_leaf": 20, "l2_regularization": 1.0, "n_iter_no_change": 25}
#: columns the cell adds; kept explicit so the report can say what the model was told
CELL_COLUMNS = [f"cell_p{value}" for value in range(4)] + ["cell_median", "cell_expectation",
                                                           "cell_support", "cell_level"]
LEVEL_CODE = {"station": 0.0, "site": 1.0, "city": 2.0, "global": 3.0}
CONFIGS = ("control", "cell_features", "cell_features_deep", "cell_features_ordinal",
           "cell_features_et", "cell_plus_prior", "table_only", "blend_grid")
SIMULATED = "全部指标为模拟数据测试结果（第二阶段发布批次），不代表真实运营数据表现。"


def _median_of(probability: np.ndarray, classes: np.ndarray) -> np.ndarray:
    """MAE-optimal integer point value: first class whose CDF reaches one half."""
    cumulative = np.cumsum(probability, axis=1)
    return classes[(cumulative < 0.5).sum(axis=1)]


def cell_table(train: pd.DataFrame, *, steps, classes) -> prior.HourCellPrior:
    """TRAIN-only station/site/city x answer-hour distributions, reused as features or as a prior."""
    return prior.HourCellPrior.fit(prior.add_site_key(train), steps=steps, classes=classes)


def cell_features(table: prior.HourCellPrior, step: int, rows: pd.DataFrame) -> pd.DataFrame:
    """The cell a row falls in, widened into input columns.  Nothing from the row's own label."""
    probability, support, levels = table.probabilities(step, rows)
    classes = table.classes
    frame = pd.DataFrame({f"cell_p{int(value)}": probability[:, index]
                          for index, value in enumerate(classes)})
    frame["cell_median"] = _median_of(probability, classes)
    frame["cell_expectation"] = probability @ classes
    frame["cell_support"] = support
    frame["cell_level"] = [LEVEL_CODE.get(str(name), 3.0) for name in levels]
    return frame.reset_index(drop=True)


def _fit_predict(kind: str, x_train, y_train, x_score, *, seed: int, step: int,
                 classes: np.ndarray) -> np.ndarray:
    """Return a predictive pmf over ``classes`` for ``x_score`` under one configuration."""
    if kind == "table_only":
        raise AssertionError("table_only never fits; its pmf is the cell itself")
    if kind == "cell_features_ordinal":
        hazards = []
        for threshold in classes[1:]:
            model = HistGradientBoostingClassifier(random_state=seed + step, **CONTROL_PARAMS)
            model.fit(x_train, (y_train >= int(threshold)).astype(int))
            hazards.append(model.predict_proba(x_score)[:, 1])
        above = np.column_stack(hazards)                      # P(y>=1), P(y>=2), P(y>=3)
        above = np.minimum.accumulate(above, axis=1)          # enforce monotonicity
        probability = np.empty((len(above), len(classes)), dtype=float)
        probability[:, 0] = 1.0 - above[:, 0]
        for index in range(1, len(classes) - 1):
            probability[:, index] = above[:, index - 1] - above[:, index]
        probability[:, -1] = above[:, -1]
        return _normalise(np.clip(probability, 0.0, None))
    if kind == "cell_features_et":
        model = ExtraTreesClassifier(n_estimators=400, min_samples_leaf=5, random_state=seed + step,
                                     n_jobs=-1)
    else:
        parameters = DEEP_PARAMS if kind == "cell_features_deep" else CONTROL_PARAMS
        model = HistGradientBoostingClassifier(random_state=seed + step, **parameters)
    model.fit(x_train, y_train)
    raw = model.predict_proba(x_score)
    full = np.zeros((raw.shape[0], len(classes)), dtype=float)
    positions = {int(value): index for index, value in enumerate(classes)}
    for index, value in enumerate(model.classes_):
        full[:, positions[int(value)]] = raw[:, index]
    return _normalise(full)


def _normalise(probability: np.ndarray) -> np.ndarray:
    total = probability.sum(axis=1, keepdims=True)
    return np.divide(probability, total, out=np.full_like(probability, 1.0 / probability.shape[1]),
                     where=total > 0)


def _score(truth: np.ndarray, point: np.ndarray, classes: np.ndarray) -> dict:
    return {"mae": round(metrics.mae(truth, point.astype(float)), 4),
            "rmse": round(metrics.rmse(truth, point.astype(float)), 4),
            "exactHit": round(float(np.mean(point == truth)), 4),
            "withinOne": round(float(np.mean(np.abs(point - truth) <= 1)), 4),
            "points": int(len(truth))}


def screen(export_dir: Path, horizon: int, *, configurations, seed: int,
           read_test: bool) -> dict:
    frame = forecaster.build_frame(export_dir)
    data = frame.data
    columns = frame.feature_columns
    split = frame.export.split_column(horizon)
    steps = range(1, horizon + 1)
    classes = np.arange(0, int(data["capacity"].max()) + 1)
    data = prior.add_site_key(data)

    train_rows = data[split] == "TRAIN"
    score_rows = data[split] == ("TEST" if read_test else "VALIDATION")
    train = data.loc[train_rows]
    scored = data.loc[score_rows]
    for label in (AVAILABILITY.label_column(step) for step in steps):
        if train[label].isna().any() or scored[label].isna().any():
            raise ValueError(f"{label} has NaN rows in the splits being used")
    print(f"[data] train={len(train)} scored={len(scored)} ({'TEST confirmation' if read_test else 'VALIDATION selection'})")

    table = cell_table(train, steps=steps, classes=classes)
    cells = {step: cell_features(table, step, scored) for step in steps}
    cells_train = {step: cell_features(table, step, train) for step in steps}
    # ``blend_grid`` mixes the shipped model with the table, so it needs the control fitted even
    # when the caller only asked for the blend; it just is not reported in that case.
    need_control = "control" in configurations or "blend_grid" in configurations
    blend_weights: dict[str, dict] = {"blend_grid": {}}
    prior_k: dict[str, dict] = {"cell_plus_prior": {}}

    results: dict[str, dict] = {name: {"perStep": {}, "pooled": {}} for name in configurations}
    started = time.time()
    for step in steps:
        label = AVAILABILITY.label_column(step)
        truth = scored[label].to_numpy(dtype=float)
        base_train, base_score = train.loc[:, columns], scored.loc[:, columns]
        wide_columns = [*columns, *CELL_COLUMNS]
        wide_train = pd.concat([base_train.reset_index(drop=True), cells_train[step]], axis=1)[wide_columns]
        wide_score = pd.concat([base_score.reset_index(drop=True), cells[step]], axis=1)[wide_columns]

        model_pmf: dict[str, np.ndarray] = {}
        if need_control:
            model_pmf["control"] = _fit_predict("control", base_train, train[label], base_score,
                                                seed=seed, step=step, classes=classes)
        for kind in ("cell_features", "cell_features_deep", "cell_features_ordinal",
                     "cell_features_et"):
            wanted = kind in configurations or (kind == "cell_features"
                                                and "cell_plus_prior" in configurations)
            if wanted:
                model_pmf[kind] = _fit_predict(kind, wide_train, train[label], wide_score,
                                               seed=seed, step=step, classes=classes)
        if ("table_only" in configurations or "blend_grid" in configurations
                or "cell_plus_prior" in configurations):
            model_pmf["table_only"] = cells[step][[f"cell_p{value}" for value in range(4)]].to_numpy()

        if "cell_plus_prior" in configurations:
            # The shipped 0.3.0 blend, but on top of the model that can already see the cell: does
            # the prior still earn its place once the estimator has been told which station it is?
            support = cells[step]["cell_support"].to_numpy(dtype=float)
            best: tuple[float, dict] | None = None
            for pseudo_count in prior.SUPPORT_GRID:
                weight = prior.prior_weight(support, pseudo_count)[:, None]
                mixed = weight * model_pmf["table_only"] + (1 - weight) * model_pmf["cell_features"]
                candidate = _score(truth, _median_of(mixed, classes), classes)
                if best is None or candidate["mae"] < best[1]["mae"]:
                    best = (pseudo_count, candidate)
            results["cell_plus_prior"]["perStep"][step] = best[1]
            prior_k["cell_plus_prior"][step] = prior.format_pseudo_count(best[0])

        for kind, probability in model_pmf.items():
            if kind not in configurations:
                continue
            results[kind]["perStep"][step] = _score(truth, _median_of(probability, classes), classes)

        if "blend_grid" in configurations:
            best = None
            for weight in (0.0, 0.25, 0.5, 0.75, 1.0):
                mixed = (1 - weight) * model_pmf["control"] + weight * model_pmf["table_only"]
                scored_rows = _score(truth, _median_of(mixed, classes), classes)
                if best is None or scored_rows["mae"] < best[1]["mae"]:
                    best = (weight, scored_rows)
            results["blend_grid"]["perStep"][step] = best[1]
            blend_weights["blend_grid"][step] = best[0]

        print(f"[step {step:02d}] " + "  ".join(
            f"{name}={results[name]['perStep'][step]['mae']:.4f}"
            for name in CONFIGS if name in results and results[name]["perStep"]))

    for name in configurations:
        per_step = results[name]["perStep"]
        weights = np.array([per_step[step]["points"] for step in sorted(per_step)], dtype=float)
        results[name]["pooled"] = {
            key: round(float(np.average([per_step[step][key] for step in sorted(per_step)],
                                        weights=weights)), 4)
            for key in ("mae", "rmse", "exactHit", "withinOne")}
        results[name]["pooled"]["points"] = int(weights.sum())
    return {"horizonHours": horizon, "split": "TEST" if read_test else "VALIDATION",
            "seed": seed, "elapsedSeconds": round(time.time() - started, 1),
            "trainRows": int(len(train)), "scoredRows": int(len(scored)),
            "blendWeights": blend_weights, "priorK": prior_k, "results": results,
            "featureColumns": len(columns), "cellColumns": CELL_COLUMNS}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export", default=str(DEFAULT_EXPORT))
    parser.add_argument("--horizon", type=int, default=6, choices=[1, 6, 24])
    parser.add_argument("--configs", nargs="+", default=list(CONFIGS), choices=list(CONFIGS))
    parser.add_argument("--seed", type=int, default=20260913)
    parser.add_argument("--output", required=True)
    parser.add_argument("--confirm-test", action="store_true",
                        help="score TEST instead of VALIDATION; a readout, not a selection")
    arguments = parser.parse_args(argv)

    output = Path(arguments.output)
    if output.exists() and any(output.iterdir()):
        raise SystemExit(f"{output} is not empty; pass a new --output directory (published runs are "
                         f"evidence, not build artefacts)")
    if arguments.confirm_test:
        print("[warning] --confirm-test reads TEST.  Do this once, for a configuration already "
              "chosen on VALIDATION, or the number stops meaning anything.")
    payload = screen(Path(arguments.export), arguments.horizon, configurations=arguments.configs,
                     seed=arguments.seed, read_test=arguments.confirm_test)
    payload["command"] = artifacts.invocation(__name__, argv)
    payload["export"] = str(arguments.export)
    payload["caveat"] = SIMULATED
    output.mkdir(parents=True, exist_ok=True)
    (output / "model_search.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
                                              encoding="utf-8")
    (output / "model_search.md").write_text(_markdown(payload), encoding="utf-8")
    ranking = sorted(payload["results"].items(), key=lambda item: item[1]["pooled"]["mae"])
    print(f"[ranking on {payload['split']}]")
    for name, entry in ranking:
        pooled = entry["pooled"]
        print(f"  {name:24s} MAE {pooled['mae']:.4f}  RMSE {pooled['rmse']:.4f}  "
              f"exact {pooled['exactHit']:.1%}  within1 {pooled['withinOne']:.1%}")
    print(f"[done] {output} in {payload['elapsedSeconds']}s")
    return 0


def _markdown(payload: dict) -> str:
    lines = [f"# 模型筛选：h{payload['horizonHours']:02d}，在 {payload['split']} 上比较", "",
             SIMULATED, "", f"- 复现命令：`{payload['command']}`",
             f"- 训练行 {payload['trainRows']}，评分行 {payload['scoredRows']}，"
             f"基础特征 {payload['featureColumns']} 列，单元特征新增 {len(payload['cellColumns'])} 列",
             f"- 用时 {payload['elapsedSeconds']} s", "",
             "| 配置 | MAE | RMSE | 完全命中 | ±1 以内 | 相对 control |", "| --- | --- | --- | --- | --- | --- |"]
    control = payload["results"].get("control", {}).get("pooled", {}).get("mae")
    for name, entry in sorted(payload["results"].items(), key=lambda item: item[1]["pooled"]["mae"]):
        pooled = entry["pooled"]
        delta = "—" if not control or name == "control" else f"{pooled['mae'] / control - 1:+.1%}"
        lines.append(f"| `{name}` | {pooled['mae']:.4f} | {pooled['rmse']:.4f} | "
                     f"{pooled['exactHit']:.1%} | {pooled['withinOne']:.1%} | {delta} |")
    weights = payload.get("blendWeights", {}).get("blend_grid") or {}
    if weights:
        lines += ["", "`blend_grid` 每步在 VALIDATION 上选出的混合权重（0=纯模型，1=纯查表）："
                  + "，".join(f"step {step}→{weight:g}" for step, weight in sorted(
                      weights.items(), key=lambda item: int(item[0])))]
    pseudo_counts = payload.get("priorK", {}).get("cell_plus_prior") or {}
    if pseudo_counts:
        lines += ["", "`cell_plus_prior` 每步选出的伪计数 k（`off` = 完全不信查表）：",
                  "，".join(f"step {step}→{value}" for step, value in sorted(
                      pseudo_counts.items(), key=lambda item: int(item[0])))]
    names = list(payload["results"])
    steps = sorted({int(step) for name in names for step in payload["results"][name]["perStep"]})
    lines += ["", "## 每步 MAE", "", "| step | " + " | ".join(f"`{name}`" for name in names) + " |",
              "| --- | " + " | ".join("---" for _ in names) + " |"]
    for step in steps:
        cells = []
        for name in names:
            per_step = {int(key): value for key, value in payload["results"][name]["perStep"].items()}
            cells.append(f"{per_step[step]['mae']:.4f}" if step in per_step else "—")
        lines.append(f"| {step} | " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    raise SystemExit(main())
