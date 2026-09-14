"""拟合插枪启动失败风险模型，并在 VALIDATION 上定死运营阈值。

纪律：
  · 只在 ``split == TRAIN`` 上拟合；VALIDATION 用于选特征组与阈值；TEST 一次都不看
    （evaluate.py 才碰它，且只碰一次）；
  · 早停用 sklearn 内置的 TRAIN 内部随机切分，只决定模型容量，不看时间线；
  · 三档基线先立起来，模型必须打过它才有意义：
      - 全局基础率（AUC 恒 0.5，只是尺子）；
      - TRAIN 上的桩/型号/站三级平滑经验率按对数几率加权（用得到处就多信，缺处回落）；
      - **纯 as-of 桩侧先验率** ``charger_fail_rate_prior``：不学任何东西、只看"这台桩过去
        严格早于此刻"的失败率。它是本线真正的对手——如果 GBDT 打不过它，说明模型只是在
        重新拟合每台桩的常数率，那不如直接上线一张按桩查表。
  · 特征组按 VAL AUC 择一发布（全量 / 只用 as-of 历史 / 不用 as-of 历史），不是拍脑袋用全量。

产物：outputs/ml_attempt/gbdt-attempt-techfail-v1.joblib + train_metrics.json（O_EXCL）。
用法（仓库根目录）：python -m data_analysis.ml.attempt.train
"""

from __future__ import annotations

import io
import json

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import average_precision_score, log_loss, roc_auc_score

from . import common

#: 基线平滑强度：小样本桩向先验收缩，避免"某台桩只被试过 3 次就断定它会坏"。
BASELINE_ALPHA = 30.0
NEUTRAL_PRIOR = 0.5
BASELINE_SPECS = (("charger", ("charger_id",), 0.40), ("charger_model", ("charger_model",), 0.30),
                  ("station", ("station_id",), 0.30))

#: 名字里含这些片段的数值列都是"截至本次尝试之前"的 as-of 统计量，归入同一组做消融。
#: （``_ticket`` 用单数：``charger_ticket_open_now`` / ``charger_days_since_last_ticket`` 不含复数形式，
#:  早先用 ``_tickets`` 会把这两列漏进静态组，消融口径就不诚实了。）
ASOF_TOKENS = ("_n_prior", "_fail_rate_prior", "_fails_30d", "_n_30d", "_fail_rate_7d",
               "_rate_delta_7d_minus_prior", "_ticket", "_downtime_hours", "_prev_attempt_failed",
               "_min_since_last_attempt", "_days_since_last_fail", "tel_")

#: 发布口径：精确率至少达到基础率的 ~5.6 倍；达不到就如实退回"最高精确率档"，不假装达标。
PRECISION_TARGET = 0.20
MIN_FLAGGED = 30


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
    if summary.get("matrixSha256") != common.sha256_file(common.MATRIX_PATH):
        raise common.BatchMismatch("特征矩阵哈希与 build_summary 记录不一致，矩阵被改过，请重跑 build_data")
    if frame["attempt_id"].duplicated().any():
        raise AssertionError("矩阵里 attempt_id 重复，一行一次尝试的前提不成立")
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


def split_asof(numeric: list[str]) -> tuple[list[str], list[str]]:
    """把数值特征切成 (as-of 历史组, 其余组)。按列名片段判定，不靠人工维护清单。"""
    asof = [n for n in numeric if any(token in n for token in ASOF_TOKENS)]
    other = [n for n in numeric if n not in set(asof)]
    assert asof and other, f"as-of 分组退化：{len(asof)} / {len(other)}"
    return asof, other


def candidate_sets(numeric: list[str], categorical: list[str]) -> dict[str, tuple[list[str], list[str]]]:
    """三套候选：全量 / 只用 as-of 历史 / 不用 as-of 历史（后者保留全部静态与时令列）。"""
    asof, other = split_asof(numeric)
    return {"full": (numeric, categorical),
            "asOfOnly": (asof, []),
            "staticOnly": (other, categorical)}


def new_classifier(categorical_mask: np.ndarray) -> HistGradientBoostingClassifier:
    return HistGradientBoostingClassifier(
        max_iter=400, learning_rate=0.06, max_leaf_nodes=31, min_samples_leaf=40,
        l2_regularization=1.0, early_stopping=True, validation_fraction=0.15,
        n_iter_no_change=30, random_state=common.SEED, categorical_features=categorical_mask)


