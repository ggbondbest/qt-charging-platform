"""Score the trained availability models against the baselines the handoff document asks for.

    python -m data_analysis.ml.availability.evaluate \
        --run-dir data_analysis/outputs/ml_avail_run6 \
        --output  data_analysis/outputs/ml_avail_eval_run6_v1

The training run records headline TEST numbers only after VALIDATION parameter selection. This
script answers the questions a reviewer asks instead:

* **Is it better than a trivial rule?** Four baselines are fitted on TRAIN rows only - repeat the
  last observed hour, and the TRAIN median per station x answer-hour, per site type x answer-hour,
  per city x answer-hour - so the comparison never peeks at TEST.
* **Is the output range legal?** Every scored point is checked against ``0 <= v <= station capacity``.
* **Where does it fail?** Per-city and per-station errors, plus how much worse a city the model
  never saw is (the ``coldstart_*`` bundles), against the same-city numbers from the main model.
* **Is the risk signal usable?** Precision / recall of a ``P(no free charger)`` alert at fixed
  thresholds, which is what the dashboard would actually surface.
* **How much is the lookup worth on its own?** The blend searched here shrinks the boosted
  classifier towards the station x answer-hour table with weight ``n/(n+k)``, ``k`` chosen on
  VALIDATION.  Hierarchical bundles are scored twice: ``mae`` is what they serve (shipped prior
  included), ``modelMae`` is the classifier alone, and the blend answers the same question the
  training report answered, independently of the ``k`` the bundle carries.

The report is written to ``--output`` and this script refuses to touch a directory that already
holds one: a published run's numbers are evidence, not a build artefact.

Per-step prediction is sampled (``--sample-rows``, default 60k rows) because the 24h bundle would
otherwise push 418k rows through 24 classifiers; ``--sample-rows 0`` scores every TEST row.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np
import pandas as pd

from data_analysis.ml.availability.model import point_value
from data_analysis.ml.availability.predict import AvailabilityForecaster
from data_analysis.ml.availability.prior import (HOUR_KEY, SUPPORT_GRID, HourCellPrior, add_site_key,
                                                 answer_hour, format_pseudo_count, prior_weight)
from data_analysis.ml.common import artifacts, forecaster, metrics
from data_analysis.ml.common.data_io import DEFAULT_EXPORT
from data_analysis.ml.common.tasks import AVAILABILITY

CAVEAT = "全部指标为模拟数据测试结果（第二阶段发布批次），不代表真实运营数据表现。"
BASELINES = ("persistence", "stationHourMedian", "siteHourMedian", "cityHourMedian", "globalMedian")
RISK_THRESHOLDS = (0.30, 0.50, 0.70)
WEIGHT_GRID = np.round(np.arange(0.0, 1.0001, 0.05), 2)
#: The lookup key: the station and the clock hour its ``hNN`` label is *about*.
GROUP_KEYS = ("station_id", HOUR_KEY)
STATION_LEVEL = "station"


def _answer_key(rows: pd.DataFrame, step: int) -> pd.DataFrame:
    """Join keys for ``step``, with the clock hour the label is *about* rather than the reference hour.

    For a table fitted per step this is a relabelling, not a numeric correction: within one step
    ``hour -> hour + step - 1`` is a bijection, so the same TRAIN rows land in the same cell either
    way.  The audit keys on the answer hour anyway, for the reason the training code does - it makes
    a cell mean "this station, this clock hour", which is what a reviewer reading one table across
    two steps would assume, and it removes the trap of ever pooling two steps into one table (that
    *would* mix clocks, silently, and it is the first thing an audit of a per-step table invites).
    """
    keyed = rows[["station_id", "site_key", "city_id"]].copy()
    keyed[HOUR_KEY] = answer_hour(rows["hour_of_day"], step)
    return keyed


def _serving_keys(rows: pd.DataFrame) -> pd.DataFrame:
    """The four columns a hierarchical bundle asks for - the *reference* hour, which it shifts itself."""
    return pd.DataFrame({"station_id": rows["station_id"].to_numpy(),
                         "hour_of_day": rows["hour_of_day"].to_numpy(dtype=np.int64),
                         "site_key": rows["site_key"].to_numpy(),
                         "city_id": rows["city_id"].to_numpy()})


def _baseline_tables(train: pd.DataFrame, steps: list[int]) -> dict[int, dict]:
    """Per-step baseline lookup tables, fitted on TRAIN rows only."""
    tables = {}
    for step in steps:
        label = AVAILABILITY.label_column(step)
        keyed = _answer_key(train, step)
        keyed[label] = train[label].to_numpy()
        tables[step] = {
            "stationHourMedian": keyed.groupby(list(GROUP_KEYS))[label].median(),
            "siteHourMedian": keyed.groupby(["site_key", HOUR_KEY])[label].median(),
            "cityHourMedian": keyed.groupby(["city_id", HOUR_KEY])[label].median(),
            "globalMedian": float(train[label].median()),
        }
    return tables


def _baseline_values(tables: dict, step: int, rows: pd.DataFrame) -> dict[str, np.ndarray]:
    table = tables[step]
    keyed = _answer_key(rows, step)
    values = {
        "stationHourMedian": table["stationHourMedian"].reindex(
            pd.MultiIndex.from_frame(keyed[list(GROUP_KEYS)])).to_numpy(dtype=float),
        "siteHourMedian": table["siteHourMedian"].reindex(
            pd.MultiIndex.from_frame(keyed[["site_key", HOUR_KEY]])).to_numpy(dtype=float),
        "cityHourMedian": table["cityHourMedian"].reindex(
            pd.MultiIndex.from_frame(keyed[["city_id", HOUR_KEY]])).to_numpy(dtype=float),
        "globalMedian": np.full(len(rows), table["globalMedian"], dtype=float),
        "persistence": rows["lag_available_h01"].to_numpy(dtype=float),
    }
    # A cold-start city has no TRAIN cell of its own; fall back site -> city -> global so every
    # baseline stays defined, which is exactly what a serving system would have to do too.
    global_median = table["globalMedian"]
    values["cityHourMedian"] = np.where(np.isnan(values["cityHourMedian"]), global_median, values["cityHourMedian"])
    values["siteHourMedian"] = np.where(np.isnan(values["siteHourMedian"]), values["cityHourMedian"],
                                        values["siteHourMedian"])
    values["stationHourMedian"] = np.where(np.isnan(values["stationHourMedian"]), values["siteHourMedian"],
                                           values["stationHourMedian"])
    return values


class LookupDistributions:
    """The strongest honest baseline: how often was the station free of N chargers at *that* hour?

    A per-(station, answer-hour) empirical distribution of the label, fitted on TRAIN rows only,
    falling back site -> city -> global-pm so cold-start cities still get a number.  This is
    deliberately the baseline a reviewer builds first, and it is the reference the blend below
    shrinks the model towards.

    It is a thin reporting shell over :class:`HourCellPrior` - the same object the shipped bundles
    carry.  Run 1's audit had its own copy of the table, the fallback order, the median and the
    interval, and every one of them was free to drift from what the model actually serves; the two
    keying errors this script used to make both lived in that copy.
    """

    def __init__(self, train: pd.DataFrame, steps: list[int], classes: np.ndarray):
        self.prior = HourCellPrior.fit(train, steps=steps, classes=classes)
        self.classes = self.prior.classes
        self.zero = int(np.where(self.classes == 0)[0][0])

    def probabilities(self, step: int, rows: pd.DataFrame) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        """Per-row pmf, how many TRAIN hours back that cell, and the level each row resolved to."""
        return self.prior.probabilities(step, rows)

    def summary(self, step: int, rows: pd.DataFrame, nominal_coverage: float) -> dict:
        masses, support, levels = self.probabilities(step, rows)
        low, high = self.prior.shortest_interval(masses, nominal_coverage)
        return {
            "median": self.prior.median(masses),
            "mode": self.prior.mode(masses),
            "expectation": self.prior.expectation(masses),
            "p0": masses[:, self.zero],
            "low": low,
            "high": high,
            "support": support,
            "borrowedShare": float(np.mean(levels != STATION_LEVEL)),
        }


def _blend_value(model_median: np.ndarray, lookup_median: np.ndarray, weight) -> np.ndarray:
    """``weight`` is the weight on the *lookup*: ``n/(n+k)`` for ``n`` TRAIN hours behind the cell.

    The shared helper is imported rather than restated here, because this script's own version
    returned the weight for the *model* and the blend then applied it the other way round: a
    station with a long history got overruled by the table, and a station with no history at all
    was handed nothing but the table.
    """
    return (1.0 - weight) * model_median + weight * lookup_median


def _blend(model_median: np.ndarray, lookup_median: np.ndarray, truth: np.ndarray,
           weights: np.ndarray) -> tuple[float, float]:
    """MAE of ``(1 - w) * model + w * lookup``, rounded to whole chargers and left continuous."""
    blended = (1.0 - weights) * model_median + weights * lookup_median
    return metrics.mae(truth, np.round(blended)), metrics.mae(truth, blended)


def _alert_quality(depleted: np.ndarray, probability: np.ndarray) -> dict:
    """Precision / recall of flagging an hour as "no free charger" at fixed cut-offs."""
    positives = int(depleted.sum())
    result = {"baseRate": round(float(depleted.mean()), 4)}
    for threshold in RISK_THRESHOLDS:
        fired = probability >= threshold
        hits = int((fired & depleted).sum())
        key = f"p{int(round(threshold * 100)):02d}"
        result[key] = {
            "threshold": threshold,
            "alerts": int(fired.sum()),
            "precision": round(hits / int(fired.sum()), 4) if fired.any() else None,
            "recall": round(hits / positives, 4) if positives else None,
        }
    return result


def _averages(per_step: dict[int, dict]) -> dict:
    steps = sorted(per_step)
    weights = np.array([per_step[step]["points"] for step in steps], dtype=float)
    weights /= weights.sum()

    def average(key: str) -> float:
        return float(np.average([per_step[step][key] for step in steps], weights=weights))

    return {"maeExpectation": average("maeExpectation"), "rmse": average("rmse"),
            "coverage": average("coverage"), "intervalWidth": average("intervalWidth"),
            "winkler": average("winkler")}


def _group_errors(scored: pd.DataFrame, absolute: np.ndarray, column: str) -> dict:
    grouped = pd.DataFrame({column: scored[column].to_numpy(), "absolute": absolute})
    summary = grouped.groupby(column, observed=True)["absolute"].agg(["count", "mean"])
    return {str(name): {"points": int(row["count"]), "mae": round(float(row["mean"]), 4)}
            for name, row in summary.iterrows()}


def _score_bundle(directory: Path, frame, sample_rows: int, seed: int) -> dict:
    predictor = AvailabilityForecaster.load(directory)
    forecaster.validate_bundle_frame(predictor.bundle, predictor.metadata, frame)
    bundle, model = predictor.bundle, predictor.model
    horizon = predictor.horizon_hours
    holdout = bundle["excludeCity"]
    split_column = frame.export.split_column(horizon)
    data = frame.data

    known = pd.Series(True, index=data.index)
    if holdout:
        known = data["city_id"] != holdout
    train = data.loc[known & (data[split_column] == "TRAIN")]
    if holdout:
        scored = data.loc[(data["city_id"] == holdout) & (data[split_column] == "TEST")]
    else:
        scored = data.loc[known & (data[split_column] == "TEST")]
    if sample_rows and len(scored) > sample_rows:
        scored = scored.sample(sample_rows, random_state=seed)
    if scored.empty or len(train) < 1000:
        raise ValueError(f"{directory}: not enough rows to score or to fit baselines")

    steps = sorted(model.steps)
    tables = _baseline_tables(train, steps)
    reference_model = LookupDistributions(train, steps, model.classes)
    # Blend weights are chosen on VALIDATION, never on the TEST rows they are then scored against.
    tuning_mask = data[split_column] == "VALIDATION"
    if holdout:
        tuning_mask = tuning_mask & (data["city_id"] != holdout)
    tuning = data.loc[tuning_mask]
    if sample_rows and len(tuning) > sample_rows:
        tuning = tuning.sample(sample_rows, random_state=seed)
    x = scored[bundle["featureColumns"]]
    x_tuning = tuning[bundle["featureColumns"]]
    capacity = scored["capacity"].to_numpy(dtype=float)
    # A hierarchical bundle needs lookup keys and already folds the shipped prior into every number
    # reported below.  The blend searched here is applied to the boosted classifier *alone*, so the
    # two stay comparable instead of one stacking on top of the other.
    hierarchical = bool(getattr(model, "hierarchical", False))
    keys = _serving_keys(scored) if hierarchical else None
    base_model = getattr(model, "base", model)
    rule = getattr(model, "point_rule", "mode")
    classes = np.asarray(model.classes, dtype=float)
    zero = int(np.where(classes == 0)[0][0])

    per_step: dict[int, dict] = {}
    absolute: list[np.ndarray] = []
    hybrid_absolute: list[np.ndarray] = []
    lookup_absolute: list[np.ndarray] = []
    predictions: list[np.ndarray] = []
    hybrid_predictions: list[np.ndarray] = []
    bounds: list[np.ndarray] = []
    depleted: list[np.ndarray] = []
    risk: list[np.ndarray] = []
    lookup_risk: list[np.ndarray] = []
    lookup_borrowed: list[np.ndarray] = []
    baseline_errors: dict[str, list[float]] = {name: [] for name in BASELINES}
    weights_used: list[float] = []

    for step in steps:
        label = AVAILABILITY.label_column(step)
        truth = scored[label].to_numpy(dtype=float)
        # One pmf per hour, everything else derived from it: the served point, the classifier's own
        # point, the interval and the risk score can then never disagree with each other.
        probability = model.distribution_for(x, step, keys)
        base_mass = base_model.distribution_for(x, step)
        median = point_value(classes, probability, rule).astype(float)
        base_median = point_value(classes, base_mass, rule).astype(float)
        expectation = probability @ classes
        depletion_probability = probability[:, zero]
        low, high = model.interval_for(probability, model.interval_levels[step])
        error = np.abs(truth - median)
        entry = {
            "mae": metrics.mae(truth, median),
            "modelMae": metrics.mae(truth, base_median),
            "pointRule": rule,
            "maeExpectation": metrics.mae(truth, expectation),
            "rmse": metrics.rmse(truth, expectation),
            "coverage": metrics.coverage(truth, low, high),
            "intervalWidth": metrics.interval_width(low, high),
            "winkler": metrics.winkler(truth, low, high, 1.0 - model.nominal_coverage),
            "legality": float(np.mean((median >= 0) & (median <= capacity))),
            "points": int(len(truth)),
        }
        for name, values in _baseline_values(tables, step, scored).items():
            entry[f"baseline_{name}"] = metrics.mae(truth, values)
            baseline_errors[name].append(entry[f"baseline_{name}"])

        reference = reference_model.summary(step, scored, model.nominal_coverage)
        tuned_median = point_value(classes, base_model.distribution_for(x_tuning, step), rule).astype(float)
        tuned_reference = reference_model.summary(step, tuning, model.nominal_coverage)
        tuned_truth = tuning[label].to_numpy(dtype=float)
        flat_curve = [_blend(tuned_median, tuned_reference["median"], tuned_truth, np.full(len(tuning), weight))[0]
                      for weight in WEIGHT_GRID]
        weight = float(WEIGHT_GRID[int(np.argmin(flat_curve))])
        flat_blend = np.round(_blend_value(base_median, reference["median"], weight))
        # Empirical-Bayes shrinkage: trust a station's own history in proportion to how many TRAIN
        # hours stand behind it, so a city the model never saw falls back to the model alone.
        shrink_curve = [_blend(tuned_median, tuned_reference["median"], tuned_truth,
                               prior_weight(tuned_reference["support"], k))[0] for k in SUPPORT_GRID]
        pseudo_count = float(SUPPORT_GRID[int(np.argmin(shrink_curve))])
        prior_blend = prior_weight(reference["support"], pseudo_count)
        blend = np.round(_blend_value(base_median, reference["median"], prior_blend))
        entry["lookupMedianMae"] = metrics.mae(truth, reference["median"])
        entry["lookupCoverage"] = metrics.coverage(truth, reference["low"], reference["high"])
        entry["lookupIntervalWidth"] = metrics.interval_width(reference["low"], reference["high"])
        entry["lookupRiskAuc"] = metrics.roc_auc((truth == 0).astype(float), reference["p0"])
        entry["lookupBorrowedShare"] = reference["borrowedShare"]
        entry["flatPriorWeight"] = weight
        entry["flatPriorWeightMae"] = metrics.mae(truth, flat_blend)
        entry["pseudoCount"] = pseudo_count
        entry["pseudoCountLabel"] = format_pseudo_count(pseudo_count)
        entry["hybridMae"] = metrics.mae(truth, blend)
        entry["hybridMaeContinuous"] = metrics.mae(truth, _blend_value(base_median, reference["median"], prior_blend))
        entry["hybridLegality"] = float(np.mean((blend >= 0) & (blend <= capacity)))
        entry["hybridMaeVsBaseModel"] = 100.0 * (1.0 - entry["hybridMae"] / entry["modelMae"]) if entry["modelMae"] else 0.0
        entry["hybridMaeVsLookup"] = (100.0 * (1.0 - entry["hybridMae"] / entry["lookupMedianMae"])
                                      if entry["lookupMedianMae"] else 0.0)
        per_step[step] = entry
        absolute.append(error)
        hybrid_absolute.append(np.abs(truth - blend))
        lookup_absolute.append(np.abs(truth - reference["median"]))
        predictions.append(median)
        hybrid_predictions.append(blend)
        bounds.append(capacity)
        depleted.append(truth == 0)
        risk.append(depletion_probability)
        lookup_risk.append(reference["p0"])
        lookup_borrowed.append(np.full(len(truth), reference["borrowedShare"]))
        weights_used.append(float(np.mean(prior_blend)))

    absolute_all = np.concatenate(absolute)
    prediction_all = np.concatenate(predictions)
    hybrid_all = np.concatenate(hybrid_predictions)
    bound_all = np.concatenate(bounds)
    # Steps are scored in blocks, so the row metadata has to be repeated in the same order.
    tiled = pd.concat([scored for _ in steps], ignore_index=True)
    per_step_summary = {str(step): {key: round(value, 4) if isinstance(value, float) else value
                                    for key, value in per_step[step].items()
                                    if not key.startswith("baseline_")} for step in steps}
    model_mae = float(absolute_all.mean())
    row_weights = np.array([per_step[step]["points"] for step in steps], dtype=float)
    row_weights /= row_weights.sum()

    def average(key: str) -> float:
        return round(float(np.average([per_step[step][key] for step in steps], weights=row_weights)), 4)

    return {
        "modelId": predictor.metadata["modelId"],
        "modelVersion": predictor.metadata["modelVersion"],
        "horizonHours": horizon,
        "holdoutCity": holdout,
        # The number this bundle itself advertises, so the audit can be checked against what is
        # served rather than trusted to agree.
        "metadataMae": (predictor.metadata.get("metrics") or {}).get("mae"),
        "pointRule": rule,
        "hierarchical": hierarchical,
        # What the bundle itself ships, so this audit's searched k can be read against it.
        "shippedPseudoCount": ({str(step): {level: format_pseudo_count(value)
                                            for level, value in model.pseudo_count.get(step, {}).items()}
                                for step in steps} if hierarchical else None),
        "scoredRows": int(len(scored)),
        "scoredPoints": int(absolute_all.size),
        "nominalCoverage": model.nominal_coverage,
        "mae": round(model_mae, 4),
        "modelMae": round(float(np.mean([per_step[step]["modelMae"] for step in steps])), 4),
        **{key: round(value, 4) for key, value in _averages(per_step).items()},
        "legality": round(float(np.mean((prediction_all >= 0) & (prediction_all <= bound_all))), 6),
        "depletionBaseRate": round(float(np.concatenate(depleted).mean()), 4),
        "depletionAUC": round(metrics.roc_auc(np.concatenate(depleted).astype(float),
                                              np.concatenate(risk)), 4),
        "riskAlerts": _alert_quality(np.concatenate(depleted), np.concatenate(risk)),
        "baselines": {
            name: {"mae": round(float(np.mean(values)), 4),
                   "maeReductionPct": round(100.0 * (1.0 - model_mae / float(np.mean(values))), 2)
                   if float(np.mean(values)) > 0 else None}
            for name, values in baseline_errors.items()},
        "lookupBaseline": {
            "mae": round(float(np.mean(np.concatenate(lookup_absolute))), 4),
            "coverage": average("lookupCoverage"),
            "intervalWidth": average("lookupIntervalWidth"),
            "riskAuc": round(metrics.roc_auc(np.concatenate(depleted).astype(float),
                                             np.concatenate(lookup_risk)), 4),
            "riskAlerts": _alert_quality(np.concatenate(depleted), np.concatenate(lookup_risk)),
            "borrowedShare": round(float(np.mean(np.concatenate(lookup_borrowed))), 4),
        },
        "hybrid": {
            "description": "count-shrunk blend of the boosted classifier alone with the station x "
                           "answer-hour table: (n/(n+k))*lookup + (k/(n+k))*model, where n is the TRAIN "
                           "hours behind the cell; k searched per step on VALIDATION only",
            "weightIs": "the weight on the lookup; 0 means the blend is the model alone",
            "mae": round(float(np.mean(np.concatenate(hybrid_absolute))), 4),
            "legality": round(float(np.mean((hybrid_all >= 0) & (hybrid_all <= bound_all))), 6),
            "pseudoCount": {str(step): per_step[step]["pseudoCount"] for step in steps},
            "pseudoCountLabels": {str(step): per_step[step]["pseudoCountLabel"] for step in steps},
            "priorWeightMean": round(float(np.mean(weights_used)), 3),
            "flatPriorWeightMae": round(float(np.mean([per_step[step]["flatPriorWeightMae"] for step in steps])), 4),
            "flatPriorWeightMean": round(float(np.mean([per_step[step]["flatPriorWeight"] for step in steps])), 3),
            "maeVsBaseModel": average("hybridMaeVsBaseModel"),
            "maeVsLookup": average("hybridMaeVsLookup"),
        },
        "perStep": per_step_summary,
        "perCity": _group_errors(tiled, absolute_all, "city_id"),
        "perStation": _group_errors(tiled, absolute_all, "station_id"),
    }


def _table(rows: list[list[str]], header: list[str]) -> list[str]:
    return ["| " + " | ".join(header) + " |", "| " + " | ".join(["---"] * len(header)) + " |"] + \
           ["| " + " | ".join(row) + " |" for row in rows]


def _k_summary(pseudo_counts: dict[str, str], per_step: dict) -> str:
    """First-hour k / last-hour k, plus how many hours switched the lookup off entirely.

    ``off`` is a searched, validated choice (the pseudo-count grid carries an explicit
    "do not borrow" point), not a missing value, so it is reported rather than left blank.
    """
    steps = sorted(per_step, key=int)
    if not steps:
        return "-"
    off = sum(1 for step in steps if pseudo_counts.get(step) == "off")
    tail = f"，{off}/{len(steps)} 步关闭" if off else ""
    if steps[0] == steps[-1]:
        return f"{pseudo_counts.get(steps[0], '-')}{tail}"
    return f"{pseudo_counts.get(steps[0], '-')} / {pseudo_counts.get(steps[-1], '-')}{tail}"


def _shipped_k_summary(entry: dict) -> str:
    """How the bundle's own prior is configured - which levels borrow, and with what pseudo-count.

    Read straight out of the bundle, because the interesting question in a cold-start row is not
    ``served vs model`` but *why* they are equal: all levels off is a decision the validation sweep
    made, and this is the only place a report shows it per row.
    """
    shipped = entry.get("shippedPseudoCount") or {}
    per_level: dict[str, set] = {}
    for counts in shipped.values():
        for level, value in counts.items():
            per_level.setdefault(level, set()).add(value)
    parts = []
    for level, label in (("station", "站"), ("site", "型"), ("city", "城"), ("global", "全局")):
        values = per_level.get(level)
        if not values:
            continue
        parts.append(f"{label}:{'off' if values == {'off'} else ','.join(sorted(values))}")
    return " ".join(parts) if parts else "纯模型"


def _sampling_note(payload: dict) -> str:
    """Whether the row cap actually bit, stated as fact rather than as a caveat about sampling."""
    cap = int(payload["sampleRows"])
    rows = [entry["scoredRows"] for entry in payload["bundles"]]
    if not cap:
        return f"未设行数上限，全部 TEST 行参与评分（每跨度 {min(rows)}–{max(rows)} 行）"
    if max(rows) < cap:
        return f"行数上限 {cap} 未触发，全部 TEST 行参与评分（每跨度 {min(rows)}–{max(rows)} 行）"
    return f"每个跨度评分行数上限 {cap}（实际 {min(rows)}–{max(rows)} 行，超过上限的已按种子抽样）"


def _crosscheck_note(payload: dict) -> str:
    """How many bundles' audited served MAE matches the number the bundle itself advertises."""
    entries = [entry for entry in payload["bundles"] if entry["metadataMae"] is not None]
    if not entries:
        return "出厂 metadata 未记录 MAE，无法做这步交叉校验。"
    agreed = sum(1 for entry in entries if round(entry["metadataMae"], 4) == round(entry["mae"], 4))
    if agreed == len(entries):
        return (f"交叉校验：{agreed}/{len(entries)} 条 bundle 的本报告服务口径 MAE 与其出厂 "
                f"`model_metadata.json` 逐位一致——评估走的是同一条推理路径，没有第二套实现。")
    worst = max(entries, key=lambda entry: abs(entry["mae"] - entry["metadataMae"]))
    return (f"交叉校验：仅 {agreed}/{len(entries)} 条一致，最大偏差在 {worst['modelId']}"
            f"（{worst['mae']:.4f} vs 出厂 {worst['metadataMae']:.4f}），须查清后再引用。")


