"""滚动重训：把"这套东西稳不稳"和"一次性盲测"分开回答。

第十线之前的老问题：单次 TEST 盲测只有一条时间线上的一个数，赢了也可能是窗口运气。
本脚本在 **TEST 域之外**（全部落在 TRAIN/VALIDATION 日历内，一次都不碰 TEST）做 10 轮
"用过去 7 日窗之前的行训练 → 预测接下来 14 日窗"的滚动评估，回答四个问题：

  1. 模型赢便宜查表基线是不是每轮都赢（符号检验给 p 值），还是只赢在盲测那一段？
  2. 工单历史信号会不会"老化"——训练窗越靠后，ticketOnly 组单独重训的 AUC 是否走低？
  3. 遥测/负荷/尝试这三族 ops 信号在滚动里值不值它们的行数（opsOnly 逐轮 AUC）？
  4. 冻结阈值（VALIDATION 上定死的那个）搬到每个滚动窗，精确率还站不站得住 2×基础率？

与盲测的关系要说清楚：**滚动数字不是盲测指标**——它的训练窗与评估窗都来自盲测之前的
数据，容量与特征组也已经在 VALIDATION 上被看过一眼（选择污染了这些轮次的乐观偏差），
它检验的是稳定性与时间外推形状，不是"模型没见过的数据上行不行"。后一句话只有
evaluate.py 的 TEST 能回答。

用法（仓库根目录）：python -m data_analysis.ml.health.rolling
"""

from __future__ import annotations

import json
from math import comb

import joblib
import numpy as np
import pandas as pd
from sklearn.metrics import average_precision_score, roc_auc_score

from . import common
from .train import (HYPER_GRID, MAX_ALERT_SHARE, PRECISION_MULTIPLE,
                    design, fit_baselines, new_classifier, predict_baseline, split_group)

ROUNDS = 10
FIRST_EVAL_POSITION = 48   # 第一轮评估窗起点在"可用日序列"中的位置（此前全做训练）
STEP_POSITIONS = 8         # 相邻轮次起点间隔（可用日）
WINDOW_DAYS = 14           # 每轮评估窗长度（北京日历日）
PURGE_DAYS = common.PURGE_DAYS


def sign_test_p(wins: int, n: int) -> float:
    """双侧精确符号检验（H0: 每轮五五开），只用在轮次独立近似成立的地方。"""
    if wins * 2 == n:
        return 1.0
    k = max(wins, n - wins)
    tail = sum(comb(n, i) for i in range(k, n + 1)) / (2 ** n)
    return round(min(1.0, 2.0 * tail), 6)


def fold_plan(usable_days: pd.DatetimeIndex) -> list[tuple[pd.Timestamp, pd.Timestamp]]:
    """10 轮 (评估窗起, 评估窗止) ——起于可用日序列第 48 个位置，每轮右移 8 个可用日。"""
    assert len(usable_days) >= FIRST_EVAL_POSITION + STEP_POSITIONS * (ROUNDS - 1) + WINDOW_DAYS, \
        "可用日不足以铺 10 轮滚动窗"
    plan = []
    for r in range(ROUNDS):
        start = usable_days[FIRST_EVAL_POSITION + STEP_POSITIONS * r]
        plan.append((start, start + pd.Timedelta(days=WINDOW_DAYS)))
    return plan


def load_pair() -> tuple[pd.DataFrame, dict]:
    """滚动必须用**发布的那对**模型规格与阈值：bundle + 特征表哈希复核，同 evaluate.py。"""
    if not common.BUNDLE_PATH.exists():
        raise FileNotFoundError(f"缺少模型包 {common.BUNDLE_PATH}，请先跑 train")
    bundle = joblib.load(common.BUNDLE_PATH)
    common.verify_batch()
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    digest = common.sha256_file(common.FEATURES_PATH)
    if summary.get("featuresSha256") != digest or digest != bundle["featuresSha256"]:
        raise common.BatchMismatch("特征表/模型包哈希配对复核失败，请先重跑 features + train")
    if bundle.get("publishedBatchId") != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch("模型包绑定批次与当前数据层不一致")
    return pd.read_pickle(common.FEATURES_PATH), bundle


