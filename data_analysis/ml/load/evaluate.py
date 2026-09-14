"""Hold-out evaluation on TEST: model vs persistence vs same-hour-last-week.

Each contract horizon is scored under its own split column (split_1h/6h/24h),
so the numbers match exactly what the serving batch guarantees. Metrics are
results on synthetic simulation data only (模拟数据测试结果).

Non-contract horizons (h02-h05, h07-h23) fall back to ``split_24h==TEST``
because only the 24h serving batch guarantees their labels; the actual column
used is recorded per entry as ``splitColumn``. This script is a deliberate
post-selection holdout report: nothing here feeds back into model selection,
which happened only on VALIDATION in train.py.

ENRICHMENT NOTE (v0.2): this run only ADDS derived distributional metrics
(wape, smape, p90abs, p95abs) to each horizon entry of the already-frozen
v0.2 grading. It is NOT a new grading: same shipped bundle, same rows, same
predictions, same models (all deterministic), so every pre-existing field
(mae, rmse, n, coverage, rangeLegalRate, splitColumn) reproduces byte-for-byte.
The pre-enrichment file is preserved as test_metrics_pre_enrichment_v0.2.json.

Usage (repo root, after training):
    python -m data_analysis.ml.load.evaluate
"""

from __future__ import annotations

import json

import numpy as np
import pandas as pd

from . import common
from .train import BUNDLE_PATH, last_week_predictions

import joblib

OUT_DIR = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
KEY_HORIZONS = (1, 6, 24)


SMAPE_EPS = 1e-6


def regression_metrics(y_true, y_pred):
    error = y_pred - y_true
    abs_err = np.abs(error)
    denom = np.abs(y_true) + np.abs(y_pred) + SMAPE_EPS
    total_abs_y = float(np.abs(y_true).sum())
    return {
        "mae": round(float(abs_err.mean()), 3),
        "rmse": round(float(np.sqrt((error**2).mean())), 3),
        "n": int(len(y_true)),
        # distributional metrics (v0.2 enrichment): relative mass of error,
        # symmetric percent-error, and tail of the absolute error distribution
        "wape": round(float(abs_err.sum() / total_abs_y), 4) if total_abs_y > 0 else None,
        "smape": round(float((2.0 * abs_err / denom).mean()), 4),
        "p90abs": round(float(np.quantile(abs_err, 0.90)), 3),
        "p95abs": round(float(np.quantile(abs_err, 0.95)), 3),
    }


def main() -> int:
    bundle = joblib.load(BUNDLE_PATH)
    models = bundle["models"]
    frame = pd.read_pickle(OUT_DIR / "joined_usable.pkl")
    hourly = common.load_hourly_metrics()
    power_lookup = hourly.set_index(["station_id", "recorded_at"])["mean_power_kw"]

    matrix = common.features_matrix(frame)
    report: dict = {"note": "模拟数据测试结果 (synthetic data)", "horizons": {}}

    for horizon in range(1, 25):
        label_col = f"label_power_kw_h{horizon:02d}"
        split_col = common.HORIZON_SPLITS.get(horizon) or common.HORIZON_SPLITS[24]
        mask = (frame[split_col] == "TEST") & frame[label_col].notna()
        index = frame.index[mask]
        y = frame.loc[mask, label_col].to_numpy(dtype=float)
        y_pred = np.clip(
            models[horizon].predict(matrix.loc[mask]).astype(float),
            0.0,
            frame.loc[mask, "rated_capacity_kw"].to_numpy(dtype=float),
        )
        persistence = frame.loc[mask, "lag_power_kw_h01"].to_numpy(dtype=float)
        week = last_week_predictions(frame.loc[mask], power_lookup)
        found = np.isfinite(week)

        entry = {
            "splitColumn": split_col,
            "gbdt": regression_metrics(y, y_pred),
            "persistence": regression_metrics(y, persistence),
            "same_hour_last_week": {
                **regression_metrics(y[found], week[found]),
                "coverage": round(float(found.mean()), 3),
            },
            "rangeLegalRate": round(
                float(((y_pred >= 0) & (y_pred <= frame.loc[mask, "rated_capacity_kw"])).mean()), 4
            ),
        }
        report["horizons"][f"h{horizon:02d}"] = entry
        if horizon in KEY_HORIZONS:
            report.setdefault("contractHorizons", {})[f"h{horizon:02d}"] = entry

    # per-city breakdown for the three contract horizons
    per_city: dict = {}
    for horizon in KEY_HORIZONS:
        label_col = f"label_power_kw_h{horizon:02d}"
        split_col = common.HORIZON_SPLITS[horizon]
        mask = (frame[split_col] == "TEST") & frame[label_col].notna()
        index = frame.index[mask]
        y = frame.loc[mask, label_col].to_numpy(dtype=float)
        y_pred = np.clip(
            models[horizon].predict(matrix.loc[mask]).astype(float),
            0.0,
            frame.loc[mask, "rated_capacity_kw"].to_numpy(dtype=float),
        )
        grouped = pd.DataFrame({"city": frame.loc[mask, "city_id"], "y": y, "p": y_pred})
        per_city[f"h{horizon:02d}"] = {
            city: regression_metrics(part["y"].to_numpy(), part["p"].to_numpy())
            for city, part in grouped.groupby("city")
        }
    report["perCityContractHorizons"] = per_city

    # one output file per graded model AND training batch, so every frozen
    # grading (first-blind, confirmatory, post-data-refresh rebaselines)
    # stays on disk side by side without overwrites
    report["modelId"] = str(bundle["model_id"])
    batch = str(bundle["metadata"].get("trainingPublishedBatchId", "unknownbatch"))
    report["trainingPublishedBatchId"] = batch
    suffix = batch.removeprefix("analytics-")[:8]
    metrics_path = OUT_DIR / f"test_metrics_{bundle['model_id']}_{suffix}.json"
    with open(metrics_path, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)

    print(f"{'horizon':8}{'GBDT MAE':>10}{'RMSE':>9}{'persist':>9}{'week(7d)':>10}{'n':>8}")
    for horizon in range(1, 25):
        e = report["horizons"][f"h{horizon:02d}"]
        marker = " *" if horizon in KEY_HORIZONS else ""
        print(
            f"h{horizon:02d}{marker:<5}{e['gbdt']['mae']:>9}{e['gbdt']['rmse']:>9}"
            f"{e['persistence']['mae']:>9}{e['same_hour_last_week']['mae']:>10}{e['gbdt']['n']:>8}"
        )
    print(json.dumps(report["perCityContractHorizons"], ensure_ascii=False, indent=2))
    print("saved ->", metrics_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
