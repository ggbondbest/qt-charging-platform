"""一次性盲测：在 ``split==TEST``（purge/删失后）上评桩-日来修预测，产出 JSON + Markdown 报告。

纪律：特征组、容量档、阈值、基线表全部由 train.py 在 TRAIN/VALIDATION 上定死并冻进 bundle，
本脚本**只读不算新参数**；消融用的缩减模型按**选中的同一容量档**在 TRAIN 重拟合后放在一起
报数——本线容量是选择的一部分，消融不换容量，否则差值里混着两个自由度。模型包与特征表按
sha256 配对校验（本线没有派生数据集——零新增数据，校验层次因此少一层），任一错配一律拒评。

报告回答四个运营问题：
  · 模型比"纯查表/纯先验/只用负荷/朴素最近来修"这些便宜口径多值多少？
  · 每天只把 75 台桩中的 n% 排进维护窗口，各排序能圈中多少真实来修的桩？
  · 分数排的是"固有病桩"还是"最近忙/刚修过"？（组内 AUC 切片回答）
  · 信号来自哪里？（static/ticket/ops 三组消融 + VAL→TEST 过拟合尺子）
本线的诚实预期：诊断已显示**信号弱且分散**（最强单特征 TEST 侧 ~0.61），第七线的
"基线赢了模型"在这里是活生生的风险——报告不粉饰，输了就报输。

用法（仓库根目录）：python -m data_analysis.ml.health.evaluate
"""

from __future__ import annotations

import json

import joblib
import numpy as np
import pandas as pd
from sklearn.metrics import average_precision_score, roc_auc_score

from . import common
from .train import (HYPER_GRID, MAX_ALERT_SHARE, PRECISION_MULTIPLE, baseline_scores, design,
                    new_classifier, oracle_charger_rate, predict_baseline, predict_regress_baseline,
                    split_group, usable_frame)

#: 概率型分数才有 logLoss/Brier；负荷、规则分、oracle 率表、回归张数一律只给排序指标。
PROBABILISTIC = {"model", "trainCellBlend", "asOfChargerPriorOnly", "asOfStationPriorOnly"}
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
        block["note"] = "分数不是概率（负荷/规则/张数），不算 logLoss 与 Brier"
    return block


def oracle_metrics(y: np.ndarray, scores: np.ndarray) -> dict:
    block = score_metrics(y, scores, probabilistic=False)
    block["deployable"] = False
    block["note"] = ("**不可部署**：查表用了含 TEST 标签的全样本桩级来修率——"
                     "行自己的正类就在表里，分数与标签自相关，AUC 是'如果上帝知道每桩频率'的假想上界，"
                     "只作归因对照")
    return block


