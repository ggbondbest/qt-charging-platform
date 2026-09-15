"""IsolationForest 会话级异常检测:TRAIN 拟合、VALIDATION 选阈值、TEST 首盲冻结。
标签只出现在评测关联里;污染率按验证集 F1 最优反推,不做任何监督拟合。
用法(仓库根目录):python -m data_analysis.ml.anomaly.detect
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
FEATURE_TABLE = common.FEATURE_TABLE
# 阈值候选:TRAIN 分数分布的上分位(异常占比先验 4%,网格扫过去)
PERCENTILE_GRID = (88, 90, 92, 94, 96, 97, 98, 99)


def build_features() -> pd.DataFrame:
    if FEATURE_TABLE.exists():
        return pd.read_pickle(FEATURE_TABLE)
    samples = common.load_table("battery_samples")
    sessions = common.load_table("charging_sessions")
    feats = common.session_features(samples, sessions)
    common.OUT_DIR.mkdir(parents=True, exist_ok=True)
    feats.to_pickle(FEATURE_TABLE)
    return feats


def main() -> int:
    started = time.time()
    feats = build_features()
    labels = common.load_table("anomaly_labels")[["session_id", "anomaly_type"]]
    y_by_session = labels.groupby("session_id")["anomaly_type"].agg(list)
    feats["y"] = feats["session_id"].isin(set(labels.session_id)).astype(int)
    print(f"sessions={len(feats)} positives={int(feats.y.sum())} "
          f"bySplit={feats.groupby('split').y.agg(['size','sum']).to_dict()['sum']}")

    train = feats[feats.split == "TRAIN"]
    medians = train[common.FEATURES].astype(float).median()  # 中位数只在 TRAIN 上拟合,NaN 填它
    scaler = StandardScaler().fit(train[common.FEATURES].astype(float).fillna(medians))
    model = IsolationForest(n_estimators=N_TREES, random_state=SEED, n_jobs=-1)
    model.fit(scaler.transform(train[common.FEATURES].astype(float).fillna(medians)))

    def score(frame: pd.DataFrame) -> np.ndarray:
        z = scaler.transform(frame[common.FEATURES].astype(float).fillna(medians))
        return -model.score_samples(z)  # 越大越异常

    train_scores = score(train)
    valid = feats[feats.split == "VALIDATION"]
    valid_scores = score(valid)
    y_valid = valid["y"].to_numpy()

    sweep = {}
    best = None
    for pct in PERCENTILE_GRID:
        thr = float(np.percentile(train_scores, pct))
        entry = common.prf(valid_scores, y_valid, thr)
        sweep[pct] = entry
        if best is None or entry["f1"] > best[1]["f1"]:
            best = (pct, entry)
    print("validation sweep:", {p: e["f1"] for p, e in sweep.items()})
    chosen_pct, chosen = best
    threshold = float(np.percentile(train_scores, chosen_pct))

    # 规则基线:TRAIN p99 的热/压差阈值,同一评测口径
    rule_thr_t = float(train["temp_spread_max"].quantile(0.99))
    rule_thr_c = float(train["celldiff_max"].quantile(0.99))
    rule_flag = ((valid["temp_spread_max"] > rule_thr_t)
                 | (valid["celldiff_max"] > rule_thr_c)).to_numpy()
    rule_tp = int((rule_flag & (y_valid == 1)).sum())
    rule_fp = int((rule_flag & (y_valid == 0)).sum())
    rule_fn = int((~rule_flag & (y_valid == 1)).sum())
    rule_p = rule_tp / max(1, rule_tp + rule_fp)
    rule_r = rule_tp / max(1, rule_tp + rule_fn)
    rule = {"precision": round(rule_p, 4), "recall": round(rule_r, 4),
            "f1": round(2 * rule_p * rule_r / max(1e-9, rule_p + rule_r), 4),
            "flagged": int(rule_flag.sum())}
    print("rule baseline (temp/cell p99):", rule)

    test = feats[feats.split == "TEST"]
    test_scores = score(test)
    y_test = test["y"].to_numpy()
    metrics_test = common.prf(test_scores, y_test, threshold)
    per_type: dict[str, dict] = {}
    test_labeled = test[test.y == 1]
    test_labeled_scores = score(test_labeled)
    flags = test_labeled_scores > threshold
    for atype in sorted(labels.anomaly_type.unique()):
        mask = test_labeled["session_id"].map(y_by_session).apply(lambda lst: atype in lst)
        n = int(mask.sum())
        hit = int((mask.to_numpy() & flags).sum())
        per_type[atype] = {"n": n, "recall": round(hit / n, 4) if n else None}
    test_rule_flag = ((test["temp_spread_max"] > rule_thr_t)
                      | (test["celldiff_max"] > rule_thr_c)).to_numpy()
    tr_tp = int((test_rule_flag & (y_test == 1)).sum())
    tr_fp = int((test_rule_flag & (y_test == 0)).sum())
    tr_fn = int((~test_rule_flag & (y_test == 1)).sum())
    tr_p = tr_tp / max(1, tr_tp + tr_fp)
    tr_r = tr_tp / max(1, tr_tp + tr_fn)
    test_rule = {"precision": round(tr_p, 4), "recall": round(tr_r, 4),
                 "f1": round(2 * tr_p * tr_r / max(1e-9, tr_p + tr_r), 4),
                 "flagged": int(test_rule_flag.sum())}

    # 产物先落齐(train metrics + bundle),再动 TEST 盲评——上次崩溃就是顺序反了
    with open(common.TRAIN_METRICS_PATH, "w", encoding="utf-8") as handle:
        json.dump({"validation": chosen, "validationRule": rule,
                   "chosenPercentile": chosen_pct, "seconds": round(time.time() - started, 1)},
                  handle, ensure_ascii=False, indent=2)
    bundle = {
        "model": model, "scaler": scaler, "threshold": threshold,
        "medians": medians, "features": common.FEATURES, "model_id": common.MODEL_ID,
        "model_version": common.MODEL_VERSION,
        "metadata": {"trainEndExclusive": str(common.TRAIN_END_EXCLUSIVE.date()),
                     "validEndExclusive": str(common.VALID_END_EXCLUSIVE.date()),
                     "testEndExclusive": str(common.TEST_END_EXCLUSIVE.date()),
                     "chosenPercentile": chosen_pct,
                     "dependencies": {"python": platform.python_version(),
                                      "scikit-learn": sklearn.__version__,
                                      "pandas": pd.__version__, "numpy": np.__version__},
                     "trainedAt": pd.Timestamp.utcnow().isoformat()},
    }
    joblib.dump(bundle, common.BUNDLE_PATH)

    report = {
        "modelId": common.MODEL_ID, "datasetId": common.DATASET_ID,
        "note": "无监督 IsolationForest;标签只在评测关联。TEST 首盲一次,重跑拒绝覆盖",
        "splits": {"trainEndExclusive": str(common.TRAIN_END_EXCLUSIVE.date()),
                   "validEndExclusive": str(common.VALID_END_EXCLUSIVE.date()),
                   "testEndExclusive": str(common.TEST_END_EXCLUSIVE.date())},
        "thresholdRule": f"TRAIN 分数分布上分位 pct={chosen_pct}(阈值在 VALIDATION 按 F1 选定)",
        "validationSweep": sweep, "validation": chosen, "validationRule": rule,
        "test": metrics_test, "testRule": test_rule, "testRecallByType": per_type,
        "positives": int(feats.y.sum()), "sessions": int(len(feats)),
    }
    common.write_new_json(common.TEST_METRICS_PATH, report)
    print(json.dumps({"validation": chosen, "test": metrics_test,
                      "testRule": test_rule, "recallByType": per_type},
                     ensure_ascii=False, indent=1))
    print("bundle ->", common.BUNDLE_PATH, f"({round(time.time() - started, 1)}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
