"""拟合桩-日健康度模型：分类（未来 7 天来不来修）+ 回归（大概来几张单），并在 VALIDATION 定死排程阈值。

纪律：
  · 只在 ``split==TRAIN`` 且未被 purge/删失的行上拟合；VALIDATION 选特征组与阈值；TEST 一次
    都不看（evaluate.py 才碰）——本线的 purge/删失在 features 落表时就已标好，这里只做过滤；
  · 早停用 sklearn 内置的 TRAIN 内部随机切分，只决定模型容量，不看时间线；
  · **七档基线**（本线的"廉价口径"全部可部署，oracle 一档除外）：
      1. 全局基础率（TRAIN 常数，尺子）；
      2. TRAIN 三级平滑查表（桩 / 站 / 站型×星期，对数几率加权）——"不建模、纯查表"的工程下限；
      3. 纯因果桩先验 ``charger_ticket_rate_shrunk``（as-of 收缩率，不学东西）；
      4. 纯因果站先验 ``station_ticket_rate_shrunk``——"这个站的设备风气"值多少 AUC；
      5. **只用负荷** ``usage_energy_kwh_30d``——"用得多坏得多"的朴素运维直觉，交给数据定量；
      6. 朴素规则 **最近一次来修**（``days_since_last_report`` 取倒数）——把"刚修过又要修"的直觉
         钉成可对照的分数；
      7. **oracle：全样本桩级来修率**——用了含 TEST 的标签（不可部署），只用来读"每桩固有频率
         知识"的天花板，predict.py 拒绝它。
  · 四组候选特征集（静态/工单历史/运维信号/全部）按 VAL AUC 择一发布；
  · 阈值口径是**运营预算**：精确率 ≥ 2×VALIDATION 基础率、且当日提醒不超过**每日 75 台桩的 25%**
    （ceil→20 台/日）的档里取召回最大；没有档满足就退回"最高精确率档"并如实标注。

产物：outputs/ml_health/gbdt-charger-health-7d-v1.joblib + train_metrics.json（O_EXCL）。
用法（仓库根目录）：python -m data_analysis.ml.health.train
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

#: 查表基线的平滑强度与中性先验（与第七/八线同口径，便于跨线比较"基线值多少"这句话）。
BASELINE_ALPHA = 30.0
NEUTRAL_PRIOR = 0.5
BASELINE_SPECS = (("charger", ("charger_id",), 0.30),
                  ("station", ("station_id",), 0.35),
                  ("site_dow", ("site_type", "day_of_week"), 0.35))

#: 运营口径：精确率相对基础率的倍数、单日提醒上限（占当日在册桩数比例）、最少提醒条数。
PRECISION_MULTIPLE = 2.0
MAX_ALERT_SHARE = 0.25
MIN_FLAGGED = 30


def usable_frame(frame: pd.DataFrame, split: str) -> pd.DataFrame:
    """某段的可建模行：剔除 purge 与删失行（两标记在 features 落表时已冻结）。"""
    part = frame[(frame["split"] == split) & ~frame["purged"] & ~frame["censored"]]
    return part.reset_index(drop=True)


def load_matrix() -> tuple[pd.DataFrame, dict]:
    """读特征表并复查两件事：批次、特征表哈希。任一不符即拒用。

    本线没有 derived 数据集（不落新数据），"零新增数据"在代码层的形状就是这里没有 derived 复查。
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
    assert not frame.duplicated(subset=["charger_id", "business_date"]).any(), \
        "特征表里出现重复桩·日，一行一桩一日的口径不成立"
    assert frame["y_ticket7"].isin((0, 1)).all() and (frame["tickets_next7d"] >= 0).all()
    assert set(frame["split"]) == {"TRAIN", "VALIDATION", "TEST"}, "面板行应全部落段"
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


def _hyper_params(hyper: dict | None) -> dict:
    """容量档 → sklearn 参数：`name` 只是台账标签，不能进构造器。"""
    return {k: v for k, v in (hyper or {}).items() if k != "name"}


def new_classifier(categorical_mask: np.ndarray,
                   hyper: dict | None = None) -> HistGradientBoostingClassifier:
    params = dict(max_iter=400, learning_rate=0.06, max_leaf_nodes=31, min_samples_leaf=40)
    params.update(_hyper_params(hyper))
    return HistGradientBoostingClassifier(
        **params, l2_regularization=1.0, early_stopping=True, validation_fraction=0.15,
        n_iter_no_change=30, random_state=common.SEED, categorical_features=categorical_mask)


def new_regressor(hyper: dict | None = None) -> HistGradientBoostingRegressor:
    params = dict(max_iter=400, learning_rate=0.06, max_leaf_nodes=31, min_samples_leaf=40)
    params.update(_hyper_params(hyper))
    return HistGradientBoostingRegressor(
        **params, l2_regularization=1.0, early_stopping=True, validation_fraction=0.15,
        n_iter_no_change=30, random_state=common.SEED)


