"""十轮滚动重训：每轮"重新拟合一次"，看插枪启动失败风险的表现在时间上稳不稳。

为什么这条线也需要滚动研究：单次冻结模型只回答"这一刀切得好不好"，不回答
  1. ``staticOnly`` 打赢 ``full`` 是不是只在那一片 VALIDATION 上运气好（train.py 就是按 VAL AUC 选的）；
  2. **桩级 as-of 历史会不会随着数据变多而变有用**——滚动轮次天然给了这个实验：第 0 轮只有
     58 天历史（每台桩约 20 个正样本），第 9 轮有 166 天（约 55 个）。如果"历史越久越值钱"，
     ``charger_fail_rate_prior`` 与 ``asOfOnly`` 组的轮内 AUC 应该逐轮上行；
  3. 阈值能不能一劳永逸（每轮重选的阈值漂多少）。

口径与第五线一致：``fitEnd`` 之前拟合（其中尾部 ``VALIDATION_DAYS`` 天只用于选特征集与阈值），
``fitEnd`` 起 ``HORIZON_DAYS`` 天评测；58 + 12×10 = 178 天正好铺满 mlSplits 窗口。
滚动折会重复用到 evaluate.py 那次盲测的 TEST 窗口，因此这里的均值**不是盲测指标**，
它只回答"稳不稳 / 每轮该选哪套"。每轮结果 O_EXCL 落盘，支持断点续跑。

用法（仓库根目录）：python -m data_analysis.ml.attempt.rolling --rounds 10
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
from .train import (PRECISION_TARGET, candidate_sets, design, fit_baselines, new_classifier,
                    pick_threshold, predict_baseline)

WARMUP_DAYS = 58   #: 第一轮拟合窗长度（再短则桩级历史太薄）；58 + 12×10 = 178，十轮正好铺满窗口
HORIZON_DAYS = 12  #: 每轮评测窗
VALIDATION_DAYS = 12  #: 每轮用于选特征集与阈值的拟合窗尾部
MIN_EVAL_ROWS = 500
MIN_EVAL_POSITIVES = 30
CANDIDATE_KEYS = ("full", "asOfOnly", "staticOnly")


def fold_plan(bounds: dict[str, pd.Timestamp], rounds: int) -> list[dict]:
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


def run_fold(fold: dict, frame: pd.DataFrame, numeric: list[str], categorical: list[str]) -> dict:
    """一轮 = 重新拟合 + 重新选特征集 + 重新定阈值，然后只看未来 12 天。"""
    fit_end, val_start, eval_end = fold["fitEnd"], fold["valStart"], fold["evalEnd"]
    stamps = frame["attempted_at"]
    train = frame[stamps < val_start]
    val = frame[(stamps >= val_start) & (stamps < fit_end)]
    ev = frame[(stamps >= fit_end) & (stamps < eval_end)]
    if len(ev) < MIN_EVAL_ROWS or int(ev["y_tech"].sum()) < MIN_EVAL_POSITIVES:
        raise AssertionError(f"第 {fold['round']} 轮评测窗太小：n={len(ev)} "
                             f"正样本={int(ev['y_tech'].sum())}")
    y_val = val["y_tech"].to_numpy(dtype=int)
    y_ev = ev["y_tech"].to_numpy(dtype=int)

    candidates = candidate_sets(numeric, categorical)
    scores: dict[str, dict] = {}
    for label, (num, cat) in candidates.items():
        x_fit, mask = design(train, num, cat)
        x_val, _ = design(val, num, cat)
        model = new_classifier(mask)
        model.fit(x_fit, train["y_tech"].to_numpy(dtype=int))
        prob_val = model.predict_proba(x_val)[:, 1]
        scores[label] = {"valAuc": float(roc_auc_score(y_val, prob_val)), "model": model,
                         "features": int(x_fit.shape[1])}
    chosen = max(scores, key=lambda key: scores[key]["valAuc"])
    chosen_features = candidates[chosen]

    x_ev, _ = design(ev, *chosen_features)
    prob = scores[chosen]["model"].predict_proba(x_ev)[:, 1]
    x_val_chosen, _ = design(val, *chosen_features)
    point = pick_threshold(scores[chosen]["model"].predict_proba(x_val_chosen)[:, 1], y_val)
    flag = prob >= point["threshold"]
    precision = float(y_ev[flag].mean()) if flag.any() else None
    recall = float(flag[y_ev == 1].sum() / max(1, int(y_ev.sum())))

    # 两个不学习的对手：TRAIN 窗内查表混合 + 纯 as-of 桩先验（后者逐轮变强就说明历史有用）
    lookup = predict_baseline(fit_baselines(train), ev)
    prior = ev["charger_fail_rate_prior"].to_numpy(dtype=float)
    auc_lookup = float(roc_auc_score(y_ev, lookup))
    auc_prior = float(roc_auc_score(y_ev, prior))
    auc_model = float(roc_auc_score(y_ev, prob))

    return {
        "round": int(fold["round"]), "fitEnd": str(fold["fitEnd"]), "evalEnd": str(fold["evalEnd"]),
        "trainRows": int(len(train)), "validationRows": int(len(val)), "evalRows": int(len(ev)),
        "evalPositives": int(y_ev.sum()), "evalBaseRate": round(float(y_ev.mean()), 4),
        "chargerHistoryMedianPriorN": round(float(ev["charger_n_prior"].median()), 1),
        "chosenSet": chosen, "chosenFeatures": scores[chosen]["features"],
        "valAucBySet": {key: round(value["valAuc"], 4) for key, value in scores.items()},
        "auc": round(auc_model, 4),
        "aucLookupBaseline": round(auc_lookup, 4),
        "aucAsOfPriorBaseline": round(auc_prior, 4),
        "aucGainVsLookupPct": round(100.0 * (auc_model / auc_lookup - 1.0), 2),
        "aucGainVsPriorPct": round(100.0 * (auc_model / auc_prior - 1.0), 2),
        "prAuc": round(float(average_precision_score(y_ev, prob)), 4),
        "brier": round(common.brier(y_ev, prob), 4),
        "liftAt2pct": round(common.lift_at(y_ev, prob, 0.02), 3),
        "liftAt5pct": round(common.lift_at(y_ev, prob, 0.05), 3),
        "threshold": round(point["threshold"], 6), "thresholdRule": point["rule"],
        "thresholdTargetMet": bool(point["targetMet"]),
        "alertRate": round(float(flag.mean()), 4),
        "evalPrecision": None if precision is None else round(precision, 4),
        "evalRecall": round(recall, 4),
        "evalF1": (round(2 * precision * recall / (precision + recall), 4)
                   if precision and (precision + recall) > 0 else None),
        "simulatedNote": common.simulated_note(),
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

    gain = pd.to_numeric(frame["aucGainVsLookupPct"], errors="coerce").to_numpy(dtype=float)
    summary = {
        "rounds": len(rounds), "horizonDays": HORIZON_DAYS, "warmupDays": WARMUP_DAYS,
        "validationDays": VALIDATION_DAYS, "precisionTarget": PRECISION_TARGET,
        "chosenSetCounts": {key: int(value) for key, value in frame["chosenSet"].value_counts().items()},
        "metrics": {name: block(name) for name in
                    ("auc", "aucLookupBaseline", "aucAsOfPriorBaseline", "aucGainVsLookupPct",
                     "aucGainVsPriorPct", "prAuc", "brier", "liftAt2pct", "liftAt5pct",
                     "evalPrecision", "evalRecall", "evalF1", "alertRate", "threshold")},
        "valAucBySet": {key: [row["valAucBySet"][key] for row in rounds] for key in CANDIDATE_KEYS},
        "staticMinusFullByRound": [round(row["valAucBySet"]["staticOnly"] - row["valAucBySet"]["full"], 4)
                                   for row in rounds],
        "priorAucByRound": [row["aucAsOfPriorBaseline"] for row in rounds],
        "trend": {
            "aucFirstThreeMean": round(float(frame["auc"].head(3).mean()), 4),
            "aucLastThreeMean": round(float(frame["auc"].tail(3).mean()), 4),
            "aucDriftPct": round(100.0 * (float(frame["auc"].tail(3).mean())
                                          / frame["auc"].head(3).mean() - 1.0), 2),
            "priorAucFirstThreeMean": round(float(frame["aucAsOfPriorBaseline"].head(3).mean()), 4),
            "priorAucLastThreeMean": round(float(frame["aucAsOfPriorBaseline"].tail(3).mean()), 4),
            "chargerHistoryMedianPriorNFirst": frame["chargerHistoryMedianPriorN"].iloc[0],
            "chargerHistoryMedianPriorNLast": frame["chargerHistoryMedianPriorN"].iloc[-1],
            "baseRateFirst": frame["evalBaseRate"].iloc[0], "baseRateLast": frame["evalBaseRate"].iloc[-1],
            "gainPositiveRounds": int((gain > 0).sum()), "gainRounds": int(len(gain)),
            "signTestP": round(sign_test_p(int((gain > 0).sum()), int(len(gain))), 6),
            "thresholdTargetMetRounds": int(sum(1 for row in rounds if row["thresholdTargetMet"])),
        },
        "roundsDetail": rounds,
        "caveat": ("滚动折会重复用到 evaluate.py 那次盲测的 TEST 窗口，因此本均值不是盲测指标；"
                   "它只回答三件事：重训后稳不稳、每轮该选哪套特征、桩级历史随不随时间变有用。"),
        "disclaimer": common.simulated_note(),
    }
    return summary


def markdown(summary: dict, rounds: list[dict]) -> str:
    metrics = summary["metrics"]
    trend = summary["trend"]
    lines = [
        f"# 插枪启动失败线 · {summary['rounds']} 轮滚动重训研究",
        "",
        "> " + summary["disclaimer"],
        "",
        f"- 口径：每轮用 `fitEnd` 之前拟合、其后 {summary['horizonDays']} 天评测，拟合窗尾 "
        f"{summary['validationDays']} 天只用于选特征集与阈值",
        "- 每轮候选：全量 / 只用 as-of 历史 / 不用 as-of 历史，按验证段 AUC 择一",
        f"- {summary['caveat']}",
        "",
        "| 指标 | 均值 | 标准差 | 首轮 | 末轮 | 最低 | 最高 |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for name in ("auc", "aucLookupBaseline", "aucAsOfPriorBaseline", "aucGainVsLookupPct",
                 "aucGainVsPriorPct", "prAuc", "brier", "liftAt2pct", "liftAt5pct",
                 "evalPrecision", "evalRecall", "evalF1", "threshold"):
        row = metrics[name]
        lines.append(f"| {name} | {row['mean']:.4f} | {row['std']:.4f} | {row['first']:.4f} | "
                     f"{row['last']:.4f} | {row['min']:.4f} | {row['max']:.4f} |")
    val = summary["valAucBySet"]
    lines += ["", f"- 特征集选择次数：{summary['chosenSetCounts']}",
              f"- 验证段 AUC 均值：全量 {np.mean(val['full']):.4f} / 仅 as-of {np.mean(val['asOfOnly']):.4f}"
              f" / 仅静态 {np.mean(val['staticOnly']):.4f}",
              f"- 静态组减全量组的逐轮验证段 AUC 差：{summary['staticMinusFullByRound']}",
              f"- 纯 as-of 桩先验的逐轮 AUC：{summary['priorAucByRound']}"
              f"（首轮 3 轮均值 {trend['priorAucFirstThreeMean']:.4f} → 末 3 轮 "
              f"{trend['priorAucLastThreeMean']:.4f}）",
              f"- 每台桩评测时的历史尝试数中位数：首轮 {trend['chargerHistoryMedianPriorNFirst']} → "
              f"末轮 {trend['chargerHistoryMedianPriorNLast']}",
              f"- 趋势：前 3 轮模型 AUC 均值 {trend['aucFirstThreeMean']:.4f} → 后 3 轮 "
              f"{trend['aucLastThreeMean']:.4f}（{trend['aucDriftPct']:+.2f}%）；"
              f"相对查表基线的增益在 {trend['gainPositiveRounds']}/{trend['gainRounds']} 轮为正"
              f"（符号检验 p≈{trend['signTestP']:.4f}）；评测窗基础失败率 "
              f"{trend['baseRateFirst']:.4f} → {trend['baseRateLast']:.4f}",
              f"- 阈值达标准确率目标（≥{PRECISION_TARGET:.2f}）的轮数："
              f"{trend['thresholdTargetMetRounds']}/{summary['rounds']}——"
              "未达标轮次一律按「退回精确率最高档」如实标注，不改动运营口径去凑达标",
              "", "## 逐轮明细", "",
              "| 轮 | 拟合截止 | 训练行数 | 评测行数 | 正类 | 基础率 | 选中特征集 | 特征数 | AUC | "
              "查表基线 | as-of先验 | Δvs查表 | PR-AUC | Brier | lift@5% | 阈值 | 告警率 | 精确率 | 召回 | F1 |",
              "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | "
              "--- | --- | --- | --- | --- |"]
    for row in rounds:
        f1 = "—" if row["evalF1"] is None else f"{row['evalF1']:.4f}"
        precision = "—" if row["evalPrecision"] is None or not np.isfinite(row["evalPrecision"]) \
            else f"{row['evalPrecision']:.4f}"
        lines.append(
            f"| {row['round']} | {row['fitEnd'][:10]} | {row['trainRows']:,} | {row['evalRows']:,} | "
            f"{row['evalPositives']} | {row['evalBaseRate']:.4f} | {row['chosenSet']} | "
            f"{row['chosenFeatures']} | {row['auc']:.4f} | {row['aucLookupBaseline']:.4f} | "
            f"{row['aucAsOfPriorBaseline']:.4f} | {row['aucGainVsLookupPct']:+.2f}% | "
            f"{row['prAuc']:.4f} | {row['brier']:.4f} | {row['liftAt5pct']:.2f} | {row['threshold']:.4f} | "
            f"{row['alertRate']:.2%} | {precision} | {row['evalRecall']:.4f} | {f1} |")
    lines += ["", "> " + summary["disclaimer"], ""]
    return "\n".join(lines)


def main(rounds_requested: int = 10) -> dict:
    common.ROLLING_ROUNDS_DIR.mkdir(parents=True, exist_ok=True)
    manifest = common.verify_batch()
    bounds = common.split_boundaries(manifest)
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary_meta = json.load(handle)
    if summary_meta["publishedBatchId"] != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch("build_summary 绑定的批次与当前数据层不一致，先重跑 build_data")
    if summary_meta.get("matrixSha256") != common.sha256_file(common.MATRIX_PATH):
        raise common.BatchMismatch("特征矩阵哈希与 build_summary 不一致，先重跑 build_data")
    numeric = summary_meta["features"]["numeric"]
    categorical = summary_meta["features"]["categorical"]

    frame = pd.read_pickle(common.MATRIX_PATH)
    frame = frame[frame["split"] != "EXCLUDED"].reset_index(drop=True)
    print(f"[rolling] 在窗样本 {len(frame):,} 行，"
          f"{frame['attempted_at'].min()} → {frame['attempted_at'].max()}（UTC）")

    results: list[dict] = []
    for fold in fold_plan(bounds, rounds_requested):
        path = common.ROLLING_ROUNDS_DIR / f"round_{fold['round']:02d}.json"
        if path.exists():
            with open(path, encoding="utf-8") as handle:
                results.append(json.load(handle))
            print(f"[rolling] 第 {fold['round']} 轮已存在，复用（断点续跑）")
            continue
        started = time.perf_counter()
        record = run_fold(fold, frame, numeric, categorical)
        record["seconds"] = round(time.perf_counter() - started, 1)
        common.write_new_json(path, record)
        results.append(record)
        print(f"[rolling] 第 {record['round']:2d} 轮 n={record['evalRows']:>5,} "
              f"选中={record['chosenSet']:11s} AUC={record['auc']:.4f} "
              f"查表={record['aucLookupBaseline']:.4f} as-of先验={record['aucAsOfPriorBaseline']:.4f} "
              f"阈值={record['threshold']:.4f} 耗时={record['seconds']:.0f}s")

    results.sort(key=lambda row: row["round"])
    summary = summarize(results)
    if not common.ROLLING_SUMMARY_PATH.exists():
        common.write_new_json(common.ROLLING_SUMMARY_PATH, summary)
    if not common.ROLLING_SUMMARY_MD.exists():
        common.write_new_bytes(common.ROLLING_SUMMARY_MD, markdown(summary, results).encode("utf-8"))
    metrics = summary["metrics"]
    print(f"[rolling] AUC 均值 {metrics['auc']['mean']:.4f}±{metrics['auc']['std']:.4f} "
          f"（{summary['rounds']} 轮）；相对查表基线增益均值 "
          f"{metrics['aucGainVsLookupPct']['mean']:+.2f}%，为正的轮数 "
          f"{summary['trend']['gainPositiveRounds']}/{summary['trend']['gainRounds']}")
    print(f"[rolling] 纯 as-of 桩先验均值 {metrics['aucAsOfPriorBaseline']['mean']:.4f}"
          f"（首轮 {metrics['aucAsOfPriorBaseline']['first']:.4f} → 末轮 "
          f"{metrics['aucAsOfPriorBaseline']['last']:.4f}）")
    print(f"[rolling] 特征集选择次数 {summary['chosenSetCounts']}")
    print(f"[rolling] 阈值范围 {metrics['threshold']['min']:.4f}–{metrics['threshold']['max']:.4f} "
          f"均值 {metrics['threshold']['mean']:.4f}")
    print(f"[rolling] -> {common.ROLLING_SUMMARY_MD}")
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="插枪启动失败线十轮滚动重训研究")
    parser.add_argument("--rounds", type=int, default=10)
    summary = main(parser.parse_args().rounds)
    # main 返回的是字典，直接 SystemExit(dict) 会把整个 summary 打到 stderr 并退出码 1
    raise SystemExit(0 if summary.get("rounds") else 1)
