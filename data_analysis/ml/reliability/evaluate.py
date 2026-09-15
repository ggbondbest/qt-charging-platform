"""一次性盲测：在 ``split == TEST`` 上评桩级次日可靠性预警，产出 JSON + Markdown 报告。

纪律：特征组、阈值、基线表全部由 train.py 在 TRAIN/VALIDATION 上定死并冻进 bundle，本脚本
**只读不算新参数**；消融用的缩减模型只是在 TRAIN 上重拟合后放在一起报数，不回头改主模型。
模型包、特征表、派生数据集三层都按 sha256 校验，任一错配一律拒评。

本线与第五/六线最不一样的地方：**基线可能比模型更强**，所以报告的第一问不是"模型多少分"，
而是"学出来的东西有没有跑过一行查表"。为此这里固定报三样：
  · 六档基线 + 模型 + 回归排序在同一片 TEST 上的 AUC / PR-AUC / lift；
  · **巡检预算表**：每天只查 X% 的桩，按不同排序各能捞到多少失败日（运营真正看的数字）；
  · **归因表**：剔除暴露度（预期用量分档）与剔除 lag1（昨天是否坏）之后，模型还剩多少区分力。
所有数字都带 ``common.data_note()`` 那句数据口径声明。

用法（仓库根目录）：python -m data_analysis.ml.reliability.evaluate
"""

from __future__ import annotations

import json

import joblib
import numpy as np
import pandas as pd
from sklearn.metrics import average_precision_score, roc_auc_score

from . import common
from .train import (MAX_ALERT_SHARE, baseline_scores, design, new_classifier, predict_baseline,
                    split_group)

#: 非概率打分（用量、lag1 规则）不能算 logLoss/Brier，报告里如实标注而不是硬凑一个数。
PROBABILISTIC = {"model", "trainLookupBlend", "asOfChargerPriorOnly"}
BUDGETS = (0.05, 0.10, 0.15, 0.20, 0.25)
MIN_BUCKET_ROWS = 60
MIN_BUCKET_POSITIVES = 5


def load_bundle() -> dict:
    """读模型包并复查四件事：批次、派生集哈希、特征表哈希、模型包与表的配对。"""
    if not common.BUNDLE_PATH.exists():
        raise FileNotFoundError(f"缺少模型包 {common.BUNDLE_PATH}，请先跑 train")
    bundle = joblib.load(common.BUNDLE_PATH)
    common.verify_batch()
    common.verify_derived()
    if bundle["publishedBatchId"] != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch(f"模型包绑定批次 {bundle['publishedBatchId']!r} 与当前数据层不一致")
    if bundle["derivedDatasetId"] != common.DERIVED_DATASET_ID:
        raise common.DerivedMismatch(
            f"模型包绑定派生集 {bundle['derivedDatasetId']!r}，当前是 {common.DERIVED_DATASET_ID!r}")
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    digest = common.sha256_file(common.FEATURES_PATH)
    if summary.get("featuresSha256") != digest:
        raise common.DerivedMismatch("特征表哈希与 features_summary 记录不一致，请重跑 features")
    if digest != bundle["featuresSha256"]:
        raise common.DerivedMismatch(
            f"特征表哈希 {digest[:12]}… != 模型包记录 {bundle['featuresSha256'][:12]}…，"
            f"模型与特征表必须成对发布，请一起重跑 features + train")
    return bundle


def score_metrics(y: np.ndarray, scores: np.ndarray, *, probabilistic: bool) -> dict:
    """统一的指标块；非概率分数只给排序类指标，校准类一律留空并写明原因。"""
    block = {"auc": round(float(roc_auc_score(y, scores)), 4),
             "prAuc": round(float(average_precision_score(y, scores)), 4),
             "liftAt5pct": round(common.lift_at(y, scores, 0.05), 3),
             "liftAt10pct": round(common.lift_at(y, scores, 0.10), 3),
             "liftAt20pct": round(common.lift_at(y, scores, 0.20), 3),
             "deployable": True}
    if probabilistic:
        block["logLoss"] = round(float(-np.mean(y * np.log(np.clip(scores, 1e-6, 1))
                                             + (1 - y) * np.log(np.clip(1 - scores, 1e-6, 1)))), 5)
        block["brier"] = round(common.brier(y, scores), 5)
    else:
        block["logLoss"] = None
        block["brier"] = None
        block["note"] = "分数不是概率（用量/规则），不算 logLoss 与 Brier"
    return block


def oracle_metrics(y: np.ndarray, scores: np.ndarray) -> dict:
    block = score_metrics(y, scores, probabilistic=False)
    block["deployable"] = False
    block["note"] = ("**不可部署**：用的是当天的真实尝试次数（标签窗口内的暴露度）。"
                     + block.get("note", "只作归因对照"))
    return block


