"""第五线（排队结果与等待）的回归测试。

分两类：
  · 纯逻辑测试——切分边界、批次核对、独占写入、as-of 口径、特征黑名单，秒级、无数据依赖，CI 也能跑；
  · 产物测试——需要本地先跑过 build_data/train/evaluate（`data_analysis/outputs/` 在 .gitignore 内，
    所以 CI 与刚 clone 的人这里会自动 skip，而不是假装通过）。
"""

from __future__ import annotations

import importlib
import json
import tempfile
import unittest
from pathlib import Path

import numpy as np
import pandas as pd

from data_analysis.ml.queue import build_data, common

ARTIFACTS = [common.MATRIX_PATH, common.BUILD_SUMMARY_PATH, common.BUNDLE_PATH,
             common.TRAIN_METRICS_PATH, common.TEST_REPORT_PATH]
HAVE_ARTIFACTS = all(path.exists() for path in ARTIFACTS)
try:
    import sklearn  # noqa: F401
    HAVE_SKLEARN = True
    from data_analysis.ml.queue import rolling  # rolling 依赖 sklearn，缺环境时整类跳过
except ImportError:  # pragma: no cover
    HAVE_SKLEARN = False


def _bounds() -> dict[str, pd.Timestamp]:
    shift = pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
    return {"startInclusive": pd.Timestamp("2025-12-01") - shift,
            "trainEndExclusive": pd.Timestamp("2026-03-30") - shift,
            "validationEndExclusive": pd.Timestamp("2026-04-29") - shift,
            "testEndExclusive": pd.Timestamp("2026-05-29") - shift}


class ManifestAndSplits(unittest.TestCase):
    def test_verify_batch_fails_closed_on_other_batch(self) -> None:
        original = common.EXPECTED_PUBLISHED_BATCH_ID
        common.EXPECTED_PUBLISHED_BATCH_ID = "analytics-" + "0" * 32
        try:
            with self.assertRaises(common.BatchMismatch):
                common.verify_batch()
        finally:
            common.EXPECTED_PUBLISHED_BATCH_ID = original

    def test_verify_batch_accepts_the_bound_batch(self) -> None:
        manifest = common.read_manifest()
        self.assertEqual(common.verify_batch(manifest)["publishedBatchId"],
                         common.EXPECTED_PUBLISHED_BATCH_ID)

    def test_boundaries_are_business_calendar_days_converted_to_utc(self) -> None:
        manifest = common.read_manifest()
        bounds = common.split_boundaries(manifest)
        # mlSplits 的日期是北京时间日历日，数据是 UTC naive，边界必须整体 -8h
        self.assertEqual(bounds["trainEndExclusive"],
                         pd.Timestamp(manifest["mlSplits"]["trainEnd"]) - pd.Timedelta(hours=8))
        self.assertEqual((bounds["trainEndExclusive"] - bounds["startInclusive"]).days, 118)
        self.assertEqual((bounds["validationEndExclusive"] - bounds["trainEndExclusive"]).days, 30)
        self.assertEqual((bounds["testEndExclusive"] - bounds["validationEndExclusive"]).days, 30)

    def test_assign_split_boundary_ownership(self) -> None:
        bounds = _bounds()
        stamps = pd.to_datetime([bounds["startInclusive"] - pd.Timedelta(seconds=1),
                                 bounds["startInclusive"],
                                 bounds["trainEndExclusive"] - pd.Timedelta(seconds=1),
                                 bounds["trainEndExclusive"],
                                 bounds["validationEndExclusive"],
                                 bounds["testEndExclusive"],
                                 bounds["testEndExclusive"] + pd.Timedelta(seconds=1)])
        got = list(common.assign_split(stamps, bounds))
        self.assertEqual(got, ["EXCLUDED", "TRAIN", "TRAIN", "VALIDATION", "TEST",
                               "EXCLUDED", "EXCLUDED"])

    def test_assign_split_never_invents_labels(self) -> None:
        bounds = _bounds()
        out = common.assign_split(pd.to_datetime(["2020-01-01", "2030-01-01"]), bounds)
        self.assertTrue((out == "EXCLUDED").all())


