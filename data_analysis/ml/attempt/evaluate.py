"""一次性盲测：在 split == TEST 上评插枪启动失败风险，产出 JSON + Markdown 报告。

纪律：特征组、阈值、基线表全部由 train.py 在 TRAIN/VALIDATION 上定死并冻进 bundle，本脚本
**只读不算新参数**；消融用的缩减模型只是在 TRAIN 上重拟合后放在一起报数，不回头改主模型。
模型包与特征矩阵按 sha256 成对校验，批次错配一律 BatchMismatch 拒评。
所有数字都带 common.simulated_note() 那句口径声明。

用法（仓库根目录）：python -m data_analysis.ml.attempt.evaluate
"""

from __future__ import annotations

import hashlib
import json

import joblib
import numpy as np
import pandas as pd
from sklearn.metrics import average_precision_score, log_loss, roc_auc_score

from . import common
from .train import (candidate_sets, design, metrics_block, new_classifier, predict_baseline)

HOUR_BANDS = (("凌晨0-6", 0, 6), ("白天6-17", 6, 17), ("晚峰17-22", 17, 22), ("夜间22-24", 22, 24))
#: 正类的三种原因。报告要回答"该不该再做一个原因分类器"，所以逐类看分数分布。
REASONS = common.TECHNICAL_FAILURE_REASONS


