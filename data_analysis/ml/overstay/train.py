"""拟合会话级占桩超时模型：分类（会不会超时 ≥30 分钟）+ 回归（大概占多久），并在 VALIDATION 定死提醒阈值。

纪律：
  · 只在 ``split == TRAIN`` 上拟合；VALIDATION 选特征组与阈值；TEST 一次都不看（evaluate.py 才碰）；
  · 早停用 sklearn 内置的 TRAIN 内部随机切分，只决定模型容量，不看时间线；
  · **七档基线**先立起来（本线的基线与第七线相反——第七线是"基线赢了模型"的负面样板，本线
    侦察时模型就实打实超过廉价基线，所以基线的作用是**量出廉价口径值多少**，而不是劝退模型）：
      1. 全局基础率（常数，AUC 恒 0.5，只是尺子）；
      2. TRAIN 三级平滑查表（站 / 站型×时段块 / 站型×车型，对数几率加权）——"不建模、纯查表"
         的工程下限，也是运营侧最容易自己提出来的方案；
      3. 纯因果用户先验 ``user_over_rate_prior``（as-of，不学东西，只看该用户拔枪可见的历史超时率）；
      4. 纯因果站点先验 ``station_over_rate_prior``——"这个站的风气"值多少 AUC；
      5. **只用计划时长** ``planned_hours``（目标电量 Wh/1000 除以桩额定功率 kW，决策时点可算的保守估计）——
         它和 6. 一起构成"起充前到底能知道多少结束后的事"的对照；
      6. **oracle：真实充电时长** ``duration_min``——标注不可部署（要等 ended_at），用来读出
         "排序收益里有多少来自『知道这单充多久』"；
      7. 朴素规则：该用户**上一次已拔枪**的会话超时 ⇒ 本场继续提醒（``user_last_over``）——
         把"惯犯直觉"交给数据定量，而不是想当然。
  · 四组候选特征集（只画像/时钟/天气、只跨轴历史、只排队运营、全部）按 VAL AUC 择一发布。
  · 阈值口径是**运营预算**：精确率 ≥ 2×VALIDATION 基础率、且当日提醒不超过当日开始会话数 25%
    的档里取召回最大；没有档满足就退回"最高精确率档"并如实标注，不改口径去凑达标。

产物：outputs/ml_overstay/gbdt-session-overstay-v1.joblib + train_metrics.json（O_EXCL）。
用法（仓库根目录）：python -m data_analysis.ml.overstay.train
"""

from __future__ import annotations

import io
import json

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingClassifier, HistGradientBoostingRegressor
from sklearn.metrics import average_precision_score, log_loss, roc_auc_score

from . import common

#: 查表基线的平滑强度与中性先验（与第七线同口径，便于跨线比较"基线值多少"这句话）。
BASELINE_ALPHA = 30.0
NEUTRAL_PRIOR = 0.5
BASELINE_SPECS = (("station", ("station_id",), 0.30),
                  ("site_hour", ("site_type", "hour_block"), 0.35),
                  ("site_class", ("site_type", "vehicle_class"), 0.35))

#: 运营口径：精确率相对基础率的倍数、当日提醒上限（占当日开始会话数比例）、最少提醒条数。
PRECISION_MULTIPLE = 2.0
MAX_ALERT_SHARE = 0.25
MIN_FLAGGED = 30


def load_matrix() -> tuple[pd.DataFrame, dict]:
    """读特征表并复查两件事：批次、特征表哈希。任一不符即拒用。

    本线没有 derived 数据集（不落新数据），所以第七线的 derived 复查在这里不存在——
    这正是"本线不新增任何数据"在代码层的形状。
    """
    if not common.FEATURES_PATH.exists():
        raise FileNotFoundError(f"缺少特征表 {common.FEATURES_PATH}，请先跑 features")
    frame = pd.read_pickle(common.FEATURES_PATH)
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    common.verify_batch()
    if summary.get("publishedBatchId") != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch(
            f"特征表构建时绑定的批次 {summary.get('publishedBatchId')!r} 与当前 "
            f"{common.EXPECTED_PUBLISHED_BATCH_ID!r} 不一致，请重跑 features")
    if summary.get("featuresSha256") != common.sha256_file(common.FEATURES_PATH):
        raise common.BatchMismatch("特征表哈希与 features_summary 记录不一致，表被改过，请重跑 features")
    assert frame["session_id"].is_unique, "特征表里出现重复 session_id，一行一场会话的前提不成立"
    assert (frame["over_min"] >= 0).all() and (frame["duration_min"] > 0).all()
    return frame, summary


