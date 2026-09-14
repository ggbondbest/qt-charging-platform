"""v4:语境基线信号 + rank 融合(盲评链最后一张牌)。
用法(仓库根目录):python -m data_analysis.ml.anomaly.detect_v4
链上教训:v1 会话均值抓得住持续偏移、稀释瞬时(0.147);v2 会话内 z 反之(v3 已证明两
视角互补度远低于预期,贪心融合退回纯 v1,0.149)。共同盲区:两者都拿"本会话"或"全局
粗分布"当参照。v4 换成逐点语境参照——期望电流查 (charger_id × SOC档) 中位数表、期望温度
查 (charger_id × 电流档)、期望压差查 charger 基线,参考表只在 TRAIN 点上学(契约)。
压流→cur_ratio 洼地、过热→temp_excess 高地,都不被会话重归一化抹掉。
信号池 = v3 的 7 个 + 3 个语境 + 1 个(v1特征+语境3列)混合 IF;子集与阈值仍只在
VALIDATION 贪心选。新 model_id、独立 TEST 首盲;v1/v2/v3 冻结文件不动。
"""

from __future__ import annotations

import json
import time

import joblib
import numpy as np
import pandas as pd
from sklearn.preprocessing import StandardScaler
from sklearn.ensemble import IsolationForest

from . import common
from .detect_v3 import SEED, best_entry, rank_in_split, session_signals

SPLITS = ("TRAIN", "VALIDATION", "TEST")
CTX_FEATURES = ["ctxCurDeficit", "ctxTempExcess", "ctxCellExcess"]