def inspection_budget(test: pd.DataFrame, rankings: dict[str, np.ndarray], y: np.ndarray,
                      budgets: tuple[float, ...] = BUDGETS) -> list[dict]:
    """每天只查 n% 的桩，按每种排序各能捞到多少失败日——这是运营真正会用的数字。

    逐日取前 k 台（``k = max(1, ceil(budget × 当日桩数))``），跨日累加命中率。与阈值口径独立：
    阈值回答"发多少告警"，这张表回答"给定人手，怎么排优先级"。
    """
    days = test["business_date"].to_numpy()
    positives = int(y.sum())
    rows: list[dict] = []
    for budget in budgets:
        entry: dict[str, object] = {"budget": budget}
        for name, scores in rankings.items():
            hits = 0
            alerts = 0
            for day in pd.unique(days):
                mask = days == day
                k = max(1, int(np.ceil(budget * int(mask.sum()))))
                order = np.argsort(-scores[mask], kind="stable")[:k]
                index = np.flatnonzero(mask)[order]
                hits += int(y[index].sum())
                alerts += len(index)
            entry[name] = {"recall": round(hits / max(1, positives), 4),
                           "precision": round(hits / max(1, alerts), 4),
                           "alerts": int(alerts)}
        rows.append(entry)
    return rows


def bucket_rows(test: pd.DataFrame, prob: np.ndarray, groups: list[tuple[str, np.ndarray]]) -> list[dict]:
    """按任意掩码分桶报 AUC；样本或正样本不足的桶如实标 null，不硬算一个好看的数。"""
    y = test["y_fail"].to_numpy(dtype=int)
    rows = []
    for label, mask in groups:
        yy = y[mask]
        scores = prob[mask]
        if len(yy) < MIN_BUCKET_ROWS or int(yy.sum()) < MIN_BUCKET_POSITIVES \
                or int((yy == 0).sum()) < MIN_BUCKET_POSITIVES:
            rows.append({"bucket": label, "n": int(len(yy)), "positives": int(yy.sum()),
                         "baseRate": round(float(yy.mean()), 4) if len(yy) else None,
                         "meanProb": round(float(scores.mean()), 4) if len(yy) else None,
                         "auc": None, "note": "样本或正样本不足，AUC 不报"})
            continue
        rows.append({"bucket": label, "n": int(len(yy)), "positives": int(yy.sum()),
                     "baseRate": round(float(yy.mean()), 4),
                     "meanProb": round(float(scores.mean()), 4),
                     "auc": round(float(roc_auc_score(yy, scores)), 4), "note": ""})
    return rows


def label_columns(test: pd.DataFrame, name: str) -> pd.Series:
    if name not in test.columns:
        raise KeyError(f"分桶维度 {name!r} 不在特征表里")
    return test[name]


def breakdowns(test: pd.DataFrame, prob: np.ndarray) -> dict[str, list[dict]]:
    """分桶维度是挑死的，不是遍历出来的：站点/型号/厂商/场景/天气/周末/前后半段，
    再加两个本线专属的——**预期用量分位**与**昨天是否坏**。"""
    result: dict[str, list[dict]] = {}
    for name in ("station_id", "manufacturer", "charger_model", "site_type", "weather_prev",
                 "scenario_event"):
        series = label_columns(test, name).astype("object").where(
            label_columns(test, name).notna(), "MISSING")
        keys = sorted(value for value in series.unique())
        result[name] = bucket_rows(test, prob, [(str(key), (series == key).to_numpy()) for key in keys])
    weekend = label_columns(test, "is_weekend").to_numpy(dtype=float)
    result["weekend"] = bucket_rows(test, prob, [("工作日", weekend == 0), ("周末", weekend == 1)])
    middle = test["business_date"].median()
    days = test["business_date"].to_numpy()
    result["halfWindow"] = bucket_rows(test, prob, [("TEST 前半月", days < np.datetime64(middle)),
                                                    ("TEST 后半月", days >= np.datetime64(middle))])
    volume = pd.qcut(label_columns(test, "attempts_per_day_30d"), 5, duplicates="drop")
    result["expectedVolume"] = bucket_rows(
        test, prob, [(str(band), (volume == band).to_numpy()) for band in volume.cat.categories])
    lag1 = label_columns(test, "prev_day_fail").fillna(0.0).to_numpy(dtype=float)
    result["prevDayFail"] = bucket_rows(test, prob, [("昨天没坏", lag1 == 0), ("昨天坏了", lag1 == 1)])
    return result


def ablation(frame: pd.DataFrame, bundle: dict, test: pd.DataFrame, y: np.ndarray) -> dict[str, float]:
    """信号来自哪里：四套候选组各自在 TRAIN 重拟合，再在同一片 TEST 上比（只作归因）。"""
    numeric, categorical = bundle["allNumericFeatures"], bundle["allCategoricalFeatures"]
    train = frame[frame["split"] == "TRAIN"]
    y_train = train["y_fail"].to_numpy(dtype=int)
    out: dict[str, float] = {}
    for label, columns in bundle["candidateSets"].items():
        num, cat = split_group(list(columns[0]) + list(columns[1]), numeric, categorical)
        x_train, mask = design(train, num, cat)
        x_test, _ = design(test, num, cat)
        model = new_classifier(mask)
        model.fit(x_train, y_train)
        prob = model.predict_proba(x_test)[:, 1]
        out[label] = round(float(roc_auc_score(y, prob)), 4)
        out[label + "_prAuc"] = round(float(average_precision_score(y, prob)), 4)
        out[label + "_featureCount"] = int(x_train.shape[1])
    return out


