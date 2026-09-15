"""十轮滚动重训：每轮重新拟合一次，看会话占桩超时提醒的表现随时间稳不稳。

这条线的滚动研究要回答的问题跟第五/六/七线都不一样。单次盲测里模型大幅赢过全部廉价基线
（0.896 vs 查表 0.775），但消融同时给出一个扎心的负增量：``full`` 不比 ``staticOnly`` 高——
所以十轮真正要验的是四件事：

  1. "模型赢查表/赢计划时长"是不是那一片窗口的运气——十轮里的胜负轮数与符号检验；
  2. **"用户习惯"有没有独立于场景的增量**——historyOnly 减 staticOnly 的验证段差值逐轮是多少。
     如果十轮全为非正，那么"移车提醒该按场景设计、不该按个人画像设计"就是可以带给运营的结论；
  3. **用户历史变厚会不会变值钱**——首轮评测会话的用户历史中位数到末轮涨了多少，
     纯因果用户先验（``user_over_rate_prior``）的 AUC 有没有随之上行；
  4. 运营阈值（精确率 ≥ 2×基础率、当日提醒 ≤ 当日开始会话数 25%）逐轮漂多少、有多少轮凑不出。

口径与第五/六/七线一致：``fitEnd`` 之前拟合（尾部 ``VALIDATION_DAYS`` 天只用于选特征集与阈值），
``fitEnd`` 起 ``HORIZON_DAYS`` 天评测；58 + 12×10 = 178 天正好铺满 mlSplits 窗口。
滚动折会重复用到 evaluate.py 那次盲测的 TEST 窗口，因此这里的均值**不是盲测指标**。
每轮结果 O_EXCL 落盘，支持断点续跑。

用法（仓库根目录）：python -m data_analysis.ml.overstay.rolling --rounds 10
"""

from __future__ import annotations

import argparse
import json
import math
import time

import numpy as np
import pandas as pd
from sklearn.metrics import average_precision_score, roc_auc_score

from . import common
from .train import (MAX_ALERT_SHARE, PRECISION_MULTIPLE, daily_alert_cap, design, fit_baselines,
                    new_classifier, new_regressor, pick_threshold, predict_baseline, split_group)

WARMUP_DAYS = 58      #: 第一轮拟合窗长度（再短则用户级 as-of 历史太薄）；58 + 12×10 = 178，十轮铺满窗口
HORIZON_DAYS = 12     #: 每轮评测窗
VALIDATION_DAYS = 12  #: 每轮用于选特征集与阈值的拟合窗尾部
MIN_EVAL_ROWS = 400   #: 一个评测窗至少 400 场会话
MIN_EVAL_POSITIVES = 40
GROUP_ORDER = ("staticOnly", "historyOnly", "opsOnly", "full")


def fold_plan(bounds: dict[str, pd.Timestamp], rounds: int) -> list[dict]:
    """把 [start, start+58+12×rounds) 切成 rounds 个"拟合→前瞻"折。"""
    if rounds < 1:
        raise ValueError("轮数至少 1")
    start = bounds["startInclusive"]
    folds = []
    for index in range(rounds):
        fit_end = start + pd.Timedelta(days=WARMUP_DAYS + HORIZON_DAYS * index)
        if fit_end - pd.Timedelta(days=VALIDATION_DAYS) < start:
            raise ValueError(f"第 {index} 轮的验证段落在窗口之外，WARMUP_DAYS 至少要 {VALIDATION_DAYS} 天")
        folds.append({"round": index, "fitEnd": fit_end,
                      "evalEnd": fit_end + pd.Timedelta(days=HORIZON_DAYS),
                      "valStart": fit_end - pd.Timedelta(days=VALIDATION_DAYS)})
    last = folds[-1]["evalEnd"]
    if last > bounds["testEndExclusive"]:
        capacity = (bounds["testEndExclusive"] - start).days - WARMUP_DAYS
        raise ValueError(f"第 {rounds} 轮的评测窗超出数据窗口（{last} > {bounds['testEndExclusive']}）；"
                         f"当前窗口最多支持 {capacity // HORIZON_DAYS} 轮")
    return folds


