"""拟合两个模型：白排风险（二分类）与等待时长（回归）。

纪律：
  · 只在 split == TRAIN 上拟合，VALIDATION 用来选运营阈值与报告基线对比，TEST 一次都不看
    （evaluate.py 才碰它，且只碰一次）；
  · 早停用 sklearn 内置的 TRAIN 内部随机切分——它只决定模型容量，不看时间线，
    不外泄 VALIDATION/TEST；
  · 先落三档基线（全局率 / 排位经验率 / 站点×小时×排位 三级平滑经验率），模型必须打过它才有意义；
  · 回归只在 SERVED 行上拟合与评价：ABANDONED 的等待是删失数据，拿 0 或截断值冒充会把 MAE 做好看。

产物：outputs/ml_queue/gbdt-queue-abandon-v1.joblib（两个模型 + 特征设计 + 基线表 + 阈值）
      outputs/ml_queue/train_metrics.json

用法（仓库根目录）：python -m data_analysis.ml.queue.train
"""

from __future__ import annotations

import io
import json

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingClassifier, HistGradientBoostingRegressor
from sklearn.metrics import log_loss, roc_auc_score

from . import common

#: 平滑强度：小样本桶向先验收缩，避免"某站某小时排过 1 次就弃单"被当成规律。
BASELINE_ALPHA = 30.0
NEUTRAL_PRIOR = 0.5


def load_matrix() -> tuple[pd.DataFrame, dict]:
    if not common.MATRIX_PATH.exists():
        raise FileNotFoundError(f"缺少特征矩阵 {common.MATRIX_PATH}，请先跑 build_data")
    frame = pd.read_pickle(common.MATRIX_PATH)
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    common.verify_batch()
    if summary.get("publishedBatchId") != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch(
            f"矩阵构建时绑定的批次 {summary.get('publishedBatchId')!r} 与当前 "
            f"{common.EXPECTED_PUBLISHED_BATCH_ID!r} 不一致，请重跑 build_data")
    if frame["queue_id"].duplicated().any():
        raise AssertionError("矩阵里 queue_id 重复，一行一次排队的前提不成立")
    return frame, summary


def design(frame: pd.DataFrame, numeric: list[str], categorical: list[str]
           ) -> tuple[pd.DataFrame, np.ndarray]:
    """特征矩阵：数值列转 float（NaN 交给 HGBT 原生处理），类别列转 category，bool 转 int。"""
    columns = numeric + categorical
    missing = [c for c in columns if c not in frame.columns]
    if missing:
        raise KeyError(f"特征列在矩阵里不存在: {missing}")
    matrix = frame[columns].copy()
    for name in numeric:
        series = matrix[name]
        if str(series.dtype) == "boolean":
            matrix[name] = series.astype("int8")
        matrix[name] = pd.to_numeric(series, errors="coerce").astype("float64")
    for name in categorical:
        matrix[name] = (matrix[name].astype("object").where(matrix[name].notna(), "MISSING")
                        .astype("category"))
    mask = np.array([name in categorical for name in columns], dtype=bool)
    return matrix, mask


def _smoothed_rate(keys: pd.DataFrame, y: pd.Series, alpha: float) -> pd.DataFrame:
    grouped = pd.DataFrame({"y": y.to_numpy()}, index=pd.MultiIndex.from_frame(keys)).groupby(level=list(range(keys.shape[1])))
    stats = grouped["y"].agg(["sum", "size"]).reset_index()
    stats["rate"] = (stats["sum"] + alpha * NEUTRAL_PRIOR) / (stats["size"] + alpha)
    return stats


def fit_baselines(train: pd.DataFrame) -> dict[str, pd.DataFrame]:
    """三档基线，全部只在 TRAIN 上统计。查不到的键在预测时回落到全局率。"""
    y = train["y_waste"]
    return {
        "global": pd.DataFrame({"rate": [float(y.mean())]}),
        "position": _smoothed_rate(train[["position_at_join"]], y, BASELINE_ALPHA),
        "station_hour": _smoothed_rate(train[["station_id", "hour_local"]], y, BASELINE_ALPHA),
        "station_hour_position": _smoothed_rate(
            train[["station_id", "hour_local", "position_at_join"]], y, BASELINE_ALPHA),
    }


BASELINE_SPECS = (
    ("position", ("position_at_join",), 0.35),
    ("station_hour", ("station_id", "hour_local"), 0.30),
    ("station_hour_position", ("station_id", "hour_local", "position_at_join"), 0.35),
)