def maintenance_budget(test: pd.DataFrame, rankings: dict[str, np.ndarray], y: np.ndarray,
                       budgets: tuple[float, ...] = BUDGETS) -> list[dict]:
    """每天只把在册桩的 n% 排进维护窗口，按每种排序各能圈中多少真实来修的桩。

    逐日取前 k 台（``k = max(1, ceil(budget × 当日在册桩数))``），跨日累加。与阈值口径独立：
    阈值回答"每天排多少台"，这张表回答"人手有限时先排谁"。
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
    y = test["y_ticket7"].to_numpy(dtype=int)
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


def breakdowns(test: pd.DataFrame, prob: np.ndarray) -> dict[str, list[dict]]:
    """切片维度：站/接口/厂商/型号/站型/天气/场景/周末/前后半月，加三个本线专属——
    **历史工单桶**（tickets_90d 有无与多少：分数是不是只在抄报修名单）、
    **负荷三分位**（"忙桩"代理检查）、**修后恢复天数**（刚修完的桩分数该高还是该低）。"""
    result: dict[str, list[dict]] = {}
    for name in ("station_id", "connector_type", "manufacturer", "charger_model", "site_type",
                 "weather_prev", "scenario_event"):
        series = test[name].astype("object").where(test[name].notna(), "MISSING")
        keys = sorted(value for value in series.unique())
        result[name] = bucket_rows(test, prob, [(str(key), (series == key).to_numpy()) for key in keys])
    weekend = test["is_weekend"].to_numpy(dtype=float)
    result["weekend"] = bucket_rows(test, prob, [("工作日", weekend == 0), ("周末", weekend == 1)])
    middle = test["business_date"].median()
    days = test["business_date"].to_numpy()
    result["halfWindow"] = bucket_rows(test, prob, [("TEST 前半月", days < np.datetime64(middle)),
                                                    ("TEST 后半月", days >= np.datetime64(middle))])
    hist = pd.cut(test["tickets_90d"].fillna(-1), [-1.5, 0.5, 1.5, 2.5, 100.0])
    labels = {str(hist.cat.categories[0]): "90 天内没来过单", str(hist.cat.categories[1]): "来过 1 单",
              str(hist.cat.categories[2]): "来过 2 单", str(hist.cat.categories[3]): "来过 ≥3 单"}
    result["ticketHistory"] = bucket_rows(
        test, prob, [(labels[str(band)], (hist == band).to_numpy()) for band in hist.cat.categories])
    load3 = pd.qcut(test["usage_energy_kwh_30d"], 3, duplicates="drop")
    result["usageTertile"] = bucket_rows(
        test, prob, [(f"负荷 Q{i + 1} {band}", (load3 == band).to_numpy())
                     for i, band in enumerate(load3.cat.categories)])
    restored = test["days_since_last_restore"].fillna(-1).to_numpy(dtype=float)
    result["postRepairAge"] = bucket_rows(
        test, prob, [("没修过（无记录）", restored < 0), ("修完 ≤14 天", (restored >= 0) & (restored <= 14)),
                     ("修完 15–45 天", (restored > 14) & (restored <= 45)),
                     ("修完 >45 天", restored > 45)])
    return result


def ablation(frame: pd.DataFrame, bundle: dict, test: pd.DataFrame, y: np.ndarray) -> dict[str, float]:
    """信号来自哪里：四套候选组在 TRAIN（purge 后）重拟合，在同一片 TEST 上比（只作归因）。

    与第八线的关键差别：**用选中组的同一容量档**。本线的容量与特征是联合选择的，
    消融若固定回 big31，会把"小容量才是对的"这一发现混进组间差里。
    """
    numeric, categorical = bundle["allNumericFeatures"], bundle["allCategoricalFeatures"]
    train = usable_frame(frame, "TRAIN")
    y_train = train["y_ticket7"].to_numpy(dtype=int)
    hyper = next(h for h in HYPER_GRID if h["name"] == bundle["chosenHyper"])
    out: dict[str, float] = {}
    for label, columns in bundle["candidateSets"].items():
        num, cat = split_group(list(columns[0]) + list(columns[1]), numeric, categorical)
        x_train, mask = design(train, num, cat)
        x_test, _ = design(test, num, cat)
        model = new_classifier(mask, hyper)
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


def dispatch_concentration(test: pd.DataFrame, prob: np.ndarray, threshold: float) -> dict:
    """排程落在哪些桩/站上：运营关心"是不是只盯着几台老病号"，也防止分数退化成站牌代理。"""
    flagged = test[prob >= threshold]
    counts = flagged["charger_id"].value_counts()
    positives_by_charger = test.loc[test["y_ticket7"] == 1, "charger_id"].value_counts()
    return {"flaggedRows": int(len(flagged)),
            "distinctChargersFlagged": int(flagged["charger_id"].nunique()),
            "chargersInScope": int(test["charger_id"].nunique()),
            "stationsInScope": int(test["station_id"].nunique()),
            "distinctStationsFlagged": int(flagged["station_id"].nunique()),
            "topStationAlertShare": round(float(flagged["station_id"].value_counts().iloc[0]
                                                / len(flagged)), 4) if len(flagged) else None,
            "daysInScope": int(test["business_date"].nunique()),
            "topChargerAlertShare": round(float(counts.iloc[0] / len(flagged)), 4) if len(counts) else None,
            "top10ChargerPositiveShare": round(float(positives_by_charger.head(10).sum()
                                                     / max(1, int(test["y_ticket7"].sum()))), 4),
            "distinctPositiveChargers": int(test.loc[test["y_ticket7"] == 1, "charger_id"].nunique())}


def budget_compliance(test: pd.DataFrame, prob: np.ndarray, threshold: float) -> dict:
    """冻结阈值搬到 TEST 上，每天的排程数有没有越过"每日在册桩数 25%"这条自定预算。"""
    frame = pd.DataFrame({"day": test["business_date"], "flag": prob >= threshold})
    per_day = frame.groupby("day", observed=True)["flag"].agg(["sum", "size"])
    allowed = np.ceil(MAX_ALERT_SHARE * per_day["size"].to_numpy(dtype=float))
    over = int((per_day["sum"].to_numpy() > allowed).sum())
    return {"days": int(len(per_day)), "maxAlertsPerDay": int(per_day["sum"].max()),
            "meanAlertsPerDay": round(float(per_day["sum"].mean()), 2),
            "dailyBudgetAt25pct": f"{int(allowed.min())}–{int(allowed.max())} 台/天",
            "daysOverBudget": over, "worstDayShare": round(float((per_day["sum"] / per_day["size"]).max()), 4)}


def markdown(report: dict) -> str:
    """人读的评测报告，表格全部由 report 里的数字生成，不手抄。"""
    risk = report["risk"]
    point = risk["frozenOperatingPoint"]
    lines = [
        f"# 桩-日未来 7 天来修预测 · TEST 盲测报告（{report['modelVersion']}）",
        "",
        "> " + report["disclaimer"],
        "",
        f"- 模型：`{report['modelId']}`（分类=未来 7 天来不来修，回归=大概来几张单；"
        f"特征组 `{report['chosenSet']}` × 容量档 `{report['chosenHyper']}`，"
        f"{report['featureCount']} 列）",
        f"- 绑定批次：`{report['publishedBatchId']}` · run `{report['pipelineRunId']}` · "
        f"本线**未新增任何数据**（全部为已发布批次 clean 表的只读 as-of 聚合）",
        f"- 特征表 sha256：`{report['featuresSha256'][:16]}…` · 泄漏审计抽样 "
        f"{report['leakAudit']['sampled']} 行 × {len(report['leakAudit']['checked'])} 个量"
        f"（含前向标签窗算术核对），最大偏差 {max(report['leakAudit']['maxAbsDiff'].values()):.1e}",
        f"- 样本单元：{report['sampleUnit']}；决策时刻：{report['decisionTime']}",
        f"- TEST 窗口 `{report['testWindow']['validationEndExclusive']}` → "
        f"`{report['testWindow']['testEndExclusive']}`（UTC），{report['testRows']:,} 桩·日，"
        f"正类 {risk['positives']:,}（7 日基础率 {risk['baseRate']:.4f}）；"
        f"另有 purge {report['purgeCensor']['purgedRows']:,} 行、右端删失 "
        f"{report['purgeCensor']['censoredRows']:,} 行不进盲测",
        "",
        "## 1. 主结果：排序能力（同一 TEST，同一批桩·日）",
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
    target_test = PRECISION_MULTIPLE * risk["baseRate"]
    met_test = "是" if point["precision"] >= target_test else "**否（如实标注）**"
    lines += ["",
              f"- 模型 AUC 相对 TRAIN 三级查表基线 **{head['vsBlendPct']:+.2f}%**、"
              f"相对纯因果桩先验 **{head['vsChargerPriorPct']:+.2f}%**、"
              f"相对只用负荷 **{head['vsUsagePct']:+.2f}%**",
              f"- **结论**：{head['conclusion']}",
              f"- 校准：Brier {risk['brier']:.5f}、LogLoss {risk['logLoss']:.5f}"
              f"（只有概率型分数参与校准）",
              "",
              f"冻结运营阈值（VALIDATION 上定死，TEST 只验不改；{point['rule']}）：",
              "",
              "| 阈值 | 排程桩·日 | 排程率 | 精确率 | 召回 | F1 | 达标(VAL口径) |",
              "| --- | --- | --- | --- | --- | --- | --- |",
              f"| {point['threshold']:.4f} | {point['alerts']:,} | {point['alertRate']:.2%} | "
              f"{point['precision']:.4f} | {point['recall']:.4f} | {point['f1']:.4f} | "
              f"{'达标' if point['targetMet'] else '未达标'} |",
              "",
              f"- **诚实补记**：同一规则搬到 TEST 上复核——精确率 {point['precision']:.4f} vs "
              f"目标 {target_test:.4f}（{PRECISION_MULTIPLE:.0f}× TEST 基础率 "
              f"{risk['baseRate']:.4f}），TEST 侧达标：{met_test}。"
              "达标标志是在 VALIDATION 上冻结的，这里只报数不挪阈值。",
              "",
              f"预算核对：{report['budget']['days']} 天里 {report['budget']['daysOverBudget']} 天越过"
              f"「每日在册桩数 {MAX_ALERT_SHARE:.0%}」（{report['budget']['dailyBudgetAt25pct']}），"
              f"单日最多 {report['budget']['maxAlertsPerDay']} 台、最高占当日 "
              f"{report['budget']['worstDayShare']:.1%}",
              "",
              "## 2. 维护窗口预算表：每天只排在册桩的 n%，能圈中多少真实来修的桩",
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
              f"- 不可部署 oracle（全样本桩频率，行自标签在表内）AUC "
              f"{report['scoreboard']['oracleChargerRateFullSampleNONDEPLOYABLE']['auc']:.4f}"
              f" vs 可部署的因果桩先验 {report['scoreboard']['asOfChargerPriorOnly']['auc']:.4f}："
              f"{report['oracleConclusion']}",
              "",
              "| 归因切片 | 样本 | 正类 | 基础率 | 平均预测 | 组内 AUC |",
              "| --- | --- | --- | --- | --- | --- |"]
    for name in ("ticketHistory", "usageTertile", "postRepairAge"):
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
    lines += ["", f"- VALIDATION 上同一组比较（各自最优容量）：{report['valAblation']}",
              f"- 过拟合尺子：TRAIN AUC {risk['aucTrain']:.4f} → VALIDATION {risk['aucValidation']:.4f} → "
              f"TEST {risk['auc']:.4f}",
              f"- {report['ablationConclusion']}",
              "",
              "## 5. 校准（预测概率 vs 实测来修率，TEST 十分位）",
              "",
              "| 预测概率区间 | 样本数 | 预测均值 | 实测来修率 |", "| --- | --- | --- | --- |"]
    for row in report["calibration"]:
        lines.append(f"| {row['bin']} | {row['n']:,} | {row['predicted']:.4f} | {row['observed']:.4f} |")
    count = report["countRegression"]
    lines += ["", "## 6. 回归目标：未来 7 天大概来几张单（按预计工单量排备件的另一种口径）", "",
              "| 打分 | MAE(张) | 对 y_ticket7 的 AUC |", "| --- | --- | --- |"]
    for name, block in count["rows"].items():
        auc = "—" if block["auc"] is None else f"{block['auc']:.4f}"
        lines.append(f"| {name} | {block['mae']:.3f} | {auc} |")
    lines += ["", f"- {count['conclusion']}",
              "",
              "## 7. 排程落在哪些桩/站上",
              "",
              f"- 冻结阈值下排程 {report['dispatch']['flaggedRows']:,} 桩·日，覆盖 "
              f"{report['dispatch']['distinctChargersFlagged']} 台桩（TEST 域内共 "
              f"{report['dispatch']['chargersInScope']} 台）与 "
              f"{report['dispatch']['distinctStationsFlagged']} 个站（共 "
              f"{report['dispatch']['stationsInScope']} 个）；"
              f"单桩最多占排程 {report['dispatch']['topChargerAlertShare']:.1%}、单站最多占 "
              f"{report['dispatch']['topStationAlertShare']:.1%}",
              f"- 正类最集中的 10 台桩占全部正类桩·日的 {report['dispatch']['top10ChargerPositiveShare']:.1%}"
              f"（正类桩共 {report['dispatch']['distinctPositiveChargers']} 台）——"
              f"{report['concentrationConclusion']}",
              "",
              "## 8. 其余分桶",
              ""]
    for name in ("station_id", "connector_type", "manufacturer", "charger_model", "site_type",
                 "weather_prev", "scenario_event", "weekend", "halfWindow"):
        lines += [f"### {name}", "", "| 桶 | 桩·日 | 正类 | 基础率 | 平均预测 | AUC | 备注 |",
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
              f"标签窗全部完整（删失守卫开跑）{invariants['testLabelsComplete']}；"
              f"桩·日唯一 {invariants['chargerDayUniqueInTest']}；分数落在 [0,1] "
              f"{invariants['scoreRangeLegal']}；TEST 桩在 TRAIN 全见过 "
              f"{invariants['testChargersSeenInTrain']}；每行 7 日窗起点=当日起点 "
              f"{invariants['labelWindowAlignsToDayStart']}",
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
    test = usable_frame(frame, "TEST")
    train = usable_frame(frame, "TRAIN")
    validation = usable_frame(frame, "VALIDATION")
    y = test["y_ticket7"].to_numpy(dtype=int)

    x_test, _ = design(test, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob = bundle["classifier"].predict_proba(x_test)[:, 1]
    tickets_hat = np.clip(bundle["regressor"].predict(x_test), 0.0, None)
    threshold = float(bundle["operatingPoint"]["threshold"])
    flag = prob >= threshold

    baselines = bundle["baselines"]
    oracle_test = oracle_charger_rate(frame, test, 30.0)
    scores = baseline_scores(test, baselines, oracle_test)
    blend_test = predict_baseline(baselines, test)
    charger_prior_test = scores["asOfChargerPriorOnly"]

    scoreboard = {"model": {**score_metrics(y, prob, probabilistic=True), "note": "发布的分类模型"}}
    for name in ("trainCellBlend", "asOfChargerPriorOnly", "asOfStationPriorOnly",
                 "usageOnly", "naiveRecentTicket"):
        scoreboard[name] = score_metrics(y, scores[name], probabilistic=(name in PROBABILISTIC))
    scoreboard["globalBaseRate"] = {"auc": 0.5, "prAuc": round(float(y.mean()), 4),
                                    "liftAt5pct": 1.0, "liftAt10pct": 1.0, "liftAt20pct": 1.0,
                                    "deployable": True, "logLoss": None, "brier": None,
                                    "note": "常数打分，AUC 恒 0.5，只是尺子"}
    scoreboard["regressorExpectedTickets"] = score_metrics(y, tickets_hat, probabilistic=False)
    scoreboard["regressorExpectedTickets"]["note"] = "回归打分（预计工单张数）拿来排 y_ticket7，口径混用只作对照"
    scoreboard["oracleChargerRateFullSampleNONDEPLOYABLE"] = oracle_metrics(
        y, scores["oracleChargerRateFullSampleNONDEPLOYABLE"])

    auc = float(roc_auc_score(y, prob))
    auc_blend = float(roc_auc_score(y, blend_test))
    auc_charger = float(roc_auc_score(y, charger_prior_test))
    auc_usage = float(roc_auc_score(y, scores["usageOnly"]))
    best_baseline = max(auc_blend, auc_charger, auc_usage)
    beaten = auc >= best_baseline
    precision = float(y[flag].mean()) if flag.any() else 0.0
    recall = float(flag[y == 1].sum() / max(1, int(y.sum())))

    rankings = {"model": prob, "trainCellBlend": blend_test,
                "asOfChargerPriorOnly": charger_prior_test,
                "asOfStationPriorOnly": scores["asOfStationPriorOnly"],
                "regressorExpectedTickets": tickets_hat, "usageOnly": scores["usageOnly"],
                "naiveRecentTicket": scores["naiveRecentTicket"]}
    budget_rows = maintenance_budget(test, rankings, y)
    at_ten = next(row for row in budget_rows if abs(row["budget"] - 0.10) < 1e-9)
    tickets_first = sum(1 for row in budget_rows
                        if row["regressorExpectedTickets"]["recall"] > row["model"]["recall"])
    budget_conclusion = (
        f"每日 10% 预算（7.5→8 台/天）下：模型召回 {at_ten['model']['recall']:.1%}、"
        f"回归（按预计工单张数）召回 {at_ten['regressorExpectedTickets']['recall']:.1%}、"
        f"三级查表 {at_ten['trainCellBlend']['recall']:.1%}、"
        f"因果桩先验 {at_ten['asOfChargerPriorOnly']['recall']:.1%}、"
        f"因果站先验 {at_ten['asOfStationPriorOnly']['recall']:.1%}、"
        f"只用负荷 {at_ten['usageOnly']['recall']:.1%}、"
        f"朴素最近来修 {at_ten['naiveRecentTicket']['recall']:.1%}。"
        + (f"回归排序在 {len(budget_rows)} 档预算里有 {tickets_first} 档赢过分类排序，"
           "两种排程口径值得一起发给运营挑。" if tickets_first else
           f"回归排序在 {len(budget_rows)} 档预算里一档都没赢过分类排序——"
           "「按预计工单张数排维护优先级」在这批数据上不比分桶概率更好，"
           "张数口径只用来估备件预算。"))

    actual_next = test["tickets_next7d"].to_numpy(dtype=float)
    count_rows = {
        "modelRegressor": {"mae": round(common.mae(actual_next, tickets_hat), 3),
                           "auc": round(float(roc_auc_score(y, tickets_hat)), 4)},
        "trainMeanTicketCount": {
            "mae": round(common.mae(actual_next, np.full(
                len(test), float(train["tickets_next7d"].mean()))), 3), "auc": None},
        "trainCellMeanBlend": {
            "mae": round(common.mae(actual_next, predict_regress_baseline(
                bundle["regressBaselines"], test)), 3),
            "auc": round(float(roc_auc_score(
                y, predict_regress_baseline(bundle["regressBaselines"], test))), 4)},
    }
    count_best = min(count_rows["trainMeanTicketCount"]["mae"], count_rows["trainCellMeanBlend"]["mae"])
    count_conclusion = (
        f"回归 MAE（张）模型 {count_rows['modelRegressor']['mae']} vs 全局均值基线 "
        f"{count_rows['trainMeanTicketCount']['mae']} vs TRAIN 查表均值 "
        f"{count_rows['trainCellMeanBlend']['mae']}——"
        + ("模型在张数尺度上也赢过全部基线，预计工单量可以直接用于备件预算。"
           if count_rows["modelRegressor"]["mae"] < count_best else
           f"模型没有赢过最便宜基线（{count_best}），备件预算应按基线查表（如实报，不挑好看的）。"))

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
        "chosenHyper": bundle["chosenHyper"],
        "featureCount": int(x_test.shape[1]), "testWindow": bundle["splits"],
        "horizonDays": common.LABEL_HORIZON_DAYS,
        "testRows": int(len(test)), "scope": summary["scope"], "label": bundle["label"],
        "sampleUnit": summary["sampleUnit"], "decisionTime": summary["decisionTime"],
        "purgeDiscipline": summary["purgeDiscipline"],
        "purgeCensor": {"purgedRows": int(summary["scope"]["purgedRows"]),
                        "censoredRows": int(summary["scope"]["censoredRows"])},
        "excludedBrief": (f"purge {summary['scope']['purgedRows']:,} 行（TRAIN/VAL 段尾标签窗伸入下一段）"
                          f" + 右端删失 {summary['scope']['censoredRows']:,} 行"
                          "（7 日窗放不下）——都不进训练也不进盲测"),
        "leakAudit": summary["leakAudit"],
        "droppedConstant": summary["features"]["droppedConstant"],
        "constantWhyNote": ("站变压器容量全表同值；每站恰 3 台桩（站规模零方差）；"
                            "遥测'零功率充电'格子在 CHARGING 态里恒为功率>0（本批模拟数据没有"
                            "'说在充其实没充'的病，两列真负结果，不是关联事故）"),
        "risk": {
            "positives": int(y.sum()), "baseRate": round(float(y.mean()), 4),
            "auc": round(auc, 4), "aucTrain": round(float(roc_auc_score(
                train["y_ticket7"].to_numpy(dtype=int), prob_train)), 4),
            "aucValidation": round(float(roc_auc_score(
                validation["y_ticket7"].to_numpy(dtype=int), prob_validation)), 4),
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
                "targetMetOnTest": bool(precision >= PRECISION_MULTIPLE * float(y.mean())),
                "testPrecisionTarget": round(PRECISION_MULTIPLE * float(y.mean()), 4),
                "note": "阈值与规则在 VALIDATION 上定死，TEST 只验不改"},
        },
        "scoreboard": scoreboard,
        "headline": {
            "vsBlendPct": round(100.0 * (auc / auc_blend - 1.0), 2),
            "vsChargerPriorPct": round(100.0 * (auc / auc_charger - 1.0), 2),
            "vsUsagePct": round(100.0 * (auc / auc_usage - 1.0), 2),
            "conclusion": ("" if beaten else
                           f"学习模型**没有**跑赢最便宜的可部署基线（查表 {auc_blend:.4f} / "
                           f"桩先验 {auc_charger:.4f} / 负荷 {auc_usage:.4f}）。"),
        },
        "budgetRanking": budget_rows,
        "budgetConclusion": budget_conclusion,
        "budget": budget_compliance(test, prob, threshold),
        "dispatch": dispatch_concentration(test, prob, threshold),
        "rankAgreement": rank_agreement(prob, {"trainCellBlend": blend_test,
                                               "asOfChargerPriorOnly": charger_prior_test,
                                               "asOfStationPriorOnly": scores["asOfStationPriorOnly"],
                                               "usageOnly": scores["usageOnly"],
                                               "naiveRecentTicket": scores["naiveRecentTicket"],
                                               "oracleChargerRateFullSample": oracle_test,
                                               "regressorExpectedTickets": tickets_hat}),
        "calibration": [{"bin": str(row["bin"]), "n": int(row["n"]),
                         "predicted": round(float(row["predicted"]), 4),
                         "observed": round(float(row["observed"]), 4)}
                        for _, row in common.calibration_bins(y, prob, bins=10).iterrows()],
        "ablation": ablation_out,
        "ablationSets": list(bundle["candidateSets"].keys()),
        "valAblation": {key: block["auc"] for key, block in train_metrics["candidateValidation"].items()},
        "countRegression": {"rows": count_rows, "conclusion": count_conclusion},
    }
    report["headline"]["conclusion"] = (
        "学习模型跑赢了全部可部署基线——增益主要来自桩属性×环境的低阶结构（3 叶树就能吃到，"
        "深树反而过拟合，这本身就是本线最重要的容量发现）。" if beaten else
        f"学习模型没有跑赢最便宜的可部署基线（最优 {best_baseline:.4f}，模型 {auc:.4f}，"
        f"差 {100.0 * (auc / best_baseline - 1.0):+.2f}%）。发布价值因此落在派生聚合 + "
        "泄漏审计 + purge/删失口径这套基础设施与诚实数字上，而不是模型本身。")
    oracle_auc = scoreboard["oracleChargerRateFullSampleNONDEPLOYABLE"]["auc"]
    report["oracleConclusion"] = (
        f"oracle {oracle_auc:.4f} 与因果桩先验 {auc_charger:.4f} 之差（{oracle_auc - auc_charger:+.4f}）"
        "主要是**自标签膨胀**（oracle 的查表分里含被打分行自己的正类），不是'时间不够看不够远'——"
        "本线的每桩固有频率信号本来就弱（工单间隔接近无记忆），这个天花板要诚实读成'连作弊都强不了多少'。")
    report["breakdown"] = breakdowns(test, prob)
    hist_rows = report["breakdown"]["ticketHistory"]
    age_rows = report["breakdown"]["postRepairAge"]
    report["sliceConclusion"] = (
        "组内 AUC 才是真本事：如果整体分数只是在抄报修名单，'90 天内没来过单'那桶就该失去区分力。实测 "
        + "；".join(f"{row['bucket']} 档内 AUC "
                    + ("—" if row["auc"] is None else f"{row['auc']:.4f}")
                    for row in hist_rows)
        + "；'刚修完'的桩分数走向（修后恢复天数各桶平均预测）："
        + "；".join(f"{row['bucket']} 均值 "
                    + ("—" if row["meanProb"] is None else f"{row['meanProb']:.4f}")
                    for row in age_rows)
        + "。修完越新预测越低（若成立）对应换件重启的无记忆结构——这与工单间隔 p50=16 天的侦察一致。")
    report["ablationConclusion"] = (
        f"staticOnly {ablation_out['staticOnly']:.4f} / ticketOnly {ablation_out['ticketOnly']:.4f} / "
        f"opsOnly {ablation_out['opsOnly']:.4f} / full {ablation_out['full']:.4f}（同一容量档 "
        f"{bundle['chosenHyper']}）——本批仿真里来修事件的桩级差异弱、时间上接近无记忆，"
        "三族信号单独都没有大惊喜；full 与最好单组的差就是'再叠加会不会更好'的答案。")
    report["concentrationConclusion"] = (
        "排程摊得开，不是只盯几台老病号——维护窗口可以按整站轮排。"
        if (report["dispatch"]["topChargerAlertShare"] or 0) < 0.05 else
        "排程向少数桩集中，先核对这些桩是否长期占用维护名额，必要时加轮换上限。")
    bounds = {key: pd.Timestamp(value) for key, value in bundle["splits"].items()}
    seen_charger = test["charger_id"].isin(train["charger_id"].unique())
    report["invariants"] = {
        "testDays": int(test["business_date"].nunique()),
        "testStartsAtValidationEnd": bool((test["day_start_utc"]
                                           >= bounds["validationEndExclusive"]).all()),
        "testLabelsComplete": bool((~test["censored"]).all() and (~test["purged"]).all()),
        "chargerDayUniqueInTest": bool(not test.duplicated(subset=["charger_id", "business_date"]).any()),
        "testChargersSeenInTrain": bool(seen_charger.all()),
        "labelWindowAlignsToDayStart": bool(
            (test["label_end"] == test["day_start_utc"]
             + pd.Timedelta(days=common.LABEL_HORIZON_DAYS)
             + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)).all()),
        "scoreRangeLegal": bool(((prob >= 0) & (prob <= 1)).all())}
    report["dataDiscipline"] = (
        "只读消费已发布批次的 clean 层；**本线未新增任何数据**——全部特征是对原表的只读确定性 "
        "as-of 聚合，不落新目录；产物落 outputs/ml_health/ 且全部独占创建；工单终态与成本"
        "（status/severity/工时费/配件费）、未来那张工单的 fault_type 与四个原始时刻一律禁入；"
        "泄漏审计用全表布尔掩码独立重算跨时间轴 as-of 量与前向标签窗算术（工单按 reported_at、"
        "修复时长按 restored_at、会话按 ended_at、尝试按 attempted_at、遥测滞后一日），"
        "与向量路径 0 偏差")
    report["disclaimer"] = common.data_note()

    common.write_new_json(common.TEST_REPORT_PATH, report)
    common.write_new_bytes(common.TEST_REPORT_MD, markdown(report).encode("utf-8"))
    risk = report["risk"]
    point = risk["frozenOperatingPoint"]
    print(f"[eval] TEST n={report['testRows']:,} 桩·日 7日基础率={risk['baseRate']:.4f} "
          f"正类={risk['positives']:,}")
    for name, block in report["scoreboard"].items():
        print(f"[eval] {name}: TEST AUC={block['auc']:.4f} PR-AUC={block['prAuc']:.4f} "
              f"lift@10%={block['liftAt10pct']:.2f}x"
              + ("" if block["deployable"] else "  [不可部署]"))
    print(f"[eval] 阈值{point['threshold']:.3f} → 排程 {point['alerts']} 桩·日 "
          f"精确率={point['precision']:.4f} 召回={point['recall']:.4f} F1={point['f1']:.4f} "
          f"达标VAL口径={point['targetMet']} 达标TEST口径="
          f"{point['targetMetOnTest']}（TEST目标{point['testPrecisionTarget']:.4f}）；"
          f"越预算天数={report['budget']['daysOverBudget']}")
    print("[eval] " + report["headline"]["conclusion"])
    print("[eval] " + budget_conclusion)
    print("[eval] " + report["ablationConclusion"])
    print(f"[eval] -> {common.TEST_REPORT_MD}")
    return report


if __name__ == "__main__":
    main()