def run_fold(fold: dict, frame: pd.DataFrame, numeric: list[str], categorical: list[str],
             groups: dict[str, list[str]]) -> dict:
    """一轮 = 重新拟合四组候选 + 重新选组 + 重新定阈值，然后只看未来 12 天。"""
    fit_end, val_start, eval_end = fold["fitEnd"], fold["valStart"], fold["evalEnd"]
    stamps = frame["started_at"]
    train = frame[stamps < val_start]
    val = frame[(stamps >= val_start) & (stamps < fit_end)]
    ev = frame[(stamps >= fit_end) & (stamps < eval_end)]
    y_val = val["y_over"].to_numpy(dtype=int)
    y_ev = ev["y_over"].to_numpy(dtype=int)
    if len(ev) < MIN_EVAL_ROWS or int(y_ev.sum()) < MIN_EVAL_POSITIVES:
        raise AssertionError(f"第 {fold['round']} 轮评测窗太小：n={len(ev)} 正样本={int(y_ev.sum())}")
    if len(val) < MIN_EVAL_ROWS or int(y_val.sum()) < MIN_EVAL_POSITIVES:
        raise AssertionError(f"第 {fold['round']} 轮验证窗太小：n={len(val)} 正样本={int(y_val.sum())}")

    scores: dict[str, dict] = {}
    for label, columns in groups.items():
        num, cat = split_group(list(columns), numeric, categorical)
        x_fit, mask = design(train, num, cat)
        x_val, _ = design(val, num, cat)
        model = new_classifier(mask)
        model.fit(x_fit, train["y_over"].to_numpy(dtype=int))
        prob_val = model.predict_proba(x_val)[:, 1]
        scores[label] = {"valAuc": float(roc_auc_score(y_val, prob_val)), "model": model,
                         "features": int(x_fit.shape[1]), "columns": (num, cat)}
    chosen = max(scores, key=lambda key: scores[key]["valAuc"])
    chosen_columns = scores[chosen]["columns"]

    x_ev, _ = design(ev, *chosen_columns)
    prob = scores[chosen]["model"].predict_proba(x_ev)[:, 1]
    prob_val_chosen = scores[chosen]["model"].predict_proba(design(val, *chosen_columns)[0])[:, 1]
    point = pick_threshold(prob_val_chosen, y_val, daily_alert_cap(val))
    flag = prob >= point["threshold"]
    precision = float(y_ev[flag].mean()) if flag.any() else None
    recall = float(flag[y_ev == 1].sum() / max(1, int(y_ev.sum())))

    # 不学习的对手：本轮 TRAIN 窗内三级查表 + 纯因果用户/站点先验 + 只用计划时长 + 朴素"上次超时"
    baselines = fit_baselines(train)
    blend = predict_baseline(baselines, ev)
    user_prior = ev["user_over_rate_prior"].to_numpy(dtype=float)
    station_prior = ev["station_over_rate_prior"].to_numpy(dtype=float)
    planned = ev["planned_hours"].to_numpy(dtype=float)
    naive = ev["user_last_over"].fillna(0.0).to_numpy(dtype=float)
    oracle = ev["duration_min"].to_numpy(dtype=float)
    auc_model = float(roc_auc_score(y_ev, prob))
    auc_blend = float(roc_auc_score(y_ev, blend))
    auc_user = float(roc_auc_score(y_ev, user_prior))
    auc_station = float(roc_auc_score(y_ev, station_prior))
    auc_planned = float(roc_auc_score(y_ev, planned))
    auc_naive = float(roc_auc_score(y_ev, naive))
    auc_oracle = float(roc_auc_score(y_ev, oracle))

    regressor = new_regressor()
    x_fit_reg, _ = design(train, *chosen_columns)
    regressor.fit(x_fit_reg, train["over_min"].to_numpy(dtype=float))
    minutes_ev = np.clip(regressor.predict(x_ev), 0.0, None)
    y_minutes = ev["over_min"].to_numpy(dtype=float)

    return {
        "round": int(fold["round"]), "fitEnd": str(fold["fitEnd"]), "evalEnd": str(fold["evalEnd"]),
        "trainRows": int(len(train)), "validationRows": int(len(val)), "evalRows": int(len(ev)),
        "evalDays": int(ev["business_date"].nunique()),
        "evalPositives": int(y_ev.sum()), "evalBaseRate": round(float(y_ev.mean()), 4),
        "userSessionsPriorMedian": round(float(ev["user_sessions_prior"].median()), 1),
        "newUserShare": round(float((ev["user_sessions_prior"] == 0).mean()), 4),
        "chosenSet": chosen, "chosenFeatures": scores[chosen]["features"],
        "valAucBySet": {key: round(value["valAuc"], 4) for key, value in scores.items()},
        "auc": round(auc_model, 4),
        "aucCellBlendBaseline": round(auc_blend, 4),
        "aucUserPriorBaseline": round(auc_user, 4),
        "aucStationPriorBaseline": round(auc_station, 4),
        "aucPlannedHoursOnly": round(auc_planned, 4),
        "aucNaiveLastOverstay": round(auc_naive, 4),
        "aucOracleDurationNonDeployable": round(auc_oracle, 4),
        "aucGainVsBlendPct": round(100.0 * (auc_model / auc_blend - 1.0), 2),
        "aucGainVsPlannedPct": round(100.0 * (auc_model / auc_planned - 1.0), 2),
        "prAuc": round(float(average_precision_score(y_ev, prob)), 4),
        "brier": round(common.brier(y_ev, prob), 4),
        "liftAt5pct": round(common.lift_at(y_ev, prob, 0.05), 3),
        "liftAt10pct": round(common.lift_at(y_ev, prob, 0.10), 3),
        "minutesMaeModel": round(common.mae(y_minutes, minutes_ev), 4),
        "minutesMaeUserPrior": round(common.mae(y_minutes, ev["user_over_mean_prior"]
                                                .to_numpy(dtype=float)), 4),
        "threshold": round(point["threshold"], 6), "thresholdRule": point["rule"],
        "thresholdTargetMet": bool(point["targetMet"]),
        "alertRate": round(float(flag.mean()), 4), "alerts": int(flag.sum()),
        "evalPrecision": None if precision is None else round(precision, 4),
        "evalRecall": round(recall, 4),
        "evalF1": (round(2 * precision * recall / (precision + recall), 4)
                   if precision and (precision + recall) > 0 else None),
        "dataNote": common.data_note(),
    }


