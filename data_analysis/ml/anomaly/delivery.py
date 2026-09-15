"""Completed-session anomaly screening with fixed TRAIN-only references.

Unlike the retired v3/v4 scripts, adding an unrelated test session does not
change any existing session's score. Missing charger contexts fall back to a
TRAIN charger median, then a TRAIN global median, never to 'safe by default'.
"""
from __future__ import annotations

import numpy as np
import pandas as pd
from sklearn.ensemble import IsolationForest

from . import common

MODEL_ID = "fixed-reference-session-anomaly-v5"
TRAIN_END = pd.Timestamp("2026-04-30T16:00:00")
VALID_END = pd.Timestamp("2026-05-14T16:00:00")
TEST_END = pd.Timestamp("2026-05-29T16:00:00")
CONTEXT_FEATURES = ["contextTempExcess", "contextCellExcess", "contextCurrentDeficit"]
FEATURES = common.FEATURES + CONTEXT_FEATURES


def split_sessions(sessions):
    """Whole session must finish inside its split; crossing sessions are purged."""
    start = pd.to_datetime(sessions.started_at)
    end = pd.to_datetime(sessions.ended_at)
    result = pd.Series("PURGED", index=sessions.index, dtype=object)
    result[(start < TRAIN_END) & (end < TRAIN_END)] = "TRAIN"
    result[(start >= TRAIN_END) & (start < VALID_END) & (end < VALID_END)] = "VALIDATION"
    result[(start >= VALID_END) & (start < TEST_END) & (end < TEST_END)] = "TEST"
    return result


