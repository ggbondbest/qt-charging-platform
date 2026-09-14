"""v2:采样点级 IsolationForest + 会话内 TopK 聚合(候选含与 v1 的排序融合)。
用法(仓库根目录):python -m data_analysis.ml.anomaly.detect_v2
动机(v1 的机制性失败):anomaly_labels 是带 recorded_at 的"事件",v1 的会话均值把瞬时
异常稀释 ~10 倍。v2 与标签同粒度打分,会话分 = 组内点分 TopK;robust z 特征把"相对本
会话基线的尖峰"显式放大。候选池 = {绝对特征, 纯对比特征} × topk{1,2,3} × {纯点级, 与v1
会话分排序融合},全部只在 VALIDATION 选优;TEST 首盲一次,独立 model_id,v1 冻结不动。
"""

from __future__ import annotations

import json
import platform
import time

import joblib
import numpy as np
import pandas as pd
import sklearn
from sklearn.preprocessing import StandardScaler
from sklearn.ensemble import IsolationForest

from . import common

SEED = 42
N_TREES = 300
TOPK_GRID = (1, 2, 3)
PERCENTILE_GRID = (88, 90, 92, 94, 95, 96, 97, 98)
FEATURE_SETS = {"abs": common.FEATURES_V2, "contrast": common.FEATURES_V2C}
SPLITS = ("TRAIN", "VALIDATION", "TEST")


def build_points() -> pd.DataFrame:
    if common.POINT_TABLE.exists():
        return pd.read_pickle(common.POINT_TABLE)
    samples = common.load_table("battery_samples")
    sessions = common.load_table("charging_sessions")
    pts = common.point_features(samples, sessions)
    common.OUT_DIR.mkdir(parents=True, exist_ok=True)
    pts.to_pickle(common.POINT_TABLE)
    return pts


def fit_point_model(pts: pd.DataFrame, feats: list[str]) -> tuple:
    train_pts = pts[pts.split == "TRAIN"]
    medians = train_pts[feats].astype(float).median()
    x_train = train_pts[feats].astype(float).fillna(medians).to_numpy()
    scaler = StandardScaler().fit(x_train)
    model = IsolationForest(n_estimators=N_TREES, random_state=SEED, n_jobs=-1)
    model.fit(scaler.transform(x_train))
    return model, scaler, medians


def score_points(pts: pd.DataFrame, feats: list[str], model, scaler, medians) -> pd.Series:
    x = scaler.transform(pts[feats].astype(float).fillna(medians).to_numpy())
    return pd.Series(-model.score_samples(x), index=pts.session_id.to_numpy(), name="score")