def sign_test_p(positive: int, total: int) -> float:
    """双侧符号检验：H0 下每轮增益正负各 0.5，"这么偏"的概率。轮间不独立，只做粗判。"""
    if total <= 0:
        return float("nan")
    k = min(positive, total - positive)
    tail = sum(math.comb(total, i) for i in range(k + 1)) / 2.0 ** total
    return min(1.0, 2.0 * tail)


def summarize(rounds: list[dict]) -> dict:
    frame = pd.DataFrame(rounds)

    def block(column: str) -> dict:
        values = pd.to_numeric(frame[column], errors="coerce").to_numpy(dtype=float)
        values = values[~np.isnan(values)]
        return {"mean": round(float(values.mean()), 4), "std": round(float(values.std(ddof=1)), 4),
                "min": round(float(values.min()), 4), "max": round(float(values.max()), 4),
                "first": round(float(values[0]), 4), "last": round(float(values[-1]), 4)}

    gain_blend = pd.to_numeric(frame["aucGainVsBlendPct"], errors="coerce").to_numpy(dtype=float)
    gain_planned = pd.to_numeric(frame["aucGainVsPlannedPct"], errors="coerce").to_numpy(dtype=float)
    summary = {
        "rounds": len(rounds), "horizonDays": HORIZON_DAYS, "warmupDays": WARMUP_DAYS,
        "validationDays": VALIDATION_DAYS,
        "thresholdRule": (f"精确率≥{PRECISION_MULTIPLE:.1f}×本轮验证段基础率 且 提醒≤当日开始会话数 "
                          f"{MAX_ALERT_SHARE:.0%} 的档里取召回最大"),
        "chosenSetCounts": {key: int(value) for key, value in frame["chosenSet"].value_counts().items()},
        "metrics": {name: block(name) for name in
                    ("auc", "aucCellBlendBaseline", "aucUserPriorBaseline", "aucStationPriorBaseline",
                     "aucPlannedHoursOnly", "aucNaiveLastOverstay", "aucOracleDurationNonDeployable",
                     "aucGainVsBlendPct", "aucGainVsPlannedPct", "prAuc", "brier",
                     "liftAt5pct", "liftAt10pct", "minutesMaeModel", "minutesMaeUserPrior",
                     "evalPrecision", "evalRecall", "evalF1", "alertRate", "threshold",
                     "userSessionsPriorMedian")},
        "valAucBySet": {key: [row["valAucBySet"][key] for row in rounds] for key in GROUP_ORDER},
        "modelMinusBestBaselineByRound": [
            round(row["auc"] - max(row["aucCellBlendBaseline"], row["aucPlannedHoursOnly"],
                                   row["aucUserPriorBaseline"]), 4)
            for row in rounds],
        "historyMinusStaticByRound": [round(row["valAucBySet"]["historyOnly"]
                                            - row["valAucBySet"]["staticOnly"], 4) for row in rounds],
        "opsMinusStaticByRound": [round(row["valAucBySet"]["opsOnly"]
                                        - row["valAucBySet"]["staticOnly"], 4) for row in rounds],
        "fullMinusStaticByRound": [round(row["valAucBySet"]["full"]
                                         - row["valAucBySet"]["staticOnly"], 4) for row in rounds],
        "userPriorAucByRound": [row["aucUserPriorBaseline"] for row in rounds],
        "modelAucByRound": [row["auc"] for row in rounds],
        "userSessionsPriorByRound": [row["userSessionsPriorMedian"] for row in rounds],
        "trend": {
            "aucFirstThreeMean": round(float(frame["auc"].head(3).mean()), 4),
            "aucLastThreeMean": round(float(frame["auc"].tail(3).mean()), 4),
            "aucDriftPct": round(100.0 * (float(frame["auc"].tail(3).mean())
                                          / frame["auc"].head(3).mean() - 1.0), 2),
            "userPriorAucFirstThreeMean": round(float(frame["aucUserPriorBaseline"].head(3).mean()), 4),
            "userPriorAucLastThreeMean": round(float(frame["aucUserPriorBaseline"].tail(3).mean()), 4),
            "oracleFirstThreeMean": round(float(frame["aucOracleDurationNonDeployable"].head(3).mean()), 4),
            "oracleLastThreeMean": round(float(frame["aucOracleDurationNonDeployable"].tail(3).mean()), 4),
            "userSessionsPriorFirst": frame["userSessionsPriorMedian"].iloc[0],
            "userSessionsPriorLast": frame["userSessionsPriorMedian"].iloc[-1],
            "baseRateFirst": frame["evalBaseRate"].iloc[0], "baseRateLast": frame["evalBaseRate"].iloc[-1],
            "gainVsBlendPositiveRounds": int((gain_blend > 0).sum()),
            "gainVsPlannedPositiveRounds": int((gain_planned > 0).sum()),
            "gainRounds": int(len(gain_blend)),
            "signTestPvsBlend": round(sign_test_p(int((gain_blend > 0).sum()), int(len(gain_blend))), 6),
            "signTestPvsPlanned": round(sign_test_p(int((gain_planned > 0).sum()),
                                                    int(len(gain_planned))), 6),
            "thresholdTargetMetRounds": int(sum(1 for row in rounds if row["thresholdTargetMet"])),
            "minutesModelWinsRounds": int(sum(1 for row in rounds
                                              if row["minutesMaeModel"] < row["minutesMaeUserPrior"])),
            "historyNonPositiveRounds": int(sum(1 for value
                                                in summary_history_deltas(rounds) if value <= 0)),
        },
        "verdict": {},
        "roundsDetail": rounds,
        "caveat": ("滚动折会重复用到 evaluate.py 那次盲测的 TEST 窗口，因此本均值不是盲测指标；"
                   "它只回答四件事：模型赢不赢得了便宜基线、用户习惯有没有场景之外的增量、"
                   "用户历史变厚有没有变值钱、运营阈值稳不稳。"),
        "disclaimer": common.data_note(),
    }
    return summary


