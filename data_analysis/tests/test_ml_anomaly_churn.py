"""异常/流失两线公共件单测:PRF 口径、时间/用户切分、冻结复用语义、会话特征组内差分。
仅依赖 numpy/pandas(hashlib 走标准库);裸 CI 自跳。不 import sklearn(训练脚本另测)。
"""

import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import numpy as np

try:
    import pandas as pd

    from data_analysis.ml.anomaly import common as ac
    from data_analysis.ml.churn import common as cc

    HAS_DEPS = True
except ImportError:
    HAS_DEPS = False


@unittest.skipUnless(HAS_DEPS, "pandas not installed")
class PrfMath(unittest.TestCase):
    def test_counts_and_scores(self):
        scores = np.array([0.9, 0.2, 0.6, 0.1])
        y = np.array([1, 0, 1, 0])
        entry = ac.prf(scores, y, threshold=0.5)
        self.assertEqual((entry["tp"], entry["fp"], entry["fn"]), (2, 0, 0))
        self.assertEqual((entry["precision"], entry["recall"], entry["f1"]), (1.0, 1.0, 1.0))
        half = ac.prf(scores, y, threshold=0.85)
        self.assertEqual((half["tp"], half["fp"], half["fn"]), (1, 0, 1))
        self.assertEqual(half["precision"], 1.0)
        self.assertEqual(half["recall"], 0.5)

    def test_zero_flag_no_div_error(self):
        entry = ac.prf(np.zeros(3), np.ones(3), threshold=1.0)
        self.assertEqual(entry["f1"], 0.0)


@unittest.skipUnless(HAS_DEPS, "pandas not installed")
class Splits(unittest.TestCase):
    def test_anomaly_time_boundaries(self):
        ts = pd.to_datetime(["2026-04-30", "2026-05-01", "2026-05-14",
                             "2026-05-15", "2026-05-29", "2026-05-30"])
        self.assertEqual(ac.split_of(pd.Series(ts)).tolist(),
                         ["TRAIN", "VALIDATION", "VALIDATION", "TEST", "TEST", "EXCLUDED"])

    def test_churn_user_split_sizes_and_stability(self):
        ids = pd.Series([f"U-X-{i:06d}" for i in range(4000)])
        first = cc.user_split(ids)
        share = first.value_counts(normalize=True)
        self.assertGreater(share["TRAIN"], 0.65)
        self.assertLess(share["TRAIN"], 0.75)
        self.assertTrue((share[["VALIDATION", "TEST"]] - 0.15).abs().max() < 0.05)
        # 同一输入两次切分必须一致(哈希切分,不吃随机流)
        self.assertTrue((cc.user_split(ids) == first).all())


@unittest.skipUnless(HAS_DEPS, "pandas not installed")
class FreezeReuse(unittest.TestCase):
    def setUp(self):
        self._tmp = TemporaryDirectory()
        self.out = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def _quiet_reuse(self, module, path, payload):
        import contextlib, io
        with contextlib.redirect_stdout(io.StringIO()):
            module.write_new_json(path, payload)

    def test_create_then_reuse_then_reject(self):
        path = self.out / "frozen.json"
        ac.write_new_json(path, {"a": 1, "b": {"c": [1, 2]}})
        # 等价重算:通过且文件一字不动
        before = path.read_bytes()
        self._quiet_reuse(ac, path, {"a": 1, "b": {"c": [1, 2]}})
        self.assertEqual(path.read_bytes(), before)
        # 真正不同:拒绝,旧内容保持
        with self.assertRaises(FileExistsError):
            ac.write_new_json(path, {"a": 2})
        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["a"], 1)
        # churn 线同一语义
        path2 = self.out / "f2.json"
        cc.write_new_json(path2, {"v": [1.5]})
        self._quiet_reuse(cc, path2, {"v": [1.5]})
        with self.assertRaises(FileExistsError):
            cc.write_new_json(path2, {"v": [1.6]})


@unittest.skipUnless(HAS_DEPS, "pandas not installed")
class SessionFeatures(unittest.TestCase):
    def test_within_session_differences_only(self):
        """组内差分不能跨会话:两个会话拼接处不得产生伪电压跳变。"""
        t0 = pd.Timestamp("2026-01-01")
        samples = pd.DataFrame({
            "session_id": ["A"] * 3 + ["B"] * 3,
            "charger_id": ["CH1"] * 6,
            "recorded_at": pd.to_datetime([t0 + pd.Timedelta(minutes=5 * i) for i in list(range(3)) * 2]),
            "soc_pct": [10.0, 20.0, 30.0, 50.0, 60.0, 70.0],
            "pack_voltage_v": [300.0, 310.0, 320.0, 400.0, 410.0, 420.0],
            "charge_current_a": [100.0] * 6,
            "max_cell_voltage_v": [3.7] * 6, "min_cell_voltage_v": [3.68] * 6,
            "max_temperature_c": [30.0, 32.0, 34.0, 40.0, 42.0, 44.0],
            "min_temperature_c": [28.0] * 6,
        })
        sessions = pd.DataFrame({
            "session_id": ["A", "B"],
            "started_at": [t0, t0 + pd.Timedelta(minutes=10)],
            "ended_at": [t0 + pd.Timedelta(minutes=10), t0 + pd.Timedelta(minutes=20)],
            "energy_wh": [1000, 2000],
        })
        feats = ac.session_features(samples, sessions).set_index("session_id")
        # A 的每样本电压步进 = 10V;若跨会话污染,B 的首样本差分会变成 80V
        self.assertAlmostEqual(feats.loc["B", "dvolt_per_min"], 10.0, places=6)
        self.assertAlmostEqual(feats.loc["A", "soc_gain"], 20.0, places=6)
        self.assertAlmostEqual(feats.loc["A", "avg_power_kw"], 6.0, places=6)  # 1kWh/10min


if __name__ == "__main__":
    unittest.main()