class FrozenWrites(unittest.TestCase):
    def test_write_new_json_refuses_to_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.json"
            common.write_new_json(path, {"x": 1})
            with self.assertRaises(FileExistsError):
                common.write_new_json(path, {"x": 2})
            with open(path, encoding="utf-8") as handle:
                self.assertEqual(json.load(handle)["x"], 1)

    def test_write_new_pickle_refuses_to_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.pkl"
            common.write_new_pickle(path, pd.DataFrame({"a": [1]}))
            with self.assertRaises(FileExistsError):
                common.write_new_pickle(path, pd.DataFrame({"a": [2]}))

    def test_require_empty_run_dir_refuses_a_populated_dir(self) -> None:
        original = common.OUT_DIR
        self.addCleanup(setattr, common, "OUT_DIR", original)
        with tempfile.TemporaryDirectory() as tmp:
            common.OUT_DIR = Path(tmp)
            (common.OUT_DIR / "leftover.json").write_text("{}", encoding="utf-8")
            with self.assertRaises(FileExistsError):
                common.require_empty_run_dir()
            common.require_empty_run_dir(extra_allowed=("leftover.json",))
        self.assertEqual(common.OUT_DIR, Path(tmp))  # addCleanup 之后再恢复，这里先确认临时目录生效


class AsOfSemantics(unittest.TestCase):
    """as-of 口径的回归：这是本线最容易写错、也最难在结果里看出来的地方。"""

    def _frame(self) -> pd.DataFrame:
        rows = [
            # queue_id joined_at           resolved_at        wait_min y_waste
            ("Q1", "2026-01-10 12:00:00", "2026-01-10 12:10:00", 10.0, 0.0),
            ("Q2", "2026-01-10 12:05:00", "2026-01-10 12:15:00", 10.0, 0.0),
            ("Q3", "2026-01-10 12:06:00", "2026-01-10 12:30:00", np.nan, 1.0),  # 未来才结束
            ("Q4", "2026-01-10 12:15:00", "2026-01-10 12:20:00", np.nan, 1.0),  # 与查询同刻结束
        ]
        frame = pd.DataFrame(rows, columns=["queue_id", "joined_at", "resolved_at", "wait_min",
                                           "y_waste"])
        for column in ("joined_at", "resolved_at"):
            frame[column] = pd.to_datetime(frame[column])
        return frame

    def test_strict_earlier_only_and_same_instant_departure_counts_as_open(self) -> None:
        asof = build_data.StationAsOf(self._frame())
        values = asof.query(np.datetime64("2026-01-10T12:20:00", "ns"))
        # Q3 还没结束（未来）+ Q4 恰好 12:20 结束（同刻，保守视作仍未离开）→ 2
        self.assertEqual(values["open_now"], 2.0)
        self.assertEqual(values["arrivals_60m"], 4.0)   # Q1..Q4 都在 12:20 之前加入
        self.assertEqual(values["arrivals_15m"], 3.0)   # 12:05 含端点：Q2、Q3、Q4
        self.assertEqual(values["wait_obs_3h"], 2.0)    # 只有 Q1、Q2 在 12:20 前结束且等待已知
        self.assertAlmostEqual(values["wait_mean_3h"], 10.0)
        # 已知结局只有 Q1、Q2（都是成功）→ (0 + 20*0.5) / (2 + 20)，同刻结束的 Q4 不算已知
        self.assertEqual(values["waste_n_7d"], 2.0)
        self.assertAlmostEqual(values["waste_rate_prior"], (0.0 + 20 * 0.5) / 22.0, places=12)
        self.assertTrue(np.isnan(values["waste_rate_7d_raw"]) or values["waste_rate_7d_raw"] == 0.0)

    def test_a_query_before_any_history_is_neutral_not_guessed(self) -> None:
        asof = build_data.StationAsOf(self._frame())
        values = asof.query(np.datetime64("2026-01-01T00:00:00", "ns"))
        self.assertEqual(values["open_now"], 0.0)
        self.assertTrue(np.isnan(values["wait_mean_3h"]), "无历史应给 NaN，不该编一个 0")
        self.assertAlmostEqual(values["waste_rate_prior"], 0.5, places=12)

    def test_station_asof_keys_match_the_declared_columns(self) -> None:
        asof = build_data.StationAsOf(self._frame())
        self.assertEqual(sorted(asof.query(np.datetime64("2026-01-10T12:20:00", "ns"))),
                         sorted(build_data.STATION_ASOF_COLUMNS))