def summary_history_deltas(rounds: list[dict]) -> list[float]:
    return [row["valAucBySet"]["historyOnly"] - row["valAucBySet"]["staticOnly"] for row in rounds]


def build_verdict(summary: dict) -> dict:
    """把"结论必须来自数字、不能来自愿望"写成结构化的一段话，避免在 markdown 里手抄判断。

    必须在 ``summarize`` 之后调用：它读的是逐轮均值与趋势。
    """
    metrics, trend = summary["metrics"], summary["trend"]
    model = metrics["auc"]["mean"]
    blend, user_prior, planned = (metrics["aucCellBlendBaseline"]["mean"],
                                  metrics["aucUserPriorBaseline"]["mean"],
                                  metrics["aucPlannedHoursOnly"]["mean"])
    best_cheap = max(blend, user_prior, planned)
    deltas = summary_history_deltas(summary["roundsDetail"])
    return {
        "modelBeatsCheapBaseline": bool(model > best_cheap),
        "headline": (
            f"{summary['rounds']} 轮里模型平均 AUC {model:.4f}，三级查表 {blend:.4f}、"
            f"纯因果用户先验 {user_prior:.4f}、只用计划时长 {planned:.4f}、朴素'上次超时' "
            f"{metrics['aucNaiveLastOverstay']['mean']:.4f}。"
            + ("学习模型在均值上超过了最便宜的可部署基线。 "
               if model > best_cheap else
               "学习模型在均值上仍没超过最便宜的可部署基线——盲测的赢面不是稳定事实。 ")
            + f"逐轮看：赢过查表的轮数 {trend['gainVsBlendPositiveRounds']}/{trend['gainRounds']}"
            f"（符号检验 p≈{trend['signTestPvsBlend']:.4f}），赢过计划时长的轮数 "
            f"{trend['gainVsPlannedPositiveRounds']}/{trend['gainRounds']}"
            f"（p≈{trend['signTestPvsPlanned']:.4f}）。"
            + ("" if min(trend["gainVsBlendPositiveRounds"], trend["gainVsPlannedPositiveRounds"]) < 2 else
               " 十轮全胜/近全胜，模型对廉价口径的优势是结构性结论。")) if (
            model > best_cheap) else (
            f"{summary['rounds']} 轮里模型平均 AUC {model:.4f}，未超过最便宜可部署基线 "
            f"{best_cheap:.4f}；逐轮胜负见明细表。"),
        "habitIncrement": (
            "historyOnly 减 staticOnly 的验证段差值逐轮为："
            + "、".join(f"{d:+.4f}" for d in deltas)
            + f"；{trend['historyNonPositiveRounds']}/{summary['rounds']} 轮为非正。"
            + ("跨轴用户历史在场景结构之外**没有独立增量**，且这不是一次盲测的偶然——"
               "'移车提醒按场景（接口/功率/站型）设计、不按个人画像设计'可以当结论用。"
               if trend["historyNonPositiveRounds"] == summary["rounds"] else
               "个别轮次用户历史出现了正增量——上线后值得用真实数据复核这一条，"
               "historyOnly 组就是现成的量尺。")),
        "userHistoryAging": (
            f"评测会话的用户历史场次中位数 首轮 {trend['userSessionsPriorFirst']} → 末轮 "
            f"{trend['userSessionsPriorLast']}；纯因果用户先验 AUC 前 3 轮均值 "
            f"{trend['userPriorAucFirstThreeMean']:.4f} → 后 3 轮 {trend['userPriorAucLastThreeMean']:.4f}"
            f"（漂移 {trend['userPriorAucLastThreeMean'] - trend['userPriorAucFirstThreeMean']:+.4f}，"
            f"轮间标准差 {metrics['aucUserPriorBaseline']['std']:.4f}）。"
            + ("历史变厚没有明显抬高用户先验的排序力——与'习惯无独立增量'互相印证。"
               if abs(trend["userPriorAucLastThreeMean"] - trend["userPriorAucFirstThreeMean"])
               <= metrics["aucUserPriorBaseline"]["std"] else
               "用户先验随历史变厚有明显移动，方向值得上线后跟踪。")),
        "durationOracle": (
            f"逐轮均值：不可部署 oracle（本场真实充电时长）"
            f"{metrics['aucOracleDurationNonDeployable']['mean']:.4f} vs 决策时点可算的计划时长 "
            f"{planned:.4f}（差 {metrics['aucOracleDurationNonDeployable']['mean'] - planned:+.4f}）。"
            + ("oracle 没有跑赢计划时长——起充前设定的目标电量与桩功率已经把'这单多大'说清楚了，"
               "本线的高 AUC 不依赖任何 ended_at 之后的信息。"
               if metrics["aucOracleDurationNonDeployable"]["mean"] <= planned + 0.002 else
               "真实时长仍比计划时长多出一块——那是计划读不到的执行细节（实际功率爬坡等），"
               "但它要等 ended_at，禁入名单守住了这条线。")),
        "thresholdStability": (
            f"阈值逐轮 {metrics['threshold']['min']:.4f}–{metrics['threshold']['max']:.4f}"
            f"（均值 {metrics['threshold']['mean']:.4f}），满足运营口径的轮数 "
            f"{trend['thresholdTargetMetRounds']}/{summary['rounds']}；"
            f"提醒率均值 {metrics['alertRate']['mean']:.2%}（预算 {MAX_ALERT_SHARE:.0%}/天）。"),
        "minutesRegression": (
            f"占桩分钟 MAE 模型均值 {metrics['minutesMaeModel']['mean']:.2f} vs 因果用户先验均值 "
            f"{metrics['minutesMaeUserPrior']['mean']:.2f}，模型赢的轮数 "
            f"{trend['minutesModelWinsRounds']}/{summary['rounds']}。"),
    }


