"""拟合站×tick 下一 tick 总充电负荷回归（kW），在 VALIDATION 选容量档与基线口径，TEST 一次不碰。

三个可部署基线（读"要不要建模"这句话的量）：
  1. ``globalMean``    TRAIN 常数——尺子；
  2. ``persistence``   当 tick 实测负荷（``load_lag1``）——"下一 tick 和现在一样"的朴素物理；
  3. ``climatology``   站×小时（北京时间 hod）的 TRAIN 平均负荷——周期性查表，工程下限。
模型 ``HistGradientBoostingRegressor`` 在数值滞后特征上拟合，容量两档（big31/mid15）按 **VALIDATION
RMSE** 择一发布；TEST 完全不参与任何一步。附带"余量"读数：预测的站·tick 峰值 vs 360kW 变压器。

产物：outputs/ml_transformer/gbdt-station-tick-load-v1.joblib + train_metrics.json（O_EXCL）。
用法（仓库根目录）：python -m data_analysis.ml.transformer.train
"""

from __future__ import annotations

import io
import json

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingRegressor

from . import common
from .features import numeric_feature_columns

#: 负荷量级远大于零，RMSE 对尖峰更敏感（分配关心的正是峰值），故以 RMSE 选容量档、MAE 同报。
HYPER_GRID = (
    {"name": "big31", "max_leaf_nodes": 31, "learning_rate": 0.06, "max_iter": 400,
     "min_samples_leaf": 40},
    {"name": "mid15", "max_leaf_nodes": 15, "learning_rate": 0.05, "max_iter": 300,
     "min_samples_leaf": 60},
)


