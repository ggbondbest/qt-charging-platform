"""TEST 留出评测:GBDT vs 持续性基线 vs 上周同时刻;契约时距各按自己的 split 列(split_1h/6h/24h)计分。
非契约时距(h02-h05/h07-h23)回退 split_24h==TEST(只有 24h 批次承诺其标签),实际列记在 splitColumn。
数字只做报告,不回填选型。
v0.2 增补:只向已冻结评分 ADD 分布指标(wape/smape/p90abs/p95abs),原有字段须逐字节复现,
复现不上=上游变了;增补前文件保留为 test_metrics_pre_enrichment_v0.2.json。
用法(仓库根目录,先完成训练):python -m data_analysis.ml.load.evaluate
"""

from __future__ import annotations

import json

import numpy as np
import pandas as pd

from . import common
from .train import BUNDLE_PATH

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
        # WAPE=总绝对误差/总真实值量;sMAPE 分母加 SMAPE_EPS 防 0/0,但近零样本上单点误差被推到
        # 上限、少量近零行即抬高整列均值,须配 MAE 读;P90/P95=绝对误差尾部。
        "wape": round(float(abs_err.sum() / total_abs_y), 4) if total_abs_y > 0 else None,
        "smape": round(float((2.0 * abs_err / denom).mean()), 4),
        "p90abs": round(float(np.quantile(abs_err, 0.90)), 3),
        "p95abs": round(float(np.quantile(abs_err, 0.95)), 3),
    }


def main() -> int:
    bundle = joblib.load(BUNDLE_PATH)
    models = bundle["models"]
    frame = pd.read_pickle(common.require_prepared(common.read_manifest()))
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
        week = common.last_week_predictions(frame.loc[mask], power_lookup, horizon)
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

    # 契约时距分城市(per-city)拆解:整体 MAE 达标不代表各城市达标
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

    # TEST 每模型只批一次;文件名带 model_id+训练批次做隔离,重跑/换批不覆盖历史冻结评分
    report["modelId"] = str(bundle["model_id"])
    batch = str(bundle["metadata"].get("trainingPublishedBatchId", "unknownbatch"))
    report["trainingPublishedBatchId"] = batch
    suffix = batch.removeprefix("analytics-")[:8]
    metrics_path = OUT_DIR / f"test_metrics_{bundle['model_id']}_{suffix}.json"
    # 已存在=这个模型/批次已批过 TEST,直接拒绝(评审 P2#4);想重评请换批次或显式改名归档。
    common.write_new_json(metrics_path, report)

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
