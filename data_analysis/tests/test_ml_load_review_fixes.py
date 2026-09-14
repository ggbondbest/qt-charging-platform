"""PR 评审 5 个 P2 修复的回归测试(test_ml_load_review_fixes)。
CI 的 generator job 无 pip install,numpy/pandas/sklearn 缺失时整体自跳,不阻塞发现式收集。
覆盖:P2#1 .000Z 时间戳等价解析;P2#2 周基线 horizon 对齐;P2#3 审计失败不发布可用缓存;
P2#4 冻结评分拒绝覆盖;P2#5 场景目标小时 off-by-one。全程用合成小数据,不读数据集、不碰模型。
"""

import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

try:
    import numpy as np  # noqa: F401
    import pandas as pd

    from data_analysis.ml.load import common

    HAS_DEPS = True
except ImportError:  # CI generator job:裸 python,无数据科学栈
    HAS_DEPS = False


def _utc(text: str) -> datetime:
    return datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def _synthetic_history(ref_utc: datetime) -> list[dict]:
    rows = []
    for offset in range(24, 0, -1):
        moment = ref_utc - timedelta(hours=offset)
        rows.append(
            {
                "station_id": "ST_TEST",
                "city_id": "C_TEST",
                "recorded_at": moment.strftime("%Y-%m-%dT%H:%M:%SZ"),
                "mean_power_kw": 10.0 + offset,
                "capacity": 8,
                "rated_capacity_kw": 120.0,
                "end_available_count": 3,
            }
        )
    return rows


@unittest.skipUnless(HAS_DEPS, "numpy/pandas not installed")
class TimestampParsing(unittest.TestCase):
    """P2#1:契约允许 .000Z 等小数秒;解析器与窗口连续性检查必须按时间点而非字符串比对。"""

    def test_fractional_seconds_equal_plain(self):
        self.assertEqual(
            common.parse_utc("2026-05-01T00:00:00.000Z"),
            common.parse_utc("2026-05-01T00:00:00Z"),
        )

    def test_feature_row_accepts_equivalent_history_formats(self):
        ref = _utc("2026-05-01T00:00:00Z")
        plain = _synthetic_history(ref)
        fractional = [
            dict(row, recorded_at=row["recorded_at"][:-1] + ".000Z") for row in plain
        ]
        self.assertEqual(
            common.build_feature_row(plain, "2026-05-01T00:00:00Z", {}),
            common.build_feature_row(fractional, "2026-05-01T00:00:00.000Z", {}),
        )

    def test_history_gap_still_rejected(self):
        plain = _synthetic_history(_utc("2026-05-01T00:00:00Z"))
        plain[7]["recorded_at"] = _utc(plain[7]["recorded_at"]) - timedelta(hours=1)
        plain[7]["mean_power_kw"] = 0.0  # 保持有限值,确保报错来自连续性而非取值
        with self.assertRaises(ValueError):
            common.build_feature_row(plain, "2026-05-01T00:00:00Z", {})


@unittest.skipUnless(HAS_DEPS, "numpy/pandas not installed")
class LastWeekAlignment(unittest.TestCase):
    """P2#2:第 h 小时目标锚在 reference+(h-1)h,周基线必须取上周同一小时而非固定 reference-168h。"""

    def setUp(self):
        reference = _utc("2026-05-08T00:00:00Z")
        # 每个候选锚点小时放不同值:ref-168h=1 … ref-145h=24
        hours = [reference - timedelta(hours=168 - k) for k in range(0, 24)]
        self.lookup = pd.Series(
            np.arange(1.0, 25.0),
            index=pd.MultiIndex.from_arrays([["ST_TEST"] * 24, hours]),
            name="mean_power_kw",
        )
        self.frame = pd.DataFrame(
            {
                "station_id": ["ST_TEST"],
                "reference_dt": pd.to_datetime([reference], utc=True),
            }
        )

    def test_horizon_anchors(self):
        self.assertEqual(common.last_week_predictions(self.frame, self.lookup, 1)[0], 1.0)
        self.assertEqual(common.last_week_predictions(self.frame, self.lookup, 6)[0], 6.0)
        self.assertEqual(common.last_week_predictions(self.frame, self.lookup, 24)[0], 24.0)

    def test_missing_anchor_is_nan(self):
        self.assertTrue(
            np.isnan(common.last_week_predictions(self.frame, self.lookup, 25)[0])
        )