class FeatureSelection(unittest.TestCase):
    def test_label_sourced_and_constant_columns_never_become_features(self) -> None:
        frame = pd.DataFrame({
            "queue_id": ["a", "b", "c"], "status": ["SERVED", "ABANDONED", "SERVED"],
            "outcome": ["SERVED", "ABANDONED", "SERVED"], "y_waste": [0, 1, 0],
            "wait_min": [5.0, np.nan, 7.0], "called_at": pd.to_datetime(["2026-01-01", "2026-01-02",
                                                                         "2026-01-03"]),
            "resolved_at": pd.to_datetime(["2026-01-01", "2026-01-02", "2026-01-03"]),
            "session_id": ["S1", None, "S2"], "split": ["TRAIN"] * 3,
            "joined_at": pd.to_datetime(["2026-01-01", "2026-01-02", "2026-01-03"]),
            "joined_local": pd.to_datetime(["2026-01-01", "2026-01-02", "2026-01-03"]),
            "business_date": pd.to_datetime(["2026-01-01", "2026-01-02", "2026-01-03"]),
            "open_now": [0.0, 1.0, 2.0], "position_at_join": [1, 2, 3],
            "transformer_kw": [360.0, 360.0, 360.0],           # 零方差 → 剔除
            "site_type": ["MALL", "HUB", "MALL"], "weather": ["RAIN", "SUN", "RAIN"],
        })
        numeric, categorical, constant = build_data.pick_features(frame)
        self.assertEqual(numeric, ["open_now", "position_at_join"])
        self.assertEqual(categorical, ["site_type", "weather"])
        self.assertEqual(list(constant), ["transformer_kw"])
        self.assertFalse(set(numeric) & set(build_data.FORBIDDEN_FEATURES))
        self.assertFalse(set(categorical) & set(build_data.FORBIDDEN_FEATURES))

    def test_unknown_object_column_is_rejected_not_silently_used(self) -> None:
        frame = pd.DataFrame({"note": ["x", "y"], "open_now": [1.0, 2.0]})
        with self.assertRaises(AssertionError):
            build_data.pick_features(frame)

    def test_design_encodes_categories_and_floats(self) -> None:
        if not HAVE_SKLEARN:  # pragma: no cover
            self.skipTest("本机无 scikit-learn")
        train_module = importlib.import_module("data_analysis.ml.queue.train")
        frame = pd.DataFrame({"open_now": [1, 2, 3], "site_type": ["A", "B", "B"],
                              "is_weekend": pd.array([True, False, True], dtype="boolean")})
        matrix, mask = train_module.design(frame, ["open_now", "is_weekend"], ["site_type"])
        self.assertEqual(list(mask), [False, False, True])
        self.assertEqual(matrix["open_now"].dtype, np.dtype("float64"))
        self.assertEqual(str(matrix["site_type"].dtype), "category")


class MetricHelpers(unittest.TestCase):
    def test_lift_at_rewards_a_perfect_ranking_only(self) -> None:
        y = np.array([1, 1, 0, 0, 0, 0, 0, 0, 0, 0])
        self.assertAlmostEqual(common.lift_at(y, y.astype(float), 0.2), 1 / 0.2, places=6)
        self.assertAlmostEqual(common.lift_at(y, -y.astype(float), 0.2), 0.0, places=6)

    def test_brier_is_zero_only_for_perfect_probabilities(self) -> None:
        y = np.array([1, 0, 1, 0])
        self.assertEqual(common.brier(y, y.astype(float)), 0.0)
        self.assertAlmostEqual(common.brier(y, np.full(4, y.mean())),
                               float(np.mean((y - y.mean()) ** 2)), places=12)

    def test_calibration_bins_partition_every_row(self) -> None:
        rng = np.random.default_rng(0)
        prob = rng.random(500)
        table = common.calibration_bins((prob > 0.5).astype(int), prob, bins=5)
        self.assertEqual(int(table["n"].sum()), 500)

    def test_simulated_note_names_the_batch_and_the_caveat(self) -> None:
        note = common.simulated_note()
        self.assertIn("模拟数据", note)
        self.assertIn(common.EXPECTED_PUBLISHED_BATCH_ID, note)
        self.assertIn("不代表真实运营数据", note)


