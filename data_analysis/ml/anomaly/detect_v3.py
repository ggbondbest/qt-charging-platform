"""v3:会话级信号 rank 融合。用法(仓库根目录):python -m data_analysis.ml.anomaly.detect_v3
机制链:v1(会话均值)抓得住"持续偏移"但稀释瞬时;v2(会话内 z+点级 TopK)抓得住瞬时但
把持续偏移重归一化掉(TEST 实锤:POWER_DERATING 召回 0.7%)。v3 不再逼模型用同一个特征
空间兼顾两种形态,而是并排算 7 个会话级信号(v1 分、v2 峰分、热尖峰、压流、骤停、电流突变),
每 split 内转百分位秩后做子集平均;子集在 VALIDATION 上贪心选、阈值取 TRAIN 分布上分位,
全部选择只碰 TRAIN/VALIDATION。新 model_id、独立 TEST 首盲;v1/v2 冻结文件不动。
标签依旧只进评测关联(契约 P2)。
"""

from __future__ import annotations

import json
import platform
import time

import joblib
import numpy as np
import pandas as pd
import sklearn
from sklearn.ensemble import IsolationForest  # noqa: F401  (v1/v2 bundle 反序列化需要)

from . import common

SEED = 42
PERCENTILE_GRID = (88, 90, 92, 93, 94, 95, 96, 97, 98)
SPLITS = ("TRAIN", "VALIDATION", "TEST")


def session_signals() -> dict[str, pd.Series]:
    """7 个会话级异常信号,全部无标签参与。索引 = session_id。"""
    sig: dict[str, pd.Series] = {}

    b1 = joblib.load(common.BUNDLE_PATH)
    sf = pd.read_pickle(common.FEATURE_TABLE).set_index("session_id")
    x1 = b1["scaler"].transform(sf[b1["features"]].astype(float).fillna(b1["medians"]))
    sig["v1Mean"] = pd.Series(-b1["model"].score_samples(x1), index=sf.index)

    pts = pd.read_pickle(common.POINT_TABLE)
    b2 = joblib.load(common.BUNDLE_PATH_V2)
    x2 = b2["scaler"].transform(pts[b2["features"]].astype(float).fillna(b2["medians"]).to_numpy())
    p2 = pd.Series(-b2["model"].score_samples(x2), index=pts.session_id.to_numpy())
    sig["v2Max"] = common.topk_scores(p2, 1)
    sig["v2Top3"] = common.topk_scores(p2, 3)

    g = pts.groupby("session_id", sort=False)
    heat = np.maximum(pts["z_temp_spread"].to_numpy(), pts["z_max_temperature_c"].to_numpy())
    sig["thermalSpike"] = pd.Series(heat, index=pts.session_id.to_numpy()).groupby(level=0).max()
    sig["currentDeficit"] = -g["z_charge_current_a"].min()
    sig["socStall"] = -g["dsoc"].min()
    sig["currentJump"] = pts.assign(ad=pts["dcur"].abs()).groupby("session_id", sort=False)["ad"].max()
    return {k: v.reindex(sf.index) for k, v in sig.items()}


def rank_in_split(sig: pd.Series, sid_by_split: dict) -> dict:
    """fillna(0):缺信号会话按"最不异常"钉底(旧 NaN 语义是静默 FN,两都不完美,缺失数进
    train_metrics 披露)。评审实锤的口径边界:融合分是 split 内百分位秩——TRAIN 的 p96 阈值
    恒≈0.96,套到任何窗口都是"固定告警预算"的 transductive 批筛,不是可部署的固定决策函数;
    "阈值在 TRAIN 拟合"仅对纯 v1/v2 原始分候选成立。v3 首盲已冻结,此语义以勘误形式记录,
    追改冻结件=违盲评纪律;带固定阈值部署形态的下一代(若做)必须新 model_id 新首盲。"""
    return {s: sig.reindex(sid_by_split[s]).rank(pct=True).fillna(0.0).to_numpy() for s in SPLITS}


def best_entry(score_by_split: dict, y_by_split: dict) -> tuple:
    """阈值只在 TRAIN 分布上扫,成绩看 VALIDATION;返回 (pct, prf entry)。"""
    best = None
    for pct in PERCENTILE_GRID:
        thr = float(np.percentile(score_by_split["TRAIN"], pct))
        entry = common.prf(score_by_split["VALIDATION"], y_by_split["VALIDATION"], thr)
        if best is None or entry["f1"] > best[1]["f1"]:
            best = (pct, entry)
    return best


