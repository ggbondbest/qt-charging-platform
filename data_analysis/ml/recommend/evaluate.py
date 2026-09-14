"""TEST 首盲冻结:模型与全部基线在 2026-05-15~29 事件上一次性批阅,write_new_json 拒绝二次覆盖。
选型全程只用 VALIDATION;TEST 数字只做报告,不回填任何超参。
用法(先完成 train):python -m data_analysis.ml.recommend.evaluate
"""

from __future__ import annotations

import joblib
import numpy as np
import pandas as pd

from . import common
from .train import BASELINES, baseline_scores

TEST_METRICS_PATH = common.OUT_DIR / f"test_metrics_{common.MODEL_ID}.json"


def _ranks(rows: pd.DataFrame, scores: np.ndarray) -> np.ndarray:
    return common.rank_within_groups(scores, rows["label"].to_numpy(dtype=float))


def main() -> int:
    bundle = joblib.load(common.BUNDLE_PATH)
    model = bundle["model"]
    feature_columns = bundle["feature_columns"]
    rows = pd.read_pickle(common.LONG_TABLE)
    test = rows[rows["split"] == "TEST"]

    scores = model.predict_proba(test[feature_columns])[:, 1]
    model_metrics = common.ranking_metrics(_ranks(test, scores), 5)

    counts = np.bincount(_ranks(test, scores).astype(int), minlength=6)[1:6]
    distribution = {str(i + 1): round(float(c / counts.sum()), 4)
                    for i, c in enumerate(counts)}

    rng = np.random.default_rng(7)
    baselines = {name: common.ranking_metrics(
        _ranks(test, baseline_scores(test, name, rng)), 5)
        for name in list(BASELINES) + ["random"]}

    per_city = {}
    for city, part in test.groupby("city_id", sort=True):
        per_city[city] = common.ranking_metrics(
            _ranks(part, model.predict_proba(part[feature_columns])[:, 1]), 5)

    report = {
        "modelId": common.MODEL_ID,
        "modelVersion": common.MODEL_VERSION,
        "datasetId": common.DATASET_ID,
        "note": "模拟数据上的选址排序首盲;TEST 只在定稿后批阅这一次,重跑会因拒绝覆盖而失败",
        "splits": bundle["metadata"]["splits"],
        "testEvents": int(len(test) // 5),
        "model": model_metrics,
        "rankDistribution": distribution,
        "baselines": baselines,
        "perCity": per_city,
    }
    common.write_new_json(TEST_METRICS_PATH, report)

    print(f"{'method':22}{'hit@1':>8}{'hit@3':>8}{'mrr':>8}{'ndcg@5':>8}{'meanRank':>10}")
    for name, entry in {"model": model_metrics, **baselines}.items():
        print(f"{name:22}{entry['hitAt1']:>8}{entry['hitAt3']:>8}{entry['mrr']:>8}"
              f"{entry['ndcgAt5']:>8}{entry['meanRank']:>10}")
    print("correct-station rank distribution:", distribution)
    print("saved (frozen) ->", TEST_METRICS_PATH)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
