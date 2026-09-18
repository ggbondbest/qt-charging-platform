"""推荐线公共件单测:组内排名/悲观平局、排序指标、时间切分边界。
只依赖 numpy/pandas(recommend.common 不引 sklearn),裸 CI 环境整体自跳。
"""

import unittest

try:
    import numpy as np
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


@unittest.skipUnless(HAS_DEPS, "numpy/pandas not installed")
class FrozenCreation(unittest.TestCase):
    """首盲评分只许创建:拒绝覆盖 + 同路径并发恰好一个成功(负荷线复审 P2#B 同款)。"""

    def test_refuses_overwrite_and_race_has_one_winner(self):
        import json
        import threading
        from pathlib import Path
        from tempfile import TemporaryDirectory

        with TemporaryDirectory() as tmp:
            path = Path(tmp) / "sub" / "test_metrics.json"
            common.write_new_json(path, {"model": "first", "ndcg": 0.5})
            with self.assertRaises(FileExistsError):
                common.write_new_json(path, {"model": "second", "ndcg": 0.9})
            self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["model"], "first")

            barrier = threading.Barrier(2)
            outcomes = {}

            def worker(name: str):
                race_path = Path(tmp) / "race.json"
                barrier.wait()
                try:
                    common.write_new_json(race_path, {"model": name})
                    outcomes[name] = "ok"
                except FileExistsError:
                    outcomes[name] = "exists"

            threads = [threading.Thread(target=worker, args=(n,)) for n in ("w1", "w2")]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            self.assertEqual(sorted(outcomes.values()), ["exists", "ok"])
            winner = next(n for n, r in outcomes.items() if r == "ok")
            self.assertEqual(
                json.loads((Path(tmp) / "race.json").read_text(encoding="utf-8"))["model"],
                winner,
            )


if __name__ == "__main__":
    unittest.main()