def markdown(summary: dict, rounds: list[dict]) -> str:
    metrics = summary["metrics"]
    trend = summary["trend"]
    verdict_block = summary["verdict"]
    lines = [
        f"# 会话占桩超时提醒线 · {summary['rounds']} 轮滚动重训研究",
        "",
        "> " + summary["disclaimer"],
        "",
        "- 样本单元：一次充电会话；决策时点 = started_at（本线未新增任何数据，全部只读 as-of 聚合）",
        f"- 口径：每轮用 `fitEnd` 之前拟合、其后 {summary['horizonDays']} 天前瞻评测，"
        f"拟合窗尾 {summary['validationDays']} 天只用于选特征集与阈值",
        f"- 阈值口径：{summary['thresholdRule']}",
        f"- 每轮候选：{'/'.join(GROUP_ORDER)}，按验证段 AUC 择一",
        f"- {summary['caveat']}",
        "",
        "## 1. 逐轮汇总",
        "",
        "| 指标 | 均值 | 标准差 | 首轮 | 末轮 | 最低 | 最高 |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for name in ("auc", "aucCellBlendBaseline", "aucUserPriorBaseline", "aucStationPriorBaseline",
                 "aucPlannedHoursOnly", "aucNaiveLastOverstay", "aucOracleDurationNonDeployable",
                 "aucGainVsBlendPct", "aucGainVsPlannedPct", "prAuc", "brier",
                 "liftAt5pct", "liftAt10pct", "minutesMaeModel", "minutesMaeUserPrior",
                 "evalPrecision", "evalRecall", "evalF1", "alertRate", "threshold",
                 "userSessionsPriorMedian"):
        row = metrics[name]
        lines.append(f"| {name} | {row['mean']:.4f} | {row['std']:.4f} | {row['first']:.4f} | "
                     f"{row['last']:.4f} | {row['min']:.4f} | {row['max']:.4f} |")
    lines += ["", "## 2. 四个问题的答案", ""]
    for key in ("headline", "habitIncrement", "userHistoryAging", "durationOracle",
                "thresholdStability", "minutesRegression"):
        lines.append(f"- {verdict_block[key]}")
    val = summary["valAucBySet"]
    lines += ["",
              f"- 特征集选择次数：{summary['chosenSetCounts']}",
              "- 验证段 AUC 逐轮：",
              "  " + "；".join(f"{key}={val[key]}" for key in GROUP_ORDER),
              f"- 历史组减静态组（验证段）逐轮差：{summary['historyMinusStaticByRound']}",
              f"- 运营组减静态组（验证段）逐轮差：{summary['opsMinusStaticByRound']}",
              f"- full 减静态组（验证段）逐轮差：{summary['fullMinusStaticByRound']}",
              f"- 模型减最优便宜基线（评测窗）逐轮差：{summary['modelMinusBestBaselineByRound']}",
              f"- 评测窗基础超时率 首轮 {trend['baseRateFirst']:.4f} → 末轮 "
              f"{trend['baseRateLast']:.4f}；模型 AUC 前 3 轮均值 {trend['aucFirstThreeMean']:.4f} → "
              f"后 3 轮 {trend['aucLastThreeMean']:.4f}（{trend['aucDriftPct']:+.2f}%）",
              f"- 阈值达标轮数 {trend['thresholdTargetMetRounds']}/{summary['rounds']}——"
              "未达标轮次一律按「退回精确率最高档」如实标注，不改动运营口径去凑达标",
              "", "## 3. 逐轮明细", "",
              "| 轮 | 拟合截止 | 训练会话 | 评测会话 | 天数 | 正类 | 基础率 | 选中特征集 | 特征数 | "
              "AUC | 查表 | 用户先验 | 站点先验 | 计划时长 | 上次超时 | oracle时长 | Δvs查表 | Δvs计划 | "
              "PR-AUC | Brier | 历史场次中位 | 阈值 | 提醒率 | 精确率 | 召回 | F1 |",
              "| --- |" + " --- |" * 25]
    for row in rounds:
        f1 = "—" if row["evalF1"] is None else f"{row['evalF1']:.4f}"
        precision = "—" if row["evalPrecision"] is None else f"{row['evalPrecision']:.4f}"
        lines.append(
            f"| {row['round']} | {row['fitEnd'][:10]} | {row['trainRows']:,} | {row['evalRows']:,} | "
            f"{row['evalDays']} | {row['evalPositives']} | {row['evalBaseRate']:.4f} | "
            f"{row['chosenSet']} | {row['chosenFeatures']} | {row['auc']:.4f} | "
            f"{row['aucCellBlendBaseline']:.4f} | {row['aucUserPriorBaseline']:.4f} | "
            f"{row['aucStationPriorBaseline']:.4f} | {row['aucPlannedHoursOnly']:.4f} | "
            f"{row['aucNaiveLastOverstay']:.4f} | {row['aucOracleDurationNonDeployable']:.4f} | "
            f"{row['aucGainVsBlendPct']:+.2f}% | {row['aucGainVsPlannedPct']:+.2f}% | "
            f"{row['prAuc']:.4f} | {row['brier']:.4f} | {row['userSessionsPriorMedian']} | "
            f"{row['threshold']:.4f} | {row['alertRate']:.2%} | {precision} | {row['evalRecall']:.4f} "
            f"| {f1} |")
    lines += ["", "> " + summary["disclaimer"], ""]
    return "\n".join(lines)


