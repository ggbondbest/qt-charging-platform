"""十轮滚动重训：把"一次冻结模型"变成"能不能每季重训一次、重训后到底稳不稳"的实测。

为什么是滚动而不是"多跑几个 epoch"：本线是 GBDT，`max_iter` 早停在 400 内自动收敛，重复拟合同一份
训练集只会得到同一个模型（seed 固定），那十轮是自欺。真正的"多轮"是**换时间窗重训**：
每轮用 `fitEnd` 之前的数据拟合、在它之后 12 天上评测，窗口逐轮向前滚，10 轮正好铺满 178 天窗口。

每轮干三件事：
  1. 在最近 `VALIDATION_DAYS` 天上选特征集（全量 vs 动态+排位）与告警阈值（精确率≥0.50 下召回最大）；
  2. 在评测窗上算 AUC / PR-AUC / Brier / lift@10% / 精确率 / 召回 / F1，以及 SERVED 子集的等待 MAE，
     同时给同轮的三级平滑经验率基线，看增益是否随轮次变化；
  3. 轮次结果落 `rounds/round_NN.json`（O_EXCL），**已存在就复用不重算**——中途断电可续。

口径警告：这些折会**重复用到** `evaluate.py` 那条一次盲测的 TEST 窗口，所以轮次均值不是盲测指标，
只回答"滚动重训后表现稳不稳、特征集该选哪个"。产物写 `outputs/ml_queue_rolling/`，与盲测目录互不覆盖。

用法（仓库根目录）：python -m data_analysis.ml.queue.rolling --rounds 10
"""

from __future__ import annotations

import argparse
import json
import time

import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingClassifier, HistGradientBoostingRegressor
from sklearn.metrics import average_precision_score, mean_absolute_error, roc_auc_score

from . import common
from .evaluate import DYNAMIC_PREFIXES
from .train import design, fit_baselines, predict_baseline

WARMUP_DAYS = 58   #: 第一轮拟合窗长度（再短则站点历史太薄）；58 + 12×10 = 178，十轮正好铺满窗口
HORIZON_DAYS = 12  #: 每轮评测窗
VALIDATION_DAYS = 12  #: 每轮用于选特征集与阈值的拟合窗尾部
MIN_EVAL_ROWS = 300  #: 评测窗行数下限，低于它这轮不予采信
MIN_EVAL_POSITIVES = 30


def fold_plan(bounds: dict[str, pd.Timestamp], rounds: int) -> list[dict]:
    if rounds < 1:
        raise ValueError("轮数至少 1")
    start = bounds["startInclusive"]
    folds = []
    for index in range(rounds):
        fit_end = start + pd.Timedelta(days=WARMUP_DAYS + HORIZON_DAYS * index)
        if fit_end - pd.Timedelta(days=VALIDATION_DAYS) < start:
            raise ValueError(f"第 {index} 轮的验证段落在窗口之外，WARMUP_DAYS 至少要 "
                             f"{VALIDATION_DAYS} 天")
        folds.append({"round": index, "fitEnd": fit_end,
                      "evalEnd": fit_end + pd.Timedelta(days=HORIZON_DAYS),
                      "valStart": fit_end - pd.Timedelta(days=VALIDATION_DAYS)})
    last = folds[-1]["evalEnd"]
    if last > bounds["testEndExclusive"]:
        capacity = (bounds["testEndExclusive"] - start).days - WARMUP_DAYS
        raise ValueError(f"第 {rounds} 轮的评测窗超出数据窗口（{last} > {bounds['testEndExclusive']}）；"
                         f"当前窗口最多支持 {capacity // HORIZON_DAYS} 轮")
    return folds


def candidate_sets(numeric: list[str], categorical: list[str]) -> dict[str, tuple[list[str], list[str]]]:
    """两个候选特征集：全量，以及"队列/桩动态 + 排位"（第五线盲测里疑似更优的那套）。"""
    dynamics = [n for n in numeric if n.startswith(DYNAMIC_PREFIXES)] + ["position_at_join"]
    return {"full": (numeric, categorical),
            "dynamicsPlusPosition": (dynamics, [])}