def design(frame: pd.DataFrame, numeric: list[str], categorical: list[str]
           ) -> tuple[pd.DataFrame, np.ndarray]:
    """特征矩阵：数值列转 float（NaN 交给 HGBT 原生处理），类别列转 category，bool 转 int。"""
    columns = numeric + categorical
    missing = [c for c in columns if c not in frame.columns]
    if missing:
        raise KeyError(f"特征列在表里不存在: {missing}")
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


def split_group(columns: list[str], numeric: list[str], categorical: list[str]
                ) -> tuple[list[str], list[str]]:
    """候选集（一个混合列名清单）→ (数值, 类别) 两列，保持全表口径不跑偏。"""
    unknown = set(columns) - set(numeric) - set(categorical)
    assert not unknown, f"候选集引用了未知列 {sorted(unknown)}"
    return ([c for c in columns if c in set(numeric)], [c for c in columns if c in set(categorical)])


def new_classifier(categorical_mask: np.ndarray) -> HistGradientBoostingClassifier:
    return HistGradientBoostingClassifier(
        max_iter=400, learning_rate=0.06, max_leaf_nodes=31, min_samples_leaf=40,
        l2_regularization=1.0, early_stopping=True, validation_fraction=0.15,
        n_iter_no_change=30, random_state=common.SEED, categorical_features=categorical_mask)


def new_regressor() -> HistGradientBoostingRegressor:
    return HistGradientBoostingRegressor(
        max_iter=400, learning_rate=0.06, max_leaf_nodes=31, min_samples_leaf=40,
        l2_regularization=1.0, early_stopping=True, validation_fraction=0.15,
        n_iter_no_change=30, random_state=common.SEED)


def _smoothed_rate(keys: pd.DataFrame, y: pd.Series, alpha: float) -> pd.DataFrame:
    index = pd.MultiIndex.from_frame(keys)
    stats = pd.DataFrame({"y": y.to_numpy()}, index=index).groupby(level=list(range(keys.shape[1])))["y"]
    table = stats.agg(["sum", "size"]).reset_index()
    table["rate"] = (table["sum"] + alpha * NEUTRAL_PRIOR) / (table["size"] + alpha)
    return table


def fit_baselines(train: pd.DataFrame) -> dict[str, pd.DataFrame]:
    """TRAIN 上的三档查表基线。查不到的键在预测时回落全局率。"""
    y = train["y_over"]
    return {"global": pd.DataFrame({"rate": [float(y.mean())]}),
            "station": _smoothed_rate(train[["station_id"]], y, BASELINE_ALPHA),
            "site_hour": _smoothed_rate(train[["site_type", "hour_block"]], y, BASELINE_ALPHA),
            "site_class": _smoothed_rate(train[["site_type", "vehicle_class"]], y, BASELINE_ALPHA)}


