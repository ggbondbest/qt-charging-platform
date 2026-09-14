"""冻结评分的场景切片(post-hoc 描述性报告,不参与模型决策);契约时距各按自己的 split 列计分。
切片维度:weekend、holiday、workday、target_hour_bucket(按目标小时的北京时刻度 reference_dt+(h-1)h+8h:
0-6/7-9/10-16/17-21/22-23)、capacity_tertile(全网额定功率仅 74/187 kW 两值,两个 quantile cutoff 均落在 187,
mid 桶恒空,属数据性质;small=74、large=187)、city_id。格子指标 MAE/RMSE/P95abs/n,n<MIN_CELL_N 打 low_n 标记。
预测管线与 evaluate.py 相同;TEST 数字与冻结 test_metrics.json 交叉核对,对不上=切片代码或数据漂移。
用法(仓库根目录):python -m data_analysis.ml.load.scenario_analysis
"""

from __future__ import annotations

import json
import os

# 须先于 pandas/sklearn 导入:Anaconda/Windows 下两套 OpenMP 运行时同时加载会崩进程
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
    """被预测小时 = reference + (horizon-1)h(契约第 1 个预测点是 reference 起的整点区间);
    用 +horizon 会把目标整体归晚一小时,h01 有 1/5 的行走错桶(评审 P2#5)。"""
    hour = (reference_dt + pd.Timedelta(hours=horizon - 1) + common.BUSINESS_TZ_OFFSET).dt.hour
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
    """从切片表提炼 worst/best 格子摘要,纯归纳,无新建模或新预测。"""
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

    # 一致性核对:同批行同管线,MAE/n 应与冻结 TEST 评分完全相等;不等=切片代码漂移
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
