"""十轮滚动重训：每轮重新拟合一次，看桩级次日可靠性预警的表现随时间稳不稳。

这条线的滚动研究要回答的问题跟第五/六线不一样。第五、六线问的是"哪套特征该发布"；本线在
单次盲测里已经看到 **学习的 GBT 没有跑赢一行查表**，所以十轮真正要验的是三件事：

  1. 这个"没跑赢"是不是那一片 VALIDATION 的运气——十轮里模型赢过查表/因果先验的轮数是多少；
  2. **桩级历史会不会随着面板变厚而变有用**——第 0 轮每台桩只有约 46 天历史，第 9 轮约 154 天。
     如果 ``charger_days_prior`` 变厚能换来 AUC 上行，那么发布口径应该改成"上线后攒够历史再切模型"；
  3. 运营阈值（精确率 ≥ 2×基础率、告警 ≤ 当日 25% 预算）能不能一劳永逸，逐轮阈值漂多少、
     有多少轮根本凑不出满足预算的档。

口径与第五/六线一致：``fitEnd`` 之前拟合（尾部 ``VALIDATION_DAYS`` 天只用于选特征集与阈值），
``fitEnd`` 起 ``HORIZON_DAYS`` 天评测；58 + 12×10 = 178 天正好铺满 mlSplits 窗口。
滚动折会重复用到 evaluate.py 那次盲测的 TEST 窗口，因此这里的均值**不是盲测指标**。
每轮结果 O_EXCL 落盘，支持断点续跑。

用法（仓库根目录）：python -m data_analysis.ml.reliability.rolling --rounds 10
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

WARMUP_DAYS = 58      #: 第一轮拟合窗长度（再短则桩级日历史太薄）；58 + 12×10 = 178，十轮铺满窗口
HORIZON_DAYS = 12     #: 每轮评测窗
VALIDATION_DAYS = 12  #: 每轮用于选特征集与阈值的拟合窗尾部
MIN_EVAL_ROWS = 400   #: 一个评测窗至少 400 桩日（约 33 台桩 × 12 天）
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
    stamps = frame["day_start_utc"]
    train = frame[stamps < val_start]
    val = frame[(stamps >= val_start) & (stamps < fit_end)]
    ev = frame[(stamps >= fit_end) & (stamps < eval_end)]
    y_val = val["y_fail"].to_numpy(dtype=int)
    y_ev = ev["y_fail"].to_numpy(dtype=int)
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
        model.fit(x_fit, train["y_fail"].to_numpy(dtype=int))
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

    # 不学习的对手：本轮 TRAIN 窗内查表混合 + 纯因果桩先验 + 只用因果预期用量 + 朴素 lag1
    baselines = fit_baselines(train)
    lookup = predict_baseline(baselines, ev)
    prior = ev["charger_fail_rate_prior"].to_numpy(dtype=float)
    volume = ev["attempts_per_day_30d"].to_numpy(dtype=float)
    lag1 = ev["prev_day_fail"].fillna(0.0).to_numpy(dtype=float)
    oracle = ev["attempts_on_day"].to_numpy(dtype=float)
    auc_model = float(roc_auc_score(y_ev, prob))
    auc_lookup = float(roc_auc_score(y_ev, lookup))
    auc_prior = float(roc_auc_score(y_ev, prior))
    auc_volume = float(roc_auc_score(y_ev, volume))
    auc_lag1 = float(roc_auc_score(y_ev, lag1))
    auc_oracle = float(roc_auc_score(y_ev, oracle))

    regressor = new_regressor()
    x_fit_reg, _ = design(train, *chosen_columns)
    regressor.fit(x_fit_reg, train["y_fails"].to_numpy(dtype=int))
    count_ev = np.clip(regressor.predict(x_ev), 0.0, None)
    count_baseline_ev = (ev["attempts_per_day_30d"].to_numpy(dtype=float)
                         * ev["charger_fail_rate_attempt_prior"].to_numpy(dtype=float))
    y_count = ev["y_fails"].to_numpy(dtype=float)

    return {
        "round": int(fold["round"]), "fitEnd": str(fold["fitEnd"]), "evalEnd": str(fold["evalEnd"]),
        "trainRows": int(len(train)), "validationRows": int(len(val)), "evalRows": int(len(ev)),
        "evalDays": int(ev["business_date"].nunique()),
        "evalPositives": int(y_ev.sum()), "evalBaseRate": round(float(y_ev.mean()), 4),
        "chargerDaysPriorMedian": round(float(ev["charger_days_prior"].median()), 1),
        "chargerFailRatePriorStd": round(float(pd.Series(prior).std()), 5),
        "chosenSet": chosen, "chosenFeatures": scores[chosen]["features"],
        "valAucBySet": {key: round(value["valAuc"], 4) for key, value in scores.items()},
        "auc": round(auc_model, 4),
        "aucLookupBaseline": round(auc_lookup, 4),
        "aucAsOfPriorBaseline": round(auc_prior, 4),
        "aucExpectedVolumeOnly": round(auc_volume, 4),
        "aucNaiveLag1": round(auc_lag1, 4),
        "aucOracleNonDeployable": round(auc_oracle, 4),
        "aucGainVsLookupPct": round(100.0 * (auc_model / auc_lookup - 1.0), 2),
        "aucGainVsPriorPct": round(100.0 * (auc_model / auc_prior - 1.0), 2),
        "prAuc": round(float(average_precision_score(y_ev, prob)), 4),
        "brier": round(common.brier(y_ev, prob), 4),
        "liftAt5pct": round(common.lift_at(y_ev, prob, 0.05), 3),
        "liftAt10pct": round(common.lift_at(y_ev, prob, 0.10), 3),
        "countMaeModel": round(common.mae(y_count, count_ev), 5),
        "countMaeVolumePrior": round(common.mae(y_count, count_baseline_ev), 5),
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

    gain_lookup = pd.to_numeric(frame["aucGainVsLookupPct"], errors="coerce").to_numpy(dtype=float)
    gain_prior = pd.to_numeric(frame["aucGainVsPriorPct"], errors="coerce").to_numpy(dtype=float)
    summary = {
        "rounds": len(rounds), "horizonDays": HORIZON_DAYS, "warmupDays": WARMUP_DAYS,
        "validationDays": VALIDATION_DAYS,
        "thresholdRule": (f"精确率≥{PRECISION_MULTIPLE:.1f}×本轮验证段基础率 且 告警≤当日在用桩数 "
                          f"{MAX_ALERT_SHARE:.0%} 的档里取召回最大"),
        "chosenSetCounts": {key: int(value) for key, value in frame["chosenSet"].value_counts().items()},
        "metrics": {name: block(name) for name in
                    ("auc", "aucLookupBaseline", "aucAsOfPriorBaseline", "aucExpectedVolumeOnly",
                     "aucNaiveLag1", "aucOracleNonDeployable", "aucGainVsLookupPct",
                     "aucGainVsPriorPct", "prAuc", "brier", "liftAt5pct", "liftAt10pct",
                     "countMaeModel", "countMaeVolumePrior", "evalPrecision", "evalRecall",
                     "evalF1", "alertRate", "threshold", "chargerDaysPriorMedian")},
        "valAucBySet": {key: [row["valAucBySet"][key] for row in rounds] for key in GROUP_ORDER},
        "modelMinusBestBaselineByRound": [
            round(row["auc"] - max(row["aucLookupBaseline"], row["aucAsOfPriorBaseline"]), 4)
            for row in rounds],
        "historyMinusStaticByRound": [round(row["valAucBySet"]["historyOnly"]
                                            - row["valAucBySet"]["staticOnly"], 4) for row in rounds],
        "opsMinusStaticByRound": [round(row["valAucBySet"]["opsOnly"]
                                        - row["valAucBySet"]["staticOnly"], 4) for row in rounds],
        "priorAucByRound": [row["aucAsOfPriorBaseline"] for row in rounds],
        "modelAucByRound": [row["auc"] for row in rounds],
        "chargerDaysPriorByRound": [row["chargerDaysPriorMedian"] for row in rounds],
        "trend": {
            "aucFirstThreeMean": round(float(frame["auc"].head(3).mean()), 4),
            "aucLastThreeMean": round(float(frame["auc"].tail(3).mean()), 4),
            "aucDriftPct": round(100.0 * (float(frame["auc"].tail(3).mean())
                                          / frame["auc"].head(3).mean() - 1.0), 2),
            "priorAucFirstThreeMean": round(float(frame["aucAsOfPriorBaseline"].head(3).mean()), 4),
            "priorAucLastThreeMean": round(float(frame["aucAsOfPriorBaseline"].tail(3).mean()), 4),
            "oracleFirstThreeMean": round(float(frame["aucOracleNonDeployable"].head(3).mean()), 4),
            "oracleLastThreeMean": round(float(frame["aucOracleNonDeployable"].tail(3).mean()), 4),
            "chargerDaysPriorFirst": frame["chargerDaysPriorMedian"].iloc[0],
            "chargerDaysPriorLast": frame["chargerDaysPriorMedian"].iloc[-1],
            "baseRateFirst": frame["evalBaseRate"].iloc[0], "baseRateLast": frame["evalBaseRate"].iloc[-1],
            "gainVsLookupPositiveRounds": int((gain_lookup > 0).sum()),
            "gainVsPriorPositiveRounds": int((gain_prior > 0).sum()),
            "gainRounds": int(len(gain_prior)),
            "signTestPvsLookup": round(sign_test_p(int((gain_lookup > 0).sum()), int(len(gain_lookup))), 6),
            "signTestPvsPrior": round(sign_test_p(int((gain_prior > 0).sum()), int(len(gain_prior))), 6),
            "thresholdTargetMetRounds": int(sum(1 for row in rounds if row["thresholdTargetMet"])),
            "countMaeModelWinsRounds": int(sum(1 for row in rounds
                                               if row["countMaeModel"] < row["countMaeVolumePrior"])),
        },
        "verdict": {},
        "roundsDetail": rounds,
        "caveat": ("滚动折会重复用到 evaluate.py 那次盲测的 TEST 窗口，因此本均值不是盲测指标；"
                   "它只回答三件事：模型赢不赢得了便宜基线、桩级历史随面板变厚有没有变值钱、"
                   "运营阈值稳不稳。"),
        "disclaimer": common.data_note(),
    }
    return summary


def build_verdict(summary: dict) -> dict:
    """把"结论必须来自数字、不能来自愿望"写成结构化的一段话，避免在 markdown 里手抄判断。

    必须在 ``summarize`` 之后调用：它读的是逐轮均值与趋势。
    """
    metrics, trend = summary["metrics"], summary["trend"]
    model, prior, lookup = metrics["auc"]["mean"], metrics["aucAsOfPriorBaseline"]["mean"], \
        metrics["aucLookupBaseline"]["mean"]
    best_cheap = max(prior, lookup)
    prior_std = metrics["aucAsOfPriorBaseline"]["std"]
    prior_drift = trend["priorAucLastThreeMean"] - trend["priorAucFirstThreeMean"]
    oracle_mean = metrics["aucOracleNonDeployable"]["mean"]
    return {
        "modelBeatsCheapBaseline": bool(model > best_cheap),
        "headline": (
            f"{summary['rounds']} 轮里模型平均 AUC {model:.4f}，查表基线 {lookup:.4f}、"
            f"纯因果桩先验 {prior:.4f}、只用预期用量 {metrics['aucExpectedVolumeOnly']['mean']:.4f}、"
            f"朴素 lag1 {metrics['aucNaiveLag1']['mean']:.4f}。"
            + ("学习模型在均值上超过了最便宜的可部署基线。 "
               if model > best_cheap else
               "学习模型在均值上仍没超过最便宜的可部署基线——单次盲测的结论不是偶然。 ")
            + f"逐轮看：赢过查表的轮数 {trend['gainVsLookupPositiveRounds']}/{trend['gainRounds']}"
            f"（符号检验 p≈{trend['signTestPvsLookup']:.4f}），赢过因果先验的轮数 "
            f"{trend['gainVsPriorPositiveRounds']}/{trend['gainRounds']}"
            f"（p≈{trend['signTestPvsPrior']:.4f}）。"
            + ("" if min(trend["signTestPvsLookup"], trend["signTestPvsPrior"]) >= 0.05 else
               (" 两个 p 值都已小于 0.05，也就是说'GBT 打不过一行查表'在本线是显著结论，不是抽样噪声"
                "——发布口径因此以便宜基线为准，模型只作为对照一并给出。"
                if model < best_cheap else
                " 两个 p 值都已小于 0.05：模型稳定优于便宜基线，这条可以当结论用。"))
            + ("" if min(trend["gainVsLookupPositiveRounds"], trend["gainVsPriorPositiveRounds"]) > 0 else
               " 十轮无一轮胜出，这是结构性结论而非某刀切坏。")),
        "historyGetsBetterWithAge": (
            f"每台桩评测时的历史天数中位数 首轮 {trend['chargerDaysPriorFirst']} → 末轮 "
            f"{trend['chargerDaysPriorLast']}；纯因果桩先验 AUC 前 3 轮均值 "
            f"{trend['priorAucFirstThreeMean']:.4f} → 后 3 轮 {trend['priorAucLastThreeMean']:.4f}"
            f"（漂移 {prior_drift:+.4f}，轮间标准差 {prior_std:.4f}）"
            + ("；方向支持'历史越厚越值钱'，但漂移小于轮间波动，不足以定论——"
               "要判定得把窗口拉到一年以上的真实数据。"
               if 0 < prior_drift <= prior_std else
               ("；历史变厚明显换到了排序力（漂移大于轮间标准差）。" if prior_drift > prior_std else
                "；历史变厚没有换来排序力——面板足够稠密，第一轮 65 天就已经把每台桩的日失败率定死了。"))),
        "exposureOracle": (
            f"逐轮均值：不可部署 oracle（真用当天尝试次数）{oracle_mean:.4f} vs 可部署的纯因果桩先验 "
            f"{prior:.4f}（差 {oracle_mean - prior:+.4f}）；oracle 前 3 轮 "
            f"{trend['oracleFirstThreeMean']:.4f} → 后 3 轮 {trend['oracleLastThreeMean']:.4f}，"
            f"因果先验同期 {trend['priorAucFirstThreeMean']:.4f}→{trend['priorAucLastThreeMean']:.4f}。"
            + ("oracle 没有跑赢因果先验——"
               "说明'知道明天会被用几次'这件事，因果特征已经从历史里估出来了，"
               "本线不需要、也没有偷看当天暴露度。"
               if oracle_mean <= prior + 0.002 else
               "oracle 明显高于因果先验——排序收益里还有一块是当天暴露度，"
               "上线前必须确认线上打分时刻拿不到它，本线的特征名单因此硬性禁止当日量。")),
        "thresholdStability": (
            f"阈值逐轮 {metrics['threshold']['min']:.4f}–{metrics['threshold']['max']:.4f}"
            f"（均值 {metrics['threshold']['mean']:.4f}），满足运营口径的轮数 "
            f"{trend['thresholdTargetMetRounds']}/{summary['rounds']}；"
            f"告警率均值 {metrics['alertRate']['mean']:.2%}（预算 {MAX_ALERT_SHARE:.0%}/天）。"),
        "countRegression": (
            f"回归 MAE 模型均值 {metrics['countMaeModel']['mean']:.5f} vs 因果"
            f"「预期用量×桩失败率」{metrics['countMaeVolumePrior']['mean']:.5f}，"
            f"模型赢的轮数 {trend['countMaeModelWinsRounds']}/{summary['rounds']}。"),
    }


def markdown(summary: dict, rounds: list[dict]) -> str:
    metrics = summary["metrics"]
    trend = summary["trend"]
    verdict_block = summary["verdict"]
    lines = [
        f"# 桩级次日可靠性预警线 · {summary['rounds']} 轮滚动重训研究",
        "",
        "> " + summary["disclaimer"],
        "",
        "- 样本单元：一台桩 × 一个北京日历日（当日 ≥1 次插枪启动尝试）",
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
    for name in ("auc", "aucLookupBaseline", "aucAsOfPriorBaseline", "aucExpectedVolumeOnly",
                 "aucNaiveLag1", "aucOracleNonDeployable", "aucGainVsLookupPct", "aucGainVsPriorPct",
                 "prAuc", "brier", "liftAt5pct", "liftAt10pct", "countMaeModel",
                 "countMaeVolumePrior", "evalPrecision", "evalRecall", "evalF1", "alertRate",
                 "threshold", "chargerDaysPriorMedian"):
        row = metrics[name]
        lines.append(f"| {name} | {row['mean']:.4f} | {row['std']:.4f} | {row['first']:.4f} | "
                     f"{row['last']:.4f} | {row['min']:.4f} | {row['max']:.4f} |")
    lines += ["", "## 2. 三个问题的答案", ""]
    for key in ("headline", "historyGetsBetterWithAge", "exposureOracle", "thresholdStability",
                "countRegression"):
        lines.append(f"- {verdict_block[key]}")
    val = summary["valAucBySet"]
    lines += ["",
              f"- 特征集选择次数：{summary['chosenSetCounts']}",
              "- 验证段 AUC 逐轮：",
              "  " + "；".join(f"{key}={val[key]}" for key in GROUP_ORDER),
              f"- 历史组减静态组（验证段）逐轮差：{summary['historyMinusStaticByRound']}",
              f"- 运营组减静态组（验证段）逐轮差：{summary['opsMinusStaticByRound']}",
              f"- 模型减最优便宜基线（评测窗）逐轮差：{summary['modelMinusBestBaselineByRound']}",
              f"- 评测窗基础失败率 首轮 {trend['baseRateFirst']:.4f} → 末轮 "
              f"{trend['baseRateLast']:.4f}；模型 AUC 前 3 轮均值 {trend['aucFirstThreeMean']:.4f} → "
              f"后 3 轮 {trend['aucLastThreeMean']:.4f}（{trend['aucDriftPct']:+.2f}%）",
              f"- 阈值达标轮数 {trend['thresholdTargetMetRounds']}/{summary['rounds']}——"
              "未达标轮次一律按「退回精确率最高档」如实标注，不改动运营口径去凑达标",
              "", "## 3. 逐轮明细", "",
              "| 轮 | 拟合截止 | 训练桩日 | 评测桩日 | 天数 | 正类 | 基础率 | 选中特征集 | 特征数 | "
              "AUC | 查表 | 因果先验 | 预期用量 | lag1 | oracle | Δvs查表 | Δvs先验 | PR-AUC | Brier | "
              "历史天数 | 阈值 | 告警率 | 精确率 | 召回 | F1 |",
              "| --- |" + " --- |" * 23]
    for row in rounds:
        f1 = "—" if row["evalF1"] is None else f"{row['evalF1']:.4f}"
        precision = "—" if row["evalPrecision"] is None else f"{row['evalPrecision']:.4f}"
        lines.append(
            f"| {row['round']} | {row['fitEnd'][:10]} | {row['trainRows']:,} | {row['evalRows']:,} | "
            f"{row['evalDays']} | {row['evalPositives']} | {row['evalBaseRate']:.4f} | "
            f"{row['chosenSet']} | {row['chosenFeatures']} | {row['auc']:.4f} | "
            f"{row['aucLookupBaseline']:.4f} | {row['aucAsOfPriorBaseline']:.4f} | "
            f"{row['aucExpectedVolumeOnly']:.4f} | {row['aucNaiveLag1']:.4f} | "
            f"{row['aucOracleNonDeployable']:.4f} | {row['aucGainVsLookupPct']:+.2f}% | "
            f"{row['aucGainVsPriorPct']:+.2f}% | {row['prAuc']:.4f} | {row['brier']:.4f} | "
            f"{row['chargerDaysPriorMedian']} | {row['threshold']:.4f} | {row['alertRate']:.2%} | "
            f"{precision} | {row['evalRecall']:.4f} | {f1} |")
    lines += ["", "> " + summary["disclaimer"], ""]
    return "\n".join(lines)


def main(rounds_requested: int = 10) -> dict:
    common.ROLLING_ROUNDS_DIR.mkdir(parents=True, exist_ok=True)
    manifest = common.verify_batch()
    common.verify_derived()
    bounds = common.split_boundaries(manifest)
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary_meta = json.load(handle)
    if summary_meta["publishedBatchId"] != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch("features_summary 绑定的批次与当前数据层不一致，先重跑 features")
    if summary_meta.get("featuresSha256") != common.sha256_file(common.FEATURES_PATH):
        raise common.DerivedMismatch("特征表哈希与 features_summary 不一致，先重跑 features")
    numeric = summary_meta["features"]["numeric"]
    categorical = summary_meta["features"]["categorical"]
    groups = summary_meta["features"]["groups"]
    if set(groups) != set(GROUP_ORDER):
        raise AssertionError(f"候选组与预期不符：{sorted(groups)} != {sorted(GROUP_ORDER)}")

    frame = pd.read_pickle(common.FEATURES_PATH)
    frame = frame[frame["split"] != "EXCLUDED"].reset_index(drop=True)
    print(f"[rolling] 在窗桩日 {len(frame):,} 行，"
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
              f"查表={record['aucLookupBaseline']:.4f} 先验={record['aucAsOfPriorBaseline']:.4f} "
              f"用量={record['aucExpectedVolumeOnly']:.4f} lag1={record['aucNaiveLag1']:.4f} "
              f"oracle={record['aucOracleNonDeployable']:.4f} 阈值={record['threshold']:.4f} "
              f"历史天数={record['chargerDaysPriorMedian']} 耗时={record['seconds']:.0f}s")

    results.sort(key=lambda row: row["round"])
    summary = summarize(results)
    summary["verdict"] = build_verdict(summary)
    if not common.ROLLING_SUMMARY_PATH.exists():
        common.write_new_json(common.ROLLING_SUMMARY_PATH, summary)
    if not common.ROLLING_SUMMARY_MD.exists():
        common.write_new_bytes(common.ROLLING_SUMMARY_MD, markdown(summary, results).encode("utf-8"))
    metrics = summary["metrics"]
    print(f"[rolling] AUC 均值 {metrics['auc']['mean']:.4f}±{metrics['auc']['std']:.4f}"
          f"（{summary['rounds']} 轮）；查表 {metrics['aucLookupBaseline']['mean']:.4f} "
          f"因果先验 {metrics['aucAsOfPriorBaseline']['mean']:.4f} "
          f"预期用量 {metrics['aucExpectedVolumeOnly']['mean']:.4f} "
          f"lag1 {metrics['aucNaiveLag1']['mean']:.4f} "
          f"oracle {metrics['aucOracleNonDeployable']['mean']:.4f}")
    trend = summary["trend"]
    print(f"[rolling] 模型赢过查表 {trend['gainVsLookupPositiveRounds']}/{trend['gainRounds']} 轮"
          f"（p={trend['signTestPvsLookup']:.4f}）、赢过因果先验 "
          f"{trend['gainVsPriorPositiveRounds']}/{trend['gainRounds']} 轮"
          f"（p={trend['signTestPvsPrior']:.4f}）；选择次数 {summary['chosenSetCounts']}")
    print(f"[rolling] 阈值范围 {metrics['threshold']['min']:.4f}–{metrics['threshold']['max']:.4f} "
          f"均值 {metrics['threshold']['mean']:.4f}；达标轮数 "
          f"{trend['thresholdTargetMetRounds']}/{summary['rounds']}")
    print(f"[rolling] -> {common.ROLLING_SUMMARY_MD}")
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="桩级次日可靠性预警线十轮滚动重训研究")
    parser.add_argument("--rounds", type=int, default=10)
    rolling_summary = main(parser.parse_args().rounds)
    # main 返回的是字典，直接 SystemExit(dict) 会把整个 summary 打到 stderr 并退出码 1
    raise SystemExit(0 if rolling_summary.get("rounds") else 1)