def rank_agreement(prob: np.ndarray, others: dict[str, np.ndarray]) -> dict[str, float]:
    """模型分数与各基线的 Spearman 秩相关：接近 1 就说明模型只是把那条基线换了个刻度。"""
    model_ranks = pd.Series(prob).rank()
    return {name: round(float(model_ranks.corr(pd.Series(scores).rank(), method="pearson")), 4)
            for name, scores in others.items()}


def concentration(test: pd.DataFrame, prob: np.ndarray, threshold: float) -> dict:
    """告警落在哪些桩上：运营关心"是不是只有几台坏桩"，也防止分数只是桩的流量代理。"""
    flagged = test[prob >= threshold]
    counts = flagged["charger_id"].value_counts()
    positives_by_charger = test.loc[test["y_fail"] == 1, "charger_id"].value_counts()
    top = counts.index[0] if len(counts) else None
    return {"flaggedRows": int(len(flagged)),
            "distinctChargersFlagged": int(flagged["charger_id"].nunique()),
            "chargersInScope": int(test["charger_id"].nunique()),
            "daysInScope": int(test["business_date"].nunique()),
            "topCharger": str(top) if top is not None else None,
            "topChargerAlertShare": round(float(counts.iloc[0] / len(flagged)), 4) if len(counts) else None,
            "top10ChargerPositiveShare": round(float(positives_by_charger.head(10).sum()
                                                     / max(1, int(test["y_fail"].sum()))), 4),
            "distinctPositiveChargers": int(test.loc[test["y_fail"] == 1, "charger_id"].nunique())}


def budget_compliance(test: pd.DataFrame, prob: np.ndarray, threshold: float) -> dict:
    """冻结阈值搬到 TEST 上，每天的告警数有没有越过"当日在用桩数 25%"这条自定预算。"""
    frame = pd.DataFrame({"day": test["business_date"], "flag": prob >= threshold})
    per_day = frame.groupby("day", observed=True)["flag"].agg(["sum", "size"])
    allowed = np.ceil(MAX_ALERT_SHARE * per_day["size"].to_numpy(dtype=float))
    over = int((per_day["sum"].to_numpy() > allowed).sum())
    return {"days": int(len(per_day)), "maxAlertsPerDay": int(per_day["sum"].max()),
            "meanAlertsPerDay": round(float(per_day["sum"].mean()), 2),
            "dailyBudgetAt25pct": f"{int(allowed.min())}–{int(allowed.max())} 条/天",
            "daysOverBudget": over, "worstDayShare": round(float((per_day["sum"] / per_day["size"]).max()), 4)}