def context_signals() -> pd.DataFrame:
    """会话级语境偏差:(cur/expCur) 组内最小、(temp-expTemp) 组内最大、(celldiff-exp) 最大。"""
    if common.CTX_TABLE.exists():
        return pd.read_pickle(common.CTX_TABLE)
    pts = pd.read_pickle(common.POINT_TABLE).copy()
    pts["socb"] = (pts["soc_pct"] // 10).clip(0, 9)
    pts["curb"] = (pts["charge_current_a"] // 50).astype(int)
    tr = pts[pts.split == "TRAIN"]
    exp_cur = tr.groupby(["charger_id", "socb"])["charge_current_a"].median().rename("expCur").reset_index()
    exp_temp = tr.groupby(["charger_id", "curb"])["max_temperature_c"].median().rename("expTemp").reset_index()
    exp_cell = tr.groupby("charger_id")["celldiff"].median().rename("expCell").reset_index()
    pts = pts.merge(exp_cur, on=["charger_id", "socb"], how="left") \
             .merge(exp_temp, on=["charger_id", "curb"], how="left") \
             .merge(exp_cell, on=["charger_id"], how="left")
    pts["curRatio"] = pts["charge_current_a"] / pts["expCur"].clip(lower=1.0)
    pts["tempExcess"] = pts["max_temperature_c"] - pts["expTemp"]
    pts["cellExcess"] = pts["celldiff"] - pts["expCell"]
    g = pts.groupby("session_id", sort=False)
    out = pd.DataFrame({"curRatioMin": g["curRatio"].min(),
                        "tempExcessMax": g["tempExcess"].max(),
                        "cellExcessMax": g["cellExcess"].max()})
    out["ctxCurDeficit"] = -out["curRatioMin"]
    out["ctxTempExcess"] = out["tempExcessMax"]
    out["ctxCellExcess"] = out["cellExcessMax"]
    common.CTX_TABLE.parent.mkdir(parents=True, exist_ok=True)
    out.to_pickle(common.CTX_TABLE)
    return out


def mixed_iforest(ctx: pd.DataFrame) -> pd.Series:
    """v1 的 22 会话特征 + 3 语境列 → 单一 IsolationForest(对照候选:一个模型吃所有信息)。"""
    sf = pd.read_pickle(common.FEATURE_TABLE).set_index("session_id")
    feats = common.FEATURES + CTX_FEATURES
    table = sf[common.FEATURES].join(ctx[CTX_FEATURES])
    tr = table[table.join(sf[["split"]]).split == "TRAIN"]
    medians = tr[feats].astype(float).median()
    x = table[feats].astype(float).fillna(medians)
    scaler = StandardScaler().fit(tr[feats].astype(float).fillna(medians))
    model = IsolationForest(n_estimators=300, random_state=SEED, n_jobs=-1)
    model.fit(scaler.transform(x.loc[tr.index]))
    return pd.Series(-model.score_samples(scaler.transform(x)), index=table.index)


def main() -> int:
    started = time.time()
    pts = pd.read_pickle(common.POINT_TABLE)
    labels = common.load_table("anomaly_labels")
    ses = (pts.groupby("session_id", sort=False).agg(split=("split", "first")).reset_index())
    ses["y"] = ses.session_id.isin(set(labels.session_id)).astype(int)
    sid_by_split = {s: ses[ses.split == s].session_id for s in SPLITS}
    y_by_split = {s: ses[ses.split == s].y.to_numpy() for s in SPLITS}

    ctx = context_signals()
    signals = session_signals()  # v3 的 7 信号(v1Mean/v2Max/v2Top3/thermalSpike/...)
    signals["ctxCurDeficit"] = ctx["ctxCurDeficit"]
    signals["ctxTempExcess"] = ctx["tempExcessMax"]
    signals["ctxCellExcess"] = ctx["cellExcessMax"]
    signals["ctxIF"] = mixed_iforest(ctx)
    ranks = {n: rank_in_split(s, sid_by_split) for n, s in signals.items()}

    singles = {}
    for name in signals:
        pct, entry = best_entry(ranks[name], y_by_split)
        singles[name] = {"pct": pct, "f1": entry["f1"]}
        print(f"  {name}: pct={pct} f1={entry['f1']}")

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
    rng = np.random.default_rng(SEED)
    reps = []
    n_flag = int(chosen_entry["flagged"])
    for _ in range(20):
        r = rng.random(len(y_valid))
        thr = np.sort(r)[::-1][max(0, n_flag - 1)]
        reps.append(common.prf(r, y_valid, thr)["f1"])
    rnd = {"randomF1Mean": round(float(np.mean(reps)), 4), "flagBudget": n_flag}

    metrics_test = common.prf(fused["TEST"], y_by_split["TEST"], threshold)
    test_ses = ses[ses.split == "TEST"]
    flags = fused["TEST"] > threshold
    types = labels.groupby("session_id")["anomaly_type"].agg(list)
    tl = [types.get(sid) for sid in test_ses.session_id]
    per_type = {}
    for atype in sorted(labels.anomaly_type.unique()):
        mask = np.array([isinstance(t, list) and atype in t for t in tl])
        n = int(mask.sum())
        per_type[atype] = {"n": n, "recall": round(int((mask & flags).sum()) / n, 4) if n else None}

    lineage = {}
    for key, path in (("v1", common.TEST_METRICS_PATH), ("v2", common.TEST_METRICS_PATH_V2),
                      ("v3", common.TEST_METRICS_PATH_V3)):
        lineage[key + "TestF1"] = json.loads(path.read_text(encoding="utf-8"))["test"]["f1"]

    with open(common.TRAIN_METRICS_PATH_V4, "w", encoding="utf-8") as handle:
        json.dump({"singles": singles, "greedyHistory": history, "chosenSignals": chosen,
                   "chosenPercentile": best_pct, "validation": chosen_entry,
                   "seconds": round(time.time() - started, 1)}, handle, ensure_ascii=False, indent=2)
    joblib.dump({"signals": chosen, "percentile": best_pct, "threshold": threshold,
                 "model_id": common.MODEL_ID_V4, "model_version": common.MODEL_VERSION_V4,
                 "note": "语境参照表来自 detect_v4.context_signals(TRAIN only);重算入口 detect_v4",
                 "metadata": {"seed": SEED, "dependencies": {"pandas": pd.__version__, "numpy": np.__version__},
                              "trainedAt": pd.Timestamp.utcnow().isoformat()}},
                common.BUNDLE_PATH_V4)

    report = {
        "modelId": common.MODEL_ID_V4, "datasetId": common.DATASET_ID,
        "note": "语境基线+rank融合;标签只在评测关联。TEST 首盲一次,重跑要求逐字节等价",
        "splits": {"trainEndExclusive": str(common.TRAIN_END_EXCLUSIVE.date()),
                   "validEndExclusive": str(common.VALID_END_EXCLUSIVE.date()),
                   "testEndExclusive": str(common.TEST_END_EXCLUSIVE.date())},
        "singlesValidation": singles, "greedyHistory": history,
        "chosenSignals": chosen, "chosenPercentile": best_pct,
        "validation": chosen_entry, "randomRef": rnd,
        "test": metrics_test, "testRecallByType": per_type, "lineage": lineage,
        "positives": int(ses.y.sum()), "sessions": int(len(ses)),
    }
    common.write_new_json(common.TEST_METRICS_PATH_V4, report)
    print(json.dumps({"chosenSignals": chosen, "validation": chosen_entry, "test": metrics_test,
                      "recallByType": per_type, "lineage": lineage}, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
