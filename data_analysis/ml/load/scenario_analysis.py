"""Scenario breakdown of the shipped hgb-deep bundle (descriptive reporting only).

Post-hoc slicing of the ALREADY-FROZEN scoring for the three contract horizons
(h01/h06/h24, each under its own split column split_1h/split_6h/split_24h).
Nothing here feeds model selection: predictions use the identical pipeline as
evaluate.py (bundle model predict, then clip to [0, rated_capacity_kw]) and the
TEST numbers are cross-checked against the frozen ``test_metrics.json``.

Slice dimensions per horizon and split (TEST and VALIDATION):
  weekend           is_weekend == True / False
  holiday           is_public_holiday == 1 vs 0
  workday           not weekend and not holiday vs the rest
  target_hour_bucket  Beijing-time hour of the *predicted* hour
                    (reference_dt + horizon + 8h): late_night 0-6,
                    morning_peak 7-9, daytime 10-16, evening_peak 17-21,
                    night 22-23
  capacity_tertile  quantile(1/3)/quantile(2/3) cutoffs on rated_capacity_kw.
                    NOTE: the fleet has only two distinct ratings (74 / 187 kW)
                    so both cutoffs land on 187 and the "mid" bin is empty;
                    small == 74 kW stations, large == 187 kW stations.
  city_id           one cell per city

Metrics per cell: MAE, RMSE, P95abs (95th percentile of |error|), n.
Cells with n < MIN_CELL_N are still reported but flagged ``low_n``.

Usage (repo root):
    python -m data_analysis.ml.load.scenario_analysis
"""

from __future__ import annotations

import json
import os

# Must precede pandas/sklearn imports in this Anaconda/Windows environment.
os.environ.setdefault("KMP_DUPLICATE_LIB_OK", "TRUE")

import joblib
import numpy as np
import pandas as pd

from . import common
from .train import BUNDLE_PATH

OUT_DIR = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load" / "analysis"
KEY_HORIZONS = (1, 6, 24)
SPLITS = ("TEST", "VALIDATION")
MIN_CELL_N = 30


def target_hour_bucket(horizon: int, reference_dt: pd.Series) -> pd.Series:
    """Beijing-time hour bucket of the predicted hour (reference + horizon)."""
    hour = (reference_dt + pd.Timedelta(hours=horizon) + common.BUSINESS_TZ_OFFSET).dt.hour
    bins = pd.cut(
        hour,
        bins=[-1, 6, 9, 16, 21, 23.999],
        labels=["late_night_0_6", "morning_peak_7_9", "daytime_10_16",
                "evening_peak_17_21", "night_22_23"],
    )
    return bins.astype(str)


def capacity_tertile(frame: pd.DataFrame) -> pd.Series:
    cutoffs = frame["rated_capacity_kw"].quantile([1 / 3, 2 / 3])
    lo, hi = float(cutoffs.iloc[0]), float(cutoffs.iloc[1])
    cap = frame["rated_capacity_kw"]
    return pd.Series(
        np.where(cap < lo, "small", np.where(cap < hi, "mid", "large")),
        index=frame.index,
    )


def cell_metrics(y: np.ndarray, pred: np.ndarray) -> dict:
    abs_err = np.abs(pred - y)
    return {
        "mae": round(float(abs_err.mean()), 3),
        "rmse": round(float(np.sqrt((abs_err ** 2).mean())), 3),
        "p95abs": round(float(np.percentile(abs_err, 95)), 3),
        "n": int(len(y)),
        "low_n": bool(len(y) < MIN_CELL_N),
    }


def dimension_tables(sub: pd.DataFrame, y: np.ndarray, pred: np.ndarray, horizon: int) -> dict:
    is_weekend = sub["is_weekend"].astype(bool)
    is_holiday = sub["is_public_holiday"].astype(bool)
    dims = {
        "overall": pd.Series("all", index=sub.index, dtype=object),
        "weekend": np.where(is_weekend, "weekend", "non_weekend"),
        "holiday": np.where(is_holiday, "holiday", "non_holiday"),
        "workday": np.where(
            (~is_weekend) & (~is_holiday), "workday", "non_workday"
        ),
        "target_hour_bucket": target_hour_bucket(horizon, sub["reference_dt"]),
        "capacity_tertile": capacity_tertile(sub),
        "city_id": sub["city_id"].astype(str),
    }
    table: dict = {}
    for name, labels in dims.items():
        labels = pd.Series(labels, index=sub.index).astype(str)
        grouped = pd.DataFrame({"cell": labels, "y": y, "p": pred}).groupby("cell")
        table[name] = {
            cell: cell_metrics(part["y"].to_numpy(), part["p"].to_numpy())
            for cell, part in grouped
        }
    return table


