"""pointwise 排序训练:每行是 (事件×候选),HistGradientBoostingClassifier 学 P(被选中|特征),推理时按分排序。
只用 split=TRAIN 行拟合;模型质量与基线对比全部在 VALIDATION 上算,TEST 一行不看、一分不批。
产物:outputs/ml_recommend/gbdt-rank-incity-v1.joblib + train_metrics.json(可重写,非冻结件)。

用法(先跑 build_data):python -m data_analysis.ml.recommend.train
"""

from __future__ import annotations

import json
import platform
import time

import joblib
import numpy as np
import pandas as pd
import sklearn
from sklearn.ensemble import HistGradientBoostingClassifier

from . import common

SEED = 42
PARAMS = {
    "max_iter": 400,
    "learning_rate": 0.05,
    "max_leaf_nodes": 31,
    "min_samples_leaf": 40,
    "l2_regularization": 1.0,
    "early_stopping": False,
    "random_state": SEED,
}

# 基线只用表中已有列即可复算,与模型同一排名代码,输赢没有口径差
BASELINES = {
    "popularity28d": ("station_starts_r28", 1.0),
    "meanRating28d": ("station_mean_rating_r28", 1.0),
    "loyalty_lastVisit": ("pair_days_since_last_visit", -1.0),  # 最近光顾的站排最前
    "userHistoryCount": ("pair_attempts_before", 1.0),
}


def rank_frame(rows: pd.DataFrame, scores: np.ndarray) -> dict:
    """rows 必须是按 (event_id, cand) 定序、每事件 5 行的切片;scores 同序。"""
    ranks = common.rank_within_groups(scores, rows["label"].to_numpy(dtype=float))
    return common.ranking_metrics(ranks, n_candidates=5)


def baseline_scores(rows: pd.DataFrame, name: str, rng: np.random.Generator) -> np.ndarray:
    """NaN(没来过/无评分)统一给最差分,不给基线留平局侥幸。"""
    if name == "random":
        return rng.random(len(rows))
    column, sign = BASELINES[name]
    score = sign * rows[column].astype(float).to_numpy()
    return np.where(np.isfinite(score), score, -1e12)


def main() -> int:
    started = time.time()
    common.ensure_out_dir()
    rows = pd.read_pickle(common.LONG_TABLE)
    summary = json.loads(common.SUMMARY_PATH.read_text(encoding="utf-8"))
    feature_columns = summary["featureColumns"]

    train = rows[rows["split"] == "TRAIN"]
    valid = rows[rows["split"] == "VALIDATION"]
    x_train = train[feature_columns]
    y_train = train["label"].astype(int)
    model = HistGradientBoostingClassifier(categorical_features="from_dtype", **PARAMS)
    model.fit(x_train, y_train)
    print(f"fit {len(train)} rows in {time.time() - started:.1f}s")

    rng = np.random.default_rng(SEED)
    valid_scores = model.predict_proba(valid[feature_columns])[:, 1]
    report = {"model": rank_frame(valid, valid_scores)}
    for name in list(BASELINES) + ["random"]:
        report[name] = rank_frame(valid, baseline_scores(valid, name, rng))

    print(f"{'method':22}{'hit@1':>8}{'hit@3':>8}{'mrr':>8}{'ndcg@5':>8}{'meanRank':>10}")
    for name, entry in report.items():
        print(f"{name:22}{entry['hitAt1']:>8}{entry['hitAt3']:>8}{entry['mrr']:>8}"
              f"{entry['ndcgAt5']:>8}{entry['meanRank']:>10}")

    category_levels = {c: train[c].cat.categories.tolist() for c in summary["categorical"]}
    bundle = {
        "model": model,
        "feature_columns": feature_columns,
        "category_levels": category_levels,
        "model_id": common.MODEL_ID,
        "model_version": common.MODEL_VERSION,
        "metadata": {
            "modelId": common.MODEL_ID,
            "modelVersion": common.MODEL_VERSION,
            "datasetId": common.DATASET_ID,
            "task": "station recommendation (learning-to-rank, pointwise)",
            "candidatesPerEvent": 5,
            "params": PARAMS,
            "seed": SEED,
            "splits": summary["splits"],
            "trainRows": int(len(train)),
            "validRows": int(len(valid)),
            "validationMetrics": report,
            "leakageNote": "全部历史特征按事件时刻严格取先;日级 rolling shift(1),配对特征 merge_asof 拒绝恰好相等",
            "boundaryNote": "只做选址排序;空闲桩预测属另一成员,本线不建模可用性",
            "dependencies": {"python": platform.python_version(),
                             "scikit-learn": sklearn.__version__,
                             "pandas": pd.__version__, "numpy": np.__version__},
            "trainedAt": pd.Timestamp.utcnow().isoformat(),
        },
    }
    joblib.dump(bundle, common.BUNDLE_PATH)
    with open(common.TRAIN_METRICS_PATH, "w", encoding="utf-8") as handle:
        json.dump({"modelId": common.MODEL_ID, "validation": report,
                   "seconds": round(time.time() - started, 1)}, handle,
                  ensure_ascii=False, indent=2)
    print("bundle ->", common.BUNDLE_PATH, f"({round(time.time() - started, 1)}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