def load_matrix() -> tuple[pd.DataFrame, dict]:
    """读 tick 帧并复查批次与特征表哈希；本线无 derived（零新增数据），故无 derived 复查。"""
    if not common.TICK_FEATURES_PATH.exists():
        raise FileNotFoundError(f"缺少 tick 帧 {common.TICK_FEATURES_PATH}，请先跑 features")
    frame = pd.read_pickle(common.TICK_FEATURES_PATH)
    with open(common.TICK_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    common.verify_batch()
    if summary.get("publishedBatchId") != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch(
            f"tick 帧绑定批次 {summary.get('publishedBatchId')!r} 与当前 "
            f"{common.EXPECTED_PUBLISHED_BATCH_ID!r} 不一致，请重跑 features")
    if summary.get("featuresSha256") != common.sha256_file(common.TICK_FEATURES_PATH):
        raise common.BatchMismatch("tick 帧哈希与 features_summary 不一致，表被改过，请重跑 features")
    assert not frame.duplicated(subset=["station_id", "tick_ts"]).any(), "站·tick 出现重复行"
    assert set(frame.loc[~frame["dropped_last"], "split"]) >= {"TRAIN", "VALIDATION", "TEST"}
    return frame, summary


def usable(frame: pd.DataFrame, split: str) -> pd.DataFrame:
    return frame[(frame["split"] == split) & ~frame["dropped_last"]].reset_index(drop=True)


def design(frame: pd.DataFrame, columns: list[str]) -> pd.DataFrame:
    missing = [c for c in columns if c not in frame.columns]
    if missing:
        raise KeyError(f"特征列不存在: {missing}")
    matrix = frame[columns].copy()
    for name in columns:
        matrix[name] = pd.to_numeric(matrix[name], errors="coerce").astype("float64")
    return matrix


def new_regressor(hyper: dict | None) -> HistGradientBoostingRegressor:
    params = dict(max_iter=400, learning_rate=0.06, max_leaf_nodes=31, min_samples_leaf=40)
    params.update({k: v for k, v in (hyper or {}).items() if k != "name"})
    return HistGradientBoostingRegressor(
        **params, l2_regularization=1.0, early_stopping=True, validation_fraction=0.15,
        n_iter_no_change=30, random_state=common.SEED)


def _climatology(train: pd.DataFrame) -> pd.DataFrame:
    """站×小时（北京 hod）的下一 tick 平均负荷；缺失逐级回落（站×hod → 站 → 全局）。"""
    work = train.copy()
    beijing = work["tick_ts"] + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
    work["hod"] = beijing.dt.hour
    global_mean = float(work["total_kw_next"].mean())
    by_station = work.groupby("station_id")["total_kw_next"].mean()
    table = (work.groupby(["station_id", "hod"])["total_kw_next"].mean().rename("mean").reset_index())
    return {"global": global_mean, "by_station": by_station, "table": table}


def predict_climatology(clim: dict, frame: pd.DataFrame) -> np.ndarray:
    beijing = frame["tick_ts"] + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
    helper = pd.DataFrame({"station_id": frame["station_id"].to_numpy(), "hod": beijing.dt.hour.to_numpy()})
    merged = helper.merge(clim["table"], on=["station_id", "hod"], how="left", validate="many_to_one")
    values = merged["mean"].to_numpy(dtype=float)
    station_fill = frame["station_id"].map(clim["by_station"]).to_numpy(dtype=float)
    return np.where(np.isnan(values), np.where(np.isnan(station_fill), clim["global"], station_fill), values)


def _metrics(y: np.ndarray, pred: np.ndarray) -> dict:
    return {"mae": round(float(common.metrics.mae(y, pred)), 4),
            "rmse": round(float(common.metrics.rmse(y, pred)), 4)}


def main() -> dict:
    common.require_empty_run_dir(
        common.OUT_DIR,
        extra_allowed=(common.TICK_FEATURES_PATH.name, common.TICK_SUMMARY_PATH.name,
                       common.DEMAND_LONG_PATH.name))
    frame, summary = load_matrix()
    columns = numeric_feature_columns()
    train = usable(frame, "TRAIN")
    validation = usable(frame, "VALIDATION")
    held_out = int(usable(frame, "TEST").shape[0])
    y_train = train["total_kw_next"].to_numpy(dtype=float)
    y_valid = validation["total_kw_next"].to_numpy(dtype=float)
    print(f"[train] TRAIN={len(train):,} VALIDATION={len(validation):,} TEST(本次不碰)={held_out:,}")

    clim = _climatology(train)
    global_mean = clim["global"]
    baselines = {
        "globalMean": np.full(len(validation), global_mean),
        "persistence": validation["load_lag1"].to_numpy(dtype=float),
        "climatology": predict_climatology(clim, validation),
    }
    baseline_valid = {name: _metrics(y_valid, pred) for name, pred in baselines.items()}
    for name, block in baseline_valid.items():
        print(f"[train] 基线 {name}: VAL MAE={block['mae']:.3f} RMSE={block['rmse']:.3f}")

    x_train, x_valid = design(train, columns), design(validation, columns)
    grid: dict[str, dict] = {}
    best = None
    for hyper in HYPER_GRID:
        model = new_regressor(hyper)
        model.fit(x_train, y_train)
        pred = model.predict(x_valid)
        block = _metrics(y_valid, pred)
        block.update({"hyper": hyper["name"], "iterations": int(model.n_iter_), "featureCount": len(columns)})
        grid[hyper["name"]] = block
        print(f"[train] 模型 {hyper['name']}: VAL MAE={block['mae']:.3f} RMSE={block['rmse']:.3f}")
        if best is None or block["rmse"] < best[1]["rmse"]:
            best = (model, block, hyper)
    chosen_model, chosen_block, chosen_hyper = best

    # 余量读数：验证段"预测下一 tick 峰值"离 360kW 还有多远（本批从不越限，这句是量化而非例外）。
    pred_valid = chosen_model.predict(x_valid)
    over_cap = int((pred_valid > common.TRANSFORMER_KW).sum())
    margin_p50 = float(np.median(common.TRANSFORMER_KW - pred_valid))
    margin_p99 = float(np.quantile(common.TRANSFORMER_KW - pred_valid, 0.01))  # 最小余量的 1% 分位

    persistence_rmse = baseline_valid["persistence"]["rmse"]
    metrics = {
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "datasetId": common.DATASET_ID, "modelId": common.MODEL_ID,
        "modelVersion": common.MODEL_VERSION, "seed": common.SEED,
        "featuresSha256": summary["featuresSha256"], "label": summary["label"],
        "sampleUnit": summary["sampleUnit"], "decisionTime": summary["decisionTime"],
        "trainRows": int(len(train)), "validationRows": int(len(validation)),
        "testRowsUntouched": held_out, "featureColumns": columns,
        "selectionRule": "两档容量（big31/mid15）按 VALIDATION RMSE 择一；TEST 不参与选择",
        "chosenHyper": chosen_hyper["name"], "hyperGrid": grid, "baselineValidation": baseline_valid,
        "validation": {**chosen_block,
                       "rmseGainVsPersistencePct": round(100.0 * (1.0 - chosen_block["rmse"] / persistence_rmse), 2)},
        "headroom": {"transformerKw": common.TRANSFORMER_KW, "predictedOverCapValidationTicks": over_cap,
                     "marginP50Kw": round(margin_p50, 2), "marginP99WorstKw": round(margin_p99, 2)},
        "disclaimer": common.data_note(),
    }

    bundle = {
        "regressor": chosen_model, "featureNames": columns,
        "climatology": clim, "chosenHyper": chosen_hyper["name"],
        "modelId": common.MODEL_ID, "modelVersion": common.MODEL_VERSION, "seed": common.SEED,
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "datasetId": common.DATASET_ID, "featuresSha256": summary["featuresSha256"],
        "label": summary["label"], "dataNote": common.data_note(),
    }
    buffer = io.BytesIO()
    joblib.dump(bundle, buffer)
    common.write_new_bytes(common.BUNDLE_PATH, buffer.getvalue())
    metrics["bundlePath"] = str(common.BUNDLE_PATH.relative_to(common.DATA_ANALYSIS_ROOT.parent))
    metrics["bundleSha256"] = common.sha256_file(common.BUNDLE_PATH)
    common.write_new_json(common.TRAIN_METRICS_PATH, metrics)

    print(f"[train] 选中 {chosen_hyper['name']}: VAL RMSE={chosen_block['rmse']:.3f} "
          f"较 persistence 改善 {metrics['validation']['rmseGainVsPersistencePct']}%")
    print(f"[train] 余量：预测超 360kW 的 tick={over_cap} · 中位余量 {margin_p50:.1f}kW · "
          f"最差 1% 分位余量 {margin_p99:.1f}kW")
    print(f"[train] -> {common.BUNDLE_PATH}")
    return metrics


if __name__ == "__main__":
    main()
