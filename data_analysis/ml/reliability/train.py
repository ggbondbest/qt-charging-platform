"""拟合桩级次日可靠性模型：分类（明天会不会出问题）+ 回归（明天大概坏几次），并在 VALIDATION 定死巡检阈值。

纪律：
  · 只在 ``split == TRAIN`` 上拟合；VALIDATION 选特征组与阈值；TEST 一次都不看（evaluate.py 才碰）；
  · 早停用 sklearn 内置的 TRAIN 内部随机切分，只决定模型容量，不看时间线；
  · **六档基线**先立起来（本线的基线比模型更重要，因为标签被暴露度牵着走）：
      1. 全局基础率（常数，AUC 恒 0.5，只是尺子）；
      2. TRAIN 三级平滑查表（桩/型号/站，对数几率加权）；
      3. 纯因果桩先验 ``charger_fail_rate_prior``（不学东西，只看这台桩昨天为止的失败日率）；
      4. **只用因果日均尝试量** ``attempts_per_day_30d``——"知道明天大概会被用几次"值多少 AUC；
      5. **oracle：真用明天当天的尝试次数** ``attempts_on_day``——标注不可部署，用来读出
         "排序收益里有多少其实是暴露度"；
      6. 朴素 lag1 规则（昨天失败 ⇒ 今天继续告警）——实测这一条在本线是**正号**的
         （P(今日失败日|昨天坏)=0.3446 vs 昨天好=0.1822），留着它是为了让这个直觉被数据
         量化，而不是被想当然地接受或否掉；它的 VAL AUC=0.5975 也顺带给出尺子。
  · 四组候选特征集（只身份/计划、只面板历史、只运营信号、全部）按 VAL AUC 择一发布。
  · 阈值口径是**运营预算**：精确率 ≥ 2×VALIDATION 基础率、且告警不超过当日桩数 25% 的档里取召回最大；
    没有档满足就退回"最高精确率档"并如实标注，不改口径去凑达标。

产物：outputs/ml_reliability/gbdt-charger-day-reliability-v1.joblib + train_metrics.json（O_EXCL）。
用法（仓库根目录）：python -m data_analysis.ml.reliability.train
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

#: 查表基线的平滑强度与中性先验（与第六线同口径，便于跨线比较"基线值多少"这句话）。
BASELINE_ALPHA = 30.0
NEUTRAL_PRIOR = 0.5
BASELINE_SPECS = (("charger", ("charger_id",), 0.40), ("charger_model", ("charger_model",), 0.30),
                  ("station", ("station_id",), 0.30))

#: 运营口径：精确率相对基础率的倍数、当日告警上限（占在用桩数比例）、最少告警条数。
PRECISION_MULTIPLE = 2.0
MAX_ALERT_SHARE = 0.25
MIN_FLAGGED = 30


def load_matrix() -> tuple[pd.DataFrame, dict]:
    """读特征表并复查三件事：批次、派生集哈希、特征表哈希。任一不符即拒用。"""
    if not common.FEATURES_PATH.exists():
        raise FileNotFoundError(f"缺少特征表 {common.FEATURES_PATH}，请先跑 features")
    frame = pd.read_pickle(common.FEATURES_PATH)
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    common.verify_batch()
    common.verify_derived()
    if summary.get("publishedBatchId") != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch(
            f"特征表构建时绑定的批次 {summary.get('publishedBatchId')!r} 与当前 "
            f"{common.EXPECTED_PUBLISHED_BATCH_ID!r} 不一致，请重跑 features")
    if summary.get("featuresSha256") != common.sha256_file(common.FEATURES_PATH):
        raise common.DerivedMismatch("特征表哈希与 features_summary 记录不一致，表被改过，请重跑 features")
    assert not frame.duplicated(subset=["charger_id", "business_date"]).any(), \
        "特征表里出现重复的 (桩, 日)，一行一天的前提不成立"
    assert (frame["attempts_on_day"] > 0).all(), "样本里混进了 0 尝试的桩日"
    assert int((frame["y_fail"] == 1).sum()) == int((frame["tech_fails_on_day"] > 0).sum())
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
    y = train["y_fail"]
    return {"global": pd.DataFrame({"rate": [float(y.mean())]}),
            "charger": _smoothed_rate(train[["charger_id"]], y, BASELINE_ALPHA),
            "charger_model": _smoothed_rate(train[["charger_model"]], y, BASELINE_ALPHA),
            "station": _smoothed_rate(train[["station_id"]], y, BASELINE_ALPHA)}


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
    """六档基线的打分。名字里带 oracle 的一律不可部署，只在报告里做对照。"""
    return {
        "globalBaseRate": np.full(len(frame), float(baselines["global"]["rate"].iloc[0])),
        "trainLookupBlend": predict_baseline(baselines, frame),
        "asOfChargerPriorOnly": frame["charger_fail_rate_prior"].to_numpy(dtype=float),
        "expectedVolumeOnly": frame["attempts_per_day_30d"].to_numpy(dtype=float),
        "oracleActualVolumeNONDEPLOYABLE": frame["attempts_on_day"].to_numpy(dtype=float),
        "naiveLag1Alert": frame["prev_day_fail"].fillna(0.0).to_numpy(dtype=float),
    }


def pick_threshold(prob: np.ndarray, y: np.ndarray, alert_cap: np.ndarray | None = None) -> dict:
    """VALIDATION 上选阈值：精确率 ≥ ``PRECISION_MULTIPLE``×基础率、告警不超预算，取召回最大。

    ``alert_cap`` 是**当日在用桩数**构成的逐日预算上限（``ceil(25% × 当日桩数)``）：告警是每天
    发一批的，用全局比例卡预算会把"哪天查多少台"这件事糊掉。没有档满足时不改口径去凑，
    只退回"最高精确率档"并把这句话冻进产物。
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
    rule = (f"精确率≥{target:.3f}（={PRECISION_MULTIPLE:.1f}×VALIDATION 基础率）且告警≤当日预算"
            f"（{MAX_ALERT_SHARE:.0%} 在用桩数）的档里取召回最大")
    if best is not None:
        return {**best, "rule": rule, "precisionTarget": round(target, 5), "targetMet": True}
    if fallback is None:
        raise RuntimeError("VALIDATION 上没有任何档位能凑出 30 条告警，运营口径需要重新定义")
    return {**fallback, "rule": f"{rule}——无档满足，退回最高精确率档（如实标注，不假装达标）",
            "precisionTarget": round(target, 5), "targetMet": False}