def pick_threshold(prob: np.ndarray, y: np.ndarray) -> dict:
    """在验证段上按「精确率≥0.50 取召回最大」定阈值；找不到就退回精确率最高的档位并如实标注。"""
    grid = sorted(set(np.round(np.quantile(prob, np.linspace(0.5, 0.99, 50)), 6)))
    best = None
    fallback = None
    for threshold in grid:
        flag = prob >= threshold
        if flag.sum() < 30:
            continue
        precision = float(y[flag].mean())
        recall = float(flag[y == 1].sum() / max(1, int(y.sum())))
        if fallback is None or precision > fallback["precision"]:
            fallback = {"threshold": float(threshold), "precision": precision, "recall": recall,
                        "alertRate": float(flag.mean())}
        if precision >= 0.50 and (best is None or recall > best["recall"]):
            best = {"threshold": float(threshold), "precision": precision, "recall": recall,
                    "alertRate": float(flag.mean())}
    if best is not None:
        best["rule"] = "精确率≥0.50 下召回最大"
        return best
    fallback["rule"] = "无档位达精确率0.50，退回精确率最高档（如实标注，不假装达标）"
    return fallback


def run_fold(fold: dict, frame: pd.DataFrame, numeric: list[str], categorical: list[str]) -> dict:
    fit_end, val_start, eval_end = fold["fitEnd"], fold["valStart"], fold["evalEnd"]
    stamps = frame["joined_at"]
    # 拟合段 = fitEnd 之前、验证段之前的部分；验证段只用来选特征集与阈值，不参与拟合。
    val = frame[(stamps >= val_start) & (stamps < fit_end)]
    ev = frame[(stamps >= fit_end) & (stamps < eval_end)]
    train = frame[stamps < val_start]
    if len(ev) < MIN_EVAL_ROWS or int(ev["y_waste"].sum()) < MIN_EVAL_POSITIVES:
        raise AssertionError(f"第 {fold['round']} 轮评测窗太小：n={len(ev)} "
                             f"正样本={int(ev['y_waste'].sum())}")

    candidates = candidate_sets(numeric, categorical)
    y_val = val["y_waste"].to_numpy(dtype=int)
    scores = {}
    for label, (num, cat) in candidates.items():
        x_fit, mask = design(train, num, cat)
        x_val, _ = design(val, num, cat)
        model = HistGradientBoostingClassifier(
            max_iter=400, learning_rate=0.06, max_leaf_nodes=31, min_samples_leaf=40,
            l2_regularization=1.0, early_stopping=True, validation_fraction=0.15,
            n_iter_no_change=30, random_state=common.SEED, categorical_features=mask)
        model.fit(x_fit, train["y_waste"].to_numpy(dtype=int))
        prob_val = model.predict_proba(x_val)[:, 1]
        scores[label] = {"valAuc": float(roc_auc_score(y_val, prob_val)), "model": model,
                        "features": int(x_fit.shape[1])}
    chosen = max(scores, key=lambda key: scores[key]["valAuc"])

    x_ev, _ = design(ev, *candidates[chosen])
    prob = scores[chosen]["model"].predict_proba(x_ev)[:, 1]
    y_ev = ev["y_waste"].to_numpy(dtype=int)
    point = pick_threshold(scores[chosen]["model"].predict_proba(design(val, *candidates[chosen])[0])[:, 1],
                           y_val)
    flag = prob >= point["threshold"]
    precision = float(y_ev[flag].mean()) if flag.any() else float("nan")
    recall = float(flag[y_ev == 1].sum() / max(1, int(y_ev.sum())))

    baselines = fit_baselines(train)
    base_auc = float(roc_auc_score(y_ev, predict_baseline(baselines, ev)))
    # 只按排位的朴素基线：本轮拟合窗里没见过的排位（早轮窗短，常有）回落到本轮训练集基础率，
    # 不能留 NaN——AUC 不接受缺失值，悄悄丢行又会让不同轮的样本集不一致。
    position_rate = train.groupby("position_at_join")["y_waste"].mean()
    position_score = ev["position_at_join"].map(position_rate).fillna(float(train["y_waste"].mean()))
    naive_auc = float(roc_auc_score(y_ev, position_score.to_numpy(dtype=float)))
    unseen_positions = int(ev["position_at_join"].map(position_rate).isna().sum())

    served_train = train[train["y_waste"] == 0]
    served_ev = ev[ev["y_waste"] == 0]
    x_st, st_mask = design(served_train, *candidates[chosen])
    regressor = HistGradientBoostingRegressor(
        loss="absolute_error", max_iter=400, learning_rate=0.06, max_leaf_nodes=31,
        min_samples_leaf=40, l2_regularization=1.0, early_stopping=True, validation_fraction=0.15,
        n_iter_no_change=30, random_state=common.SEED, categorical_features=st_mask)
    regressor.fit(x_st, served_train["wait_min"].to_numpy(dtype=float))
    x_sv, _ = design(served_ev, *candidates[chosen])
    truth = served_ev["wait_min"].to_numpy(dtype=float)
    model_mae = float(mean_absolute_error(truth, np.clip(regressor.predict(x_sv), 0.0, None)))
    global_median = float(served_train["wait_min"].median())
    position_median = served_train.groupby("position_at_join")["wait_min"].median()
    baseline_mae = float(mean_absolute_error(
        truth, served_ev["position_at_join"].map(position_median).fillna(global_median)))

    return {
        "round": int(fold["round"]), "fitEnd": str(fold["fitEnd"]), "evalEnd": str(fold["evalEnd"]),
        "trainRows": int(len(train)), "validationRows": int(len(val)), "evalRows": int(len(ev)),
        "unseenPositionsInEval": unseen_positions,
        "evalPositives": int(y_ev.sum()), "evalBaseRate": round(float(y_ev.mean()), 4),
        "chosenSet": chosen, "chosenFeatures": scores[chosen]["features"],
        "valAucBySet": {key: round(value["valAuc"], 4) for key, value in scores.items()},
        "auc": round(float(roc_auc_score(y_ev, prob)), 4),
        "aucStrongBaseline": round(base_auc, 4),
        "aucPositionOnlyBaseline": round(naive_auc, 4),
        "aucGainVsStrongBaselinePct": round(100.0 * (float(roc_auc_score(y_ev, prob)) / base_auc - 1.0), 2),
        "prAuc": round(float(average_precision_score(y_ev, prob)), 4),
        "brier": round(common.brier(y_ev, prob), 4),
        "liftAt10pct": round(common.lift_at(y_ev, prob, 0.10), 4),
        "threshold": round(point["threshold"], 6), "thresholdRule": point["rule"],
        "alertRate": round(float(flag.mean()), 4),
        "evalPrecision": round(precision, 4), "evalRecall": round(recall, 4),
        "evalF1": round(2 * precision * recall / max(1e-9, precision + recall), 4),
        "waitRows": int(len(served_ev)), "waitMae": round(model_mae, 4),
        "waitMaeBaseline": round(baseline_mae, 4),
        "waitMaeGainPct": round(100.0 * (1.0 - model_mae / baseline_mae), 2),
        "simulatedNote": common.simulated_note(),
    }