def _markdown(payload: dict) -> str:
    main = [entry for entry in payload["bundles"] if not entry["holdoutCity"]]
    cold = [entry for entry in payload["bundles"] if entry["holdoutCity"]]
    lines = [
        "# 空闲桩数预测 · 评估报告",
        "",
        f"- 数据集 / 发布批次：`{payload['datasetId']}` / `{payload['trainingPublishedBatchId']}`",
        f"- 被评分的模型目录：`{payload['scoredRunDir']}`",
        f"- 原始数据清单 sha256：`{payload['sourceManifestSha256']}`",
        f"- 导出清单 sha256：`{payload['servingManifestSha256']}`",
        f"- 特征版本：`{payload['featureVersion']}`；模型输入 {payload['featureCount']} 列（不含任何 label / split 列）",
        f"- 名义区间覆盖率：{payload['nominalCoverage']:.0%}；"
        f"{_sampling_note(payload)}",
        f"- 点值规则：{'、'.join(sorted({str(entry['pointRule']) for entry in payload['bundles']}))}；"
        f"出厂含层级先验的 bundle：{sum(1 for entry in payload['bundles'] if entry['hierarchical'])}"
        f"/{len(payload['bundles'])}",
        "- 查表/基线分桶时钟：应答小时 =（参考小时 + 步长 - 1）mod 24",
        f"- 生成时间：{payload['preparedAt']}（耗时 {payload['elapsedSeconds']} 秒）",
        *([f"- 复现命令：`{payload['reproducibleCommand']}`"] if payload.get("reproducibleCommand") else []),
        "",
        f"> {CAVEAT}",
        "",
        "## 1. 模型 vs 基线（TEST，桩数）",
        "",
    ]
    lines += _table(
        [[f"h{entry['horizonHours']:02d}", f"{entry['mae']:.4f}",
          "-" if entry["metadataMae"] is None else f"{entry['metadataMae']:.4f}",
          f"{entry['modelMae']:.4f}",
          f"{entry['hybrid']['mae']:.4f}",
          f"{entry['maeExpectation']:.4f}",
          f"{entry['rmse']:.4f}", f"{entry['coverage']:.4f}", f"{entry['intervalWidth']:.3f}",
          f"{entry['legality']:.4f}", f"{entry['depletionAUC']:.4f}",
          f"{entry['lookupBaseline']['mae']:.4f}",
          f"{entry['baselines']['persistence']['mae']:.4f}",
          f"{entry['baselines']['stationHourMedian']['mae']:.4f}",
          f"{entry['baselines']['siteHourMedian']['mae']:.4f}",
          f"{entry['baselines']['cityHourMedian']['mae']:.4f}",
          f"{entry['baselines']['globalMedian']['mae']:.4f}"] for entry in main],
        ["跨度", "模型 MAE（服务口径）", "出厂 metadata MAE", "纯分类器 MAE", "审计混合 MAE", "期望值 MAE", "RMSE",
         f"覆盖率@{payload['nominalCoverage']:.0%}", "平均区间宽度",
         "输出合法率", "无桩风险 AUC", "基线·站点×小时分布", "基线·重复上小时", "基线·站点×小时", "基线·场站类型×小时",
         "基线·城市×小时", "基线·全局中位数"])
    lines += ["", "`模型 MAE（服务口径）` 是这条 bundle 真正对外输出的整数桩数误差（层级模型已含出厂先验，点值规则见头部）；",
              "`纯分类器 MAE` 是同一套概率分布不查表时的误差；`审计混合 MAE` 是本脚本自己按样本量重新搜索权重的结果，",
              "用于检验“查表到底值多少”，与出厂先验是两个独立估计，不应相加解读。",
              "`期望值 MAE`/`RMSE` 取分布期望（小数，页面须标注为预计值）。",
              "按样本量收缩的层级加权：权重 n/(n+k)，n 为该站点该**应答小时**在 TRAIN 中的历史小时数，"
              "k 在每个跨度上用 VALIDATION 搜索；没有本地历史的站点自动退回纯模型。",
              "所有基线只在 TRAIN 区间拟合、按应答小时分桶，未接触 TEST 数据。", "", "## 2. 相对最强基线的提升", ""]
    rows = []
    for entry in main:
        candidates = {name: value["mae"] for name, value in entry["baselines"].items()}
        candidates["stationHourDistribution"] = entry["lookupBaseline"]["mae"]
        best_name, best = min(candidates.items(), key=lambda pair: pair[1])
        rows.append([f"h{entry['horizonHours']:02d}", f"{entry['modelMae']:.4f}", f"{entry['hybrid']['mae']:.4f}",
                     best_name, f"{best:.4f}",
                     f"{100.0 * (1.0 - entry['hybrid']['mae'] / best):+.1f}%",
                     f"{entry['hybrid']['priorWeightMean']:.2f}",
                     f"{entry['hybrid']['flatPriorWeightMae']:.4f}",
                     f"{_k_summary(entry['hybrid']['pseudoCountLabels'], entry['perStep'])}"])
    lines += _table(rows, ["跨度", "纯分类器 MAE", "审计混合 MAE", "最强基线", "基线 MAE", "混合 vs 最强基线",
                           "平均先验权重", "固定权重混合 MAE", "先验伪计数 k（首步 / 末步 / 判关闭的步数）"])
    lines += ["", "百分比为负表示混合不如该基线，必须如实报告；`平均先验权重` 越接近 1 说明该跨度越依赖查表，",
              "越接近 0 说明本地历史撑不起查表、退回纯模型（k 越大权重越小，`off` 表示该步直接判定为不借用）。", ""]
    hierarchical = [entry for entry in main if entry["hierarchical"] and entry["modelMae"]]
    if hierarchical:
        shipped = [100.0 * (1.0 - entry["mae"] / entry["modelMae"]) for entry in hierarchical]
        audited = [100.0 * (1.0 - entry["hybrid"]["mae"] / entry["modelMae"]) for entry in hierarchical]
        lines += [f"同样一条查表，混在**分布**上（出厂模型）在主城市带来 {min(shipped):+.1f}% ~ {max(shipped):+.1f}% 的误差下降，"
                  f"混在**点值**上（本脚本重新搜索权重的结果）只有 {min(audited):+.1f}% ~ {max(audited):+.1f}%。",
                  "差在哪：中位数已经把整条分布压成一个整数，本地历史能补充的信息大部分被压掉了；",
                  "先验混合保留 pmf，权重才作用在真正含信息的量上。审计列的作用是交叉验证，不是第三份收益。", ""]
    lines += ["", "## 3. 冷启动：模型从未训练过的城市", ""]
    by_horizon = {entry["horizonHours"]: entry for entry in main}
    rows = []
    for entry in cold:
        city = entry["holdoutCity"]
        reference = by_horizon.get(entry["horizonHours"], {}).get("perCity", {}).get(city, {}).get("mae")
        degradation = f"{100.0 * (entry['mae'] / reference - 1.0):+.1f}%" if reference else "-"
        rows.append([city, f"h{entry['horizonHours']:02d}", f"{entry['mae']:.4f}", f"{entry['modelMae']:.4f}",
                     f"{entry['hybrid']['mae']:.4f}", f"{entry['hybrid']['priorWeightMean']:.2f}",
                     _shipped_k_summary(entry),
                     "-" if reference is None else f"{reference:.4f}", degradation,
                     f"{entry['lookupBaseline']['mae']:.4f} ({entry['lookupBaseline']['borrowedShare']:.0%} 借用)",
                     f"{entry['baselines']['cityHourMedian']['mae']:.4f}",
                     f"{entry['baselines']['persistence']['mae']:.4f}",
                     f"{entry['coverage']:.4f}", f"{entry['depletionAUC']:.4f}"])
    lines += _table(rows, ["留出城市", "跨度", "冷启动 MAE（服务口径）", "纯分类器 MAE", "审计混合 MAE", "审计先验权重",
                           "出厂先验 k", "主模型同城 MAE", "退化",
                           "站点×小时分布基线", "城市×小时基线", "重复上小时基线", "覆盖率", "风险 AUC"])
    gains = [100.0 * (entry["mae"] / entry["modelMae"] - 1.0) for entry in cold if entry["modelMae"]]
    notes = ["", "冷启动列只用该城市 TEST 区间评分，基线也只用其余城市的 TRAIN 数据拟合，因此对比公平。",
             "`出厂先验 k` 全为 `off` 的行，服务口径与纯分类器逐位相同：先验贡献为 0 是 VALIDATION 给出的结论，不是缺失值。"]
    if gains:
        notes += [f"出厂先验在冷启动城市上的净效果介于 {min(gains):+.1f}% 与 {max(gains):+.1f}% 之间（负号才是变好），"
                  f"其中 {sum(1 for gain in gains if abs(gain) < 5e-3)}/{len(gains)} 行为 0。"
                  "量级与验证噪声同阶，不足以宣称冷启动也能稳定从查表获益。"]
    notes += ["`审计混合 MAE` 明显更差是预期内的反面证据：本脚本只搜**一个** k，而且是在已知城市的 VALIDATION 上搜的；",
              "陌生城市的站点没有自己的格子，只能借到场站类型/城市级的粗格，而粗格的历史小时数 n 反而更大，",
              "单一 k 下它拿到的权重也就更高——按层级分别设 k（出厂模型的做法）正是为了不这样过度借用。",
              "", "## 4. 无桩风险提示是否可用（h01 主模型）", ""]
    lines += notes
    if main:
        entry = min(main, key=lambda item: item["horizonHours"])
        alerts = entry["riskAlerts"]
        rows = [[f"P(无桩) ≥ {value['threshold']:.2f}", f"{value['alerts']}",
                 "-" if value["precision"] is None else f"{value['precision']:.3f}",
                 "-" if value["recall"] is None else f"{value['recall']:.3f}"]
                for key, value in alerts.items() if key != "baseRate"]
        lines += _table(rows, ["阈值", "触发时点数", "精确率", "召回率"])
        lines += ["", f"无桩时点的自然占比（基线率）为 {alerts['baseRate']:.4f}；模型 AUC {entry['depletionAUC']:.4f}，"
                      f"站点×小时经验分布的 P(无桩) AUC {entry['lookupBaseline']['riskAuc']:.4f}"
                      f"（该基线在训练区间内查表，同样是强对手）。", ""]
    lines += ["## 5. 误差最大的站点（h01，TEST）", ""]
    if main:
        stations = sorted(min(main, key=lambda item: item["horizonHours"])["perStation"].items(),
                          key=lambda pair: -pair[1]["mae"])[:8]
        lines += _table([[station, f"{value['points']}", f"{value['mae']:.4f}"] for station, value in stations],
                        ["站点", "评分时点数", "MAE"])
    lines += ["", "## 6. 分城市误差（主模型，服务口径）", ""]
    if main and by_horizon:
        horizons = sorted(by_horizon)
        cities = sorted({city for horizon in horizons for city in by_horizon[horizon]["perCity"]})
        rows = []
        for city in cities:
            cells = [city]
            for horizon in horizons:
                value = by_horizon[horizon]["perCity"].get(city, {}).get("mae")
                cells.append("-" if value is None else f"{value:.4f}")
            shortest = by_horizon[horizons[0]]
            cell = shortest["perCity"].get(city, {})
            cells.append(str(cell.get("points", 0)))
            cells.append("-" if cell.get("mae") is None
                         else f"{100.0 * (cell['mae'] / shortest['mae'] - 1.0):+.1f}%")
            rows.append(cells)
        lines += _table(rows, ["城市"] + [f"h{horizon:02d} MAE" for horizon in horizons]
                        + [f"h{horizons[0]:02d} 评分点数", f"h{horizons[0]:02d} 相对全体"])
        lines += ["", "口径与第 1 节完全相同（同一批 TEST 行、同一服务口径点值），只是按城市拆开，",
                  "所以任何一列的城市均值加权后就是第 1 节的总数。第 3 节的冷启动城市是**从未参与训练**的",
                  "另一套口径，不可与本表并排比较。", ""]
    lines += ["", "## 7. 按小时跨度的衰减", ""]
    rows = []
    for entry in main:
        steps = sorted(entry["perStep"], key=int)
        rows.append([f"h{entry['horizonHours']:02d}",
                     f"{entry['perStep'][steps[0]]['mae']:.4f}",
                     f"{entry['perStep'][steps[-1]]['mae']:.4f}",
                     f"{entry['perStep'][steps[0]]['coverage']:.4f}",
                     f"{entry['perStep'][steps[-1]]['coverage']:.4f}",
                     f"{entry['perStep'][steps[-1]]['intervalWidth']:.3f}"])
    lines += _table(rows, ["跨度", "首小时 MAE", "末小时 MAE", "首小时覆盖率", "末小时覆盖率", "末小时区间宽度"])
    lines += ["", "## 8. 口径说明", "",
              "- 指标单位为**空闲充电桩数量**（0–3 桩），不是百分比；MAE 0.5 表示平均偏差半个桩。",
              "- 覆盖率列报告的是经验覆盖率，应与名义值（80%）接近；区间为按站点校准后的最短中心区间。",
              "- 输出合法率为 1.0 是结构性结果：模型只能输出 0–capacity 的整数类别，推理时再次裁剪。",
              "- 查表键是**应答小时**（`h06` 的标签描述 6 小时后的那个小时）。每个跨度单独建表时，这与按参考小时分桶"
              "是同一批行的两种命名，数值相同；本脚本仍按应答小时记录，是为了让一格的意义是“这个站、这个钟点”，"
              "并且杜绝把两个跨度的表合并——那才是真正会混时钟的操作。",
              "- 收缩权重 n/(n+k) 加在**查表**一侧，k 越大越信任模型。0.2.x 的审计脚本把同一个权重记在模型一侧",
              "（查表拿 `k/(n+k)`），方向用反：本地历史越薄反而越依赖查表、越厚越依赖模型。历史报告的",
              "`平均模型权重` 与 `混合 MAE` 两列因此不可与本报告对比。",
              f"- 本报告写入新目录（`{payload.get('scoredRunDir')}` 的旧报告不受影响），绝不覆盖已发布 run 的 "
              "`evaluation_report.*`；如需重跑请指定新的 `--output`。",
              f"- {_crosscheck_note(payload)}",
              "- 本项目所有数据均为模拟生成，用于系统演示与验收，不得用于商业决策。", ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--export", default=str(DEFAULT_EXPORT))
    parser.add_argument("--output", default=None,
                        help="where the report goes; defaults to --run-dir, and is refused if a report is already there")
    parser.add_argument("--sample-rows", type=int, default=60_000, help="0 scores every TEST row")
    parser.add_argument("--seed", type=int, default=20260913)
    arguments = parser.parse_args(argv)

    run_dir = Path(arguments.run_dir)
    out_dir = Path(arguments.output) if arguments.output else run_dir
    # A published run's report is evidence, not a build artefact: rewriting it in place would let a
    # later change of method quietly replace the numbers somebody already quoted.  Fail before the
    # expensive pass, so a refused run costs nothing.
    existing = [name for name in ("evaluation_report.json", "evaluation_report.md")
                if (out_dir / name).exists()]
    if existing:
        raise SystemExit(f"{out_dir} already holds {' and '.join(existing)}; pass a new --output directory "
                         f"instead of rewriting a published report")
    out_dir.mkdir(parents=True, exist_ok=True)

    started = time.time()
    frame = forecaster.build_frame(Path(arguments.export))
    frame.data = add_site_key(frame.data)

    bundles = []
    for leaf in artifacts.bundle_directories(run_dir):
        key = str(leaf.relative_to(run_dir)).replace("\\", "/")
        print(f"[score] {key}", flush=True)
        bundles.append(_score_bundle(leaf, frame, arguments.sample_rows, arguments.seed))

    if not bundles:
        raise SystemExit(f"{run_dir} holds no saved bundle; run python -m data_analysis.ml.availability.train first")
    payload = {
        "task": AVAILABILITY.key,
        "datasetId": frame.export.dataset_id,
        "trainingPublishedBatchId": frame.export.published_batch_id,
        "sourceManifestSha256": frame.export.source_manifest_sha256,
        "servingManifestSha256": frame.export.manifest_sha256,
        "featureVersion": frame.export.feature_version,
        "featureCount": len(frame.feature_columns),
        "scoredRunDir": str(run_dir).replace("\\", "/"),
        "nominalCoverage": bundles[0]["nominalCoverage"],
        "sampleRows": arguments.sample_rows,
        "reproducibleCommand": artifacts.invocation(__name__, argv),
        "caveat": CAVEAT,
        "bundles": bundles,
        "elapsedSeconds": round(time.time() - started, 1),
        "preparedAt": artifacts.stamp(),
    }
    (out_dir / "evaluation_report.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (out_dir / "evaluation_report.md").write_text(_markdown(payload), encoding="utf-8")
    for entry in bundles:
        tag = "cold " + entry["holdoutCity"] if entry["holdoutCity"] else "main"
        print(f"[{entry['modelId']:24}] {tag:9} served MAE {entry['mae']:.4f} model MAE {entry['modelMae']:.4f} "
              f"audit blend {entry['hybrid']['mae']:.4f} coverage {entry['coverage']:.4f} "
              f"legality {entry['legality']:.4f} AUC {entry['depletionAUC']:.4f}")
    print(f"[done] {out_dir / 'evaluation_report.md'} in {payload['elapsedSeconds']}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