def main() -> int:
    started = time.time()
    pts = build_points()
    labels = common.load_table("anomaly_labels")[["session_id", "recorded_at", "anomaly_type"]]
    labels["recorded_at"] = pd.to_datetime(labels["recorded_at"])

    pos_sessions = set(labels.session_id)
    ses = (pts.groupby("session_id", sort=False).agg(split=("split", "first")).reset_index())
    ses["y"] = ses.session_id.isin(pos_sessions).astype(int)
    sid_by_split = {s: ses[ses.split == s].session_id for s in SPLITS}
    y_by_split = {s: ses[ses.split == s].y.to_numpy() for s in SPLITS}
    print(f"points={len(pts)} sessions={len(ses)} positives={int(ses.y.sum())} "
          f"bySplit={ses.groupby('split').y.agg(['size', 'sum']).to_dict()['sum']}")

    # v1 会话分(仅作为候选融合项;它的 TEST 盲评早已消耗,这里只复用其打分函数)
    b1 = joblib.load(common.BUNDLE_PATH)
    sf = pd.read_pickle(common.FEATURE_TABLE)
    x1 = b1["scaler"].transform(sf[b1["features"]].astype(float).fillna(b1["medians"]))
    s1 = pd.Series(-b1["model"].score_samples(x1), index=sf.session_id.to_numpy())

    candidates: dict[str, dict[str, np.ndarray]] = {}
    models: dict[str, tuple] = {}
    for fs_name, feats in FEATURE_SETS.items():
        model, scaler, medians = fit_point_model(pts, feats)
        models[fs_name] = (model, scaler, medians)
        score = score_points(pts, feats, model, scaler, medians)
        print(f"[{fs_name}] point model fitted ({round(time.time() - started, 1)}s)")
        for k in TOPK_GRID:
            s_k = common.topk_scores(score, k)
            pure = {s: s_k.reindex(sid_by_split[s]).to_numpy() for s in SPLITS}
            candidates[f"{fs_name}/top{k}"] = pure
            # 融合 = 各 split 内部按百分位秩平均(v1 会话结构信号 + v2 瞬时信号)
            blended = {}
            for s in SPLITS:
                r2 = pd.Series(pure[s], index=sid_by_split[s]).rank(pct=True)
                r1 = s1.reindex(sid_by_split[s]).rank(pct=True)
                blended[s] = ((r1 + r2) / 2.0).to_numpy()
            candidates[f"{fs_name}/top{k}+v1blend"] = blended

    sweep: dict = {}
    best = None
    for cand, by_split in candidates.items():
        for pct in PERCENTILE_GRID:
            thr = float(np.percentile(by_split["TRAIN"], pct))
            entry = common.prf(by_split["VALIDATION"], y_by_split["VALIDATION"], thr)
            sweep[f"{cand}@pct{pct}"] = entry
            if best is None or entry["f1"] > best[1]["f1"]:
                best = ((cand, pct), entry)
    (best_cand, best_pct), chosen = best
    print("validation best:", best_cand, "pct", best_pct, "f1", chosen["f1"])
    top10 = sorted(sweep.items(), key=lambda kv: -kv[1]["f1"])[:10]
    print("validation top10:", {k: v["f1"] for k, v in top10})
    threshold = float(np.percentile(candidates[best_cand]["TRAIN"], best_pct))

    # 随机参照:同一告警预算下随机排序的期望 F1(seed 固定,可复算)
    y_valid = y_by_split["VALIDATION"]
    n_flag = int(chosen["flagged"])
    rng = np.random.default_rng(SEED)
    reps = []
    for _ in range(20):
        r = rng.random(len(y_valid))
        thr = np.sort(r)[::-1][max(0, n_flag - 1)]
        reps.append(common.prf(r, y_valid, thr)["f1"])
    rnd = {"randomF1Mean": round(float(np.mean(reps)), 4), "flagBudget": n_flag}

    test_scores = candidates[best_cand]["TEST"]
    metrics_test = common.prf(test_scores, y_by_split["TEST"], threshold)

    y_by_session = labels.groupby("session_id")["anomaly_type"].agg(list)
    test_ses = ses[ses.split == "TEST"]
    flags = test_scores > threshold
    types = [y_by_session.get(sid) for sid in test_ses.session_id]
    per_type: dict[str, dict] = {}
    for atype in sorted(labels.anomaly_type.unique()):
        mask = np.array([isinstance(t, list) and atype in t for t in types])
        n = int(mask.sum())
        hit = int((mask & flags).sum())
        per_type[atype] = {"n": n, "recall": round(hit / n, 4) if n else None}

    v1_test = json.loads(common.TEST_METRICS_PATH.read_text(encoding="utf-8"))["test"]

    # 产物先落齐(train metrics + bundle),最后才动 TEST 盲评文件
    with open(common.TRAIN_METRICS_PATH_V2, "w", encoding="utf-8") as handle:
        json.dump({"candidates": len(candidates), "validationBest": {"cand": best_cand, "percentile": best_pct, **chosen},
                   "validationSweep": sweep, "seconds": round(time.time() - started, 1)},
                  handle, ensure_ascii=False, indent=2)
    fs_name = best_cand.split("/")[0]
    blend = "+v1blend" in best_cand
    bundle = {
        "model": models[fs_name][0], "scaler": models[fs_name][1], "medians": models[fs_name][2],
        "threshold": threshold, "features": FEATURE_SETS[fs_name], "featureSet": fs_name,
        "topk": int(best_cand.split("top")[1].split("+")[0]), "blendV1": blend,
        "v1BundlePath": str(common.BUNDLE_PATH) if blend else None,
        "model_id": common.MODEL_ID_V2, "model_version": common.MODEL_VERSION_V2,
        "metadata": {"trainEndExclusive": str(common.TRAIN_END_EXCLUSIVE.date()),
                     "validEndExclusive": str(common.VALID_END_EXCLUSIVE.date()),
                     "testEndExclusive": str(common.TEST_END_EXCLUSIVE.date()),
                     "chosenPercentile": best_pct,
                     "dependencies": {"python": platform.python_version(),
                                      "scikit-learn": sklearn.__version__,
                                      "pandas": pd.__version__, "numpy": np.__version__},
                     "trainedAt": pd.Timestamp.utcnow().isoformat()},
    }
    joblib.dump(bundle, common.BUNDLE_PATH_V2)

    report = {
        "modelId": common.MODEL_ID_V2, "datasetId": common.DATASET_ID,
        "note": "点级 IF+TopK 聚合(候选含 v1 融合);标签只在评测关联。TEST 首盲一次,重跑要求逐字节等价",
        "splits": {"trainEndExclusive": str(common.TRAIN_END_EXCLUSIVE.date()),
                   "validEndExclusive": str(common.VALID_END_EXCLUSIVE.date()),
                   "testEndExclusive": str(common.TEST_END_EXCLUSIVE.date())},
        "chosenCandidate": best_cand,
        "thresholdRule": f"TRAIN 会话分上分位 pct={best_pct}(候选与阈值均在 VALIDATION 按 F1 选定)",
        "validation": chosen, "validationSweepTop10": {k: v for k, v in top10},
        "randomRef": rnd, "test": metrics_test, "testRecallByType": per_type,
        "v1Comparison": {"v1TestF1": v1_test["f1"], "v1TestPrecision": v1_test["precision"],
                         "v1TestRecall": v1_test["recall"]},
        "positives": int(ses.y.sum()), "sessions": int(len(ses)), "points": int(len(pts)),
    }
    common.write_new_json(common.TEST_METRICS_PATH_V2, report)
    print(json.dumps({"chosen": best_cand, "validation": chosen, "test": metrics_test,
                      "recallByType": per_type, "v1TestF1": v1_test["f1"]},
                     ensure_ascii=False, indent=1))
    print("bundle ->", common.BUNDLE_PATH_V2, f"({round(time.time() - started, 1)}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