def _smoothed_rate(keys: pd.DataFrame, y: pd.Series, alpha: float) -> pd.DataFrame:
    index = pd.MultiIndex.from_frame(keys)
    stats = pd.DataFrame({"y": y.to_numpy()}, index=index).groupby(level=list(range(keys.shape[1])))["y"]
    table = stats.agg(["sum", "size"]).reset_index()
    table["rate"] = (table["sum"] + alpha * NEUTRAL_PRIOR) / (table["size"] + alpha)
    return table


def fit_baselines(train: pd.DataFrame) -> dict[str, pd.DataFrame]:
    """TRAIN 上的三档查表基线。查不到的键在预测时回落全局率。"""
    y = train["y_tech"]
    return {"global": pd.DataFrame({"rate": [float(y.mean())]}),
            "charger": _smoothed_rate(train[["charger_id"]], y, BASELINE_ALPHA),
            "charger_model": _smoothed_rate(train[["charger_model"]], y, BASELINE_ALPHA),
            "station": _smoothed_rate(train[["station_id"]], y, BASELINE_ALPHA)}


def predict_baseline(baselines: dict[str, pd.DataFrame], frame: pd.DataFrame) -> np.ndarray:
    """三档平滑经验率按对数几率加权平均（谁有历史就多用谁），全缺则回落全局率。

    用 merge(validate="many_to_one") 而不是索引 join：右表键若真重复会直接报错，
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


def pick_threshold(prob: np.ndarray, y: np.ndarray) -> dict:
    """VALIDATION 上按「精确率≥PRECISION_TARGET 下召回最大」选阈值；达不到就如实退回最高档。

    退回时不改口径、只改标注：写成"无档位达精确率 0.20"，TEST 报告里原样带着这句话。
    """
    grid = sorted({round(float(value), 6)
                   for value in np.quantile(prob, np.linspace(0.50, 0.99, 50))})
    best = None
    fallback = None
    for threshold in grid:
        flagged = prob >= threshold
        if flagged.sum() < MIN_FLAGGED:
            continue
        precision = float(y[flagged].mean())
        recall = float(flagged[y == 1].sum() / max(1, int((y == 1).sum())))
        candidate = {"threshold": float(threshold), "precision": precision, "recall": recall,
                     "alertRate": float(flagged.mean())}
        if precision >= PRECISION_TARGET and (best is None or recall > best["recall"]):
            best = candidate
        if fallback is None or precision > fallback["precision"]:
            fallback = candidate
    if best is not None:
        return {**best, "rule": f"精确率≥{PRECISION_TARGET:.2f} 下召回最大", "targetMet": True}
    if fallback is None:
        raise RuntimeError("VALIDATION 上没有任何档位能凑出 30 条告警，运营口径需要重新定义")
    return {**fallback, "rule": f"无档位达精确率{PRECISION_TARGET:.2f}，退回精确率最高档（如实标注，不假装达标）",
            "targetMet": False}


def metrics_block(y: np.ndarray, prob: np.ndarray) -> dict:
    return {"auc": round(float(roc_auc_score(y, prob)), 4),
            "prAuc": round(float(average_precision_score(y, prob)), 4),
            "logLoss": round(float(log_loss(y, np.clip(prob, 1e-6, 1 - 1e-6))), 5),
            "brier": round(common.brier(y, prob), 5),
            "liftAt2pct": round(common.lift_at(y, prob, 0.02), 3),
            "liftAt5pct": round(common.lift_at(y, prob, 0.05), 3),
            "liftAt10pct": round(common.lift_at(y, prob, 0.10), 3)}


def main() -> dict:
    common.require_empty_run_dir(extra_allowed=(common.MATRIX_PATH.name,
                                                common.BUILD_SUMMARY_PATH.name))
    frame, summary = load_matrix()
    numeric = summary["features"]["numeric"]
    categorical = summary["features"]["categorical"]
    train = frame[frame["split"] == "TRAIN"].reset_index(drop=True)
    validation = frame[frame["split"] == "VALIDATION"].reset_index(drop=True)
    held_out = int((frame["split"] == "TEST").sum())
    y_train = train["y_tech"].to_numpy(dtype=int)
    y_valid = validation["y_tech"].to_numpy(dtype=int)
    print(f"[train] TRAIN={len(train):,} VALIDATION={len(validation):,} TEST(本次不碰)={held_out:,}")

    baselines = fit_baselines(train)
    baseline_valid = predict_baseline(baselines, validation)
    prior_valid = validation["charger_fail_rate_prior"].to_numpy(dtype=float)
    comparison = {
        "globalBaseRate": {"auc": 0.5, "prAuc": round(float(y_valid.mean()), 4),
                           "note": "常数打分，AUC 恒 0.5，只是尺子"},
        "trainLookupBlend": metrics_block(y_valid, baseline_valid),
        "asOfChargerPriorOnly": metrics_block(y_valid, prior_valid),
    }

    sets = candidate_sets(numeric, categorical)
    selection: dict[str, dict] = {}
    fitted: dict[str, tuple[HistGradientBoostingClassifier, np.ndarray]] = {}
    for label, (num, cat) in sets.items():
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
    operating = pick_threshold(prob_valid, y_valid)
    if chosen != "full":
        print(f"[train] 按 VAL AUC 选中的是 {chosen}（不是全量组），全量组仅作对照发布")

    metrics = {
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "datasetId": common.DATASET_ID, "modelId": common.MODEL_ID,
        "modelVersion": common.MODEL_VERSION, "seed": common.SEED,
        "matrixSha256": summary["matrixSha256"], "label": summary["label"],
        "scope": summary["scope"], "splits": summary["splits"],
        "splitBaseRate": summary["splitBaseRate"],
        "trainRows": int(len(train)), "validationRows": int(len(validation)),
        "testRowsUntouched": held_out,
        "selectionRule": "三套候选特征组按 VALIDATION ROC AUC 择一发布；TEST 完全不参与选择",
        "chosenSet": chosen, "candidateValidation": selection,
        "baselineValidation": comparison,
        "validation": {**selection[chosen], "baseRate": round(float(y_valid.mean()), 5),
                       "aucGainVsLookupPct": round(100.0 * (selection[chosen]["auc"]
                                                            / comparison["trainLookupBlend"]["auc"] - 1.0), 2),
                       "aucGainVsAsOfPriorPct": round(100.0 * (selection[chosen]["auc"]
                                                               / comparison["asOfChargerPriorOnly"]["auc"] - 1.0), 2)},
        "operatingPoint": operating,
        "numericFeatures": sets[chosen][0], "categoricalFeatures": sets[chosen][1],
        "allNumericFeatures": numeric, "allCategoricalFeatures": categorical,
        "disclaimer": common.simulated_note(),
    }

    bundle = {
        "classifier": classifier, "chosenSet": chosen,
        "featureNames": sets[chosen][0] + sets[chosen][1],
        "numericFeatures": sets[chosen][0], "categoricalFeatures": sets[chosen][1],
        "allNumericFeatures": numeric, "allCategoricalFeatures": categorical,
        "candidateSets": {key: {"numeric": value[0], "categorical": value[1]}
                          for key, value in sets.items()},
        "baselines": baselines, "operatingPoint": operating,
        "modelId": common.MODEL_ID, "modelVersion": common.MODEL_VERSION, "seed": common.SEED,
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "datasetId": common.DATASET_ID, "matrixSha256": summary["matrixSha256"],
        "splits": summary["boundaries"], "simulatedNote": common.simulated_note(),
        "label": summary["label"],
    }
    buffer = io.BytesIO()
    joblib.dump(bundle, buffer)
    common.write_new_bytes(common.BUNDLE_PATH, buffer.getvalue())
    metrics["bundlePath"] = str(common.BUNDLE_PATH.relative_to(common.DATA_ANALYSIS_ROOT.parent))
    metrics["bundleSha256"] = common.sha256_file(common.BUNDLE_PATH)
    common.write_new_json(common.TRAIN_METRICS_PATH, metrics)

    lookup = comparison["trainLookupBlend"]
    prior = comparison["asOfChargerPriorOnly"]
    print(f"[train] VAL AUC 模型({chosen})={selection[chosen]['auc']:.4f} "
          f"查表基线={lookup['auc']:.4f} 纯 as-of 桩先验={prior['auc']:.4f}")
    print(f"[train] VAL PR-AUC 模型={selection[chosen]['prAuc']:.4f} 基线={lookup['prAuc']:.4f} "
          f"先验={prior['prAuc']:.4f}；lift@5% 模型={selection[chosen]['liftAt5pct']:.2f}x "
          f"基线={lookup['liftAt5pct']:.2f}x")
    print(f"[train] 运营阈值={operating['threshold']:.4f} 精确率={operating['precision']:.4f} "
          f"召回={operating['recall']:.4f} 告警率={operating['alertRate']:.4f} "
          f"（{operating['rule']}）")
    print(f"[train] -> {common.BUNDLE_PATH}")
    return metrics


if __name__ == "__main__":
    main()