@unittest.skipUnless(HAVE_ARTIFACTS, "本地未跑过 build_data/train/evaluate，产物测试跳过")
class PublishedArtifacts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
            cls.summary = json.load(handle)
        with open(common.TRAIN_METRICS_PATH, encoding="utf-8") as handle:
            cls.train = json.load(handle)
        with open(common.TEST_REPORT_PATH, encoding="utf-8") as handle:
            cls.report = json.load(handle)

    def test_every_artifact_binds_the_same_batch(self) -> None:
        for source in (self.summary, self.train, self.report):
            self.assertEqual(source["publishedBatchId"], common.EXPECTED_PUBLISHED_BATCH_ID)
        self.assertEqual(self.report["matrixSha256"], self.summary["matrixSha256"])

    def test_matrix_carries_no_missing_labels_and_a_clean_id(self) -> None:
        frame = pd.read_pickle(common.MATRIX_PATH)
        self.assertTrue(frame["queue_id"].is_unique)
        self.assertFalse(frame["y_waste"].isna().any())
        self.assertEqual(set(frame["outcome"].unique()), {"SERVED", "ABANDONED", "CALL_EXPIRED"})
        served = frame["outcome"] == "SERVED"
        served = frame["outcome"] == "SERVED"
        self.assertFalse(frame.loc[served, "wait_min"].isna().any())
        abandoned = frame["outcome"] == "ABANDONED"
        self.assertTrue(frame.loc[abandoned, "wait_min"].isna().all(),
                        "ABANDONED 自己退队、从未被叫号，等待属删失；填 0 会假装模型很准")
        self.assertFalse(frame.loc[~abandoned, "wait_min"].isna().any(),
                         "SERVED 与 CALL_EXPIRED 都有「叫号前等待」，不该缺")

    def test_feature_list_contains_no_label_columns(self) -> None:
        used = set(self.summary["features"]["numeric"]) | set(self.summary["features"]["categorical"])
        self.assertFalse(used & set(build_data.FORBIDDEN_FEATURES))
        self.assertFalse(used & set(common.NON_FEATURE_COLUMNS))
        self.assertIn("position_at_join", used)
        self.assertNotIn("transformer_kw", used, "零方差列不该回来")

    def test_leak_audit_actually_ran_and_found_nothing(self) -> None:
        audit = self.summary["leakAudit"]
        self.assertGreaterEqual(audit["sampled"], 300)
        for name, delta in audit["maxAbsDiff"].items():
            self.assertEqual(delta, 0.0, f"{name} 的 as-of 口径与暴力重算不一致")

    def test_train_never_consulted_the_test_split(self) -> None:
        self.assertGreater(self.train["testRowsUntouched"], 0)
        self.assertEqual(self.train["trainRows"], int((pd.read_pickle(common.MATRIX_PATH)["split"]
                                                       == "TRAIN").sum()))

    def test_test_metrics_are_reported_and_above_a_declared_floor(self) -> None:
        risk = self.report["abandonRisk"]
        self.assertGreaterEqual(risk["auc"], 0.62, "低于排位基线就说明这条线白做了，必须查")
        self.assertGreater(risk["auc"], risk["aucPositionOnlyBaseline"])
        self.assertTrue(risk["frozenOperatingPoint"]["precision"] >= 0.50)
        wait = self.report["waitMinutes"]
        self.assertLess(wait["modelMae"], wait["baselineGlobalMedianMae"])
        self.assertTrue(wait["outputRangeLegal"])

    def test_reports_carry_the_simulated_data_disclaimer(self) -> None:
        note = common.simulated_note()
        for source in (self.summary, self.train, self.report):
            self.assertEqual(source["disclaimer"], note)
        with open(common.TEST_REPORT_MD, encoding="utf-8") as handle:
            self.assertIn("模拟数据", handle.read())