def prepare_points(samples, sessions):
    cols = ["session_id", "started_at", "ended_at"]
    known = sessions[cols].copy()
    known["started_at"] = pd.to_datetime(known.started_at)
    known["ended_at"] = pd.to_datetime(known.ended_at)
    pts = samples.copy()
    pts["recorded_at"] = pd.to_datetime(pts.recorded_at)
    pts = pts.merge(known, on="session_id", how="inner", validate="many_to_one")
    pts = pts[(pts.recorded_at >= pts.started_at) & (pts.recorded_at <= pts.ended_at)].copy()
    pts["cellDifference"] = pts.max_cell_voltage_v - pts.min_cell_voltage_v
    pts["currentBin"] = (pts.charge_current_a.clip(lower=0) // 50).astype(int)
    pts["socBin"] = (pts.soc_pct.clip(0, 99.99) // 10).astype(int)
    return pts


def fit_context(points, train_ids):
    train = points[points.session_id.isin(set(train_ids))]
    if train.empty:
        raise ValueError("No TRAIN battery samples")
    spec = {"temp": ("currentBin", "max_temperature_c"),
            "cell": (None, "cellDifference"), "current": ("socBin", "charge_current_a")}
    out = {}
    for key, (bin_column, column) in spec.items():
        grouping = ["charger_id"] + ([bin_column] if bin_column else [])
        out[key] = {"bin": bin_column, "column": column,
                    "byGroup": train.groupby(grouping)[column].median(),
                    "byCharger": train.groupby("charger_id")[column].median(),
                    "global": float(train[column].median())}
    return out


def context_features(points, context):
    values = {}
    fallback = pd.Series(False, index=points.index)
    for key, cfg in context.items():
        if cfg["bin"]:
            idx = pd.MultiIndex.from_arrays([points.charger_id, points[cfg["bin"]]])
            expectation = pd.Series(cfg["byGroup"].reindex(idx).to_numpy(), index=points.index)
        else:
            expectation = points.charger_id.map(cfg["byGroup"])
        fallback |= expectation.isna()
        expectation = expectation.fillna(points.charger_id.map(cfg["byCharger"])).fillna(cfg["global"])
        values[key] = points[cfg["column"]] - expectation
    raw = pd.DataFrame({"session_id": points.session_id,
                        "contextTempExcess": values["temp"], "contextCellExcess": values["cell"],
                        "contextCurrentDeficit": -values["current"], "contextFallback": fallback.astype(int)})
    return raw.groupby("session_id").max().reset_index()


def build_features(points, sessions, context):
    # Existing common extraction is retained for the audited feature vocabulary.
    # Correct time derivatives/energy units explicitly for this new model ID.
    samples = points.drop(columns=["started_at", "ended_at"])
    table = common.session_features(samples, sessions)
    table = table.drop(columns="split").merge(
        sessions[["session_id", "station_id", "charger_id"]], on="session_id", validate="one_to_one")
    table = table.merge(context_features(points, context), on="session_id", validate="one_to_one")
    table["ended_at"] = pd.to_datetime(table.ended_at)
    table["dur_min"] = (table.ended_at - table.started_at).dt.total_seconds() / 60
    if (table.dur_min <= 0).any():
        raise ValueError("Nonpositive completed charging duration")
    table["avg_power_kw"] = table.energy_wh / 1000 / (table.dur_min / 60)
    table["energy_per_min"] = table.energy_wh / table.dur_min
    sorted_points = points.sort_values(["session_id", "recorded_at"])
    groups = sorted_points.groupby("session_id")
    dt = groups.recorded_at.diff().dt.total_seconds() / 60
    for output, column in (("dsoc_per_min", "soc_pct"), ("dvolt_per_min", "pack_voltage_v"),
                           ("temp_rise_per_min", "max_temperature_c")):
        rate = groups[column].diff() / dt.where(dt > 0)
        table[output] = table.session_id.map(rate.groupby(sorted_points.session_id).mean())
    table["split"] = split_sessions(table)
    return table


def empirical_cdf(reference, values):
    """Reference must be saved TRAIN observations, not the incoming batch."""
    ref = np.asarray(reference, dtype=float)
    vals = np.asarray(values, dtype=float)
    if not len(ref) or not np.isfinite(ref).all() or not np.isfinite(vals).all():
        raise ValueError("Cannot score a nonfinite anomaly signal")
    return np.searchsorted(ref, vals, side="right") / len(ref)


def signals(bundle, table):
    x = table[FEATURES].astype(float).fillna(bundle["medians"])
    output = {"iforest": -bundle["model"].score_samples(x)}
    for name, column in (("thermal", "contextTempExcess"), ("cell", "contextCellExcess"),
                         ("current", "contextCurrentDeficit")):
        output[name] = x[column].to_numpy()
    return output


def score(bundle, table):
    raw = signals(bundle, table)
    transformed = [empirical_cdf(bundle["references"][name], raw[name]) for name in bundle["selectedSignals"]]
    return np.mean(transformed, axis=0)


def fit(table, labels, context):
    train = table[table.split == "TRAIN"]
    valid = table[table.split == "VALIDATION"]
    test = table[table.split == "TEST"]
    if any(part.empty for part in (train, valid, test)):
        raise ValueError("Anomaly needs nonempty chronological splits")
    y_ids = set(labels.session_id)
    y_valid = valid.session_id.isin(y_ids).to_numpy(dtype=int)
    # Fit without anomaly_labels. Labels only select a validation threshold and
    # signal combination, then score the held-out TEST exactly once.
    medians = train[FEATURES].astype(float).median().fillna(0)
    model = IsolationForest(n_estimators=200, random_state=42, n_jobs=2)
    model.fit(train[FEATURES].astype(float).fillna(medians))
    bundle = {"model": model, "medians": medians, "context": context, "modelId": MODEL_ID}
    train_raw = signals(bundle, train)
    valid_raw = signals(bundle, valid)
    bundle["references"] = {name: np.sort(values) for name, values in train_raw.items()}
    train_ranks = {name: empirical_cdf(bundle["references"][name], values) for name, values in train_raw.items()}
    valid_ranks = {name: empirical_cdf(bundle["references"][name], values) for name, values in valid_raw.items()}
    candidates = [("iforest",), ("thermal",), ("cell",), ("current",),
                  ("thermal", "iforest"), ("thermal", "cell"), ("thermal", "cell", "current", "iforest")]
    best = None
    for subset in candidates:
        tr_score = np.mean([train_ranks[n] for n in subset], axis=0)
        va_score = np.mean([valid_ranks[n] for n in subset], axis=0)
        for pct in (90, 94, 96, 97, 98, 99):
            threshold = float(np.quantile(tr_score, pct / 100))
            metric = common.prf(va_score, y_valid, threshold)
            key = (metric["f1"], metric["precision"], -len(subset))
            if best is None or key > best[0]:
                best = (key, subset, threshold, pct, metric)
    _, selected, threshold, pct, validation = best
    bundle.update(selectedSignals=list(selected), threshold=threshold)
    # Simple raw TRAIN p99 temperature/cell-spread rule on identical TEST rows.
    baseline_limits = {col: float(train[col].quantile(.99)) for col in ("temp_spread_max", "celldiff_max")}
    y_test = test.session_id.isin(y_ids).to_numpy(dtype=int)
    test_score = score(bundle, test)
    rule_flags = np.logical_or.reduce([test[col].to_numpy() > value for col, value in baseline_limits.items()])
    types = labels.groupby("session_id").anomaly_type.agg(set)
    type_report = {}
    flags = test_score > threshold
    for kind in sorted(labels.anomaly_type.unique()):
        mask = test.session_id.map(lambda sid: kind in types.get(sid, set())).to_numpy(dtype=bool)
        n = int(mask.sum())
        type_report[str(kind)] = {"n": n, "recall": round(float(flags[mask].mean()), 4) if n else None}
    metrics = {"validation": validation, "test": common.prf(test_score, y_test, threshold),
               "testRuleBaseline": common.prf(rule_flags.astype(float), y_test, .5),
               "testRecallByType": type_report,
               "protocol": {"trainEndExclusive": "2026-04-30T16:00:00Z", "validationEndExclusive": "2026-05-14T16:00:00Z",
                            "testEndExclusive": "2026-05-29T16:00:00Z", "businessTimezone": "Asia/Shanghai",
                            "trainSessions": len(train), "validationSessions": len(valid), "testSessions": len(test),
                            "purgedSessions": int((table.split == "PURGED").sum()),
                            "selectedSignals": list(selected), "threshold": threshold, "trainPercentile": pct,
                            "scoreSemantics": "Fixed TRAIN empirical-reference anomaly score, NOT fault probability",
                            "task": "Completed-session retrospective screening, NOT real-time device safety protection",
                            "fallbackPolicy": "Unknown charger/bin uses TRAIN charger/global reference, never future batch statistics",
                            "testContextFallbackSessions": int(test.contextFallback.sum())}}
    return bundle, metrics


def describe(bundle, row):
    value = float(score(bundle, row.to_frame().T)[0])
    features = {"durationMinutes": round(float(row.dur_min), 2), "maxTemperatureC": round(float(row.temp_max), 2),
                "temperatureAboveContextC": round(float(row.contextTempExcess), 2),
                "maxCellDifferenceV": round(float(row.celldiff_max), 4), "averagePowerKw": round(float(row.avg_power_kw), 2)}
    return {"sessionId": str(row.session_id), "stationId": str(row.station_id), "chargerId": str(row.charger_id),
            "startedAt": row.started_at.strftime("%Y-%m-%dT%H:%M:%SZ"), "endedAt": row.ended_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "anomalyScore": round(value, 6), "threshold": bundle["threshold"], "flagged": bool(value > bundle["threshold"]),
            "modelId": MODEL_ID, "dataKind": "SIMULATED", "evaluationSplit": str(row.split),
            "scoreSemantics": "FIXED_TRAIN_REFERENCE_NOT_FAULT_PROBABILITY", "features": features,
            "contextFallback": bool(row.contextFallback),
            "explanations": [f"最高温度 {features['maxTemperatureC']:g} ℃，比历史充电器工况基线高 {features['temperatureAboveContextC']:g} ℃",
                             f"最大电芯压差 {features['maxCellDifferenceV']:g} V；已结束会话时长 {features['durationMinutes']:g} 分钟"],
            "explanationSemantics": "Observed feature facts; flagged sessions require human review, not a fault diagnosis"}