@unittest.skipUnless(HAS_DEPS, "numpy/pandas not installed")
class CachePublication(unittest.TestCase):
    """P2#3:审计未通过不得产出/替换可训练缓存;P2#4:冻结评分只许创建。"""

    def setUp(self):
        self._tmp = TemporaryDirectory()
        self.out = Path(self._tmp.name)
        self.frame = pd.DataFrame({"a": [1.0, 2.0]})

    def tearDown(self):
        self._tmp.cleanup()

    def _publish(self, passed: bool):
        from data_analysis.ml.load.prepare_data import publish

        publish(self.frame, {"auditPassed": passed}, self.out)

    def test_failure_does_not_publish_pkl(self):
        self._publish(passed=False)
        self.assertFalse((self.out / "joined_usable.pkl").exists())
        self.assertTrue((self.out / "prepare_summary.json").exists())

    def test_failure_preserves_previous_good_cache(self):
        self._publish(passed=True)
        good_bytes = (self.out / "joined_usable.pkl").read_bytes()
        self._publish(passed=False)
        self.assertEqual((self.out / "joined_usable.pkl").read_bytes(), good_bytes)

    def test_success_publishes_frame(self):
        self._publish(passed=True)
        restored = pd.read_pickle(self.out / "joined_usable.pkl")
        pd.testing.assert_frame_equal(restored, self.frame)

    def test_no_temp_file_left_behind(self):
        self._publish(passed=True)
        self.assertEqual(
            [p.name for p in self.out.iterdir() if p.suffix == ".tmp"], []
        )

    def test_write_new_json_refuses_overwrite(self):
        path = self.out / "frozen.json"
        common.write_new_json(path, {"v": 1})
        with self.assertRaises(FileExistsError):
            common.write_new_json(path, {"v": 2})
        import json

        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["v"], 1)


@unittest.skipUnless(HAS_DEPS, "numpy/pandas not installed")
class PreparedGate(unittest.TestCase):
    """P2#3 的下游半边:require_prepared 按审计标志与批次把关。"""

    def _write_summary(self, payload: str):
        root = self.out / "root"
        (root / "outputs" / "ml_load").mkdir(parents=True)
        (root / "outputs" / "ml_load" / "prepare_summary.json").write_text(
            payload, encoding="utf-8"
        )
        return root

    def setUp(self):
        self._tmp = TemporaryDirectory()
        self.out = Path(self._tmp.name)
        self.manifest = {"publishedBatchId": "analytics-B"}

    def tearDown(self):
        self._tmp.cleanup()

    def test_missing_summary_rejected(self):
        with mock.patch.object(common, "DATA_ANALYSIS_ROOT", self.out / "nothing"):
            with self.assertRaises(RuntimeError):
                common.require_prepared(self.manifest)

    def test_failed_audit_rejected(self):
        root = self._write_summary('{"auditPassed": false, "publishedBatchId": "analytics-B"}')
        with mock.patch.object(common, "DATA_ANALYSIS_ROOT", root):
            with self.assertRaises(RuntimeError):
                common.require_prepared(self.manifest)

    def test_batch_mismatch_rejected(self):
        root = self._write_summary('{"auditPassed": true, "publishedBatchId": "analytics-A"}')
        with mock.patch.object(common, "DATA_ANALYSIS_ROOT", root):
            with self.assertRaises(RuntimeError):
                common.require_prepared(self.manifest)

    def test_passing_returns_pickle_path(self):
        root = self._write_summary('{"auditPassed": true, "publishedBatchId": "analytics-B"}')
        pkl = root / "outputs" / "ml_load" / "joined_usable.pkl"
        pkl.write_bytes(b"x")
        with mock.patch.object(common, "DATA_ANALYSIS_ROOT", root):
            self.assertEqual(common.require_prepared(self.manifest), pkl)


@unittest.skipUnless(HAS_DEPS, "numpy/pandas not installed")
class ScenarioHourBucket(unittest.TestCase):
    """P2#5:h01 的目标小时 = reference(契约第 1 个预测区间),不是 reference+1h。"""

    def setUp(self):
        try:
            from data_analysis.ml.load.scenario_analysis import target_hour_bucket
        except ImportError:  # sklearn/joblib 缺席
            self.skipTest("scenario_analysis requires the ML stack")
        self.bucket = target_hour_bucket

    def test_first_horizon_maps_to_reference_hour(self):
        # 22:00Z = 北京 06:00,h01 应落 late_night_0_6 而非 morning_peak_7_9
        frame = pd.DataFrame(
            {"reference_dt": pd.to_datetime(["2026-04-28T22:00:00Z"], utc=True)}
        )
        self.assertEqual(self.bucket(1, frame["reference_dt"]).iloc[0], "late_night_0_6")

    def test_horizon_shifts_bucket_by_h_minus_1(self):
        frame = pd.DataFrame(
            {"reference_dt": pd.to_datetime(["2026-04-28T05:00:00Z"], utc=True)}
        )  # 北京 13:00 → daytime;+5h 到 18:00 → evening peak
        self.assertEqual(self.bucket(1, frame["reference_dt"]).iloc[0], "daytime_10_16")
        self.assertEqual(self.bucket(6, frame["reference_dt"]).iloc[0], "evening_peak_17_21")


if __name__ == "__main__":
    unittest.main()