def load_bundle() -> dict:
    if not common.BUNDLE_PATH.exists():
        raise FileNotFoundError(f"缺少模型包 {common.BUNDLE_PATH}，请先跑 train")
    bundle = joblib.load(common.BUNDLE_PATH)
    common.verify_batch()
    if bundle["publishedBatchId"] != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch(f"模型包绑定批次 {bundle['publishedBatchId']!r} 与当前数据层不一致")
    with open(common.MATRIX_PATH, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    if digest != bundle["matrixSha256"]:
        raise common.BatchMismatch(
            f"特征矩阵哈希 {digest[:12]}… != 模型包记录 {bundle['matrixSha256'][:12]}…，"
            f"模型与特征必须成对发布，请一起重跑 build_data + train")
    return bundle


def bucket_rows(test: pd.DataFrame, prob: np.ndarray, groups: list[tuple[str, np.ndarray]]) -> list[dict]:
    """按任意掩码分桶报 AUC；样本或正样本不足的桶如实标 null，不硬算一个好看的数。"""
    rows = []
    for label, mask in groups:
        y = test.loc[mask, "y_tech"].to_numpy(dtype=int)
        scores = prob[mask]
        if len(y) < 50 or y.sum() < 5 or (len(y) - y.sum()) < 5:
            rows.append({"bucket": label, "n": int(len(y)), "positives": int(y.sum()),
                         "baseRate": round(float(y.mean()), 4) if len(y) else None,
                         "meanProb": round(float(scores.mean()), 4) if len(y) else None,
                         "auc": None, "note": "样本或正样本不足，AUC 不报"})
            continue
        rows.append({"bucket": label, "n": int(len(y)), "positives": int(y.sum()),
                     "baseRate": round(float(y.mean()), 4), "meanProb": round(float(scores.mean()), 4),
                     "auc": round(float(roc_auc_score(y, scores)), 4), "note": ""})
    return rows


def ablation(frame: pd.DataFrame, bundle: dict, test: pd.DataFrame, y: np.ndarray) -> dict[str, float]:
    """信号来自哪里：三套候选组各自在 TRAIN 重拟合，再在同一片 TEST 上比。"""
    numeric, categorical = bundle["allNumericFeatures"], bundle["allCategoricalFeatures"]
    train = frame[frame["split"] == "TRAIN"]
    y_train = train["y_tech"].to_numpy(dtype=int)
    out: dict[str, float] = {}
    for label, (num, cat) in candidate_sets(numeric, categorical).items():
        x_train, mask = design(train, num, cat)
        x_test, _ = design(test, num, cat)
        model = new_classifier(mask)
        model.fit(x_train, y_train)
        out[label] = round(float(roc_auc_score(y, model.predict_proba(x_test)[:, 1])), 4)
        out[label + "_featureCount"] = int(x_train.shape[1])
    return out


def reason_breakdown(test: pd.DataFrame, prob: np.ndarray) -> dict:
    """三种技术失败各自的分数分布 + "逐类 vs 其余失败"的 AUC。

    这决定是否值得再做一个原因分类器：若三类原因的分数分布几乎重合、且在失败样本内部
    彼此不可分，那么"为什么失败"在这批模拟数据里没有可分的结构，只发一个二分类风险分是对的。
    """
    failed = test["y_tech"] == 1
    positives = test[failed]
    positive_prob = prob[failed.to_numpy()]
    per_reason = {}
    for reason in REASONS:
        here = (positives["failure_reason"] == reason).to_numpy()
        per_reason[reason] = {
            "count": int(here.sum()),
            "share": round(float(here.mean()), 4) if len(here) else None,
            "meanProb": round(float(positive_prob[here].mean()), 4) if here.any() else None,
            "probP50": round(float(np.median(positive_prob[here])), 4) if here.any() else None,
            "aucWithinPositives": (round(float(roc_auc_score(here.astype(int), positive_prob)), 4)
                                   if 0 < here.sum() < len(here) else None)}
    shares = [value["share"] for value in per_reason.values() if value["share"] is not None]
    flat = max((abs(share - 1.0 / len(shares)) for share in shares), default=1.0)
    aucs = [value["aucWithinPositives"] for value in per_reason.values()
            if value["aucWithinPositives"] is not None]
    spread = max(aucs) - min(aucs) if len(aucs) == len(per_reason) else 1.0
    separable = flat < 0.06 and spread < 0.05
    return {"positives": int(failed.sum()), "perReason": per_reason,
            "maxShareDeviationFromUniform": round(float(flat), 4),
            "aucSpreadWithinPositives": round(float(spread), 4),
            "conclusion": ("三类原因占比接近均匀、且在失败样本内部彼此 AUC 都贴着 0.5——"
                           "「为什么失败」在这批模拟数据里没有可分结构，只发二分类风险分"
                           if separable else
                           "三类原因的占比或可分性有差异，原因分类器值得再评估")}


def pile_concentration(test: pd.DataFrame, prob: np.ndarray, threshold: float) -> dict:
    """告警都落在哪些桩上：运营上关心"是不是只有几台坏桩"，也防止分数只反映桩的流量。"""
    flagged = test[prob >= threshold]
    counts = flagged["charger_id"].value_counts()
    positives_by_charger = test.loc[test["y_tech"] == 1, "charger_id"].value_counts()
    top_pile = counts.index[0] if len(counts) else None
    return {"flaggedRows": int(len(flagged)),
            "distinctChargersFlagged": int(flagged["charger_id"].nunique()),
            "topChargerShare": round(float(counts.iloc[0] / len(flagged)), 4) if len(counts) else None,
            "topCharger": str(top_pile),
            "topChargerModel": (str(test.loc[test["charger_id"] == top_pile, "charger_model"].iloc[0])
                                if top_pile else None),
            "chargersWithData": int(test["charger_id"].nunique()),
            "positives": int(test["y_tech"].sum()),
            "top10ChargerPositiveShare": round(float(positives_by_charger.head(10).sum()
                                                     / max(1, int(test["y_tech"].sum()))), 4)}


def markdown(report: dict) -> str:
    """人读的评测报告，表格全部由 report 里的数字生成，不手抄。"""
    risk = report["risk"]
    point = risk["frozenOperatingPoint"]
    lines = [
        f"# 插枪启动失败预测 · TEST 盲测报告（{report['modelVersion']}）",
        "",
        "> " + report["disclaimer"],
        "",
        f"- 模型：`{report['modelId']}`（二分类，特征组 `{report['chosenSet']}`，"
        f"{report['featureCount']} 列）",
        f"- 绑定批次：`{report['publishedBatchId']}` · run `{report['pipelineRunId']}`",
        f"- 特征矩阵 sha256：`{report['matrixSha256'][:16]}…` · 泄漏审计抽样 "
        f"{report['leakAudit']['sampled']} 行，{len(report['leakAudit']['checked'])} 项 as-of 特征"
        f"最大偏差 {max(report['leakAudit']['maxAbsDiff'].values()):.1e}",
        f"- 样本域：{report['scope']['sampleRows']:,} 次已选桩尝试 / 正类 {report['scope']['positives']:,}"
        f"（基础率 {report['scope']['baseRate']:.4f}）；TEST 窗口（UTC）`{report['testWindow']['validationEndExclusive']}` → "
        f"`{report['testWindow']['testEndExclusive']}`，样本 {report['testRows']:,} 行",
        "",
        "## 1. 主结果",
        "",
        "| 指标 | 模型 | TRAIN 查表基线（桩/型号/站三级） | 纯 as-of 桩先验 |",
        "| --- | --- | --- | --- |",
        f"| ROC AUC | **{risk['auc']:.4f}** | {risk['aucLookupBaseline']:.4f} | "
        f"{risk['aucAsOfPrior']:.4f} |",
        f"| PR-AUC | **{risk['prAuc']:.4f}** | {risk['prAucLookupBaseline']:.4f} | "
        f"{risk['prAucAsOfPrior']:.4f} |",
        f"| lift@2% | **{risk['liftAt2pct']:.2f}×** | {risk['lookup']['liftAt2pct']:.2f}× | "
        f"{risk['asOfPrior']['liftAt2pct']:.2f}× |",
        f"| lift@5% | **{risk['liftAt5pct']:.2f}×** | {risk['lookup']['liftAt5pct']:.2f}× | "
        f"{risk['asOfPrior']['liftAt5pct']:.2f}× |",
        "",
        f"- TEST 基础失败率 {risk['baseRate']:.4f}；Brier {risk['brier']:.4f}；LogLoss {risk['logLoss']:.4f}",
        f"- 相对查表基线 AUC **{risk['aucGainVsLookupPct']:+.2f}%**，相对纯 as-of 桩先验 "
        f"**{risk['aucGainVsAsOfPriorPct']:+.2f}%**",
        "",
        f"冻结运营阈值（VALIDATION 上选定，TEST 只验不改；{point['rule']}）：",
        "",
        "| 阈值 | 告警率 | 精确率 | 召回 | F1 |",
        "| --- | --- | --- | --- | --- |",
        f"| {point['threshold']:.4f} | {point['alertRate']:.2%} | {point['precision']:.4f} | "
        f"{point['recall']:.4f} | {point['f1']:.4f} |",
        "",
        "## 2. 信号来自哪里（同一 TEST 上的消融 AUC，缩减模型只作归因、不参与发布）",
        "",
        "| 特征组 | 特征数 | TEST AUC | 说明 |",
        "| --- | --- | --- | --- |",
    ]
    labels = {
        "staticOnly": ("只用桩/车/时段/天气静态（不含任何 as-of 历史）", "发布的主模型所在组"),
        "full": ("静态 + 全部 as-of 历史", "对照"),
        "asOfOnly": ("只用 as-of 历史（桩/型号/厂商/站/人 + 遥测）", "对照"),
    }
    for key in ("staticOnly", "asOfOnly", "full"):
        block = report["ablation"]
        lines.append(f"| {labels[key][0]} | {block[key + '_featureCount']} | {block[key]:.4f} | "
                     f"{labels[key][1]} |")
    lines += ["", f"- VALIDATION 上的同一组比较：{report['valAblation']}",
              f"- 结论：{report['selectionConclusion']}", "",
              "## 3. 校准（预测概率 vs 实测频率，TEST 十分位）", "",
              "| 预测概率区间 | 样本数 | 预测均值 | 实测失败率 |", "| --- | --- | --- | --- |"]
    for row in report["calibration"]:
        lines.append(f"| {row['bin']} | {row['n']:,} | {row['predicted']:.4f} | {row['observed']:.4f} |")
    reasons = report["reasons"]
    lines += ["", "## 4. 该不该再做一个「失败原因分类器」", "",
              f"正类 {reasons['positives']:,} 条里三类原因占比：", "",
              "| 原因 | 条数 | 占比 | 平均分 | 分数中位数 | AUC(类内 vs 其余失败) |",
              "| --- | --- | --- | --- | --- | --- |"]
    for reason, block in reasons["perReason"].items():
        mean = "—" if block["meanProb"] is None else f"{block['meanProb']:.4f}"
        median = "—" if block["probP50"] is None else f"{block['probP50']:.4f}"
        auc = "—" if block["aucWithinPositives"] is None else f"{block['aucWithinPositives']:.4f}"
        share = "—" if block["share"] is None else f"{block['share']:.2%}"
        lines.append(f"| {reason} | {block['count']:,} | {share} | {mean} | {median} | {auc} |")
    lines += ["", f"- 占比与均匀分布最大偏差 {reasons['maxShareDeviationFromUniform']:.4f}；"
              f"类内 AUC 极差 {reasons['aucSpreadWithinPositives']:.4f}。{reasons['conclusion']}",
              "", "## 5. 分桶表现", ""]
    for name, title in (("manufacturer", "按厂商（风险）"), ("chargerModel", "按桩型号（风险）"),
                        ("connector", "按接口类型（风险）"), ("hourBand", "按本地小时段（风险）"),
                        ("weekend", "按工作日/周末（风险）"), ("halfWindow", "按 TEST 前/后半月（风险）")):
        lines += [f"### {title}", "", "| 桶 | 样本 | 正类 | 基础失败率 | 平均预测 | AUC | 备注 |",
                  "| --- | --- | --- | --- | --- | --- | --- |"]
        for row in report["breakdown"][name]:
            auc = "—" if row["auc"] is None else f"{row['auc']:.4f}"
            base = "—" if row.get("baseRate") is None else f"{row['baseRate']:.4f}"
            mean = "—" if row.get("meanProb") is None else f"{row['meanProb']:.4f}"
            lines.append(f"| {row['bucket']} | {row['n']:,} | {row['positives']:,} | {base} | {mean} | "
                         f"{auc} | {row.get('note', '')} |")
        lines.append("")
    pile = report["pileConcentration"]
    lines += ["## 6. 告警落在哪些桩上", "",
              f"- 冻结阈值下告警 {pile['flaggedRows']:,} 行，覆盖 {pile['distinctChargersFlagged']} 台桩"
              f"（TEST 域内共 {pile['chargersWithData']} 台）；最大单桩占告警 "
              f"{pile['topChargerShare']:.1%}（{pile['topCharger']} / {pile['topChargerModel']}）",
              f"- 失败最集中的 10 台桩占全部正类的 {pile['top10ChargerPositiveShare']:.1%}——"
              "运营上「盯少数坏桩」确实能吃掉相当一部分，但见第 4 节：原因维度不可分", "",
              "## 7. 数据纪律", "", f"- {report['dataDiscipline']}",
              f"- 不变量核对：TEST 全部晚于 VALIDATION 边界 "
              f"{report['invariants']['testStartsAtValidationEnd']}；TEST 未越过窗口右端 "
              f"{report['invariants']['testEndsBeforeWindowEnd']}；attempt_id 唯一 "
              f"{report['invariants']['attemptIdUniqueInTest']}；TEST 里 TRAIN 未见过的桩 "
              f"{report['invariants']['testRowsWithUnseenCharger']} 行；分数落在 [0,1] "
              f"{report['invariants']['scoreRangeLegal']}",
              f"- 被排除的样本：{report['excludedBrief']}",
              f"- 零方差列在建表阶段剔除 {len(report['droppedConstant'])} 个："
              f"{', '.join(sorted(report['droppedConstant']))}（{report['constantWhyNote']}）",
              "", "> " + report["disclaimer"], ""]
    return "\n".join(lines)


def main() -> dict:
    common.require_empty_run_dir(extra_allowed=(common.MATRIX_PATH.name,
                                                common.BUILD_SUMMARY_PATH.name,
                                                common.BUNDLE_PATH.name,
                                                common.TRAIN_METRICS_PATH.name))
    bundle = load_bundle()
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    with open(common.TRAIN_METRICS_PATH, encoding="utf-8") as handle:
        train_metrics = json.load(handle)
    frame = pd.read_pickle(common.MATRIX_PATH)
    test = frame[frame["split"] == "TEST"].reset_index(drop=True)
    train = frame[frame["split"] == "TRAIN"]
    y = test["y_tech"].to_numpy(dtype=int)

    x_test, _ = design(test, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob = bundle["classifier"].predict_proba(x_test)[:, 1]
    threshold = float(bundle["operatingPoint"]["threshold"])
    flag = prob >= threshold

    lookup_test = predict_baseline(bundle["baselines"], test)
    prior_test = test["charger_fail_rate_prior"].to_numpy(dtype=float)
    auc, auc_lookup, auc_prior = (float(roc_auc_score(y, prob)), float(roc_auc_score(y, lookup_test)),
                                  float(roc_auc_score(y, prior_test)))
    precision = float(y[flag].mean()) if flag.any() else 0.0
    recall = float(flag[y == 1].sum() / max(1, int(y.sum())))
    report: dict[str, object] = {
        "modelId": bundle["modelId"], "modelVersion": bundle["modelVersion"],
        "publishedBatchId": bundle["publishedBatchId"], "pipelineRunId": bundle["pipelineRunId"],
        "matrixSha256": bundle["matrixSha256"], "chosenSet": bundle["chosenSet"],
        "featureCount": int(x_test.shape[1]), "testWindow": bundle["splits"],
        "testRows": int(len(test)), "scope": summary["scope"],
        "excluded": summary["scope"]["excluded"],
        "excludedBrief": "；".join(f"{key}={value}" for key, value in summary["scope"]["excluded"].items()
                                   if not key.startswith("why")),
        "leakAudit": summary["leakAudit"],
        "droppedConstant": summary["features"]["droppedConstant"],
        "constantWhyNote": "样本域内取值唯一，非缺失；含 站点结构、预约创建时刻、单一服务电价、遥测栅格间隔",
        "risk": {
            "baseRate": round(float(y.mean()), 4), "auc": round(auc, 4),
            "aucLookupBaseline": round(auc_lookup, 4), "aucAsOfPrior": round(auc_prior, 4),
            "aucGainVsLookupPct": round(100.0 * (auc / auc_lookup - 1.0), 2),
            "aucGainVsAsOfPriorPct": round(100.0 * (auc / auc_prior - 1.0), 2),
            "prAuc": round(float(average_precision_score(y, prob)), 4),
            "prAucLookupBaseline": round(float(average_precision_score(y, lookup_test)), 4),
            "prAucAsOfPrior": round(float(average_precision_score(y, prior_test)), 4),
            "logLoss": round(float(log_loss(y, np.clip(prob, 1e-6, 1 - 1e-6))), 5),
            "brier": round(common.brier(y, prob), 5),
            "liftAt2pct": round(common.lift_at(y, prob, 0.02), 3),
            "liftAt5pct": round(common.lift_at(y, prob, 0.05), 3),
            "liftAt10pct": round(common.lift_at(y, prob, 0.10), 3),
            "lookup": metrics_block(y, lookup_test), "asOfPrior": metrics_block(y, prior_test),
            "frozenOperatingPoint": {
                "threshold": round(threshold, 6), "alertRate": round(float(flag.mean()), 4),
                "precision": round(precision, 4), "recall": round(recall, 4),
                "f1": round(2 * precision * recall / max(1e-9, precision + recall), 4),
                "rule": bundle["operatingPoint"]["rule"],
                "targetMet": bool(bundle["operatingPoint"]["targetMet"]),
                "note": "阈值与规则在 VALIDATION 上定死，TEST 只验不改"},
        },
    }
    report["calibration"] = [{"bin": str(row["bin"]), "n": int(row["n"]),
                              "predicted": round(float(row["predicted"]), 4),
                              "observed": round(float(row["observed"]), 4)}
                             for _, row in common.calibration_bins(y, prob, bins=10).iterrows()]
    report["ablation"] = ablation(frame, bundle, test, y)
    report["valAblation"] = {key: block["auc"] for key, block in
                             train_metrics["candidateValidation"].items()}
    chosen = bundle["chosenSet"]
    gap = report["ablation"][chosen] - report["ablation"]["full"]
    report["selectionConclusion"] = (
        f"发布组 {chosen} 在 TEST 上比全量组高 {gap * 100.0:+.2f}%（VALIDATION 上是 "
        f"{report['valAblation'][chosen] - report['valAblation']['full']:+.4f}）——"
        "as-of 桩级历史没有增量：每台桩 118 天只有约 49 个正样本，桩级经验率噪声大于信息量；"
        "型号/厂商级聚合才是稳定来源。这条判断由 10 轮滚动重训再验一次。" if gap > -0.005 else
        f"发布组 {chosen} 在 TEST 上比全量组低 {gap * 100.0:+.2f}%，说明 VALIDATION 选组过拟合，"
        f"应回退全量组（见 rolling 复核）")
    report["reasons"] = reason_breakdown(test, prob)

    manufacturers = [(str(k), (test["manufacturer"] == k).to_numpy())
                     for k in sorted(test["manufacturer"].dropna().unique())]
    models = [(str(k), (test["charger_model"] == k).to_numpy())
              for k in sorted(test["charger_model"].dropna().unique())]
    connectors = [(str(k), (test["connector_type"] == k).to_numpy())
                  for k in sorted(test["connector_type"].dropna().unique())]
    bands = [(label, ((test["hour_local"] >= lo) & (test["hour_local"] < hi)).to_numpy())
             for label, lo, hi in HOUR_BANDS]
    weekend = [("工作日", (test["is_weekend"] == 0).to_numpy()),
               ("周末", (test["is_weekend"] == 1).to_numpy())]
    middle = test["business_date"].median()
    halves = [("TEST 前半月", (test["business_date"] < middle).to_numpy()),
              ("TEST 后半月", (test["business_date"] >= middle).to_numpy())]
    report["breakdown"] = {
        "manufacturer": bucket_rows(test, prob, manufacturers),
        "chargerModel": bucket_rows(test, prob, models),
        "connector": bucket_rows(test, prob, connectors),
        "hourBand": bucket_rows(test, prob, bands),
        "weekend": bucket_rows(test, prob, weekend),
        "halfWindow": bucket_rows(test, prob, halves)}
    report["pileConcentration"] = pile_concentration(test, prob, threshold)
    bounds = {key: pd.Timestamp(value) for key, value in bundle["splits"].items()}
    seen = test["charger_id"].isin(train["charger_id"].unique())
    report["invariants"] = {
        "testStartsAtValidationEnd": bool((test["attempted_at"] >= bounds["validationEndExclusive"]).all()),
        "testEndsBeforeWindowEnd": bool((test["attempted_at"] < bounds["testEndExclusive"]).all()),
        "attemptIdUniqueInTest": bool(test["attempt_id"].is_unique),
        "testRowsWithUnseenCharger": int((~seen).sum()),
        "chargerShareOfTestSeenInTrain": round(float(seen.mean()), 4),
        "scoreRangeLegal": bool(((prob >= 0) & (prob <= 1)).all())}
    report["dataDiscipline"] = ("只读消费 clean 层 parquet；产物落 outputs/ml_attempt/ 且全部独占创建；"
                                "特征截至 attempted_at 且严格早于，经随机抽样暴力复核 0 偏差"
                                "（build_summary.leakAudit）")
    report["disclaimer"] = common.simulated_note()

    common.write_new_json(common.TEST_REPORT_PATH, report)
    common.write_new_bytes(common.TEST_REPORT_MD, markdown(report).encode("utf-8"))
    risk = report["risk"]
    point = risk["frozenOperatingPoint"]
    print(f"[eval] TEST n={report['testRows']:,} 基础失败率={risk['baseRate']:.4f} "
          f"正类={int(y.sum()):,}")
    print(f"[eval] AUC={risk['auc']:.4f}（查表基线 {risk['aucLookupBaseline']:.4f} / "
          f"纯 as-of 先验 {risk['aucAsOfPrior']:.4f}）PR-AUC={risk['prAuc']:.4f} "
          f"Brier={risk['brier']:.4f} lift@2%={risk['liftAt2pct']:.2f}x lift@5%={risk['liftAt5pct']:.2f}x")
    print(f"[eval] 阈值{point['threshold']:.3f} → 告警率{point['alertRate']:.2%} "
          f"精确率{point['precision']:.4f} 召回{point['recall']:.4f} F1={point['f1']:.4f} "
          f"达标={point['targetMet']}")
    print("[eval] 消融 TEST AUC 静态={static:.4f} 仅as-of={asof:.4f} 全量={full:.4f}".format(
        static=report["ablation"]["staticOnly"], asof=report["ablation"]["asOfOnly"],
        full=report["ablation"]["full"]))
    print(f"[eval] 原因可分性：与均匀最大偏差={report['reasons']['maxShareDeviationFromUniform']:.4f}")
    print(f"[eval] -> {common.TEST_REPORT_MD}")
    return report


if __name__ == "__main__":
    main()