def run_round(r: int, window: tuple[pd.Timestamp, pd.Timestamp], frame: pd.DataFrame,
              bundle: dict) -> dict:
    """一轮：7 日 embargo 之前的行训练 → 14 日窗评估，四组候选各按发布容量重拟合。"""
    eval_start, eval_end = window
    in_region = frame["split"].isin(["TRAIN", "VALIDATION"])
    usable = frame[in_region & ~frame["purged"] & ~frame["censored"]]
    # purge 纪律用真实日历距离：训练行的 7 日标签窗必须在评估窗起点之前收口。
    train = usable[usable["business_date"] + pd.Timedelta(days=PURGE_DAYS) < eval_start]
    test = usable[(usable["business_date"] >= eval_start) & (usable["business_date"] < eval_end)]
    y = test["y_ticket7"].to_numpy(dtype=int)
    y_train = train["y_ticket7"].to_numpy(dtype=int)
    hyper = next(h for h in HYPER_GRID if h["name"] == bundle["chosenHyper"])
    numeric, categorical = bundle["allNumericFeatures"], bundle["allCategoricalFeatures"]

    models: dict[str, dict] = {}
    for label, columns in bundle["candidateSets"].items():
        num, cat = split_group(list(columns[0]) + list(columns[1]), numeric, categorical)
        x_train, mask = design(train, num, cat)
        x_test, _ = design(test, num, cat)
        model = new_classifier(mask, hyper)
        model.fit(x_train, y_train)
        prob = model.predict_proba(x_test)[:, 1]
        models[label] = {"auc": round(float(roc_auc_score(y, prob)), 4),
                         "prAuc": round(float(average_precision_score(y, prob)), 4),
                         "featureCount": int(x_train.shape[1]),
                         "_prob": prob}
    chosen_auc = models[bundle["chosenSet"]]
    baselines = fit_baselines(train)
    blend = predict_baseline(baselines, test)
    scores = {"blendRefit": round(float(roc_auc_score(y, blend)), 4),
              "asOfChargerPrior": round(float(roc_auc_score(
                  y, test["charger_ticket_rate_shrunk"].to_numpy(dtype=float))), 4),
              "usageOnly": round(float(roc_auc_score(
                  y, test["usage_energy_kwh_30d"].to_numpy(dtype=float))), 4)}
    threshold = float(bundle["operatingPoint"]["threshold"])
    prob = models[bundle["chosenSet"]]["_prob"]
    flag = prob >= threshold
    precision = float(y[flag].mean()) if flag.any() else 0.0
    per_day = test.groupby("business_date")["charger_id"].count()
    cap = float(np.ceil(MAX_ALERT_SHARE * float(per_day.mean())))
    daily_flags = (pd.Series(flag.astype(int))
                   .groupby(test["business_date"].to_numpy()).sum()
                   .reindex(per_day.index, fill_value=0))
    days_over = int((daily_flags > cap).sum())
    for block in models.values():
        block.pop("_prob", None)
    return {
        "round": r,
        "trainRows": int(len(train)), "trainPositives": int(y_train.sum()),
        "trainStartDate": str(train["business_date"].min().date()),
        "evalStart": str(eval_start.date()), "evalEnd": str((eval_end - pd.Timedelta(days=1)).date()),
        "evalRows": int(len(test)), "evalDays": int(test["business_date"].nunique()),
        "evalPositives": int(y.sum()), "baseRate": round(float(y.mean()), 4),
        "models": models, "baselines": scores,
        "winVsBlend": bool(chosen_auc["auc"] > scores["blendRefit"]),
        "winVsChargerPrior": bool(chosen_auc["auc"] > scores["asOfChargerPrior"]),
        "deltaVsBlend": round(chosen_auc["auc"] - scores["blendRefit"], 4),
        "frozenThreshold": {"threshold": round(threshold, 4),
                            "alerts": int(flag.sum()),
                            "alertShare": round(float(flag.mean()), 4),
                            "precision": round(precision, 4),
                            "recall": round(float(flag[y == 1].sum() / max(1, int(y.sum()))), 4),
                            "meetsTwiceBase": bool(precision >= PRECISION_MULTIPLE * float(y.mean())),
                            "dailyCap": cap, "daysOverBudget": days_over},
    }