def summarize(rounds: list[dict]) -> dict:
    frame = pd.DataFrame(rounds)

    def block(column: str) -> dict:
        values = pd.to_numeric(frame[column], errors="coerce").to_numpy(dtype=float)
        values = values[~np.isnan(values)]
        return {"mean": round(float(values.mean()), 4), "std": round(float(values.std(ddof=1)), 4),
                "min": round(float(values.min()), 4), "max": round(float(values.max()), 4),
                "first": round(float(values[0]), 4), "last": round(float(values[-1]), 4)}

    summary = {
        "rounds": len(rounds), "horizonDays": HORIZON_DAYS, "warmupDays": WARMUP_DAYS,
        "validationDays": VALIDATION_DAYS,
        "chosenSetCounts": {k: int(v) for k, v in frame["chosenSet"].value_counts().items()},
        "metrics": {name: block(name) for name in
                    ("auc", "aucStrongBaseline", "aucPositionOnlyBaseline", "aucGainVsStrongBaselinePct",
                     "prAuc", "brier", "liftAt10pct", "evalPrecision", "evalRecall", "evalF1",
                     "alertRate", "waitMae", "waitMaeBaseline", "waitMaeGainPct")},
        "valAucBySet": {key: [row["valAucBySet"][key] for row in rounds]
                        for key in ("full", "dynamicsPlusPosition")},
        "trend": {
            "aucFirstThreeMean": round(float(frame["auc"].head(3).mean()), 4),
            "aucLastThreeMean": round(float(frame["auc"].tail(3).mean()), 4),
            "aucDriftPct": round(100.0 * (float(frame["auc"].tail(3).mean()
                                               / frame["auc"].head(3).mean() - 1.0)), 2),
            "baseRateFirst": frame["evalBaseRate"].iloc[0], "baseRateLast": frame["evalBaseRate"].iloc[-1],
        },
        "roundsDetail": rounds,
        "caveat": ("滚动折会重复用到 evaluate.py 那次盲测的 TEST 窗口，因此本均值不是盲测指标；"
                   "它只回答两件事：重训后表现稳不稳、每轮该选哪套特征。"),
        "disclaimer": common.simulated_note(),
    }
    return summary