def predict_baseline(baselines: dict[str, pd.DataFrame], frame: pd.DataFrame) -> np.ndarray:
    """最强基线：三级平滑经验率，按对数几率加权平均（谁有历史就多用谁），全缺则回落全局率。

    用 merge(validate="many_to_one") 而不是索引 join：右表键若真重复，它会直接报错，
    而不是把行数悄悄放大后在 reindex 上炸成"duplicate labels"。
    """
    global_rate = float(baselines["global"]["rate"].iloc[0])
    odds_sum = np.zeros(len(frame))
    weight_sum = np.zeros(len(frame))
    for name, keys, weight in BASELINE_SPECS:
        table = baselines[name][list(keys) + ["rate"]].copy()
        helper = pd.DataFrame({"_pos": np.arange(len(frame))})
        for key in keys:
            helper[key] = frame[key].to_numpy()
        merged = helper.merge(table, on=list(keys), how="left", validate="many_to_one")
        if not (merged["_pos"].to_numpy() == np.arange(len(frame))).all():
            raise AssertionError(f"基线 {name} 关联改变了行序")
        values = merged.sort_values("_pos")["rate"].to_numpy(dtype=float)
        known = ~np.isnan(values)
        clipped = np.clip(np.where(known, values, global_rate), 1e-4, 1 - 1e-4)
        odds_sum += weight * np.log(clipped / (1.0 - clipped))
        weight_sum += weight * known.astype(float)
    log_odds = np.where(weight_sum > 0, odds_sum / np.maximum(weight_sum, 1e-9),
                        np.log(global_rate / (1.0 - global_rate)))
    return 1.0 / (1.0 + np.exp(-log_odds))


def fit_wait_baselines(train_served: pd.DataFrame) -> dict[str, object]:
    keys = ["station_id", "hour_local"]
    grouped = train_served.groupby(keys)["wait_min"].median()
    return {"stationHourMedian": grouped.rename("median").reset_index(),
            "globalMedian": float(train_served["wait_min"].median()),
            "positionMedian": train_served.groupby("position_at_join")["wait_min"].median()}


def predict_wait(baselines: dict[str, object], frame: pd.DataFrame) -> np.ndarray:
    """等待时长基线：站点×小时中位数（TRAIN）→ 排位中位数 → 全局中位数。"""
    table = baselines["stationHourMedian"][["station_id", "hour_local", "median"]].copy()
    helper = pd.DataFrame({"_pos": np.arange(len(frame)), "station_id": frame["station_id"].to_numpy(),
                           "hour_local": frame["hour_local"].to_numpy()})
    merged = helper.merge(table, on=["station_id", "hour_local"], how="left", validate="many_to_one")
    if not (merged["_pos"].to_numpy() == np.arange(len(frame))).all():
        raise AssertionError("等待基线关联改变了行序")
    out = merged.sort_values("_pos")["median"]
    fallback = frame["position_at_join"].map(baselines["positionMedian"]).astype(float)
    out = out.reset_index(drop=True).fillna(fallback.reset_index(drop=True))
    out = out.fillna(float(baselines["globalMedian"]))
    return np.clip(out.to_numpy(dtype=float), 0.0, None)