def predict_baseline(baselines: dict[str, pd.DataFrame], frame: pd.DataFrame) -> np.ndarray:
    """三档平滑经验率按对数几率加权平均（谁有历史就多信谁），全缺则回落全局率。

    用 ``merge(validate="many_to_one")`` 而不是索引 join：右表键若真重复会直接报错，
    而不是把行数悄悄放大。
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


def baseline_scores(frame: pd.DataFrame, baselines: dict[str, pd.DataFrame]) -> dict[str, np.ndarray]:
    """七档基线的打分。名字里带 oracle 的一律不可部署，只在报告里做对照。"""
    return {
        "globalBaseRate": np.full(len(frame), float(baselines["global"]["rate"].iloc[0])),
        "trainCellBlend": predict_baseline(baselines, frame),
        "asOfUserPriorOnly": frame["user_over_rate_prior"].to_numpy(dtype=float),
        "asOfStationPriorOnly": frame["station_over_rate_prior"].to_numpy(dtype=float),
        "plannedHoursOnly": frame["planned_hours"].to_numpy(dtype=float),
        "oracleActualDurationNONDEPLOYABLE": frame["duration_min"].to_numpy(dtype=float),
        "naiveLastOverstay": frame["user_last_over"].fillna(0.0).to_numpy(dtype=float),
    }


def pick_threshold(prob: np.ndarray, y: np.ndarray, alert_cap: np.ndarray | None = None) -> dict:
    """VALIDATION 上选阈值：精确率 ≥ ``PRECISION_MULTIPLE``×基础率、提醒不超预算，取召回最大。

    ``alert_cap`` 是**当日开始会话数**构成的逐日预算上限（``ceil(25% × 当日会话数)``）：移车提醒
    是按"今天开始的这批会话"每天发一轮的，用全局比例卡预算会把"哪天提醒多少人"糊掉。
    没有档满足时不改口径去凑，只退回"最高精确率档"并把这句话冻进产物。
    """
    target = PRECISION_MULTIPLE * float(np.mean(y))
    grid = sorted({round(float(value), 6) for value in np.quantile(prob, np.linspace(0.50, 0.99, 50))})
    best = None
    fallback = None
    positives = max(1, int((y == 1).sum()))
    for threshold in grid:
        flagged = prob >= threshold
        count = int(flagged.sum())
        if count < MIN_FLAGGED:
            continue
        if alert_cap is not None and count > alert_cap.sum():
            continue
        precision = float(y[flagged].mean())
        candidate = {"threshold": float(threshold), "precision": precision,
                     "recall": float(flagged[y == 1].sum() / positives),
                     "alertRate": float(flagged.mean()), "alerts": count}
        if precision >= target and (best is None or candidate["recall"] > best["recall"]):
            best = candidate
        if fallback is None or precision > fallback["precision"]:
            fallback = candidate
    rule = (f"精确率≥{target:.3f}（={PRECISION_MULTIPLE:.1f}×VALIDATION 基础率）且提醒≤当日预算"
            f"（{MAX_ALERT_SHARE:.0%} 当日开始会话数）的档里取召回最大")
    if best is not None:
        return {**best, "rule": rule, "precisionTarget": round(target, 5), "targetMet": True}
    if fallback is None:
        raise RuntimeError("VALIDATION 上没有任何档位能凑出 30 条提醒，运营口径需要重新定义")
    return {**fallback, "rule": f"{rule}——无档满足，退回最高精确率档（如实标注，不假装达标）",
            "precisionTarget": round(target, 5), "targetMet": False}


def daily_alert_cap(frame: pd.DataFrame) -> np.ndarray:
    """把"当日开始会话数的 25%"换算成整段可分配的提醒条数上限（逐日求和成一个标量）。

    这里**不**抬到 ``MIN_FLAGGED``：预算上限与"至少要有 30 条提醒才够统计"是两条独立规则，
    混在一起等于自己放宽自己的口径。
    """
    days = frame.groupby("business_date", observed=True)["session_id"].count()
    return np.array([max(1, int(np.ceil(MAX_ALERT_SHARE * value))) for value in days])


def metrics_block(y: np.ndarray, prob: np.ndarray) -> dict:
    return {"auc": round(float(roc_auc_score(y, prob)), 4),
            "prAuc": round(float(average_precision_score(y, prob)), 4),
            "logLoss": round(float(log_loss(y, np.clip(prob, 1e-6, 1 - 1e-6))), 5),
            "brier": round(common.brier(y, prob), 5),
            "liftAt5pct": round(common.lift_at(y, prob, 0.05), 3),
            "liftAt10pct": round(common.lift_at(y, prob, 0.10), 3),
            "liftAt20pct": round(common.lift_at(y, prob, 0.20), 3)}


def _smoothed_mean(keys: pd.DataFrame, values: pd.Series, alpha: float, global_mean: float
                   ) -> pd.DataFrame:
    index = pd.MultiIndex.from_frame(keys)
    stats = pd.DataFrame({"v": values.to_numpy()}, index=index).groupby(
        level=list(range(keys.shape[1])))["v"]
    table = stats.agg(["sum", "size"]).reset_index()
    table["mean"] = (table["sum"] + alpha * global_mean) / (table["size"] + alpha)
    return table


def fit_regress_baselines(train: pd.DataFrame) -> dict[str, pd.DataFrame]:
    """回归（占多久）的查表基线：站、站型×时段块两级平滑均值 + 全局均值。"""
    values = train["over_min"]
    global_mean = float(values.mean())
    alpha = 20.0
    return {"global": pd.DataFrame({"mean": [global_mean]}),
            "station": _smoothed_mean(train[["station_id"]], values, alpha, global_mean),
            "site_hour": _smoothed_mean(train[["site_type", "hour_block"]], values, alpha,
                                        global_mean)}


def predict_regress_baseline(baselines: dict[str, pd.DataFrame], frame: pd.DataFrame) -> np.ndarray:
    """站均值为主、站型×时段块均值兜底（先验本身已向全局收缩，简单取"先查到的那级"即可）。"""
    global_mean = float(baselines["global"]["mean"].iloc[0])
    picked = np.full(len(frame), np.nan)
    for name, keys in (("station", ("station_id",)), ("site_hour", ("site_type", "hour_block"))):
        table = baselines[name][list(keys) + ["mean"]].copy()
        helper = pd.DataFrame({"_pos": np.arange(len(frame))})
        for key in keys:
            helper[key] = frame[key].to_numpy()
        merged = helper.merge(table, on=list(keys), how="left", validate="many_to_one")
        values = merged.sort_values("_pos")["mean"].to_numpy(dtype=float)
        picked = np.where(np.isnan(picked), values, picked)
    return np.where(np.isnan(picked), global_mean, picked)


def main() -> dict:
    common.require_empty_run_dir(common.OUT_DIR, extra_allowed=(common.FEATURES_PATH.name,
                                                                common.BUILD_SUMMARY_PATH.name))
    frame, summary = load_matrix()
    numeric = summary["features"]["numeric"]
    categorical = summary["features"]["categorical"]
    train = frame[frame["split"] == "TRAIN"].reset_index(drop=True)
    validation = frame[frame["split"] == "VALIDATION"].reset_index(drop=True)
    held_out = int((frame["split"] == "TEST").sum())
    y_train = train["y_over"].to_numpy(dtype=int)
    y_valid = validation["y_over"].to_numpy(dtype=int)
    print(f"[train] TRAIN={len(train):,} 场会话 VALIDATION={len(validation):,} "
          f"TEST(本次不碰)={held_out:,}")

    baselines = fit_baselines(train)
    valid_scores = baseline_scores(validation, baselines)
    comparison = {"globalBaseRate": {"auc": 0.5, "prAuc": round(float(y_valid.mean()), 4),
                                    "note": "常数打分，AUC 恒 0.5，只是尺子"}}
    for name, scores in valid_scores.items():
        if name == "globalBaseRate":
            continue
        comparison[name] = metrics_block(y_valid, scores)
    for name, block in comparison.items():
        print(f"[train] 基线 {name}: VAL AUC={block['auc']:.4f}")

    groups = summary["features"]["groups"]
    selection: dict[str, dict] = {}
    fitted: dict[str, tuple[HistGradientBoostingClassifier, np.ndarray]] = {}
    for label, columns in groups.items():
        num, cat = split_group(columns, numeric, categorical)
        x_train, mask = design(train, num, cat)
        x_valid, _ = design(validation, num, cat)
        model = new_classifier(mask)
        model.fit(x_train, y_train)
        prob_valid = model.predict_proba(x_valid)[:, 1]
        block = metrics_block(y_valid, prob_valid)
        block.update({"featureCount": int(x_train.shape[1]), "iterations": int(model.n_iter_),
                      "numericFeatures": len(num), "categoricalFeatures": len(cat)})
        selection[label] = block
        fitted[label] = (model, prob_valid)
        print(f"[train] 候选组 {label}: VAL AUC={block['auc']:.4f} PR-AUC={block['prAuc']:.4f} "
              f"特征={block['featureCount']}")

    chosen = max(selection, key=lambda key: selection[key]["auc"])
    classifier, prob_valid = fitted[chosen]
    operating = pick_threshold(prob_valid, y_valid, daily_alert_cap(validation))

    # 回归目标：这场会话充完还要占多少分钟。排序用它就是"按预计占桩分钟提醒"，
    # 与"按超时概率提醒"是两种排程——运营选一，另一条留在报告里做对照。
    num, cat = split_group(groups[chosen], numeric, categorical)
    x_train, _ = design(train, num, cat)
    x_valid, _ = design(validation, num, cat)
    regressor = new_regressor()
    regressor.fit(x_train, train["over_min"].to_numpy(dtype=float))
    minutes_valid = regressor.predict(x_valid)
    regress_baselines = fit_regress_baselines(train)
    count_metrics = {
        "model": {"mae": round(common.mae(validation["over_min"].to_numpy(dtype=float), minutes_valid), 3),
                  "iterations": int(regressor.n_iter_)},
        "trainMeanOverMin": {"mae": round(common.mae(validation["over_min"].to_numpy(dtype=float),
                                                     np.full(len(validation),
                                                             float(train["over_min"].mean()))), 3)},
        "asOfUserMeanPrior": {"mae": round(common.mae(validation["over_min"].to_numpy(dtype=float),
                                                      validation["user_over_mean_prior"]
                                                      .to_numpy(dtype=float)), 3)},
        "trainCellMeanBlend": {"mae": round(common.mae(validation["over_min"].to_numpy(dtype=float),
                                                       predict_regress_baseline(regress_baselines,
                                                                                validation)), 3)},
    }
    print(f"[train] 回归（占多久，分钟）MAE 模型={count_metrics['model']['mae']} "
          f"基线={{{', '.join(f'{k}={v['mae']}' for k, v in count_metrics.items() if k != 'model')}}}")

    blend = comparison["trainCellBlend"]
    user_prior = comparison["asOfUserPriorOnly"]
    metrics = {
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "datasetId": common.DATASET_ID, "modelId": common.MODEL_ID,
        "modelVersion": common.MODEL_VERSION, "seed": common.SEED,
        "featuresSha256": summary["featuresSha256"], "label": summary["label"],
        "sampleUnit": summary["sampleUnit"], "decisionTime": summary["decisionTime"],
        "scope": summary["scope"], "splits": summary["splits"],
        "splitBaseRate": summary["splitBaseRate"],
        "trainRows": int(len(train)), "validationRows": int(len(validation)),
        "testRowsUntouched": held_out,
        "selectionRule": "四组候选特征集按 VALIDATION ROC AUC 择一发布；TEST 完全不参与选择",
        "chosenSet": chosen, "candidateValidation": selection,
        "baselineValidation": comparison,
        "validation": {**selection[chosen], "baseRate": round(float(y_valid.mean()), 5),
                       "aucGainVsCellBlendPct": round(100.0 * (selection[chosen]["auc"] / blend["auc"] - 1.0), 2),
                       "aucGainVsUserPriorPct": round(100.0 * (selection[chosen]["auc"]
                                                               / user_prior["auc"] - 1.0), 2)},
        "minutesRegressionValidation": count_metrics,
        "operatingPoint": operating,
        "numericFeatures": num, "categoricalFeatures": cat,
        "allNumericFeatures": numeric, "allCategoricalFeatures": categorical,
        "disclaimer": common.data_note(),
    }

    bundle = {
        "classifier": classifier, "regressor": regressor, "chosenSet": chosen,
        "featureNames": num + cat, "numericFeatures": num, "categoricalFeatures": cat,
        "allNumericFeatures": numeric, "allCategoricalFeatures": categorical,
        "candidateSets": {key: split_group(value, numeric, categorical) for key, value in groups.items()},
        "baselines": baselines, "regressBaselines": regress_baselines,
        "operatingPoint": operating,
        "modelId": common.MODEL_ID, "modelVersion": common.MODEL_VERSION, "seed": common.SEED,
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "datasetId": common.DATASET_ID,
        "featuresSha256": summary["featuresSha256"], "splits": summary["boundaries"],
        "dataNote": common.data_note(), "label": summary["label"],
    }
    buffer = io.BytesIO()
    joblib.dump(bundle, buffer)
    common.write_new_bytes(common.BUNDLE_PATH, buffer.getvalue())
    metrics["bundlePath"] = str(common.BUNDLE_PATH.relative_to(common.DATA_ANALYSIS_ROOT.parent))
    metrics["bundleSha256"] = common.sha256_file(common.BUNDLE_PATH)
    common.write_new_json(common.TRAIN_METRICS_PATH, metrics)

    print(f"[train] VAL AUC 模型({chosen})={selection[chosen]['auc']:.4f} "
          f"查表基线={blend['auc']:.4f} 用户先验={user_prior['auc']:.4f} "
          f"站点先验={comparison['asOfStationPriorOnly']['auc']:.4f} "
          f"只用计划时长={comparison['plannedHoursOnly']['auc']:.4f} "
          f"oracle真实时长={comparison['oracleActualDurationNONDEPLOYABLE']['auc']:.4f} "
          f"朴素上次超时={comparison['naiveLastOverstay']['auc']:.4f}")
    print(f"[train] 运营阈值={operating['threshold']:.4f} 精确率={operating['precision']:.4f} "
          f"召回={operating['recall']:.4f} 提醒={operating['alerts']} 提醒率={operating['alertRate']:.4f} "
          f"targetMet={operating['targetMet']}")
    print(f"[train] -> {common.BUNDLE_PATH}")
    return metrics


if __name__ == "__main__":
    main()