def markdown(summary: dict, rounds: list[dict]) -> str:
    metrics = summary["metrics"]
    lines = [
        f"# 排队线 · {summary['rounds']} 轮滚动重训研究",
        "",
        "> " + summary["disclaimer"],
        "",
        f"- 口径：每轮用 `fitEnd` 之前拟合、其后 {HORIZON_DAYS} 天评测，验证段取拟合窗尾 "
        f"{summary['validationDays']} 天用于选特征集与阈值",
        f"- {summary['caveat']}",
        "",
        "| 指标 | 均值 | 标准差 | 首轮 | 末轮 | 最低 | 最高 |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for name in ("auc", "aucStrongBaseline", "aucPositionOnlyBaseline", "aucGainVsStrongBaselinePct",
                 "prAuc", "brier", "liftAt10pct", "evalPrecision", "evalRecall", "evalF1",
                 "waitMae", "waitMaeGainPct"):
        row = metrics[name]
        lines.append(f"| {name} | {row['mean']:.4f} | {row['std']:.4f} | {row['first']:.4f} | "
                     f"{row['last']:.4f} | {row['min']:.4f} | {row['max']:.4f} |")
    lines += ["", f"- 特征集选择次数：{summary['chosenSetCounts']}",
              f"- 验证段 AUC 均值：全量 {np.mean(summary['valAucBySet']['full']):.4f} / "
              f"动态+排位 {np.mean(summary['valAucBySet']['dynamicsPlusPosition']):.4f}",
              f"- 趋势：前 3 轮 AUC 均值 {summary['trend']['aucFirstThreeMean']:.4f} → "
              f"后 3 轮 {summary['trend']['aucLastThreeMean']:.4f}"
              f"（{summary['trend']['aucDriftPct']:+.2f}%）；评测窗基础白排率 "
              f"{summary['trend']['baseRateFirst']:.4f} → {summary['trend']['baseRateLast']:.4f}",
              "", "## 逐轮明细", "",
              "| 轮 | 拟合截止 | 训练行数 | 评测行数 | 基础率 | 选中特征集 | 特征数 | AUC | 强基线 | "
              "排位基线 | PR-AUC | Brier | lift@10% | 阈值 | 告警率 | 精确率 | 召回 | F1 | 等待MAE | 基线MAE |",
              "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | "
              "--- | --- | --- | --- |"]
    for row in rounds:
        lines.append(
            f"| {row['round']} | {row['fitEnd'][:10]} | {row['trainRows']:,} | {row['evalRows']:,} | "
            f"{row['evalBaseRate']:.4f} | {row['chosenSet']} | {row['chosenFeatures']} | "
            f"{row['auc']:.4f} | {row['aucStrongBaseline']:.4f} | {row['aucPositionOnlyBaseline']:.4f} | "
            f"{row['prAuc']:.4f} | {row['brier']:.4f} | {row['liftAt10pct']:.2f} | "
            f"{row['threshold']:.4f} | {row['alertRate']:.2%} | {row['evalPrecision']:.4f} | "
            f"{row['evalRecall']:.4f} | {row['evalF1']:.4f} | {row['waitMae']:.3f} | "
            f"{row['waitMaeBaseline']:.3f} |")
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
    numeric = summary_meta["features"]["numeric"]
    categorical = summary_meta["features"]["categorical"]

    frame = pd.read_pickle(common.MATRIX_PATH)
    frame = frame[frame["split"] != "EXCLUDED"].reset_index(drop=True)
    print(f"[rolling] 在窗样本 {len(frame):,} 行，"
          f"{frame['joined_at'].min()} → {frame['joined_at'].max()}（UTC）")

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
              f"选中={record['chosenSet']:20s} AUC={record['auc']:.4f} "
              f"(强基线 {record['aucStrongBaseline']:.4f}, 差 {record['aucGainVsStrongBaselinePct']:+.2f}%) "
              f"等待MAE={record['waitMae']:.3f} [{record['seconds']:.1f}s]")

    summary = summarize(results)
    if not common.ROLLING_SUMMARY_PATH.exists():
        common.write_new_json(common.ROLLING_SUMMARY_PATH, summary)
        common.write_new_bytes(common.ROLLING_SUMMARY_MD,
                               markdown(summary, results).encode("utf-8"))
    metrics = summary["metrics"]
    print(f"[rolling] {summary['rounds']} 轮 AUC 均值={metrics['auc']['mean']:.4f}"
          f"±{metrics['auc']['std']:.4f}（强基线 {metrics['aucStrongBaseline']['mean']:.4f}"
          f"±{metrics['aucStrongBaseline']['std']:.4f}），"
          f"增益均值 {metrics['aucGainVsStrongBaselinePct']['mean']:+.2f}%")
    print(f"[rolling] 特征集选择：{summary['chosenSetCounts']}；VAL AUC 均值 全量="
          f"{np.mean(summary['valAucBySet']['full']):.4f} 动态+排位="
          f"{np.mean(summary['valAucBySet']['dynamicsPlusPosition']):.4f}")
    print(f"[rolling] 等待 MAE 均值={metrics['waitMae']['mean']:.4f}±{metrics['waitMae']['std']:.4f}"
          f"（基线 {metrics['waitMaeBaseline']['mean']:.4f}，降 {metrics['waitMaeGainPct']['mean']:.2f}%）")
    print(f"[rolling] 趋势：前3轮 {summary['trend']['aucFirstThreeMean']:.4f} → "
          f"后3轮 {summary['trend']['aucLastThreeMean']:.4f}（{summary['trend']['aucDriftPct']:+.2f}%）")
    print(f"[rolling] -> {common.ROLLING_SUMMARY_MD}")
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="排队线十轮滚动重训研究")
    parser.add_argument("--rounds", type=int, default=10)
    raise SystemExit(main(parser.parse_args().rounds) and 0)