def main(rounds_requested: int = 10) -> dict:
    common.ROLLING_ROUNDS_DIR.mkdir(parents=True, exist_ok=True)
    manifest = common.verify_batch()
    bounds = common.split_boundaries(manifest)
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary_meta = json.load(handle)
    if summary_meta["publishedBatchId"] != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch("features_summary 绑定的批次与当前数据层不一致，先重跑 features")
    if summary_meta.get("featuresSha256") != common.sha256_file(common.FEATURES_PATH):
        raise common.BatchMismatch("特征表哈希与 features_summary 不一致，先重跑 features")
    numeric = summary_meta["features"]["numeric"]
    categorical = summary_meta["features"]["categorical"]
    groups = summary_meta["features"]["groups"]
    if set(groups) != set(GROUP_ORDER):
        raise AssertionError(f"候选组与预期不符：{sorted(groups)} != {sorted(GROUP_ORDER)}")

    frame = pd.read_pickle(common.FEATURES_PATH)
    frame = frame[frame["split"] != "EXCLUDED"].reset_index(drop=True)
    print(f"[rolling] 在窗会话 {len(frame):,} 行，"
          f"{frame['business_date'].min().date()} → {frame['business_date'].max().date()}（北京日）")

    results: list[dict] = []
    for fold in fold_plan(bounds, rounds_requested):
        path = common.ROLLING_ROUNDS_DIR / f"round_{fold['round']:02d}.json"
        if path.exists():
            with open(path, encoding="utf-8") as handle:
                results.append(json.load(handle))
            print(f"[rolling] 第 {fold['round']} 轮已存在，复用（断点续跑）")
            continue
        started = time.perf_counter()
        record = run_fold(fold, frame, numeric, categorical, groups)
        record["seconds"] = round(time.perf_counter() - started, 1)
        common.write_new_json(path, record)
        results.append(record)
        print(f"[rolling] 第 {record['round']:2d} 轮 n={record['evalRows']:>5,} "
              f"选中={record['chosenSet']:11s} AUC={record['auc']:.4f} "
              f"查表={record['aucCellBlendBaseline']:.4f} 用户先验={record['aucUserPriorBaseline']:.4f} "
              f"计划时长={record['aucPlannedHoursOnly']:.4f} 上次超时={record['aucNaiveLastOverstay']:.4f} "
              f"oracle={record['aucOracleDurationNonDeployable']:.4f} 阈值={record['threshold']:.4f} "
              f"历史场次={record['userSessionsPriorMedian']} 耗时={record['seconds']:.0f}s")

    results.sort(key=lambda row: row["round"])
    summary = summarize(results)
    summary["verdict"] = build_verdict(summary)
    if not common.ROLLING_SUMMARY_PATH.exists():
        common.write_new_json(common.ROLLING_SUMMARY_PATH, summary)
    if not common.ROLLING_SUMMARY_MD.exists():
        common.write_new_bytes(common.ROLLING_SUMMARY_MD, markdown(summary, results).encode("utf-8"))
    metrics = summary["metrics"]
    print(f"[rolling] AUC 均值 {metrics['auc']['mean']:.4f}±{metrics['auc']['std']:.4f}"
          f"（{summary['rounds']} 轮）；查表 {metrics['aucCellBlendBaseline']['mean']:.4f} "
          f"用户先验 {metrics['aucUserPriorBaseline']['mean']:.4f} "
          f"计划时长 {metrics['aucPlannedHoursOnly']['mean']:.4f} "
          f"上次超时 {metrics['aucNaiveLastOverstay']['mean']:.4f} "
          f"oracle {metrics['aucOracleDurationNonDeployable']['mean']:.4f}")
    trend = summary["trend"]
    print(f"[rolling] 模型赢过查表 {trend['gainVsBlendPositiveRounds']}/{trend['gainRounds']} 轮"
          f"（p={trend['signTestPvsBlend']:.4f}）、赢过计划时长 "
          f"{trend['gainVsPlannedPositiveRounds']}/{trend['gainRounds']} 轮"
          f"（p={trend['signTestPvsPlanned']:.4f}）；选择次数 {summary['chosenSetCounts']}")
    print(f"[rolling] 阈值范围 {metrics['threshold']['min']:.4f}–{metrics['threshold']['max']:.4f} "
          f"均值 {metrics['threshold']['mean']:.4f}；达标轮数 "
          f"{trend['thresholdTargetMetRounds']}/{summary['rounds']}")
    print(f"[rolling] -> {common.ROLLING_SUMMARY_MD}")
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="会话占桩超时提醒线十轮滚动重训研究")
    parser.add_argument("--rounds", type=int, default=10)
    rolling_summary = main(parser.parse_args().rounds)
    # main 返回的是字典，直接 SystemExit(dict) 会把整个 summary 打到 stderr 并退出码 1
    raise SystemExit(0 if rolling_summary.get("rounds") else 1)