#: 容量阶梯。第八线的 31 叶 × 400 轮在 12 万行/数千正例上成立；本线 TRAIN 只有 1645 个正例
#: （且相邻日的标签窗重叠 6 天、有效样本量更低），大容量树会把 VAL 方向学反（实测 full 组
#: 31 叶档 VAL AUC 0.46——比单特征先验 0.53 还低）。小容量档把"要不要容量"变成可盲评的选择。
HYPER_GRID: tuple[dict, ...] = (
    {"name": "big31", "max_leaf_nodes": 31, "learning_rate": 0.06, "max_iter": 400,
     "min_samples_leaf": 40},
    {"name": "mid15", "max_leaf_nodes": 15, "learning_rate": 0.05, "max_iter": 300,
     "min_samples_leaf": 60},
    {"name": "small7", "max_leaf_nodes": 7, "learning_rate": 0.05, "max_iter": 250,
     "min_samples_leaf": 80},
    {"name": "stump3", "max_leaf_nodes": 3, "learning_rate": 0.03, "max_iter": 400,
     "min_samples_leaf": 100},
)


def _smoothed_rate(keys: pd.DataFrame, y: pd.Series, alpha: float) -> pd.DataFrame:
    index = pd.MultiIndex.from_frame(keys)
    stats = pd.DataFrame({"y": y.to_numpy()}, index=index).groupby(level=list(range(keys.shape[1])))["y"]
    table = stats.agg(["sum", "size"]).reset_index()
    table["rate"] = (table["sum"] + alpha * NEUTRAL_PRIOR) / (table["size"] + alpha)
    return table


def fit_baselines(train: pd.DataFrame) -> dict[str, pd.DataFrame]:
    """TRAIN 上的三档查表基线。查不到的键在预测时回落全局率。"""
    y = train["y_ticket7"]
    return {"global": pd.DataFrame({"rate": [float(y.mean())]}),
            "charger": _smoothed_rate(train[["charger_id"]], y, BASELINE_ALPHA),
            "station": _smoothed_rate(train[["station_id"]], y, BASELINE_ALPHA),
            "site_dow": _smoothed_rate(train[["site_type", "day_of_week"]], y, BASELINE_ALPHA)}


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


def oracle_charger_rate(frame_all: pd.DataFrame, target: pd.DataFrame, alpha: float
                        ) -> np.ndarray:
    """oracle：用**全部行（含 TEST 标签）**的桩级来修率查表——不可部署，只为读出
    "固有每桩频率知识"的天花板。名字里的 NONDEPLOYABLE 不是谦虚：predict.py 会拒收它。"""
    table = _smoothed_rate(frame_all[["charger_id"]], frame_all["y_ticket7"], alpha)
    helper = pd.DataFrame({"_pos": np.arange(len(target)),
                           "charger_id": target["charger_id"].to_numpy()})
    merged = helper.merge(table[["charger_id", "rate"]], on="charger_id", how="left",
                          validate="many_to_one")
    values = merged.sort_values("_pos")["rate"].to_numpy(dtype=float)
    return np.where(np.isnan(values), float(frame_all["y_ticket7"].mean()), values)


def baseline_scores(frame: pd.DataFrame, baselines: dict[str, pd.DataFrame],
                    oracle: np.ndarray) -> dict[str, np.ndarray]:
    """七档基线的打分。名字里带 NONDEPLOYABLE 的一律不可部署，只在报告里做对照。"""
    days_since = pd.to_numeric(frame["days_since_last_report"], errors="coerce")
    recency = 1.0 / (1.0 + days_since.fillna(np.inf))
    return {
        "globalBaseRate": np.full(len(frame), float(baselines["global"]["rate"].iloc[0])),
        "trainCellBlend": predict_baseline(baselines, frame),
        "asOfChargerPriorOnly": frame["charger_ticket_rate_shrunk"].to_numpy(dtype=float),
        "asOfStationPriorOnly": frame["station_ticket_rate_shrunk"].to_numpy(dtype=float),
        "usageOnly": frame["usage_energy_kwh_30d"].to_numpy(dtype=float),
        "naiveRecentTicket": recency.to_numpy(dtype=float),
        "oracleChargerRateFullSampleNONDEPLOYABLE": oracle,
    }


