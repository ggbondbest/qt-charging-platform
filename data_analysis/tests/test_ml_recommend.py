"""推荐线公共件单测:组内排名/悲观平局、排序指标、时间切分边界。
只依赖 numpy/pandas(recommend.common 不引 sklearn),裸 CI 环境整体自跳。
"""

import unittest

import numpy as np

try:
    import pandas as pd

    from data_analysis.ml.recommend import common

    HAS_DEPS = True
except ImportError:
    HAS_DEPS = False


@unittest.skipUnless(HAS_DEPS, "numpy/pandas not installed")
class RankWithinGroups(unittest.TestCase):
    def test_simple_ranks(self):
        # 事件1:正例分最高→rank1;事件2:正例 0.7 仅次于 0.8→rank2
        scores = [0.9, 0.1, 0.5, 0.2, 0.3, 0.8, 0.2, 0.7, 0.65, 0.1]
        labels = [1, 0, 0, 0, 0, 0, 0, 1, 0, 0]
        ranks = common.rank_within_groups(np.array(scores), np.array(labels, dtype=float))
        self.assertEqual(ranks.tolist(), [1, 2])

    def test_pessimistic_ties(self):
        # 正例与另外两候选同分:名次按最坏算 3
        scores = [0.5, 0.5, 0.5, 0.1, 0.0]
        labels = [1, 0, 0, 0, 0]
        ranks = common.rank_within_groups(np.array(scores), np.array(labels, dtype=float))
        self.assertEqual(ranks.tolist(), [3])

    def test_requires_single_positive(self):
        with self.assertRaises(ValueError):
            common.rank_within_groups(np.zeros(5), np.array([1, 0, 0, 1, 0], dtype=float))


@unittest.skipUnless(HAS_DEPS, "numpy/pandas not installed")
class RankingMetrics(unittest.TestCase):
    def test_perfect(self):
        metrics = common.ranking_metrics(np.array([1, 1, 1]), 5)
        self.assertEqual((metrics["hitAt1"], metrics["mrr"], metrics["ndcgAt5"]), (1.0, 1.0, 1.0))

    def test_random_averages_midpack(self):
        ranks = np.array([1, 2, 3, 4, 5])
        metrics = common.ranking_metrics(ranks, 5)
        self.assertEqual(metrics["hitAt1"], 0.2)
        self.assertEqual(metrics["meanRank"], 3.0)
        self.assertEqual(metrics["medianRank"], 3.0)


@unittest.skipUnless(HAS_DEPS, "numpy/pandas not installed")
class SplitBoundaries(unittest.TestCase):
    def test_edges(self):
        ts = pd.to_datetime([
            "2026-04-30T23:59:59", "2026-05-01T00:00:00",
            "2026-05-14T23:59:59", "2026-05-15T00:00:00",
            "2026-05-29T23:59:59", "2026-05-30T00:00:00"], utc=False)
        out = common.split_of(pd.Series(ts)).tolist()
        self.assertEqual(out, ["TRAIN", "VALIDATION", "VALIDATION",
                               "TEST", "TEST", "EXCLUDED"])


if __name__ == "__main__":
    unittest.main()