def main() -> int:
    started = time.time()
    pts = pd.read_pickle(common.POINT_TABLE)
    labels = common.load_table("anomaly_labels")
    pos_sessions = set(labels.session_id)
    ses = (pts.groupby("session_id", sort=False).agg(split=("split", "first")).reset_index())
    ses["y"] = ses.session_id.isin(pos_sessions).astype(int)
    sid_by_split = {s: ses[ses.split == s].session_id for s in SPLITS}
    y_by_split = {s: ses[ses.split == s].y.to_numpy() for s in SPLITS}

    signals = session_signals()
    all_sid = pd.Index(ses.session_id)
    missing = {n: int(s.reindex(all_sid).isna().sum()) for n, s in signals.items()}
    if any(missing.values()):
        print("missing-signal counts (fillna(0) 钉底,详见 rank_in_split 注释):", missing)
    ranks = {name: rank_in_split(s, sid_by_split) for name, s in signals.items()}
    print("single-signal validation best:")
    singles = {}
    for name in signals:
        pct, entry = best_entry(ranks[name], y_by_split)
        singles[name] = {"pct": pct, "f1": entry["f1"]}
        print(f"  {name}: pct={pct} f1={entry['f1']}")

    # 贪心子集:每轮加一个让 VALIDATION F1 提升最多的信号,不升即停
    chosen: list[str] = []
    history = []
    cur_f1 = 0.0
    while True:
        trial_best = None
        for cand in signals:
            if cand in chosen:
                continue
            subset = chosen + [cand]
            fused = {s: np.mean([ranks[n][s] for n in subset], axis=0) for s in SPLITS}
            pct, entry = best_entry(fused, y_by_split)
            if trial_best is None or entry["f1"] > trial_best[2]["f1"]:
                trial_best = (cand, pct, entry)
        if trial_best is None or trial_best[2]["f1"] <= cur_f1:
            break
        chosen.append(trial_best[0])
        cur_f1 = trial_best[2]["f1"]
        history.append({"added": trial_best[0], "percentile": trial_best[1], "validationF1": cur_f1})
        print(f"  + {trial_best[0]} -> validation F1 {cur_f1}")

    fused = {s: np.mean([ranks[n][s] for n in chosen], axis=0) for s in SPLITS}
    best_pct, chosen_entry = best_entry(fused, y_by_split)
    threshold = float(np.percentile(fused["TRAIN"], best_pct))

    y_valid = y_by_split["VALIDATION"]
    n_flag = int(chosen_entry["flagged"])
    rng = np.random.default_rng(SEED)
    reps = []
    for _ in range(20):
        r = rng.random(len(y_valid))
        thr = np.sort(r)[::-1][max(0, n_flag - 1)]
        reps.append(common.prf(r, y_valid, thr)["f1"])
    rnd = {"randomF1Mean": round(float(np.mean(reps)), 4), "flagBudget": n_flag}

    metrics_test = common.prf(fused["TEST"], y_by_split["TEST"], threshold)
    test_ses = ses[ses.split == "TEST"]
    flags = fused["TEST"] > threshold
    types = labels.groupby("session_id")["anomaly_type"].agg(list)
    per_type: dict[str, dict] = {}
    tl = [types.get(sid) for sid in test_ses.session_id]
    for atype in sorted(labels.anomaly_type.unique()):
        mask = np.array([isinstance(t, list) and atype in t for t in tl])
        n = int(mask.sum())
        per_type[atype] = {"n": n, "recall": round(int((mask & flags).sum()) / n, 4) if n else None}

    v1 = json.loads(common.TEST_METRICS_PATH.read_text(encoding="utf-8"))
    v2 = json.loads(common.TEST_METRICS_PATH_V2.read_text(encoding="utf-8"))

    with open(common.TRAIN_METRICS_PATH_V3, "w", encoding="utf-8") as handle:
        json.dump({"singles": singles, "greedyHistory": history,
                   "chosenSignals": chosen, "chosenPercentile": best_pct,
                   "validation": chosen_entry, "missingSignalCounts": missing,
                   "thresholdSemantics": "split 内秩融合 = 固定告警预算的批筛口径(transductive),非可部署固定阈值;见 detect_v3.rank_in_split",
                   "seconds": round(time.time() - started, 1)},
                  handle, ensure_ascii=False, indent=2)
    joblib.dump({"signals": chosen, "percentile": best_pct, "threshold": threshold,
                 "model_id": common.MODEL_ID_V3, "model_version": common.MODEL_VERSION_V3,
                 "note": "信号依赖 v1/v2 bundle,重算入口 detect_v3.session_signals",
                 "metadata": {"seed": SEED,
                              "trainEndExclusive": str(common.TRAIN_END_EXCLUSIVE.date()),
                              "validEndExclusive": str(common.VALID_END_EXCLUSIVE.date()),
                              "testEndExclusive": str(common.TEST_END_EXCLUSIVE.date()),
                              "dependencies": {"python": platform.python_version(),
                                               "scikit-learn": sklearn.__version__,
                                               "pandas": pd.__version__},
                              "trainedAt": pd.Timestamp.utcnow().isoformat()}},
                common.BUNDLE_PATH_V3)

    report = {
        "modelId": common.MODEL_ID_V3, "datasetId": common.DATASET_ID,
        "note": "会话级 7 信号 rank 融合,子集与阈值只在 VALIDATION 选;TEST 首盲一次,重跑要求逐字节等价",
        "splits": {"trainEndExclusive": str(common.TRAIN_END_EXCLUSIVE.date()),
                   "validEndExclusive": str(common.VALID_END_EXCLUSIVE.date()),
                   "testEndExclusive": str(common.TEST_END_EXCLUSIVE.date())},
        "singlesValidation": singles, "greedyHistory": history,
        "chosenSignals": chosen, "chosenPercentile": best_pct,
        "validation": chosen_entry, "randomRef": rnd,
        "test": metrics_test, "testRecallByType": per_type,
        "lineage": {"v1TestF1": v1["test"]["f1"], "v2TestF1": v2["test"]["f1"],
                    "v2Chosen": v2["chosenCandidate"]},
        "positives": int(ses.y.sum()), "sessions": int(len(ses)),
    }
    common.write_new_json(common.TEST_METRICS_PATH_V3, report)
    print(json.dumps({"chosenSignals": chosen, "validation": chosen_entry, "test": metrics_test,
                      "recallByType": per_type}, ensure_ascii=False, indent=1))
    print("bundle ->", common.BUNDLE_PATH_V3, f"({round(time.time() - started, 1)}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