def daily_alert_cap(frame: pd.DataFrame) -> np.ndarray:
    """把"当日在用桩数的 25%"换算成整段可分配的告警条数上限（逐日求和成一个标量）。

    这里**不**抬到 ``MIN_FLAGGED``：预算上限与"至少要有 30 条告警才够统计"是两条独立规则，
    混在一起会让 75 台桩/天的预算从 19 条被抬到 30 条，等于自己放宽自己的口径。
    """
    days = frame.groupby("business_date", observed=True)["charger_id"].nunique()
    return np.array([max(1, int(np.ceil(MAX_ALERT_SHARE * value))) for value in days])


def metrics_block(y: np.ndarray, prob: np.ndarray) -> dict:
    return {"auc": round(float(roc_auc_score(y, prob)), 4),
            "prAuc": round(float(average_precision_score(y, prob)), 4),
            "logLoss": round(float(log_loss(y, np.clip(prob, 1e-6, 1 - 1e-6))), 5),
            "brier": round(common.brier(y, prob), 5),
            "liftAt5pct": round(common.lift_at(y, prob, 0.05), 3),
            "liftAt10pct": round(common.lift_at(y, prob, 0.10), 3),
            "liftAt20pct": round(common.lift_at(y, prob, 0.20), 3)}


def main() -> dict:
    common.require_empty_run_dir(common.OUT_DIR, extra_allowed=(common.FEATURES_PATH.name,
                                                                common.BUILD_SUMMARY_PATH.name))
    frame, summary = load_matrix()
    numeric = summary["features"]["numeric"]
    categorical = summary["features"]["categorical"]
    train = frame[frame["split"] == "TRAIN"].reset_index(drop=True)
    validation = frame[frame["split"] == "VALIDATION"].reset_index(drop=True)
    held_out = int((frame["split"] == "TEST").sum())
    y_train = train["y_fail"].to_numpy(dtype=int)
    y_valid = validation["y_fail"].to_numpy(dtype=int)
    print(f"[train] TRAIN={len(train):,} 桩日 VALIDATION={len(validation):,} "
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

    # 回归目标：明天坏几次。用它排序就是"按预计失败台数巡检"，与"按概率巡检"是两种排程。
    num, cat = split_group(groups[chosen], numeric, categorical)
    x_train, _ = design(train, num, cat)
    x_valid, _ = design(validation, num, cat)
    regressor = new_regressor()
    regressor.fit(x_train, train["y_fails"].to_numpy(dtype=int))
    count_valid = regressor.predict(x_valid)
    count_baselines = {
        "trainMeanFails": np.full(len(validation), float(train["y_fails"].mean())),
        "expectedVolumeTimesPrior": (validation["attempts_per_day_30d"].to_numpy(dtype=float)
                                     * validation["charger_fail_rate_attempt_prior"].to_numpy(dtype=float)),
    }
    count_metrics = {
        "model": {"mae": round(common.mae(validation["y_fails"].to_numpy(dtype=float), count_valid), 5),
                  "iterations": int(regressor.n_iter_)},
        **{name: {"mae": round(common.mae(validation["y_fails"].to_numpy(dtype=float), scores), 5)}
           for name, scores in count_baselines.items()},
    }
    print(f"[train] 回归（明天坏几次）MAE 模型={count_metrics['model']['mae']} "
          f"基线={ {k: v['mae'] for k, v in count_metrics.items() if k != 'model'} }")

    lookup = comparison["trainLookupBlend"]
    prior = comparison["asOfChargerPriorOnly"]
    metrics = {
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "derivedDatasetId": summary["derivedDatasetId"],
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
                       "aucGainVsLookupPct": round(100.0 * (selection[chosen]["auc"] / lookup["auc"] - 1.0), 2),
                       "aucGainVsAsOfPriorPct": round(100.0 * (selection[chosen]["auc"]
                                                               / prior["auc"] - 1.0), 2)},
        "countRegressionValidation": count_metrics,
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
        "baselines": baselines, "operatingPoint": operating,
        "modelId": common.MODEL_ID, "modelVersion": common.MODEL_VERSION, "seed": common.SEED,
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "derivedDatasetId": summary["derivedDatasetId"], "datasetId": common.DATASET_ID,
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
          f"查表基线={lookup['auc']:.4f} 纯因果桩先验={prior['auc']:.4f} "
          f"只用预期用量={comparison['expectedVolumeOnly']['auc']:.4f} "
          f"oracle真实用量={comparison['oracleActualVolumeNONDEPLOYABLE']['auc']:.4f} "
          f"朴素lag1={comparison['naiveLag1Alert']['auc']:.4f}")
    print(f"[train] 运营阈值={operating['threshold']:.4f} 精确率={operating['precision']:.4f} "
          f"召回={operating['recall']:.4f} 告警={operating['alerts']} 告警率={operating['alertRate']:.4f} "
          f"targetMet={operating['targetMet']}")
    print(f"[train] -> {common.BUNDLE_PATH}")
    return metrics


if __name__ == "__main__":
    main()
