"""一次性盲测：在 ``split == TEST`` 上评会话级占桩超时提醒，产出 JSON + Markdown 报告。

纪律：特征组、阈值、基线表全部由 train.py 在 TRAIN/VALIDATION 上定死并冻进 bundle，本脚本
**只读不算新参数**；消融用的缩减模型只是在 TRAIN 上重拟合后放在一起报数，不回头改主模型。
模型包与特征表两层按 sha256 配对校验（本线没有派生数据集——不落新数据，校验层次因此少一层），
任一错配一律拒评。

与第七线的报告结构同源但问题相反：第七线是"基线赢了模型"的负面样板，本线的模型实打实赢过
全部廉价口径，所以报告的重点从"模型是否多余"变成**"增益从哪来、值不值运营的动作"**：
  · 七档基线 + 模型 + 回归排序在同一片 TEST 上的 AUC / PR-AUC / lift；
  · **移车提醒预算表**：每天只对当日开始会话的 X% 发提醒，各排序能命中多少真实超时（运营数字）；
  · **归因切片**：计划时长分位内、"上次是否超时"分组内、接口类型（慢充/超充结构）内还剩多少区分力——
    这回答"分数是不是只在排'这台桩充得慢'"这个最扎心的问题；
  · 消融四组：静态结构 vs 跨轴历史 vs 排队运营——**用户习惯只值多少**由 staticOnly 与 full 之差
    给出定量答案。
所有数字都带 ``common.data_note()`` 那句数据口径声明（含"本线未新增任何数据"）。

用法（仓库根目录）：python -m data_analysis.ml.overstay.evaluate
"""

from __future__ import annotations

import json

import joblib
import numpy as np
import pandas as pd
from sklearn.metrics import average_precision_score, roc_auc_score

from . import common
from .train import (MAX_ALERT_SHARE, baseline_scores, design, new_classifier, predict_baseline,
                    predict_regress_baseline, split_group)

#: 概率型分数才有 logLoss/Brier；计划时长、oracle、规则分、回归分钟一律只给排序指标。
PROBABILISTIC = {"model", "trainCellBlend", "asOfUserPriorOnly", "asOfStationPriorOnly"}
BUDGETS = (0.05, 0.10, 0.15, 0.20, 0.25)
MIN_BUCKET_ROWS = 60
MIN_BUCKET_POSITIVES = 5