@unittest.skipUnless(HAVE_SKLEARN, "环境缺 scikit-learn，滚动研究相关用例整体跳过")
class RollingStudy(unittest.TestCase):
    """十轮滚动重训的折设计与阈值规则：纯逻辑，不需要产物，CI 可跑。"""

    def test_folds_tile_the_window_with_no_overlap_or_leak(self) -> None:
        bounds = common.split_boundaries(common.read_manifest())
        folds = rolling.fold_plan(bounds, 10)
        self.assertEqual(len(folds), 10)
        # 每轮的验证段必须落在窗口内，且拟合截止严格晚于验证段起点
        self.assertGreaterEqual(folds[0]["valStart"], bounds["startInclusive"])
        for fold in folds:
            self.assertEqual(fold["evalEnd"] - fold["fitEnd"], pd.Timedelta(days=rolling.HORIZON_DAYS))
            self.assertEqual(fold["fitEnd"] - fold["valStart"], pd.Timedelta(days=rolling.VALIDATION_DAYS))
        # 评测窗互不重叠、逐轮首尾相接，最后一轮正好压在 TEST 末端
        for previous, current in zip(folds, folds[1:]):
            self.assertEqual(previous["evalEnd"], current["fitEnd"])
            self.assertLess(previous["evalEnd"], current["evalEnd"])
        self.assertEqual(folds[-1]["evalEnd"], bounds["testEndExclusive"])

    def test_requesting_more_rounds_than_the_window_holds_is_refused(self) -> None:
        bounds = common.split_boundaries(common.read_manifest())
        with self.assertRaises(ValueError) as caught:
            rolling.fold_plan(bounds, 40)
        self.assertIn("最多支持", str(caught.exception))
        with self.assertRaises(ValueError):
            rolling.fold_plan(bounds, 0)

    def test_threshold_rule_reports_honestly_when_precision_floor_misses(self) -> None:
        rng = np.random.default_rng(0)
        y = rng.binomial(1, 0.9, 400)                    # 90% 正例：任何告警档精确率都≈0.9
        prob = y * 0.6 + rng.uniform(0, 0.4, 400)        # 单调可分，取高档仍能≥0.50
        point = rolling.pick_threshold(prob, y)
        self.assertGreaterEqual(point["precision"], 0.50)
        self.assertIn("精确率≥0.50", point["rule"])
        # 反例：标签全 0 时任何档位精确率都是 0，规则必须自曝而不假装达标
        low = rolling.pick_threshold(prob, np.zeros(400, dtype=int))
        self.assertLess(low["precision"], 0.50)
        self.assertIn("不假装达标", low["rule"])

    def test_candidate_sets_cover_full_and_dynamics_only(self) -> None:
        numeric = ["arrivals_60m", "open_now", "waste_rate_prior", "position_at_join",
                   "rated_power_kw", "hour_local"]
        categorical = ["weather"]
        sets = rolling.candidate_sets(numeric, categorical)
        self.assertEqual(sets["full"], (numeric, categorical))
        dynamic, cat = sets["dynamicsPlusPosition"]
        self.assertEqual(dynamic, ["arrivals_60m", "open_now", "waste_rate_prior", "position_at_join"])
        self.assertEqual(cat, [], "动态集不放类别列：消融里它就是纯数值那一套")


@unittest.skipUnless(HAVE_SKLEARN and common.ROLLING_SUMMARY_PATH.exists(),
                     "未跑过 rolling（或缺 sklearn），跳过产物核对")
class RollingArtifacts(unittest.TestCase):
    def setUp(self) -> None:
        with open(common.ROLLING_SUMMARY_PATH, encoding="utf-8") as handle:
            self.summary = json.load(handle)

    def test_ten_rounds_are_reported_and_aggregates_match_the_rounds(self) -> None:
        rounds = self.summary["roundsDetail"]
        self.assertEqual(len(rounds), self.summary["rounds"])
        self.assertEqual([row["round"] for row in rounds], list(range(len(rounds))))
        for name, block in self.summary["metrics"].items():
            values = np.array([row[name] for row in rounds], dtype=float)
            self.assertAlmostEqual(float(values.mean()), block["mean"], places=4,
                                   msg=f"{name} 的均值与逐轮明细对不上")
            self.assertAlmostEqual(float(values.min()), block["min"], places=4, msg=name)
            self.assertAlmostEqual(float(values.max()), block["max"], places=4, msg=name)

    def test_every_fold_scores_only_on_data_strictly_after_its_fit_cutoff(self) -> None:
        for row in self.summary["roundsDetail"]:
            self.assertLess(pd.Timestamp(row["fitEnd"]), pd.Timestamp(row["evalEnd"]))
            self.assertGreater(row["evalRows"], rolling.MIN_EVAL_ROWS)
            self.assertGreaterEqual(row["evalPositives"], rolling.MIN_EVAL_POSITIVES)
            self.assertIn(row["chosenSet"], ("full", "dynamicsPlusPosition"))
            self.assertEqual(row["simulatedNote"], common.simulated_note())

    def test_summary_states_the_blind_test_caveat(self) -> None:
        self.assertIn("盲测", self.summary["caveat"])
        self.assertEqual(self.summary["disclaimer"], common.simulated_note())
        with open(common.ROLLING_SUMMARY_MD, encoding="utf-8") as handle:
            self.assertIn("模拟数据", handle.read())


if __name__ == "__main__":
    unittest.main()