def pick_threshold(prob: np.ndarray, y: np.ndarray, alert_cap: np.ndarray | None = None) -> dict:
    """VALIDATION 上选阈值：精确率 ≥ ``PRECISION_MULTIPLE``×基础率、提醒不超预算，取召回最大。

    ``alert_cap`` 是由**每日在册桩数**（75 台 × 25% 上取整）逐日求和成的预算标量：维护班组每天
    能吃下的巡检名额与"今天有多少桩在册"挂钩，用全局比例卡预算会把"哪天排多少人"糊掉。
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
            f"（{MAX_ALERT_SHARE:.0%}×每日在册桩数）的档里取召回最大")
    if best is not None:
        return {**best, "rule": rule, "precisionTarget": round(target, 5), "targetMet": True}
    if fallback is None:
        raise RuntimeError("VALIDATION 上没有任何档位能凑出 30 条提醒，运营口径需要重新定义")
    return {**fallback, "rule": f"{rule}——无档满足，退回最高精确率档（如实标注，不假装达标）",
            "precisionTarget": round(target, 5), "targetMet": False}


def daily_alert_cap(frame: pd.DataFrame) -> np.ndarray:
    """把"每日在册桩数的 25%"换算成整段可分配的提醒条数上限（逐日求和成一个标量）。

    这里**不**抬到 ``MIN_FLAGGED``：预算上限与"至少要有 30 条提醒才够统计"是两条独立规则，
    混在一起等于自己放宽自己的口径（第八线同款纪律）。
    """
    days = frame.groupby("business_date", observed=True)["charger_id"].count()
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
    """回归（未来 7 天来几张单）的查表基线：桩、站两级平滑均值 + 全局均值。"""
    values = train["tickets_next7d"]
    global_mean = float(values.mean())
    alpha = 20.0
    return {"global": pd.DataFrame({"mean": [global_mean]}),
            "charger": _smoothed_mean(train[["charger_id"]], values, alpha, global_mean),
            "station": _smoothed_mean(train[["station_id"]], values, alpha, global_mean)}


def predict_regress_baseline(baselines: dict[str, pd.DataFrame], frame: pd.DataFrame) -> np.ndarray:
    """桩均值为主、站均值兜底（先验已向他收缩，简单取"先查到的那级"即可）。"""
    global_mean = float(baselines["global"]["mean"].iloc[0])
    picked = np.full(len(frame), np.nan)
    for name, keys in (("charger", ("charger_id",)), ("station", ("station_id",))):
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
    train = usable_frame(frame, "TRAIN")
    validation = usable_frame(frame, "VALIDATION")
    held_out = int(((frame["split"] == "TEST") & ~frame["purged"] & ~frame["censored"]).sum())
    y_train = train["y_ticket7"].to_numpy(dtype=int)
    y_valid = validation["y_ticket7"].to_numpy(dtype=int)
    print(f"[train] TRAIN={len(train):,} 桩·日 VALIDATION={len(validation):,} "
          f"TEST(本次不碰， purge/删失后)={held_out:,}")

    baselines = fit_baselines(train)
    oracle_valid = oracle_charger_rate(frame, validation, BASELINE_ALPHA)
    valid_scores = baseline_scores(validation, baselines, oracle_valid)
    comparison = {"globalBaseRate": {"auc": 0.5, "prAuc": round(float(y_valid.mean()), 4),
                                     "note": "常数打分，AUC 恒 0.5，只是尺子"}}
    for name, scores in valid_scores.items():
        if name == "globalBaseRate":
            continue
        comparison[name] = metrics_block(y_valid, scores)
    for name, block in comparison.items():
        print(f"[train] 基线 {name}: VAL AUC={block['auc']:.4f}")

    groups = summary["features"]["groups"]
    # 组 × 容量档全网格按 VAL AUC 选择。选择面只在 VALIDATION 上；TEST 不参与任何一步。
    grid_results: dict[str, dict] = {}
    best_per_group: dict[str, tuple[float, dict, HistGradientBoostingClassifier, np.ndarray]] = {}
    for label, columns in groups.items():
        num, cat = split_group(columns, numeric, categorical)
        x_train, mask = design(train, num, cat)
        x_valid, _ = design(validation, num, cat)
        for hyper in HYPER_GRID:
            model = new_classifier(mask, hyper)
            model.fit(x_train, y_train)
            prob = model.predict_proba(x_valid)[:, 1]
            block = metrics_block(y_valid, prob)
            block.update({"featureCount": int(x_train.shape[1]), "iterations": int(model.n_iter_),
                          "numericFeatures": len(num), "categoricalFeatures": len(cat),
                          "hyper": hyper["name"]})
            grid_results[f"{label}|{hyper['name']}"] = block
            top = best_per_group.get(label)
            if top is None or block["auc"] > top[0]:
                best_per_group[label] = (block["auc"], hyper, model, prob)
        cells = " ".join("{}={:.4f}".format(h["name"], grid_results[f"{label}|{h['name']}"]["auc"])
                         for h in HYPER_GRID)
        print(f"[train] 组 {label}: {cells}")

    chosen = max(best_per_group, key=lambda key: best_per_group[key][0])
    _, chosen_hyper, classifier, prob_valid = best_per_group[chosen]
    selection = {label: grid_results[f"{label}|{best_per_group[label][1]['name']}"]
                 for label in best_per_group}
    operating = pick_threshold(prob_valid, y_valid, daily_alert_cap(validation))

    # 回归目标：未来 7 天这张桩会来几张工单。排序用它就是"按预计工单量排维护班表"，
    # 与"按来修概率提醒"是两种排程——运营选一，另一条留在报告里做对照。
    num, cat = split_group(groups[chosen], numeric, categorical)
    x_train, _ = design(train, num, cat)
    x_valid, _ = design(validation, num, cat)
    regressor = new_regressor(chosen_hyper)
    regressor.fit(x_train, train["tickets_next7d"].to_numpy(dtype=float))
    count_valid = regressor.predict(x_valid)
    regress_baselines = fit_regress_baselines(train)
    actual_counts = validation["tickets_next7d"].to_numpy(dtype=float)
    count_metrics = {
        "model": {"mae": round(common.mae(actual_counts, count_valid), 3),
                  "iterations": int(regressor.n_iter_)},
        "trainMeanTicketCount": {"mae": round(common.mae(actual_counts, np.full(
            len(validation), float(train["tickets_next7d"].mean()))), 3)},
        "trainCellMeanBlend": {"mae": round(common.mae(actual_counts, predict_regress_baseline(
            regress_baselines, validation)), 3)},
    }
    others = ", ".join(f"{k}={v['mae']}" for k, v in count_metrics.items() if k != "model")
    print(f"[train] 回归（未来7天工单张数）MAE 模型={count_metrics['model']['mae']} 基线={{{others}}}")

    blend = comparison["trainCellBlend"]
    charger_prior = comparison["asOfChargerPriorOnly"]
    hyper_name = chosen_hyper["name"]
    metrics = {
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "datasetId": common.DATASET_ID, "modelId": common.MODEL_ID,
        "modelVersion": common.MODEL_VERSION, "seed": common.SEED,
        "featuresSha256": summary["featuresSha256"], "label": summary["label"],
        "sampleUnit": summary["sampleUnit"], "decisionTime": summary["decisionTime"],
        "purgeDiscipline": summary["purgeDiscipline"],
        "scope": summary["scope"], "splits": summary["scope"]["splits"],
        "usableBaseRate": summary["scope"]["usableBaseRate"],
        "trainRows": int(len(train)), "validationRows": int(len(validation)),
        "testRowsUntouched": held_out,
        "selectionRule": ("四组候选特征集 × 四档容量（big31/mid15/small7/stump3）共 16 个组合，"
                          "按 VALIDATION ROC AUC 择一发布；TEST 完全不参与选择"),
        "chosenSet": chosen, "chosenHyper": chosen_hyper["name"],
        "hyperGrid": grid_results, "candidateValidation": selection,
        "baselineValidation": comparison,
        "validation": {**selection[chosen], "baseRate": round(float(y_valid.mean()), 5),
                       "aucGainVsCellBlendPct": round(100.0 * (selection[chosen]["auc"] / blend["auc"] - 1.0), 2),
                       "aucGainVsChargerPriorPct": round(100.0 * (selection[chosen]["auc"]
                                                                  / charger_prior["auc"] - 1.0), 2)},
        "countRegressionValidation": count_metrics,
        "operatingPoint": operating,
        "numericFeatures": num, "categoricalFeatures": cat,
        "allNumericFeatures": numeric, "allCategoricalFeatures": categorical,
        "disclaimer": common.data_note(),
    }

    bundle = {
        "classifier": classifier, "regressor": regressor, "chosenSet": chosen,
        "chosenHyper": chosen_hyper["name"],
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

    print(f"[train] VAL AUC 模型({chosen}|{hyper_name})={selection[chosen]['auc']:.4f} "
          f"查表基线={blend['auc']:.4f} 桩先验={charger_prior['auc']:.4f} "
          f"站先验={comparison['asOfStationPriorOnly']['auc']:.4f} "
          f"只用负荷={comparison['usageOnly']['auc']:.4f} "
          f"朴素最近来修={comparison['naiveRecentTicket']['auc']:.4f} "
          f"oracle全样本桩率={comparison['oracleChargerRateFullSampleNONDEPLOYABLE']['auc']:.4f}")
    print(f"[train] 运营阈值={operating['threshold']:.4f} 精确率={operating['precision']:.4f} "
          f"召回={operating['recall']:.4f} 提醒={operating['alerts']} 提醒率={operating['alertRate']:.4f} "
          f"targetMet={operating['targetMet']}")
    print(f"[train] -> {common.BUNDLE_PATH}")
    return metrics


if __name__ == "__main__":
    main()