def digest_of(slices: dict) -> dict:
    """Compact worst-cell facts derived from the tables (no new modeling)."""
    digest: dict = {}
    for split in SPLITS:
        per_horizon: dict = {}
        for hz in KEY_HORIZONS:
            hs = f"h{hz:02d}"
            tables = slices[split][hs]

            def by(dim: str) -> dict:
                return {c: m["mae"] for c, m in tables[dim].items() if not m["low_n"]}

            buckets = by("target_hour_bucket")
            peak = {k: v for k, v in buckets.items() if "peak" in k}
            offpeak = {k: v for k, v in buckets.items() if "peak" not in k}
            per_horizon[hs] = {
                "overall_mae": tables["overall"]["all"]["mae"],
                "worst_hour_bucket": max(buckets, key=buckets.get) if buckets else None,
                "best_hour_bucket": min(buckets, key=buckets.get) if buckets else None,
                "peak_buckets_mae": {k: round(v, 3) for k, v in peak.items()},
                "offpeak_buckets_mae": {k: round(v, 3) for k, v in offpeak.items()},
                "holiday_mae": round(by("holiday").get("holiday", float("nan")), 3)
                if "holiday" in by("holiday") else None,
                "non_holiday_mae": round(by("holiday").get("non_holiday", float("nan")), 3),
                "workday_mae": round(by("workday").get("workday", float("nan")), 3),
                "non_workday_mae": round(by("workday").get("non_workday", float("nan")), 3),
                "weekend_mae": round(by("weekend").get("weekend", float("nan")), 3),
                "worst_city": max(by("city_id"), key=by("city_id").get),
                "worst_city_mae": round(max(by("city_id").values()), 3),
                "best_city": min(by("city_id"), key=by("city_id").get),
                "best_city_mae": round(min(by("city_id").values()), 3),
                "capacity_tertile_mae": {k: round(v, 3) for k, v in by("capacity_tertile").items()},
            }
        digest[split] = {
            "meanContractMae": round(
                float(np.mean([per_horizon[f"h{h:02d}"]["overall_mae"] for h in KEY_HORIZONS])), 4
            ),
            "horizons": per_horizon,
        }
    return digest


def main() -> int:
    bundle = joblib.load(BUNDLE_PATH)
    models = bundle["models"]
    frame = pd.read_pickle(common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load" / "joined_usable.pkl")
    matrix = common.features_matrix(frame)

    slices: dict = {split: {} for split in SPLITS}
    for horizon in KEY_HORIZONS:
        label_col = f"label_power_kw_h{horizon:02d}"
        split_col = common.HORIZON_SPLITS[horizon]
        scored = frame[frame[label_col].notna()].copy()
        scored["__pred"] = np.clip(
            models[horizon].predict(matrix.loc[scored.index]).astype(float),
            0.0,
            scored["rated_capacity_kw"].to_numpy(dtype=float),
        )
        for split in SPLITS:
            sub = scored[scored[split_col] == split]
            slices[split][f"h{horizon:02d}"] = dimension_tables(
                sub,
                sub[label_col].to_numpy(dtype=float),
                sub["__pred"].to_numpy(dtype=float),
                horizon,
            )

    report = {
        "note": "模拟数据测试结果 — descriptive scenario slices of the frozen "
                "hgb-deep scoring; not a model-selection input.",
        "modelId": bundle.get("model_id", common.MODEL_ID),
        "modelVersion": bundle.get("model_version", common.MODEL_VERSION),
        "predictionPipeline": "bundle model predict, clipped to [0, rated_capacity_kw] "
                              "(identical to evaluate.py)",
        "minCellN": MIN_CELL_N,
        "capacityTertileNote": "fleet rated_capacity_kw takes only two distinct values "
                               "(74, 187); both quantile cutoffs land on 187, so the "
                               "'mid' bin is empty by construction.",
        "slices": slices,
        "digest": digest_of(slices),
    }

    # consistency check against the frozen TEST grading (post-hoc only)
    frozen_path = OUT_DIR.parent / "test_metrics.json"
    if frozen_path.exists():
        frozen = json.loads(frozen_path.read_text(encoding="utf-8"))
        report["frozenTestConsistency"] = {
            f"h{h:02d}": {
                "frozen_mae": frozen["contractHorizons"][f"h{h:02d}"]["gbdt"]["mae"],
                "recomputed_mae": slices["TEST"][f"h{h:02d}"]["overall"]["all"]["mae"],
                "frozen_n": frozen["contractHorizons"][f"h{h:02d}"]["gbdt"]["n"],
                "recomputed_n": slices["TEST"][f"h{h:02d}"]["overall"]["all"]["n"],
            }
            for h in KEY_HORIZONS
        }

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out = OUT_DIR / "scenario_analysis.json"
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("saved ->", out)
    print(json.dumps(report["digest"], ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