def main() -> dict:
    common.require_empty_run_dir(common.ROLLING_DIR)
    common.ROLLING_ROUNDS_DIR.mkdir(parents=True, exist_ok=True)
    frame, bundle = load_pair()
    bounds = {key: pd.Timestamp(value) for key, value in bundle["splits"].items()}

    usable_region = frame[frame["split"].isin(["TRAIN", "VALIDATION"])
                          & ~frame["purged"] & ~frame["censored"]]
    usable_days = pd.DatetimeIndex(sorted(usable_region["business_date"].unique()))
    assert usable_days.max() < bounds["validationEndExclusive"] + pd.Timedelta(
        days=common.BUSINESS_OFFSET_HOURS), "滚动日历越过了 VALIDATION 右端"
    plan = fold_plan(usable_days)
    last_end = plan[-1][1]
    assert last_end <= bounds["validationEndExclusive"], \
        f"最后一轮评估窗 {last_end.date()} 伸进 TEST 域（{bounds['validationEndExclusive'].date()} 起）"

    rounds: list[dict] = []
    for r, window in enumerate(plan):
        record = run_round(r, window, frame, bundle)
        assert pd.Timestamp(record["evalEnd"]) < bounds["validationEndExclusive"], \
            "不变量：任何滚动轮次不得读取 TEST 日历"
        rounds.append(record)
        common.write_new_json(common.ROLLING_ROUNDS_DIR / f"round_{r:02d}.json", record)
        print(f"[roll] 轮{r:02d} 评估窗 {record['evalStart']}..{record['evalEnd']} "
              f"n={record['evalRows']} 基础率={record['baseRate']:.4f} "
              f"模型({bundle['chosenSet']})={record['models'][bundle['chosenSet']]['auc']:.4f} "
              f"vs查表={record['baselines']['blendRefit']:.4f} "
              f"vs桩先验={record['baselines']['asOfChargerPrior']:.4f} "
              f"阈值精确率={record['frozenThreshold']['precision']:.4f}")

    wins_blend = sum(1 for row in rounds if row["winVsBlend"])
    wins_prior = sum(1 for row in rounds if row["winVsChargerPrior"])
    ticket_aucs = [row["models"]["ticketOnly"]["auc"] for row in rounds]
    ops_aucs = [row["models"]["opsOnly"]["auc"] for row in rounds]
    static_aucs = [row["models"]["staticOnly"]["auc"] for row in rounds]
    halves = {"first5": round(float(np.mean(ticket_aucs[:5])), 4),
              "last5": round(float(np.mean(ticket_aucs[5:])), 4)}
    ops_halves = {"first5": round(float(np.mean(ops_aucs[:5])), 4),
                  "last5": round(float(np.mean(ops_aucs[5:])), 4)}
    meets = sum(1 for row in rounds if row["frozenThreshold"]["meetsTwiceBase"])
    precisions = [row["frozenThreshold"]["precision"] for row in rounds
                  if row["frozenThreshold"]["alerts"] > 0]

    summary = {
        "modelId": bundle["modelId"], "chosenSet": bundle["chosenSet"],
        "chosenHyper": bundle["chosenHyper"],
        "publishedBatchId": bundle["publishedBatchId"], "pipelineRunId": bundle["pipelineRunId"],
        "featuresSha256": bundle["featuresSha256"],
        "frozenThreshold": float(bundle["operatingPoint"]["threshold"]),
        "horizonDays": common.LABEL_HORIZON_DAYS, "purgeDays": PURGE_DAYS,
        "foldRule": (f"评估窗=可用日序列第 {FIRST_EVAL_POSITION}+{STEP_POSITIONS}r 个位置起 "
                     f"{WINDOW_DAYS} 天；训练行=评估窗起点前 {PURGE_DAYS} 日（真实日历距离）之前；"
                     "TEST 域日历一律不进"),
        "notABlindTest": ("滚动轮次全部落在 TRAIN/VALIDATION 日历内，且特征组与容量已在 "
                          "VALIDATION 上被选择过——轮次数字有乐观偏差，只用于稳定性检验，"
                          "不能当盲测报告"),
        "rounds": rounds,
        "questions": {
            "q1_winVsCheapTable": {
                "winsVsBlend": f"{wins_blend}/{ROUNDS}", "pVsBlend": sign_test_p(wins_blend, ROUNDS),
                "winsVsChargerPrior": f"{wins_prior}/{ROUNDS}",
                "pVsChargerPrior": sign_test_p(wins_prior, ROUNDS),
                "meanDeltaVsBlend": round(float(np.mean([row["deltaVsBlend"] for row in rounds])), 4)},
            "q2_ticketHistoryAgeing": {
                "ticketOnlyPerRound": ticket_aucs,
                "first5MinusLast5": round(halves["first5"] - halves["last5"], 4),
                "spearmanRoundVsAuc": round(float(pd.Series(range(ROUNDS)).corr(
                    pd.Series(ticket_aucs), method="spearman")), 4)},
            "q3_opsEarnPlace": {
                "opsOnlyPerRound": ops_aucs, "first5MinusLast5": round(
                    ops_halves["first5"] - ops_halves["last5"], 4),
                "meanByGroup": {"staticOnly": round(float(np.mean(static_aucs)), 4),
                                "ticketOnly": round(float(np.mean(ticket_aucs)), 4),
                                "opsOnly": round(float(np.mean(ops_aucs)), 4),
                                "full": round(float(np.mean(
                                    [row["models"]["full"]["auc"] for row in rounds])), 4)}},
            "q4_thresholdStability": {
                "roundsMeetingTwiceBase": f"{meets}/{ROUNDS}",
                "precisionMin": round(min(precisions), 4), "precisionMax": round(max(precisions), 4),
                "precisionMean": round(float(np.mean(precisions)), 4),
                "daysOverBudgetTotal": sum(row["frozenThreshold"]["daysOverBudget"]
                                           for row in rounds)},
        },
        "dataNote": common.data_note(),
    }
    lines = [
        "# 桩-日未来 7 天来修预测 · 滚动重训 10 轮", "",
        "> " + summary["notABlindTest"], "",
        "> " + summary["dataNote"], "",
        f"- 模型对：`{bundle['modelId']}` {bundle['chosenSet']}|{bundle['chosenHyper']} · "
        f"阈值 {summary['frozenThreshold']:.4f} · 规则：{summary['foldRule']}", "",
        "| 轮 | 评估窗 | n | 基础率 | 模型 | 查表 | 桩先验 | 负荷 | ticketOnly | opsOnly | 阈值精确率 |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for row in rounds:
        frozen = row["frozenThreshold"]
        lines.append(
            f"| {row['round']} | {row['evalStart']}..{row['evalEnd']} | {row['evalRows']:,} | "
            f"{row['baseRate']:.4f} | {row['models'][bundle['chosenSet']]['auc']:.4f} | "
            f"{row['baselines']['blendRefit']:.4f} | {row['baselines']['asOfChargerPrior']:.4f} | "
            f"{row['baselines']['usageOnly']:.4f} | {row['models']['ticketOnly']['auc']:.4f} | "
            f"{row['models']['opsOnly']['auc']:.4f} | {frozen['precision']:.4f} |")
    q = summary["questions"]
    lines += ["",
              f"**Q1 赢查表稳不稳**：{q['q1_winVsCheapTable']['winsVsBlend']} 轮赢查表基线"
              f"（符号检验 p={q['q1_winVsCheapTable']['pVsBlend']}），"
              f"{q['q1_winVsCheapTable']['winsVsChargerPrior']} 轮赢因果桩先验；"
              f"ΔAUC(对查表)均值 {q['q1_winVsCheapTable']['meanDeltaVsBlend']:+.4f}",
              f"**Q2 工单历史老化吗**：ticketOnly 前 5 轮均值 {q['q2_ticketHistoryAgeing']['first5MinusLast5']:+.4f}"
              f"（对轮序 Spearman {q['q2_ticketHistoryAgeing']['spearmanRoundVsAuc']}）",
              f"**Q3 ops 族值不值**：opsOnly 十轮均值 "
              f"{q['q3_opsEarnPlace']['meanByGroup']['opsOnly']:.4f} vs staticOnly "
              f"{q['q3_opsEarnPlace']['meanByGroup']['staticOnly']:.4f} vs ticketOnly "
              f"{q['q3_opsEarnPlace']['meanByGroup']['ticketOnly']:.4f} vs full "
              f"{q['q3_opsEarnPlace']['meanByGroup']['full']:.4f}",
              f"**Q4 冻结阈值稳不稳**：{q['q4_thresholdStability']['roundsMeetingTwiceBase']} 轮精确率"
              f"站住 2×当窗基础率；各轮精确率 "
              f"{q['q4_thresholdStability']['precisionMin']:.4f}–{q['q4_thresholdStability']['precisionMax']:.4f}"
              f"；越预算天数合计 {q['q4_thresholdStability']['daysOverBudgetTotal']}", "",
              "每轮明细在 `rounds/round_NN.json`，本文件与 JSON 同源生成。"]
    common.write_new_json(common.ROLLING_SUMMARY_PATH, summary)
    common.write_new_text(common.ROLLING_SUMMARY_MD, "\n".join(lines) + "\n")
    print(f"[roll] Q1 {q['q1_winVsCheapTable']['winsVsBlend']} 赢查表 "
          f"p={q['q1_winVsCheapTable']['pVsBlend']}；Q4 {q['q4_thresholdStability']['roundsMeetingTwiceBase']}"
          f" 轮阈值达标")
    print(f"[roll] -> {common.ROLLING_SUMMARY_MD}")
    return summary


if __name__ == "__main__":
    main()