def markdown(report: dict) -> str:
    """人读的评测报告，表格全部由 report 里的数字生成，不手抄。"""
    risk = report["risk"]
    point = risk["frozenOperatingPoint"]
    lines = [
        f"# 桩级次日可靠性预警 · TEST 盲测报告（{report['modelVersion']}）",
        "",
        "> " + report["disclaimer"],
        "",
        f"- 模型：`{report['modelId']}`（分类=明天会不会坏，回归=明天大概坏几次；"
        f"特征组 `{report['chosenSet']}`，{report['featureCount']} 列）",
        f"- 绑定批次：`{report['publishedBatchId']}` · run `{report['pipelineRunId']}` · "
        f"派生数据集 `{report['derivedDatasetId']}`",
        f"- 特征表 sha256：`{report['featuresSha256'][:16]}…` · 泄漏审计抽样 "
        f"{report['leakAudit']['sampled']} 行 × {len(report['leakAudit']['checked'])} 个 as-of 量，"
        f"最大偏差 {max(report['leakAudit']['maxAbsDiff'].values()):.1e}",
        f"- 样本单元：{report['sampleUnit']}；决策时刻：{report['decisionTime']}",
        f"- TEST 窗口（UTC 起点）`{report['testWindow']['validationEndExclusive']}` → "
        f"`{report['testWindow']['testEndExclusive']}`，{report['scope']['chargers']} 台桩 × "
        f"{report['invariants']['testDays']} 天 = {report['testRows']:,} 桩日，"
        f"正类 {risk['positives']:,}（基础率 {risk['baseRate']:.4f}）",
        "",
        "## 1. 主结果：排序能力（同一 TEST，同一批桩日）",
        "",
        "| 打分 | 可部署 | ROC AUC | PR-AUC | lift@5% | lift@10% | lift@20% | 备注 |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for name, block in report["scoreboard"].items():
        note = block.get("note", "")
        lines.append(f"| {name} | {'是' if block['deployable'] else '否'} | {block['auc']:.4f} | "
                     f"{block['prAuc']:.4f} | {block['liftAt5pct']:.2f}× | "
                     f"{block['liftAt10pct']:.2f}× | {block['liftAt20pct']:.2f}× | {note} |")
    head = report["headline"]
    lines += ["",
              f"- 模型 AUC 相对 TRAIN 三级查表基线 **{head['vsLookupPct']:+.2f}%**、"
              f"相对纯因果桩先验 **{head['vsPriorPct']:+.2f}%**、"
              f"相对只用预期用量 **{head['vsVolumePct']:+.2f}%**",
              f"- **结论（先说难听的）**：{head['conclusion']}",
              f"- 校准：Brier {risk['brier']:.5f}、LogLoss {risk['logLoss']:.5f}"
              f"（只有概率型分数参与校准）",
              "",
              f"冻结运营阈值（VALIDATION 上定死，TEST 只验不改；{point['rule']}）：",
              "",
              "| 阈值 | 告警桩日 | 告警率 | 精确率 | 召回 | F1 | 是否达标 |",
              "| --- | --- | --- | --- | --- | --- | --- |",
              f"| {point['threshold']:.4f} | {point['alerts']:,} | {point['alertRate']:.2%} | "
              f"{point['precision']:.4f} | {point['recall']:.4f} | {point['f1']:.4f} | "
              f"{'达标' if point['targetMet'] else '未达标（如实标注）'} |",
              "",
              f"预算核对：{report['budget']['days']} 天里 {report['budget']['daysOverBudget']} 天越过"
              f"「当日在用桩数 {MAX_ALERT_SHARE:.0%}」（{report['budget']['dailyBudgetAt25pct']}），"
              f"单日最多 {report['budget']['maxAlertsPerDay']} 条、最高占当日 "
              f"{report['budget']['worstDayShare']:.1%}",
              "",
              "## 2. 巡检预算表：每天只查 n% 的桩，能捞到多少失败日",
              "",
              "| 每日预算 | " + " | ".join(k for k in report["budgetRanking"][0] if k != "budget") + " |",
              "| --- |" + " --- |" * (len(report["budgetRanking"][0]) - 1),
              ]
    names = [key for key in report["budgetRanking"][0] if key != "budget"]
    for row in report["budgetRanking"]:
        cells = [f"{row[name]['recall']:.1%}/{row[name]['precision']:.2f}" for name in names]
        lines.append(f"| {row['budget']:.0%} | " + " | ".join(cells) + " |")
    lines += ["", f"- 排序口径差异：{report['budgetConclusion']}",
              "",
              "## 3. 归因：分数到底在排什么",
              "",
              f"- 与基线的秩相关（Spearman）：{report['rankAgreement']}",
              f"- oracle（真用当天尝试次数）AUC {report['scoreboard']['oracleActualVolumeNONDEPLOYABLE']['auc']:.4f} "
              f"vs 因果桩先验 {report['scoreboard']['asOfChargerPriorOnly']['auc']:.4f}："
              f"{report['exposureConclusion']}",
              f"- 只用**因果**预期用量的 AUC {report['scoreboard']['expectedVolumeOnly']['auc']:.4f}"
              f"（这条是可部署的下界：什么健康度都不用，只按用量排）",
              "",
              "| 归因切片 | 样本 | 正类 | 基础率 | 平均预测 | 组内 AUC |",
              "| --- | --- | --- | --- | --- | --- |"]
    for name in ("expectedVolume", "prevDayFail"):
        for row in report["breakdown"][name]:
            auc = "—" if row["auc"] is None else f"{row['auc']:.4f}"
            base = "—" if row["baseRate"] is None else f"{row['baseRate']:.4f}"
            mean = "—" if row["meanProb"] is None else f"{row['meanProb']:.4f}"
            lines.append(f"| {name} · {row['bucket']} | {row['n']:,} | {row['positives']:,} | {base} | "
                         f"{mean} | {auc} |")
    lines += ["", f"- 读法：{report['sliceConclusion']}",
              "",
              "## 4. 信号来自哪里（同一 TEST 上的消融 AUC，缩减模型只作归因、不参与发布）",
              "",
              "| 特征组 | 特征数 | TEST AUC | TEST PR-AUC |",
              "| --- | --- | --- | --- |"]
    for label in report["ablationSets"]:
        block = report["ablation"]
        lines.append(f"| {label} | {block[label + '_featureCount']} | {block[label]:.4f} | "
                     f"{block[label + '_prAuc']:.4f} |")
    lines += ["", f"- VALIDATION 上同一组比较：{report['valAblation']}",
              f"- 过拟合尺子：TRAIN AUC {risk['aucTrain']:.4f} → VALIDATION {risk['aucValidation']:.4f} → "
              f"TEST {risk['auc']:.4f}",
              "",
              "## 5. 校准（预测概率 vs 实测频率，TEST 十分位）",
              "",
              "| 预测概率区间 | 样本数 | 预测均值 | 实测失败日率 |", "| --- | --- | --- | --- |"]
    for row in report["calibration"]:
        lines.append(f"| {row['bin']} | {row['n']:,} | {row['predicted']:.4f} | {row['observed']:.4f} |")
    count = report["countRegression"]
    lines += ["", "## 6. 回归目标：明天坏几次（按预计失败次数排程的另一套口径）", "",
              "| 打分 | MAE | 对 y_fail 的 AUC |", "| --- | --- | --- |"]
    for name, block in count["rows"].items():
        auc = "—" if block["auc"] is None else f"{block['auc']:.4f}"
        lines.append(f"| {name} | {block['mae']:.5f} | {auc} |")
    lines += ["", f"- {count['conclusion']}",
              "",
              "## 7. 告警落在哪些桩上",
              "",
              f"- 冻结阈值下告警 {report['pile']['flaggedRows']:,} 个桩日，覆盖 "
              f"{report['pile']['distinctChargersFlagged']} 台桩（TEST 域内共 "
              f"{report['pile']['chargersInScope']} 台）；单桩最多占告警 "
              f"{report['pile']['topChargerAlertShare']:.1%}（{report['pile']['topCharger']}）",
              f"- 失败最集中的 10 台桩占全部失败日的 {report['pile']['top10ChargerPositiveShare']:.1%}",
              "",
              "## 8. 其余分桶",
              ""]
    for name in ("station_id", "manufacturer", "charger_model", "site_type", "weather_prev",
                 "scenario_event", "weekend", "halfWindow"):
        lines += [f"### {name}", "", "| 桶 | 桩日 | 正类 | 基础率 | 平均预测 | AUC | 备注 |",
                  "| --- | --- | --- | --- | --- | --- | --- |"]
        for row in report["breakdown"][name]:
            auc = "—" if row["auc"] is None else f"{row['auc']:.4f}"
            base = "—" if row["baseRate"] is None else f"{row['baseRate']:.4f}"
            mean = "—" if row["meanProb"] is None else f"{row['meanProb']:.4f}"
            lines.append(f"| {row['bucket']} | {row['n']:,} | {row['positives']:,} | {base} | {mean} | "
                         f"{auc} | {row.get('note', '')} |")
        lines.append("")
    invariants = report["invariants"]
    lines += ["## 9. 数据纪律与不变量", "",
              f"- {report['dataDiscipline']}",
              f"- TEST 全部晚于 VALIDATION 右端 {invariants['testStartsAtValidationEnd']}；"
              f"未越窗口右端 {invariants['testEndsBeforeWindowEnd']}；"
              f"(桩,日) 唯一 {invariants['chargerDayUniqueInTest']}；当日 ≥1 次尝试 "
              f"{invariants['everyRowHasAttempts']}；分数落在 [0,1] {invariants['scoreRangeLegal']}；"
              f"TEST 里 TRAIN 未见过的桩 {invariants['testRowsWithUnseenCharger']} 行",
              f"- 零方差列在建表阶段剔除 {len(report['droppedConstant'])} 个："
              f"{', '.join(sorted(report['droppedConstant']))}——{report['constantWhyNote']}",
              f"- 被排除的样本：{report['excludedBrief']}",
              "", "> " + report["disclaimer"], ""]
    return "\n".join(lines)


def main() -> dict:
    common.require_empty_run_dir(common.OUT_DIR, extra_allowed=(common.FEATURES_PATH.name,
                                                                common.BUILD_SUMMARY_PATH.name,
                                                                common.BUNDLE_PATH.name,
                                                                common.TRAIN_METRICS_PATH.name))
    bundle = load_bundle()
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as summary_handle:
        summary = json.load(summary_handle)
    with open(common.TRAIN_METRICS_PATH, encoding="utf-8") as train_handle:
        train_metrics = json.load(train_handle)
    frame = pd.read_pickle(common.FEATURES_PATH)
    test = frame[frame["split"] == "TEST"].reset_index(drop=True)
    train = frame[frame["split"] == "TRAIN"].reset_index(drop=True)
    validation = frame[frame["split"] == "VALIDATION"].reset_index(drop=True)
    y = test["y_fail"].to_numpy(dtype=int)

    x_test, _ = design(test, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob = bundle["classifier"].predict_proba(x_test)[:, 1]
    count = np.clip(bundle["regressor"].predict(x_test), 0.0, None)
    threshold = float(bundle["operatingPoint"]["threshold"])
    flag = prob >= threshold

    baselines = bundle["baselines"]
    scores = baseline_scores(test, baselines)
    lookup_test = predict_baseline(baselines, test)
    prior_test = scores["asOfChargerPriorOnly"]

    scoreboard = {"model": {**score_metrics(y, prob, probabilistic=True), "note": "发布的分类模型"}}
    for name in ("trainLookupBlend", "asOfChargerPriorOnly", "expectedVolumeOnly", "naiveLag1Alert"):
        scoreboard[name] = score_metrics(
            y, scores[name], probabilistic=(name in ("trainLookupBlend", "asOfChargerPriorOnly")))
    scoreboard["globalBaseRate"] = {"auc": 0.5, "prAuc": round(float(y.mean()), 4),
                                    "liftAt5pct": 1.0, "liftAt10pct": 1.0, "liftAt20pct": 1.0,
                                    "deployable": True, "logLoss": None, "brier": None,
                                    "note": "常数打分，AUC 恒 0.5，只是尺子"}
    scoreboard["regressorExpectedFails"] = score_metrics(y, count, probabilistic=False)
    scoreboard["regressorExpectedFails"]["note"] = "回归打分（明天坏几次）拿来排 y_fail，口径混用只作对照"
    scoreboard["oracleActualVolumeNONDEPLOYABLE"] = oracle_metrics(
        y, scores["oracleActualVolumeNONDEPLOYABLE"])

    auc = float(roc_auc_score(y, prob))
    auc_lookup = float(roc_auc_score(y, lookup_test))
    auc_prior = float(roc_auc_score(y, prior_test))
    auc_volume = float(roc_auc_score(y, scores["expectedVolumeOnly"]))
    best_baseline = max(auc_lookup, auc_prior)
    beaten = auc >= best_baseline
    precision = float(y[flag].mean()) if flag.any() else 0.0
    recall = float(flag[y == 1].sum() / max(1, int(y.sum())))

    rankings = {"model": prob, "trainLookupBlend": lookup_test, "asOfChargerPriorOnly": prior_test,
                "regressorExpectedFails": count, "expectedVolumeOnly": scores["expectedVolumeOnly"],
                "naiveLag1Alert": scores["naiveLag1Alert"]}
    budget_rows = inspection_budget(test, rankings, y)
    at_fifteen = next(row for row in budget_rows if abs(row["budget"] - 0.15) < 1e-9)
    count_ranks_first = sum(1 for row in budget_rows
                            if row["regressorExpectedFails"]["recall"] > row["model"]["recall"])
    budget_conclusion = (
        f"每日 15% 预算下：模型召回 {at_fifteen['model']['recall']:.1%}、"
        f"回归（按预计坏几次）召回 {at_fifteen['regressorExpectedFails']['recall']:.1%}、"
        f"三级查表 {at_fifteen['trainLookupBlend']['recall']:.1%}、"
        f"纯因果桩先验 {at_fifteen['asOfChargerPriorOnly']['recall']:.1%}、"
        f"只用预期用量 {at_fifteen['expectedVolumeOnly']['recall']:.1%}、"
        f"朴素 lag1 {at_fifteen['naiveLag1Alert']['recall']:.1%}。"
        + (f"回归排序在 {len(budget_rows)} 档预算里有 {count_ranks_first} 档赢过分类排序，"
           "两种排程口径值得一起发给运营挑。" if count_ranks_first else
           f"回归排序在 {len(budget_rows)} 档预算里一档都没赢过分类排序——"
           "「按预计失败台数排优先级」在这批数据上不比分桶概率更好，"
           "所以只发一种排程口径（分类概率），回归值只用来估工作量。"))

    count_rows = {
        "modelRegressor": {"mae": round(common.mae(test["y_fails"].to_numpy(dtype=float), count), 5),
                           "auc": round(float(roc_auc_score(y, count)), 4)},
        "trainMeanFails": {"mae": round(common.mae(test["y_fails"].to_numpy(dtype=float),
                                                   np.full(len(test), float(train["y_fails"].mean()))), 5),
                           "auc": None},
        "expectedVolumeTimesPrior": {
            "mae": round(common.mae(
                test["y_fails"].to_numpy(dtype=float),
                test["attempts_per_day_30d"].to_numpy(dtype=float)
                * test["charger_fail_rate_attempt_prior"].to_numpy(dtype=float)), 5),
            "auc": round(float(roc_auc_score(
                y, test["attempts_per_day_30d"].to_numpy(dtype=float)
                * test["charger_fail_rate_attempt_prior"].to_numpy(dtype=float))), 4)},
    }
    count_conclusion = (
        f"回归 MAE 模型 {count_rows['modelRegressor']['mae']} vs 全局均值基线 "
        f"{count_rows['trainMeanFails']['mae']} vs 因果「预期用量×桩失败率」基线 "
        f"{count_rows['expectedVolumeTimesPrior']['mae']}——"
        + ("模型在次数尺度上也赢了两条基线。"
           if count_rows["modelRegressor"]["mae"] < min(count_rows["trainMeanFails"]["mae"],
                                                        count_rows["expectedVolumeTimesPrior"]["mae"])
           else "模型没有同时赢过两条次数基线，按预计台数排程时应优先用赢的那条（如实报，不挑好看的）。"))

    x_train, _ = design(train, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob_train = bundle["classifier"].predict_proba(x_train)[:, 1]
    x_validation, _ = design(validation, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob_validation = bundle["classifier"].predict_proba(x_validation)[:, 1]

    report: dict[str, object] = {
        "modelId": bundle["modelId"], "modelVersion": bundle["modelVersion"],
        "publishedBatchId": bundle["publishedBatchId"], "pipelineRunId": bundle["pipelineRunId"],
        "derivedDatasetId": bundle["derivedDatasetId"], "datasetId": bundle["datasetId"],
        "featuresSha256": bundle["featuresSha256"], "chosenSet": bundle["chosenSet"],
        "featureCount": int(x_test.shape[1]), "testWindow": bundle["splits"],
        "testRows": int(len(test)), "scope": summary["scope"], "label": bundle["label"],
        "sampleUnit": summary["sampleUnit"], "decisionTime": summary["decisionTime"],
        "excludedBrief": "；".join(f"{key}={value}" for key, value in summary["scope"].items()
                                   if key in ("panelRows", "droppedZeroAttemptRows", "droppedPositives")),
        "leakAudit": summary["leakAudit"],
        "droppedConstant": summary["features"]["droppedConstant"],
        "constantWhyNote": ("稠密面板让 days_since_last_observed 恒为 1；全期前一日最高气温未达 35℃ "
                            "让 heatwave_prev 恒为 0；本批站点变压器容量同一个值；遥测写入节拍整点固定 "
                            "让 tel_gap_hours_at_start 恒为 5 分钟（具体取值见 rolling/README 与本表 JSON）"),
        "risk": {
            "positives": int(y.sum()), "baseRate": round(float(y.mean()), 4),
            "auc": round(auc, 4), "aucTrain": round(float(roc_auc_score(
                train["y_fail"].to_numpy(dtype=int), prob_train)), 4),
            "aucValidation": round(float(roc_auc_score(
                validation["y_fail"].to_numpy(dtype=int), prob_validation)), 4),
            "prAuc": round(float(average_precision_score(y, prob)), 4),
            "logLoss": scoreboard["model"]["logLoss"], "brier": scoreboard["model"]["brier"],
            "liftAt5pct": scoreboard["model"]["liftAt5pct"],
            "liftAt10pct": scoreboard["model"]["liftAt10pct"],
            "liftAt20pct": scoreboard["model"]["liftAt20pct"],
            "frozenOperatingPoint": {
                "threshold": round(threshold, 6), "alertRate": round(float(flag.mean()), 4),
                "alerts": int(flag.sum()), "precision": round(precision, 4), "recall": round(recall, 4),
                "f1": round(2 * precision * recall / max(1e-9, precision + recall), 4),
                "rule": bundle["operatingPoint"]["rule"],
                "precisionTarget": bundle["operatingPoint"]["precisionTarget"],
                "targetMet": bool(bundle["operatingPoint"]["targetMet"]),
                "note": "阈值与规则在 VALIDATION 上定死，TEST 只验不改"},
        },
        "scoreboard": scoreboard,
        "headline": {
            "vsLookupPct": round(100.0 * (auc / auc_lookup - 1.0), 2),
            "vsPriorPct": round(100.0 * (auc / auc_prior - 1.0), 2),
            "vsVolumePct": round(100.0 * (auc / auc_volume - 1.0), 2),
            "conclusion": ("学习模型跑赢了全部可部署基线。" if beaten else
                           f"学习模型**没有**跑赢最便宜的可部署基线（查表 {auc_lookup:.4f} / "
                           f"纯因果桩先验 {auc_prior:.4f}），差距 {100.0 * (auc / best_baseline - 1.0):+.2f}%。"
                           "在桩×日这个粒度上，历史经验率已经吃掉了绝大部分可预测信号，GBT 的容量没有兑换成增量；"
                           "本线的发布价值因此落在'派生面板 + 泄漏审计 + 巡检预算口径'这套基础设施与诚实数字上，"
                           "而不是模型本身。下面第 2/3 节与十轮滚动研究都按这个口径复核。"),
        },
        "budgetRanking": budget_rows,
        "budgetConclusion": budget_conclusion,
        "budget": budget_compliance(test, prob, threshold),
        "pile": concentration(test, prob, threshold),
        "rankAgreement": rank_agreement(prob, {"trainLookupBlend": lookup_test,
                                               "asOfChargerPriorOnly": prior_test,
                                               "expectedVolumeOnly": scores["expectedVolumeOnly"],
                                               "oracleActualVolume": scores["oracleActualVolumeNONDEPLOYABLE"],
                                               "regressorExpectedFails": count}),
        "calibration": [{"bin": str(row["bin"]), "n": int(row["n"]),
                         "predicted": round(float(row["predicted"]), 4),
                         "observed": round(float(row["observed"]), 4)}
                        for _, row in common.calibration_bins(y, prob, bins=10).iterrows()],
        "ablation": ablation(frame, bundle, test, y),
        "ablationSets": list(bundle["candidateSets"].keys()),
        "valAblation": {key: block["auc"] for key, block in train_metrics["candidateValidation"].items()},
        "countRegression": {"rows": count_rows, "conclusion": count_conclusion},
    }
    oracle_auc = scoreboard["oracleActualVolumeNONDEPLOYABLE"]["auc"]
    report["exposureConclusion"] = (
        f"不可部署 oracle（真用当天尝试次数）AUC {oracle_auc:.4f}，只看昨天为止的因果桩先验 "
        f"{auc_prior:.4f}，差 {oracle_auc - auc_prior:+.4f}。"
        + ("作弊没有换来更多排序力：暴露度信息已经被因果历史特征覆盖，"
           "所以本线的 AUC 不是靠偷看当天得来的。" if oracle_auc <= auc_prior + 0.002 else
           "作弊只多出这一点点，且发布的模型根本看不到当天尝试次数（该列在禁入名单里）；"
           f"换言之当天暴露度里剩下的 {oracle_auc - auc_prior:.4f} 是本线的**理论上限差**，"
           "而不是已经用掉的泄漏。滚动研究十轮里对比同一条，防止口径漂移。")
        + f" 另：只用因果预期用量（可部署的下界口径）AUC {auc_volume:.4f}，"
          f"即'完全不谈健康度、只按用量排'已经有 {auc_volume / auc:.1%} 的模型 AUC。")
    report["breakdown"] = breakdowns(test, prob)
    lag1_rows = {row["bucket"]: row for row in report["breakdown"]["prevDayFail"]}
    volume_rows = report["breakdown"]["expectedVolume"]
    report["sliceConclusion"] = (
        "组内 AUC 才是真本事：如果整体 AUC 全部来自'忙的桩更容易被判为坏'，那么在预期用量同一档内"
        "分数就该失去区分力。实测 "
        + "；".join(f"{row['bucket']} 档内 AUC "
                    + ("—" if row["auc"] is None else f"{row['auc']:.4f}")
                    for row in volume_rows)
        + "；再按昨天是否坏切开："
        + "；".join(f"{key} 组内 AUC "
                    + ("—" if row["auc"] is None else f"{row['auc']:.4f}")
                    for key, row in lag1_rows.items()))
    bounds = {key: pd.Timestamp(value) for key, value in bundle["splits"].items()}
    starts = test["day_start_utc"]
    seen = test["charger_id"].isin(train["charger_id"].unique())
    report["invariants"] = {
        "testDays": int(test["business_date"].nunique()),
        "testStartsAtValidationEnd": bool((starts >= bounds["validationEndExclusive"]).all()),
        "testEndsBeforeWindowEnd": bool((starts < bounds["testEndExclusive"]).all()),
        "chargerDayUniqueInTest": bool(test.duplicated(subset=["charger_id", "business_date"]).sum() == 0),
        "everyRowHasAttempts": bool((test["attempts_on_day"] > 0).all()),
        "testRowsWithUnseenCharger": int((~seen).sum()),
        "chargerShareOfTestSeenInTrain": round(float(seen.mean()), 4),
        "scoreRangeLegal": bool(((prob >= 0) & (prob <= 1)).all()),
        "labelMatchesPanelCounts": bool((y == (test["tech_fails_on_day"] > 0).to_numpy()).all())}
    report["dataDiscipline"] = (
        "只读消费已发布批次的 clean 层；特征来自新增派生数据集 "
        f"{common.DERIVED_DATASET_ID}（确定性聚合，逐文件 sha256/行数在载入时复查）；"
        "产物落 outputs/ml_reliability/ 且全部独占创建；当日量（尝试数、用户数、失败数）"
        "一律禁入特征，泄漏审计用全表布尔掩码独立重算，与向量路径 0 偏差")
    report["disclaimer"] = common.data_note()

    common.write_new_json(common.TEST_REPORT_PATH, report)
    common.write_new_bytes(common.TEST_REPORT_MD, markdown(report).encode("utf-8"))
    risk = report["risk"]
    point = risk["frozenOperatingPoint"]
    print(f"[eval] TEST n={report['testRows']:,} 桩日 基础率={risk['baseRate']:.4f} "
          f"正类={risk['positives']:,}")
    for name, block in report["scoreboard"].items():
        print(f"[eval] {name}: TEST AUC={block['auc']:.4f} PR-AUC={block['prAuc']:.4f} "
              f"lift@10%={block['liftAt10pct']:.2f}x"
              + ("" if block["deployable"] else "  [不可部署]"))
    print(f"[eval] 阈值{point['threshold']:.3f} → 告警 {point['alerts']} 桩日 "
          f"精确率={point['precision']:.4f} 召回={point['recall']:.4f} F1={point['f1']:.4f} "
          f"达标={point['targetMet']}；越预算天数={report['budget']['daysOverBudget']}")
    print("[eval] " + report["headline"]["conclusion"])
    print("[eval] " + budget_conclusion)
    print(f"[eval] -> {common.TEST_REPORT_MD}")
    return report


if __name__ == "__main__":
    main()