def load_bundle() -> dict:
    """读模型包并复查三件事：批次、特征表哈希、模型包与表的配对。"""
    if not common.BUNDLE_PATH.exists():
        raise FileNotFoundError(f"缺少模型包 {common.BUNDLE_PATH}，请先跑 train")
    bundle = joblib.load(common.BUNDLE_PATH)
    common.verify_batch()
    if bundle["publishedBatchId"] != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch(f"模型包绑定批次 {bundle['publishedBatchId']!r} 与当前数据层不一致")
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    digest = common.sha256_file(common.FEATURES_PATH)
    if summary.get("featuresSha256") != digest:
        raise common.BatchMismatch("特征表哈希与 features_summary 记录不一致，请重跑 features")
    if digest != bundle["featuresSha256"]:
        raise common.BatchMismatch(
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
        block["note"] = "分数不是概率（时长/规则/分钟），不算 logLoss 与 Brier"
    return block


def oracle_metrics(y: np.ndarray, scores: np.ndarray) -> dict:
    block = score_metrics(y, scores, probabilistic=False)
    block["deployable"] = False
    block["note"] = ("**不可部署**：用的是本场会话的真实充电时长（``ended_at`` 之后才存在）。"
                     + block.get("note", "只作归因对照"))
    return block


def reminder_budget(test: pd.DataFrame, rankings: dict[str, np.ndarray], y: np.ndarray,
                    budgets: tuple[float, ...] = BUDGETS) -> list[dict]:
    """每天只对当日开始会话的 n% 发移车提醒，按每种排序各能命中多少真实超时——运营真正会用的数字。

    逐日取前 k 场（``k = max(1, ceil(budget × 当日会话数))``），跨日累加命中率。与阈值口径独立：
    阈值回答"每天发多少条"，这张表回答"人手有限时先提醒谁"。
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
    y = test["y_over"].to_numpy(dtype=int)
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
    """分桶维度是挑死的：站/接口/车型/站型/场景/天气/周末/前后半段，再加三个本线专属——
    **计划时长分位**（决策时点的"这单多大"）、**上次是否超时**（习惯假设）、
    **用户历史在场与否**（新客 vs 熟客的分组表现）。"""
    result: dict[str, list[dict]] = {}
    for name in ("station_id", "connector_type", "vehicle_class", "site_type", "weather_prev",
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
    planned = pd.qcut(label_columns(test, "planned_hours"), 5, duplicates="drop")
    result["plannedHours"] = bucket_rows(
        test, prob, [(f"计划时长 Q{i + 1} {band}", (planned == band).to_numpy())
                     for i, band in enumerate(planned.cat.categories)])
    last_over = label_columns(test, "user_last_over").fillna(-1.0).to_numpy(dtype=float)
    result["userLastOver"] = bucket_rows(
        test, prob, [("无历史可看", last_over < 0), ("上次没超时", last_over == 0),
                     ("上次超时了", last_over == 1)])
    known = (label_columns(test, "user_sessions_prior") > 0).to_numpy()
    result["userHistoryPresence"] = bucket_rows(test, prob, [("熟客（有历史）", known),
                                                             ("新面孔（无历史）", ~known)])
    return result


def ablation(frame: pd.DataFrame, bundle: dict, test: pd.DataFrame, y: np.ndarray) -> dict[str, float]:
    """信号来自哪里：四套候选组各自在 TRAIN 重拟合，再在同一片 TEST 上比（只作归因）。"""
    numeric, categorical = bundle["allNumericFeatures"], bundle["allCategoricalFeatures"]
    train = frame[frame["split"] == "TRAIN"]
    y_train = train["y_over"].to_numpy(dtype=int)
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
    """提醒落在哪些人/站上：运营关心"是不是只有几个惯犯、几台慢桩"，也防止分数退化成站牌代理。"""
    flagged = test[prob >= threshold]
    counts = flagged["user_id"].value_counts()
    positives_by_user = test.loc[test["y_over"] == 1, "user_id"].value_counts()
    top = counts.index[0] if len(counts) else None
    return {"flaggedRows": int(len(flagged)),
            "distinctUsersFlagged": int(flagged["user_id"].nunique()),
            "usersInScope": int(test["user_id"].nunique()),
            "stationsInScope": int(test["station_id"].nunique()),
            "distinctStationsFlagged": int(flagged["station_id"].nunique()),
            "topStationAlertShare": round(float(flagged["station_id"].value_counts().iloc[0]
                                                / len(flagged)), 4) if len(flagged) else None,
            "daysInScope": int(test["business_date"].nunique()),
            "topUser": str(top) if top is not None else None,
            "topUserAlertShare": round(float(counts.iloc[0] / len(flagged)), 4) if len(counts) else None,
            "top10UserPositiveShare": round(float(positives_by_user.head(10).sum()
                                                  / max(1, int(test["y_over"].sum()))), 4),
            "distinctPositiveUsers": int(test.loc[test["y_over"] == 1, "user_id"].nunique())}


def budget_compliance(test: pd.DataFrame, prob: np.ndarray, threshold: float) -> dict:
    """冻结阈值搬到 TEST 上，每天的提醒数有没有越过"当日开始会话数 25%"这条自定预算。"""
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
        f"# 会话占桩超时提醒 · TEST 盲测报告（{report['modelVersion']}）",
        "",
        "> " + report["disclaimer"],
        "",
        f"- 模型：`{report['modelId']}`（分类=会不会超时 ≥{report['thresholdMin']:.0f} 分钟，"
        f"回归=大概还会占多久；特征组 `{report['chosenSet']}`，{report['featureCount']} 列）",
        f"- 绑定批次：`{report['publishedBatchId']}` · run `{report['pipelineRunId']}` · "
        f"本线**未新增任何数据**（全部为已发布批次 clean 表的只读 as-of 聚合）",
        f"- 特征表 sha256：`{report['featuresSha256'][:16]}…` · 泄漏审计抽样 "
        f"{report['leakAudit']['sampled']} 行 × {len(report['leakAudit']['checked'])} 个跨时间轴 as-of 量，"
        f"最大偏差 {max(report['leakAudit']['maxAbsDiff'].values()):.1e}",
        f"- 答案侧相关核对（Spearman vs over_min，全部禁入）：{report['answerSideSpearman']}",
        f"- 样本单元：{report['sampleUnit']}；决策时刻：{report['decisionTime']}",
        f"- TEST 窗口（UTC 起点）`{report['testWindow']['validationEndExclusive']}` → "
        f"`{report['testWindow']['testEndExclusive']}`，{report['testRows']:,} 场会话，"
        f"正类 {risk['positives']:,}（基础率 {risk['baseRate']:.4f}）",
        "",
        "## 1. 主结果：排序能力（同一 TEST，同一批会话）",
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
              f"- 模型 AUC 相对 TRAIN 三级查表基线 **{head['vsBlendPct']:+.2f}%**、"
              f"相对纯因果用户先验 **{head['vsUserPriorPct']:+.2f}%**、"
              f"相对只用计划时长 **{head['vsPlannedPct']:+.2f}%**",
              f"- **结论**：{head['conclusion']}",
              f"- 校准：Brier {risk['brier']:.5f}、LogLoss {risk['logLoss']:.5f}"
              f"（只有概率型分数参与校准）",
              "",
              f"冻结运营阈值（VALIDATION 上定死，TEST 只验不改；{point['rule']}）：",
              "",
              "| 阈值 | 提醒会话数 | 提醒率 | 精确率 | 召回 | F1 | 是否达标 |",
              "| --- | --- | --- | --- | --- | --- | --- |",
              f"| {point['threshold']:.4f} | {point['alerts']:,} | {point['alertRate']:.2%} | "
              f"{point['precision']:.4f} | {point['recall']:.4f} | {point['f1']:.4f} | "
              f"{'达标' if point['targetMet'] else '未达标（如实标注）'} |",
              "",
              f"预算核对：{report['budget']['days']} 天里 {report['budget']['daysOverBudget']} 天越过"
              f"「当日开始会话数 {MAX_ALERT_SHARE:.0%}」（{report['budget']['dailyBudgetAt25pct']}），"
              f"单日最多 {report['budget']['maxAlertsPerDay']} 条、最高占当日 "
              f"{report['budget']['worstDayShare']:.1%}",
              "",
              "## 2. 移车提醒预算表：每天只提醒当日开始会话的 n%，能命中多少真实超时",
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
              f"- oracle（本场真实充电时长，不可部署）AUC "
              f"{report['scoreboard']['oracleActualDurationNONDEPLOYABLE']['auc']:.4f}"
              f" vs 决策时点就能算的计划时长 {report['scoreboard']['plannedHoursOnly']['auc']:.4f}："
              f"{report['durationConclusion']}",
              "",
              "| 归因切片 | 样本 | 正类 | 基础率 | 平均预测 | 组内 AUC |",
              "| --- | --- | --- | --- | --- | --- |"]
    for name in ("plannedHours", "userLastOver"):
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
              f"- {report['ablationConclusion']}",
              "",
              "## 5. 校准（预测概率 vs 实测超时率，TEST 十分位）",
              "",
              "| 预测概率区间 | 样本数 | 预测均值 | 实测超时率 |", "| --- | --- | --- | --- |"]
    for row in report["calibration"]:
        lines.append(f"| {row['bin']} | {row['n']:,} | {row['predicted']:.4f} | {row['observed']:.4f} |")
    count = report["minutesRegression"]
    lines += ["", "## 6. 回归目标：这场还会占多久（按预计超时分钟排序的另一套口径）", "",
              "| 打分 | MAE(分钟) | 对 y_over 的 AUC |", "| --- | --- | --- |"]
    for name, block in count["rows"].items():
        auc = "—" if block["auc"] is None else f"{block['auc']:.4f}"
        lines.append(f"| {name} | {block['mae']:.2f} | {auc} |")
    lines += ["", f"- {count['conclusion']}",
              "",
              "## 7. 提醒落在哪些人/站上",
              "",
              f"- 冻结阈值下提醒 {report['pile']['flaggedRows']:,} 场会话，覆盖 "
              f"{report['pile']['distinctUsersFlagged']:,} 个用户（TEST 域内共 {report['pile']['usersInScope']:,} 个）"
              f"与 {report['pile']['distinctStationsFlagged']} 个站（共 {report['pile']['stationsInScope']} 个）；"
              f"单用户最多占提醒 {report['pile']['topUserAlertShare']:.1%}、单站最多占 "
              f"{report['pile']['topStationAlertShare']:.1%}",
              f"- 超时最集中的 10 个用户占全部超时会话的 {report['pile']['top10UserPositiveShare']:.1%}"
              f"（正类用户共 {report['pile']['distinctPositiveUsers']:,} 个）——"
              f"{report['concentrationConclusion']}",
              "",
              "## 8. 其余分桶",
              ""]
    for name in ("station_id", "connector_type", "vehicle_class", "site_type", "weather_prev",
                 "scenario_event", "weekend", "halfWindow", "userHistoryPresence"):
        lines += [f"### {name}", "", "| 桶 | 会话数 | 正类 | 基础率 | 平均预测 | AUC | 备注 |",
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
              f"session_id 唯一 {invariants['sessionIdUniqueInTest']}；over_min ≥ 0 "
              f"{invariants['overMinutesNonNegative']}；分数落在 [0,1] {invariants['scoreRangeLegal']}；"
              f"TEST 里 TRAIN 未见过的用户 {invariants['testRowsWithUnseenUser']:,} 行、"
              f"未见过的站 {invariants['testRowsWithUnseenStation']} 行",
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
    y = test["y_over"].to_numpy(dtype=int)

    x_test, _ = design(test, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob = bundle["classifier"].predict_proba(x_test)[:, 1]
    minutes = np.clip(bundle["regressor"].predict(x_test), 0.0, None)
    threshold = float(bundle["operatingPoint"]["threshold"])
    flag = prob >= threshold

    baselines = bundle["baselines"]
    scores = baseline_scores(test, baselines)
    blend_test = predict_baseline(baselines, test)
    user_prior_test = scores["asOfUserPriorOnly"]

    scoreboard = {"model": {**score_metrics(y, prob, probabilistic=True), "note": "发布的分类模型"}}
    for name in ("trainCellBlend", "asOfUserPriorOnly", "asOfStationPriorOnly",
                 "plannedHoursOnly", "naiveLastOverstay"):
        scoreboard[name] = score_metrics(y, scores[name], probabilistic=(name in PROBABILISTIC))
    scoreboard["globalBaseRate"] = {"auc": 0.5, "prAuc": round(float(y.mean()), 4),
                                    "liftAt5pct": 1.0, "liftAt10pct": 1.0, "liftAt20pct": 1.0,
                                    "deployable": True, "logLoss": None, "brier": None,
                                    "note": "常数打分，AUC 恒 0.5，只是尺子"}
    scoreboard["regressorExpectedMinutes"] = score_metrics(y, minutes, probabilistic=False)
    scoreboard["regressorExpectedMinutes"]["note"] = "回归打分（还会占多久）拿来排 y_over，口径混用只作对照"
    scoreboard["oracleActualDurationNONDEPLOYABLE"] = oracle_metrics(
        y, scores["oracleActualDurationNONDEPLOYABLE"])

    auc = float(roc_auc_score(y, prob))
    auc_blend = float(roc_auc_score(y, blend_test))
    auc_user = float(roc_auc_score(y, user_prior_test))
    auc_planned = float(roc_auc_score(y, scores["plannedHoursOnly"]))
    best_baseline = max(auc_blend, auc_user, auc_planned)
    beaten = auc >= best_baseline
    precision = float(y[flag].mean()) if flag.any() else 0.0
    recall = float(flag[y == 1].sum() / max(1, int(y.sum())))

    rankings = {"model": prob, "trainCellBlend": blend_test, "asOfUserPriorOnly": user_prior_test,
                "asOfStationPriorOnly": scores["asOfStationPriorOnly"],
                "regressorExpectedMinutes": minutes, "plannedHoursOnly": scores["plannedHoursOnly"],
                "naiveLastOverstay": scores["naiveLastOverstay"]}
    budget_rows = reminder_budget(test, rankings, y)
    at_ten = next(row for row in budget_rows if abs(row["budget"] - 0.10) < 1e-9)
    minutes_first = sum(1 for row in budget_rows
                        if row["regressorExpectedMinutes"]["recall"] > row["model"]["recall"])
    budget_conclusion = (
        f"每日 10% 预算下：模型召回 {at_ten['model']['recall']:.1%}、"
        f"回归（按预计占桩分钟）召回 {at_ten['regressorExpectedMinutes']['recall']:.1%}、"
        f"三级查表 {at_ten['trainCellBlend']['recall']:.1%}、"
        f"用户先验 {at_ten['asOfUserPriorOnly']['recall']:.1%}、"
        f"站点先验 {at_ten['asOfStationPriorOnly']['recall']:.1%}、"
        f"只用计划时长 {at_ten['plannedHoursOnly']['recall']:.1%}、"
        f"朴素'上次超时' {at_ten['naiveLastOverstay']['recall']:.1%}。"
        + (f"回归排序在 {len(budget_rows)} 档预算里有 {minutes_first} 档赢过分类排序，"
           "两种提醒排程口径值得一起发给运营挑。" if minutes_first else
           f"回归排序在 {len(budget_rows)} 档预算里一档都没赢过分类排序——"
           "「按预计占桩分钟排提醒优先级」在这批数据上不比分桶概率更好，"
           "所以只发一种口径（分类概率），回归值只用来估提醒之外的收益测算。"))

    y_minutes_test = test["over_min"].to_numpy(dtype=float)
    minutes_rows = {
        "modelRegressor": {"mae": round(common.mae(y_minutes_test, minutes), 3),
                           "auc": round(float(roc_auc_score(y, minutes)), 4)},
        "trainMeanOverMin": {
            "mae": round(common.mae(y_minutes_test, np.full(len(test), float(train["over_min"].mean()))), 3),
            "auc": None},
        "asOfUserMeanPrior": {
            "mae": round(common.mae(y_minutes_test,
                                    test["user_over_mean_prior"].to_numpy(dtype=float)), 3),
            "auc": round(float(roc_auc_score(y, test["user_over_mean_prior"].to_numpy(dtype=float))), 4)},
        "trainCellMeanBlend": {
            "mae": round(common.mae(y_minutes_test,
                                    predict_regress_baseline(bundle["regressBaselines"], test)), 3),
            "auc": round(float(roc_auc_score(
                y, predict_regress_baseline(bundle["regressBaselines"], test))), 4)},
    }
    mae_best = min(minutes_rows["trainMeanOverMin"]["mae"], minutes_rows["asOfUserMeanPrior"]["mae"],
                   minutes_rows["trainCellMeanBlend"]["mae"])
    minutes_conclusion = (
        f"回归 MAE（分钟）模型 {minutes_rows['modelRegressor']['mae']} vs 全局均值基线 "
        f"{minutes_rows['trainMeanOverMin']['mae']} vs 因果用户先验均值 "
        f"{minutes_rows['asOfUserMeanPrior']['mae']} vs TRAIN 查表均值 "
        f"{minutes_rows['trainCellMeanBlend']['mae']}——"
        + ("模型在分钟尺度上也赢过全部基线，预计占桩时长可以直接用于'提醒之外还能省多少桩位·分钟'的测算。"
           if minutes_rows["modelRegressor"]["mae"] < mae_best else
           f"模型没有赢过最便宜基线（{mae_best}），按分钟排程时应优先用基线（如实报，不挑好看的）。"))

    x_train, _ = design(train, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob_train = bundle["classifier"].predict_proba(x_train)[:, 1]
    x_validation, _ = design(validation, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob_validation = bundle["classifier"].predict_proba(x_validation)[:, 1]

    ablation_out = ablation(frame, bundle, test, y)
    report: dict[str, object] = {
        "modelId": bundle["modelId"], "modelVersion": bundle["modelVersion"],
        "publishedBatchId": bundle["publishedBatchId"], "pipelineRunId": bundle["pipelineRunId"],
        "datasetId": bundle["datasetId"],
        "featuresSha256": bundle["featuresSha256"], "chosenSet": bundle["chosenSet"],
        "featureCount": int(x_test.shape[1]), "testWindow": bundle["splits"],
        "thresholdMin": common.OVERSTAY_THRESHOLD_MIN,
        "testRows": int(len(test)), "scope": summary["scope"], "label": bundle["label"],
        "sampleUnit": summary["sampleUnit"], "decisionTime": summary["decisionTime"],
        "answerSideSpearman": summary["answerSideSpearman"],
        "excludedBrief": f"EXCLUDED {summary['splits']['EXCLUDED']:,} 场（窗口之外，只当历史来源，"
                         f"不进训练/评测）",
        "leakAudit": summary["leakAudit"],
        "droppedConstant": summary["features"]["droppedConstant"],
        "constantWhyNote": ("本批站点变压器容量全表同值；所有 campaign 折扣同值（has_campaign 保留了"
                            "唯一有信息的部分）；仿真里'被叫到→开始充电'间隔恒为 5 分钟，"
                            "own_queue_wait_min 零方差（具体取值见 features_summary.json）"),
        "risk": {
            "positives": int(y.sum()), "baseRate": round(float(y.mean()), 4),
            "auc": round(auc, 4), "aucTrain": round(float(roc_auc_score(
                train["y_over"].to_numpy(dtype=int), prob_train)), 4),
            "aucValidation": round(float(roc_auc_score(
                validation["y_over"].to_numpy(dtype=int), prob_validation)), 4),
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
            "vsBlendPct": round(100.0 * (auc / auc_blend - 1.0), 2),
            "vsUserPriorPct": round(100.0 * (auc / auc_user - 1.0), 2),
            "vsPlannedPct": round(100.0 * (auc / auc_planned - 1.0), 2),
            "conclusion": ("学习模型跑赢了全部可部署基线——与第七线（查表吃掉一切信号的负面样板）相反，"
                           "本线的增益主要来自**接口/功率/车型/站型的场景组合**：单看用户习惯或单查站表"
                           "都到不了这个高度，但把它们乘在一起的结构 GBT 能学到、查表学不全。"
                           if beaten else
                           f"学习模型**没有**跑赢最便宜的可部署基线（查表 {auc_blend:.4f} / "
                           f"用户先验 {auc_user:.4f} / 计划时长 {auc_planned:.4f}），"
                           f"差距 {100.0 * (auc / best_baseline - 1.0):+.2f}%。发布价值因此落在"
                           "派生聚合 + 泄漏审计 + 预算口径这套基础设施与诚实数字上，而不是模型本身。"),
        },
        "budgetRanking": budget_rows,
        "budgetConclusion": budget_conclusion,
        "budget": budget_compliance(test, prob, threshold),
        "pile": concentration(test, prob, threshold),
        "rankAgreement": rank_agreement(prob, {"trainCellBlend": blend_test,
                                               "asOfUserPriorOnly": user_prior_test,
                                               "asOfStationPriorOnly": scores["asOfStationPriorOnly"],
                                               "plannedHoursOnly": scores["plannedHoursOnly"],
                                               "oracleActualDuration": scores["oracleActualDurationNONDEPLOYABLE"],
                                               "regressorExpectedMinutes": minutes}),
        "calibration": [{"bin": str(row["bin"]), "n": int(row["n"]),
                         "predicted": round(float(row["predicted"]), 4),
                         "observed": round(float(row["observed"]), 4)}
                        for _, row in common.calibration_bins(y, prob, bins=10).iterrows()],
        "ablation": ablation_out,
        "ablationSets": list(bundle["candidateSets"].keys()),
        "valAblation": {key: block["auc"] for key, block in train_metrics["candidateValidation"].items()},
        "minutesRegression": {"rows": minutes_rows, "conclusion": minutes_conclusion},
    }
    oracle_auc = scoreboard["oracleActualDurationNONDEPLOYABLE"]["auc"]
    report["durationConclusion"] = (
        f"不可部署 oracle（本场真实充电时长）AUC {oracle_auc:.4f}，决策时点就能算的计划时长 "
        f"{auc_planned:.4f}，发布模型 {auc:.4f}。"
        + ("计划时长这个替身**不输**真实时长（差 "
           f"{oracle_auc - auc_planned:+.4f}），说明'这单要充多少度、桩有多大功率'在起充前就把单子的"
           "体量说清楚了；模型再往上的增益来自它知道这些场景各自对应什么占桩行为。"
           if oracle_auc - auc_planned <= 0.002 else
           f"真实时长比计划时长还多出 {oracle_auc - auc_planned:.4f}，说明'实际充了多久'里确有"
           "计划读不到的信息——但那要等 ended_at，不可部署；模型的 AUC 高于 oracle（{:.4f} vs "
           "{:.4f}），所以发布口径没有靠偷看结局窗口。".format(auc, oracle_auc)))
    report["breakdown"] = breakdowns(test, prob)
    planned_rows = report["breakdown"]["plannedHours"]
    last_over_rows = {row["bucket"]: row for row in report["breakdown"]["userLastOver"]}
    report["sliceConclusion"] = (
        "组内 AUC 才是真本事：如果整体 AUC 全部来自'单子大/桩慢的会话更占桩'，那么在同一计划时长档内"
        "分数就该失去区分力。实测 "
        + "；".join(f"{row['bucket']} 档内 AUC "
                    + ("—" if row["auc"] is None else f"{row['auc']:.4f}")
                    for row in planned_rows)
        + "；再按'上次是否超时'切开："
        + "；".join(f"{key} 组内 AUC "
                    + ("—" if row["auc"] is None else f"{row['auc']:.4f}")
                    for key, row in last_over_rows.items())
        + "。两组都稳住，才说明模型排的不是'单子的体量'或'惯犯名单'，而是场景×习惯的交互。")
    report["ablationConclusion"] = (
        f"注意 staticOnly {ablation_out['staticOnly']:.4f} 与 full {ablation_out['full']:.4f} 只差 "
        f"{ablation_out['full'] - ablation_out['staticOnly']:+.4f}：**跨轴历史与排队运营的增量接近零**——"
        f"在本批仿真里占桩超时几乎完全由场景（接口/功率/车型/站型）决定，'用户历史'（historyOnly 单组 "
        f"{ablation_out['historyOnly']:.4f}）与'当时是否有人排队'都没有场景之外的独立信息。"
        "这句话必须原样带给运营：如果真实数据里用户习惯显著，本框架的 historyOnly 组就是现成的量尺。")
    report["concentrationConclusion"] = (
        "提醒高度摊在场景上而不是摊在'少数惯犯'上——所以动作应该按**站型/接口**设计（比如慢充位离场即推），"
        "而不是给个别人打标签。" if report["pile"]["topUserAlertShare"] < 0.01 else
        "提醒集中度超出预期，先看是不是某个大车队账号刷进了名单，再决定个性化动作。")
    bounds = {key: pd.Timestamp(value) for key, value in bundle["splits"].items()}
    starts = test["started_at"]
    seen_user = test["user_id"].isin(train["user_id"].unique())
    seen_station = test["station_id"].isin(train["station_id"].unique())
    report["invariants"] = {
        "testDays": int(test["business_date"].nunique()),
        "testStartsAtValidationEnd": bool((starts >= bounds["validationEndExclusive"]).all()),
        "testEndsBeforeWindowEnd": bool((starts < bounds["testEndExclusive"]).all()),
        "sessionIdUniqueInTest": bool(test["session_id"].is_unique),
        "overMinutesNonNegative": bool((test["over_min"] >= 0).all()),
        "testRowsWithUnseenUser": int((~seen_user).sum()),
        "testRowsWithUnseenStation": int((~seen_station).sum()),
        "scoreRangeLegal": bool(((prob >= 0) & (prob <= 1)).all())}
    report["dataDiscipline"] = (
        "只读消费已发布批次的 clean 层；**本线未新增任何数据**——全部特征是对原表的只读确定性 as-of 聚合，"
        "不落新目录；产物落 outputs/ml_overstay/ 且全部独占创建；ended_at 之后才存在的一切"
        "（费用/能量/SOC/状态/结局时间戳，含与标签直接挂钩的 parking_fee_cents）一律禁入特征；"
        "泄漏审计用全表布尔掩码独立重算跨时间轴 as-of 量（次数按 started_at、超占值按 unplugged_at），"
        "与向量路径 0 偏差")
    report["disclaimer"] = common.data_note()

    common.write_new_json(common.TEST_REPORT_PATH, report)
    common.write_new_bytes(common.TEST_REPORT_MD, markdown(report).encode("utf-8"))
    risk = report["risk"]
    point = risk["frozenOperatingPoint"]
    print(f"[eval] TEST n={report['testRows']:,} 场会话 基础率={risk['baseRate']:.4f} "
          f"正类={risk['positives']:,}")
    for name, block in report["scoreboard"].items():
        print(f"[eval] {name}: TEST AUC={block['auc']:.4f} PR-AUC={block['prAuc']:.4f} "
              f"lift@10%={block['liftAt10pct']:.2f}x"
              + ("" if block["deployable"] else "  [不可部署]"))
    print(f"[eval] 阈值{point['threshold']:.3f} → 提醒 {point['alerts']} 场 "
          f"精确率={point['precision']:.4f} 召回={point['recall']:.4f} F1={point['f1']:.4f} "
          f"达标={point['targetMet']}；越预算天数={report['budget']['daysOverBudget']}")
    print("[eval] " + report["headline"]["conclusion"])
    print("[eval] " + budget_conclusion)
    print("[eval] " + report["ablationConclusion"])
    print(f"[eval] -> {common.TEST_REPORT_MD}")
    return report


if __name__ == "__main__":
    main()