def main() -> dict:
    common.require_empty_run_dir(extra_allowed=(common.MATRIX_PATH.name,
                                                common.BUILD_SUMMARY_PATH.name))
    frame, summary = load_matrix()
    numeric = summary["features"]["numeric"]
    categorical = summary["features"]["categorical"]

    train = frame[frame["split"] == "TRAIN"].reset_index(drop=True)
    validation = frame[frame["split"] == "VALIDATION"].reset_index(drop=True)
    held_out = int((frame["split"] == "TEST").sum())
    print(f"[train] TRAIN={len(train):,} VALIDATION={len(validation):,} "
          f"TEST(本次不碰)={held_out:,}")

    x_train, cat_mask = design(train, numeric, categorical)
    x_valid, _ = design(validation, numeric, categorical)
    y_train = train["y_waste"].to_numpy(dtype=int)
    y_valid = validation["y_waste"].to_numpy(dtype=int)

    classifier = HistGradientBoostingClassifier(
        max_iter=400, learning_rate=0.06, max_leaf_nodes=31, min_samples_leaf=40,
        l2_regularization=1.0, early_stopping=True, validation_fraction=0.15,
        n_iter_no_change=30, random_state=common.SEED, categorical_features=cat_mask)
    classifier.fit(x_train, y_train)

    baselines = fit_baselines(train)
    baseline_valid = predict_baseline(baselines, validation)
    prob_valid = classifier.predict_proba(x_valid)[:, 1]
    metrics = {
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "matrixSha256": summary["matrixSha256"],
        "trainRows": int(len(train)), "validationRows": int(len(validation)),
        "testRowsUntouched": held_out,
        "featuresUsed": int(x_train.shape[1]), "numericFeatures": len(numeric),
        "categoricalFeatures": len(categorical), "iterations": int(classifier.n_iter_),
        "validation": {
            "aucModel": float(roc_auc_score(y_valid, prob_valid)),
            "aucBaselineStrong": float(roc_auc_score(y_valid, baseline_valid)),
            "aucBaselinePositionOnly": float(roc_auc_score(
                y_valid, validation["position_at_join"].map(
                    train.groupby("position_at_join")["y_waste"].mean()).to_numpy())),
            "logLossModel": float(log_loss(y_valid, np.clip(prob_valid, 1e-6, 1 - 1e-6))),
            "baseRate": float(y_valid.mean()),
        },
    }

    #: 运营阈值在 VALIDATION 上按「精确率 ≥ 0.50」取召回最大的那档，之后 TEST 只验一次。
    grid = np.quantile(prob_valid, np.linspace(0.50, 0.99, 50))
    chosen = None
    for threshold in sorted(set(np.round(grid, 6))):
        flagged = prob_valid >= threshold
        if flagged.sum() < 50:
            continue
        precision = float(y_valid[flagged].mean())
        recall = float(flagged[y_valid == 1].sum() / max(1, int((y_valid == 1).sum())))
        if precision >= 0.50 and (chosen is None or recall > chosen["recall"]):
            chosen = {"threshold": float(threshold), "precision": precision, "recall": recall,
                      "alertRate": float(flagged.mean())}
    if chosen is None:
        raise RuntimeError("VALIDATION 上找不到精确率≥0.50 的阈值，需要放宽运营口径而不是硬凑")
    metrics["operatingPoint"] = chosen
    metrics["validation"]["aucBaselineGlobalRate"] = 0.5

    served_train = train[train["y_waste"] == 0].reset_index(drop=True)
    x_served, _ = design(served_train, numeric, categorical)
    regressor = HistGradientBoostingRegressor(
        loss="absolute_error", max_iter=400, learning_rate=0.06, max_leaf_nodes=31,
        min_samples_leaf=40, l2_regularization=1.0, early_stopping=True, validation_fraction=0.15,
        n_iter_no_change=30, random_state=common.SEED, categorical_features=cat_mask)
    regressor.fit(x_served, served_train["wait_min"].to_numpy(dtype=float))
    wait_baselines = fit_wait_baselines(served_train)
    served_valid = validation[validation["y_waste"] == 0].reset_index(drop=True)
    x_served_valid, _ = design(served_valid, numeric, categorical)
    model_mae = float(np.mean(np.abs(regressor.predict(x_served_valid)
                                     - served_valid["wait_min"].to_numpy(dtype=float))))
    baseline_mae = float(np.mean(np.abs(predict_wait(wait_baselines, served_valid)
                                        - served_valid["wait_min"].to_numpy(dtype=float))))
    metrics["waitModel"] = {
        "fitRows": int(len(served_train)),
        "loss": "absolute_error（直接优化 MAE）",
        "iterations": int(regressor.n_iter_),
        "validationMaeMinutesModel": round(model_mae, 4),
        "validationMaeMinutesBaseline": round(baseline_mae, 4),
        "maeImprovementPct": round(100.0 * (1.0 - model_mae / baseline_mae), 2),
        "note": "SERVED 子集，VALIDATION 窗口；ABANDONED 的等待不可观测（删失），不参与 MAE。",
    }

    bundle = {
        "classifier": classifier, "regressor": regressor,
        "featureNames": numeric + categorical, "numericFeatures": numeric,
        "categoricalFeatures": categorical, "categoricalMask": cat_mask,
        "baselines": baselines, "waitBaselines": wait_baselines,
        "operatingPoint": chosen, "modelId": common.MODEL_ID, "waitModelId": common.WAIT_MODEL_ID,
        "modelVersion": common.MODEL_VERSION, "seed": common.SEED,
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "datasetId": common.DATASET_ID, "matrixSha256": summary["matrixSha256"],
        "splits": summary["window"], "simulatedNote": common.simulated_note(),
        "labelUnits": summary["labelUnits"],
    }
    buffer = io.BytesIO()
    joblib.dump(bundle, buffer)
    common.write_new_bytes(common.BUNDLE_PATH, buffer.getvalue())
    metrics["bundlePath"] = str(common.BUNDLE_PATH.relative_to(common.DATA_ANALYSIS_ROOT.parent))
    metrics["bundleSha256"] = common.sha256_file(common.BUNDLE_PATH)
    metrics["disclaimer"] = common.simulated_note()
    common.write_new_json(common.TRAIN_METRICS_PATH, metrics)

    validation_block = metrics["validation"]
    print(f"[train] VAL AUC 模型={validation_block['aucModel']:.4f} "
          f"最强基线={validation_block['aucBaselineStrong']:.4f} "
          f"排位基线={validation_block['aucBaselinePositionOnly']:.4f}")
    print(f"[train] 运营阈值={chosen['threshold']:.4f} 精确率={chosen['precision']:.4f} "
          f"召回={chosen['recall']:.4f} 告警率={chosen['alertRate']:.4f}")
    print(f"[train] VAL 等待 MAE 模型={metrics['waitModel']['validationMaeMinutesModel']:.3f}min "
          f"基线={metrics['waitModel']['validationMaeMinutesBaseline']:.3f}min")
    print(f"[train] -> {common.BUNDLE_PATH}")
    return metrics


if __name__ == "__main__":
    main()
