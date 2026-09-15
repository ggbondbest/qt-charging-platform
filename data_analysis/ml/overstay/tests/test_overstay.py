"""第八线（会话占桩超时提醒）的回归测试。

分两层：
  · **纯逻辑层**——批次闸门 fail-closed、按 started_at 落段、双时间轴 as-of 语义（**计数看
    started_at、超占数值看 unplugged_at**，这是本线与第五/六/七线根本不同的地方）、
    收缩公式、禁入名单闸门、候选集前缀划分、运营阈值与逐日预算、折计划铺满、符号检验：
    不读任何产物，秒级；
  · **产物层**——需要本地先跑过 features / train / evaluate / rolling。
    ``data_analysis/outputs/`` 在 .gitignore 内，CI 与刚 clone 的人自动 skip 而不是假装通过；
    本线**未新增任何数据**，所以没有派生数据集这一层。

产物层刻意钉住几条**事实**（不是愿望），都是本线开发中真出过问题或真出过负结果的地方：
  · ``planned_hours`` 的单位：第一版把 Wh 当 kWh 用、列大了 1000 倍，靠 --remind-day 的人工
    目检才发现——现在用公式与 24h 上界双重钉住，单位再混立即炸；
  · ``historyOnly`` 在 TEST 上低于 ``staticOnly``——"用户习惯没有场景之外的独立增量"是本线
    交付给运营的负结论，十轮滚动全部为非正，测试把它冻在产物里；
  · 冻结阈值搬到 TEST 后 ``daysOverBudget == 30``（并列分块导致逐日超预算）也是如实钉住的事实，
    不许有人悄悄把口径改舒服。

用法（仓库根目录）：python -m unittest discover -s data_analysis/ml/overstay/tests -t .
"""

from __future__ import annotations

import json
import math
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import joblib
import numpy as np
import pandas as pd

from data_analysis.ml.overstay import common, features

try:  # train / evaluate / rolling 顶层引用 sklearn；缺依赖时相关测试整体跳过而不是报错
    from data_analysis.ml.overstay import evaluate, rolling, train

    HAVE_SKLEARN = True
except ImportError:  # pragma: no cover
    evaluate = rolling = train = None
    HAVE_SKLEARN = False

ARTIFACTS = (common.FEATURES_PATH, common.BUILD_SUMMARY_PATH, common.BUNDLE_PATH,
             common.TRAIN_METRICS_PATH, common.TEST_REPORT_PATH)
HAVE_ARTIFACTS = HAVE_SKLEARN and all(path.exists() for path in ARTIFACTS)
HAVE_ROLLING = HAVE_SKLEARN and common.ROLLING_SUMMARY_PATH.exists()


def _bounds() -> dict[str, pd.Timestamp]:
    """与 manifest 等价的边界，但不读盘（纯逻辑测试要能在任何机器上跑）；
    两者是否仍一致由 ``Splits.test_hardcoded_bounds_still_match_the_manifest`` 把守。"""
    shift = pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
    return {"startInclusive": pd.Timestamp("2025-12-02") - shift,
            "trainEndExclusive": pd.Timestamp("2026-03-30") - shift,
            "validationEndExclusive": pd.Timestamp("2026-04-29") - shift,
            "testEndExclusive": pd.Timestamp("2026-05-29") - shift}


def _tiny_frame() -> pd.DataFrame:
    """给 ``pick_features`` 用的最小表：10 个类别列齐、三组成员各有、含禁入/忽略/零方差列。"""
    n = 12
    frame = pd.DataFrame({
        "session_id": [f"S{i}" for i in range(n)], "user_id": ["U1", "U2"] * 6,
        "station_id": ["ST"] * n, "started_at": pd.date_range("2026-01-01", periods=n),
        "ended_at": pd.date_range("2026-01-01", periods=n) + pd.Timedelta(hours=2),
        "unplugged_at": pd.date_range("2026-01-01", periods=n) + pd.Timedelta(hours=3),
        "business_date": [pd.Timestamp("2026-01-01")] * n, "split": ["TRAIN"] * n,
        "y_over": [0, 1] * 6, "over_min": [5.0, 40.0] * 6, "duration_min": [120.0] * n,
        "status": ["FINISHED"] * n, "energy_wh": [1000.0] * n,
        "vehicle_class": ["SEDAN"] * n, "site_type": ["OFFICE"] * n,
        "scenario_event": ["NONE"] * n, "weather_prev": ["SUNNY"] * n,
        "connector_type": ["AC"] * n, "manufacturer": ["V1"] * n, "charger_model": ["M1"] * n,
        "segment": ["CORE"] * n, "membership": ["NONE"] * n, "is_weekend": [0] * n,
        "planned_hours": [float(2 * i + 1) for i in range(n)], "hour": list(range(n)),
        "hour_block": [0, 0, 1, 1, 2, 2] * 2, "target_value": [float(9 * i) for i in range(n)],
        "rated_power_kw": [60.0] * n, "battery_capacity_kwh": [70.0] * n,
        "user_age_days": [float(30 + i) for i in range(n)], "user_sessions_prior": [float(i) for i in range(n)],
        "user_over_rate_prior": [round(0.05 * i, 4) for i in range(n)], "user_over_rate_30d": [0.2] * n,
        "us_joint_sessions_prior": [float(i) for i in range(n)], "station_sessions_prior": [float(i) for i in range(n)],
        "station_over_rate_prior": [0.15] * n, "cell_over_rate_prior": [0.12] * n,
        "station_abandon_share_prior": [round(0.02 * i, 4) for i in range(n)], "queue_open_at_start": [float(i % 4) for i in range(n)],
        "from_queue": [0, 1] * 6, "own_position_at_join": [float(i) for i in range(n)],
        "temp_c_max_prev": [float(5 + i) for i in range(n)], "rainfall_mm_sum_prev": [0.5] * n,
        "has_campaign": [1] * n, "constant_column": [7] * n, "demand_multiplier": [1.0] * n,
        "target_mode": ["ENERGY"] * n})
    return frame


class BatchGate(unittest.TestCase):
    def test_verify_batch_rejects_other_batch_and_missing_field(self) -> None:
        with self.assertRaises(common.BatchMismatch):
            common.verify_batch({"publishedBatchId": "analytics-" + "0" * 32})
        with self.assertRaises(common.BatchMismatch):
            common.verify_batch({"datasetId": common.DATASET_ID})
        self.assertEqual(common.verify_batch({"publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID})
                         ["publishedBatchId"], common.EXPECTED_PUBLISHED_BATCH_ID)


class Splits(unittest.TestCase):
    def test_hardcoded_bounds_still_match_the_manifest(self) -> None:
        self.assertEqual(_bounds(), common.split_boundaries(common.read_source_manifest()))

    def test_boundaries_are_business_days_minus_eight_hours(self) -> None:
        bounds = common.split_boundaries(common.read_source_manifest())
        self.assertEqual((bounds["trainEndExclusive"] - bounds["startInclusive"]).days, 118)
        self.assertEqual((bounds["validationEndExclusive"] - bounds["trainEndExclusive"]).days, 30)
        self.assertEqual((bounds["testEndExclusive"] - bounds["validationEndExclusive"]).days, 30)
        self.assertEqual(bounds["trainEndExclusive"].strftime("%Y-%m-%d %H:%M"), "2026-03-29 16:00")

    def test_split_is_by_started_at_and_boundaries_belong_to_next_segment(self) -> None:
        bounds = _bounds()
        stamps = [bounds["startInclusive"] - pd.Timedelta(days=1), bounds["startInclusive"],
                  bounds["trainEndExclusive"] - pd.Timedelta(hours=1), bounds["trainEndExclusive"],
                  bounds["validationEndExclusive"], bounds["testEndExclusive"] - pd.Timedelta(hours=1),
                  bounds["testEndExclusive"]]
        self.assertEqual(common.assign_split(stamps, bounds).tolist(),
                         ["EXCLUDED", "TRAIN", "TRAIN", "VALIDATION", "TEST", "TEST", "EXCLUDED"])

    def test_excluded_rows_are_kept_as_history_not_dropped(self) -> None:
        """窗口外的会话是窗口内会话合法的"过去"：assign_split 必须给标签而不是筛掉。"""
        bounds = _bounds()
        stamps = pd.Series([bounds["startInclusive"] - pd.Timedelta(days=3),
                            bounds["testEndExclusive"] + pd.Timedelta(days=3)])
        self.assertEqual(common.assign_split(stamps, bounds).tolist(), ["EXCLUDED", "EXCLUDED"])


class DualAxisAsOfSemantics(unittest.TestCase):
    """本线的心脏：两套时间轴。计数按 started_at 可见，超占数值按 unplugged_at 才可见。"""

    def test_cumulative_is_strictly_earlier_and_same_instant_invisible(self) -> None:
        base = pd.Timestamp("2026-01-01")
        events = pd.DataFrame({"user_id": ["A"] * 5,
                               "started_at": [base, base, base + pd.Timedelta(days=1),
                                              base + pd.Timedelta(days=1), base + pd.Timedelta(days=2)],
                               "_v": [2.0, 3.0, 4.0, 5.0, 6.0]})
        engine = common.AsOfCounts(events, key="user_id", stamps="started_at", value="_v")
        query = pd.DataFrame({"user_id": ["A", "A", "A", "A", "B"],
                              "__query_time": pd.to_datetime([base, base + pd.Timedelta(days=1),
                                                              base + pd.Timedelta(days=2),
                                                              base + pd.Timedelta(days=9),
                                                              base + pd.Timedelta(days=9)])})
        counts, sums = engine.cumulative(query, "user_id", value="_v")
        self.assertEqual(counts.tolist(), [0.0, 2.0, 4.0, 5.0, 0.0])
        self.assertEqual(sums.tolist(), [0.0, 5.0, 14.0, 20.0, 0.0])
        with self.assertRaises(AssertionError):  # 取值列构造时绑定，传错名必须炸不能静默
            engine.cumulative(query, "user_id", value="_other")
        counter = common.AsOfCounts(events, key="user_id", stamps="started_at")
        two, also_two = counter.cumulative(query, "user_id")
        self.assertEqual(two.tolist(), also_two.tolist())  # 纯计数时二者相同

    def test_window_is_half_open_on_both_ends(self) -> None:
        base = pd.Timestamp("2026-01-01")
        events = pd.DataFrame({"user_id": ["A"] * 4,
                               "started_at": [base, base + pd.Timedelta(days=10),
                                              base + pd.Timedelta(days=10), base + pd.Timedelta(days=31)]})
        engine = common.AsOfCounts(events, key="user_id", stamps="started_at")
        query = pd.DataFrame({"user_id": ["A", "A"],
                              "__query_time": pd.to_datetime([base + pd.Timedelta(days=40)] * 2)})
        counts, _ = engine.window(query, "user_id", days=30)
        self.assertEqual(counts.tolist(), [3.0, 3.0])  # 第 0 天出窗；同刻不可见

    def test_global_prior_uses_two_separate_time_axes(self) -> None:
        """先验事件在 unplugged 轴、查询在 started 轴：拔枪恰好等于本场开始的那一刻，必须先于不可见。"""
        events = pd.to_datetime(["2026-01-01", "2026-01-01", "2026-01-02", "2026-01-03"])
        values = np.array([1.0, 0.0, 1.0, 0.0])
        queries = pd.to_datetime([pd.Timestamp("2026-01-01"), pd.Timestamp("2026-01-02"),
                                  pd.Timestamp("2026-01-02 12:00"), pd.Timestamp("2026-02-01")])
        counts, sums = common.global_prior_at(queries, events, values)
        self.assertEqual(counts.tolist(), [0.0, 2.0, 3.0, 4.0])  # 01-02 整点同刻不可见，12:00 时那条可见
        self.assertEqual(sums.tolist(), [0.0, 1.0, 2.0, 2.0])    # exclusive 前缀
        plain_counts, plain_sums = common.global_prior_at(queries, events)
        self.assertEqual(plain_counts.tolist(), counts.tolist())
        self.assertEqual(plain_sums.tolist(), counts.tolist(), "event_values=None 时值和=条数")
        late = pd.to_datetime([pd.Timestamp("2027-01-01")])  # 查询晚于一切事件 → 全部可见，不许越界
        self.assertEqual(common.global_prior_at(late, events, values)[0].tolist(), [4.0])
        self.assertEqual(common.global_prior_at(late, events, values)[1].tolist(), [2.0])

    def test_na_to_int64_is_the_reason_resolve_side_filters_notna(self) -> None:
        """钉住"为什么"：NaT 转 int64 是哨兵最小值，searchsorted 把它算成"早于一切"。
        features.attach_queue_ops 对 resolved_at.notna() 的过滤是这条陷阱的唯一防线。"""
        sentinel = pd.NaT.to_datetime64().astype("int64")
        self.assertEqual(int(sentinel), -9223372036854775808)
        self.assertEqual(int(np.searchsorted(np.sort(np.array([sentinel, 0])), 0, side="left")), 1,
                         "NaT 哨兵会被当成一条「永远可见」的事件——所以未离场记录必须先筛掉再建引擎")

    def test_smoothed_formula_matches_its_definition(self) -> None:
        got = common.smoothed(np.array([0.0, 5.0, 100.0]), np.array([0.0, 1.0, 20.0]),
                              alpha=15.0, priors=np.array([0.5, 0.5, 0.5]))
        self.assertAlmostEqual(got[0], 0.5)  # 无观测 → 完全是先验
        self.assertAlmostEqual(got[1], (1.0 + 15 * 0.5) / (5.0 + 15))
        self.assertGreater(got[1], got[2], "同为 20% 经验率，观测少的该更靠近先验")

    def test_strict_last_hides_same_instant_and_gives_nat_for_never(self) -> None:
        base = pd.Timestamp("2026-01-01")
        left = pd.DataFrame({"user_id": ["A", "A", "B"],
                             "started_at": [base + pd.Timedelta(days=1), base + pd.Timedelta(days=2),
                                            base + pd.Timedelta(days=5)]})
        events = pd.DataFrame({"user_id": ["A", "A", "A"],
                               "unplugged_at": [base, base + pd.Timedelta(days=1),
                                                base + pd.Timedelta(days=2)],
                               "over_min": [10.0, 90.0, 45.0]})
        got = common.strict_last(left, "started_at", "user_id", events, "unplugged_at", ["over_min"])
        self.assertEqual(float(got["over_min"].iloc[0]), 10.0)   # 同刻（拔枪=开始）那条不可见
        self.assertEqual(float(got["over_min"].iloc[1]), 90.0)   # 昨天拔枪的看得见
        self.assertTrue(pd.isna(got["over_min"].iloc[2]))        # B 从未拔枪 → NaN，不是 0 不是哨兵
        self.assertTrue(pd.isna(got["__last_time"].iloc[2]))

    def test_strict_last_refuses_null_join_key(self) -> None:
        left = pd.DataFrame({"user_id": ["A"], "started_at": [pd.Timestamp("2026-01-02")]})
        events = pd.DataFrame({"user_id": [None], "unplugged_at": [pd.Timestamp("2026-01-01")],
                               "over_min": [1.0]})
        with self.assertRaises(AssertionError):
            common.strict_last(left, "started_at", "user_id", events, "unplugged_at", ["over_min"])

    def test_time_gap_is_nat_safe(self) -> None:
        later = pd.to_datetime(["2026-01-03", "2026-01-03"])
        earlier = pd.to_datetime(["2026-01-01", None])
        gap_days = features.time_gap(later, earlier, 86_400.0)
        self.assertAlmostEqual(gap_days[0], 2.0)
        self.assertTrue(np.isnan(gap_days[1]))
        self.assertTrue((gap_days[~np.isnan(gap_days)] >= 0).all(), "不能出现 int64 溢出的负哨兵")

    def test_lagged_prev_never_crosses_cities(self) -> None:
        """pivot 成 日×城 再 shift：B 城首日不能吃 A 城末日的值。"""
        days = pd.date_range("2026-01-01", periods=12)
        ordered = pd.DataFrame({"city_id": ["A"] * 12 + ["B"] * 7,
                                "business_date": list(days) + list(days[5:]),
                                "temp_c_max": [10.0] * 12 + [99.0] * 7})
        keys = pd.MultiIndex.from_arrays([ordered["city_id"].to_numpy(),
                                          ordered["business_date"].to_numpy()],
                                         names=["city_id", "business_date"])
        previous = features._lagged_prev(ordered, "temp_c_max", keys)
        self.assertTrue(np.isnan(previous[12]), "B 城首日必须没有「昨天」")
        self.assertEqual(previous[13], 99.0)
        self.assertTrue(np.isnan(previous[0]))


class WriteDiscipline(unittest.TestCase):
    def test_write_helpers_never_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            common.write_new_json(directory / "a.json", {"k": 1})
            common.write_new_text(directory / "b.txt", "x")
            common.write_new_csv(directory / "c.csv", pd.DataFrame({"v": [1]}))
            common.write_new_pickle(directory / "d.pkl", pd.DataFrame({"v": [1]}))
            common.write_new_bytes(directory / "e.bin", b"x")
            with self.assertRaises(FileExistsError):
                common.write_new_json(directory / "a.json", {"k": 2})
            for name, blob in (("b.txt", b"y"), ("c.csv", b"v\n1\n"), ("e.bin", b"y")):
                with self.assertRaises(FileExistsError):
                    common.write_new_bytes(directory / name, blob)
            with self.assertRaises(FileExistsError):
                common.write_new_pickle(directory / "d.pkl", pd.DataFrame({"v": [2]}))

    def test_require_empty_run_dir_blocks_publish_dir_reuse(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.assertIsNone(common.require_empty_run_dir(directory / "nope"))
            (directory / "keep.pkl").write_bytes(b"x")
            self.assertIsNone(common.require_empty_run_dir(directory, extra_allowed=("keep.pkl",)))
            (directory / "stray.json").write_text("{}")
            with self.assertRaises(FileExistsError):
                common.require_empty_run_dir(directory, extra_allowed=("keep.pkl",))


class MetricHelpers(unittest.TestCase):
    def test_lift_brier_mae_perfect_and_degenerate_cases(self) -> None:
        y = np.array([1, 0, 1, 0, 1, 0, 1, 0, 1, 0])
        self.assertEqual(common.lift_at(y, y.astype(float), 0.5), 2.0)
        self.assertEqual(common.brier(y, y.astype(float)), 0.0)
        self.assertEqual(common.mae(np.array([1.0, 2.0]), np.array([3.0, 2.0])), 1.0)
        self.assertTrue(np.isnan(common.lift_at(np.zeros(5, dtype=int), np.arange(5), 0.4)),
                        "全负样本时 lift 必须是 NaN 而不是 0")

    def test_calibration_bins_are_exhaustive_and_count_preserving(self) -> None:
        rng = np.random.default_rng(0)
        prob = rng.random(500)
        table = common.calibration_bins((prob > 0.8).astype(int), prob, bins=10)
        self.assertEqual(int(table["n"].sum()), 500)
        self.assertTrue((table["predicted"].diff().dropna() >= 0).all())


class FeatureGuards(unittest.TestCase):
    def test_answer_side_columns_are_banned(self) -> None:
        banned = {"over_min", "y_over", "duration_min", "status", "stop_reason", "end_soc_pct",
                  "energy_wh", "grid_energy_wh", "parking_fee_cents", "total_fee_cents",
                  "electricity_fee_cents", "service_fee_cents", "discount_cents", "grid_cost_cents",
                  "unplugged_at", "ended_at", "queue_status", "resolved_at", "joined_at", "called_at"}
        self.assertTrue(banned <= set(features.FORBIDDEN_FEATURES))
        self.assertTrue({"over_min", "y_over", "duration_min", "unplugged_at"} <= set(common.NON_FEATURE_COLUMNS))

    def test_cross_axis_signals_are_not_banned(self) -> None:
        """跨轴 as-of 是本线的正牌信号；把它们禁掉等于自断双臂。"""
        for column in ("user_over_rate_prior", "station_over_rate_prior", "cell_over_rate_prior",
                       "queue_open_at_start", "station_abandon_share_prior", "user_last_over",
                       "temp_c_max_prev", "planned_hours", "from_queue"):
            self.assertNotIn(column, set(features.FORBIDDEN_FEATURES), column)
            self.assertNotIn(column, set(common.NON_FEATURE_COLUMNS), column)

    def test_ignored_columns_all_carry_a_reason(self) -> None:
        self.assertIn("demand_multiplier", features.IGNORED_COLUMNS)
        self.assertIn("status(queue_entries)", features.IGNORED_COLUMNS)
        self.assertIn("target_mode", features.IGNORED_COLUMNS)
        self.assertTrue(all(reason for reason in features.IGNORED_COLUMNS.values()),
                        "声明了读过但不用的列却没写原因")

    def test_pick_features_drops_constants_and_banned_columns(self) -> None:
        picked = features.pick_features(_tiny_frame())
        listed = set(picked["numeric"]) | set(picked["categorical"])
        for column in ("over_min", "y_over", "duration_min", "session_id", "user_id", "split",
                       "business_date", "status", "energy_wh", "constant_column",
                       "demand_multiplier", "target_mode", "hour_block", "started_at"):
            self.assertNotIn(column, listed, column)
        self.assertIn("constant_column", picked["droppedConstant"])
        self.assertEqual(set(picked["categorical"]), set(common.CATEGORICAL_FEATURES))
        for columns in picked["groups"].values():
            self.assertFalse(set(columns) & (set(features.FORBIDDEN_FEATURES)
                                             | set(common.NON_FEATURE_COLUMNS)
                                             | set(features.IGNORED_COLUMNS)))

    def test_pick_features_refuses_a_banned_column_named_as_categorical(self) -> None:
        frame = _tiny_frame()
        with mock.patch.object(common, "CATEGORICAL_FEATURES",
                               list(common.CATEGORICAL_FEATURES) + ["over_min"]):
            with self.assertRaises(AssertionError):
                features.pick_features(frame)

    def test_candidate_sets_partition_by_prefix(self) -> None:
        picked = features.pick_features(_tiny_frame())
        groups = picked["groups"]
        self.assertIn("planned_hours", groups["staticOnly"])
        self.assertIn("temp_c_max_prev", groups["staticOnly"])  # 天气留在静态组：它是日历侧上下文
        self.assertIn("user_over_rate_prior", groups["historyOnly"])
        self.assertIn("user_age_days", groups["historyOnly"],
                      "user_ 前缀把账户年龄也划进历史组——已知且接受，README 有言在前")
        self.assertIn("queue_open_at_start", groups["opsOnly"])
        self.assertIn("station_abandon_share_prior", groups["opsOnly"])
        self.assertEqual(set(groups["full"]), set(picked["numeric"]) | set(picked["categorical"]))
        self.assertTrue(all(groups.values()))

    def test_candidate_sets_refuse_banned_numeric(self) -> None:
        with self.assertRaises(AssertionError):
            features.candidate_sets(["planned_hours", "over_min"], ["is_weekend"])


@unittest.skipUnless(HAVE_SKLEARN, "没装 sklearn")
class ThresholdAndDesignLogic(unittest.TestCase):
    def test_daily_alert_cap_is_per_day_share_not_a_floor(self) -> None:
        frame = pd.DataFrame({"business_date": pd.to_datetime(["2026-01-01"] * 75 + ["2026-01-02"] * 40),
                              "session_id": [f"S{i}" for i in range(115)]})
        cap = train.daily_alert_cap(frame)
        self.assertEqual(sorted(int(value) for value in cap), [10, 19])  # ceil(25%×75)、ceil(25%×40)
        self.assertLess(int(cap.max()), train.MIN_FLAGGED, "预算上限被抬到 MIN_FLAGGED 了：那是自己放宽口径")
        self.assertEqual(int(cap.sum()), 29)

    def test_pick_threshold_respects_budget_and_target(self) -> None:
        rng = np.random.default_rng(3)
        y = (rng.random(1000) < 0.2).astype(int)
        prob = np.clip(y * 0.4 + rng.random(1000) * 0.5, 0.0, 1.0)
        point = train.pick_threshold(prob, y, alert_cap=np.full(50, 5))
        self.assertLessEqual(point["alerts"], 250, "越过了逐日预算总和")
        self.assertGreaterEqual(point["precision"], train.PRECISION_MULTIPLE * float(y.mean()) - 1e-12)
        self.assertGreaterEqual(point["alerts"], train.MIN_FLAGGED)
        self.assertTrue(point["targetMet"])
        self.assertIn("VALIDATION 基础率", point["rule"])

    def test_pick_threshold_falls_back_instead_of_loosening_the_rule(self) -> None:
        y = np.array([1, 0] * 200)
        prob = 0.5 - y * 0.2 + np.random.default_rng(0).random(400) * 0.02  # 反着排：没有档能达标
        point = train.pick_threshold(prob, y)
        self.assertFalse(point["targetMet"])
        self.assertIn("无档满足，退回最高精确率档（如实标注，不假装达标）", point["rule"])
        self.assertGreaterEqual(point["alerts"], train.MIN_FLAGGED)

    def test_pick_threshold_refuses_when_no_tier_has_enough_alerts(self) -> None:
        rng = np.random.default_rng(1)
        with self.assertRaises(RuntimeError):
            train.pick_threshold(rng.random(40), (rng.random(40) < 0.3).astype(int))

    def test_design_keeps_nan_and_marks_categoricals(self) -> None:
        frame = pd.DataFrame({"a": [1.0, np.nan, 3.0], "b": ["x", None, "y"], "c": [True, False, True]})
        matrix, mask = train.design(frame, ["a", "c"], ["b"])
        self.assertTrue(np.isnan(matrix["a"].iloc[1]), "NaN 必须留给 HGBT 原生处理")
        self.assertEqual(mask.tolist(), [False, False, True])
        self.assertEqual(list(matrix["b"].cat.categories), ["MISSING", "x", "y"])
        with self.assertRaises(KeyError):
            train.design(frame, ["a", "nope"], ["b"])

    def test_split_group_rejects_unknown_columns(self) -> None:
        with self.assertRaises(AssertionError):
            train.split_group(["ghost"], ["a"], ["b"])
        self.assertEqual(train.split_group(["b", "a"], ["a"], ["b"]), (["a"], ["b"]))

    def test_predict_baseline_prefers_keys_with_history(self) -> None:
        train_frame = pd.DataFrame({"station_id": ["S"] * 20 + ["T"] * 20,
                                    "site_type": ["OFFICE"] * 40, "hour_block": [0] * 40,
                                    "vehicle_class": ["SEDAN"] * 40,
                                    "y_over": [1, 0, 1, 0] * 5 + [0] * 20})
        baselines = train.fit_baselines(train_frame)
        query = pd.DataFrame({"station_id": ["Z", "S", "Z"], "site_type": ["MALL", "OFFICE", "MALL"],
                              "hour_block": [3, 0, 3], "vehicle_class": ["SUV", "SEDAN", "SUV"]})
        scores = train.predict_baseline(baselines, query)
        global_rate = float(baselines["global"]["rate"].iloc[0])
        self.assertAlmostEqual(float(scores[0]), global_rate, places=9)  # 全查不到 → 回落全局率
        self.assertGreater(float(scores[1]), global_rate, "S 的历史失败率高于全局，查表必须往上走")


@unittest.skipUnless(HAVE_SKLEARN, "没装 sklearn")
class RollingGeometry(unittest.TestCase):
    def test_fold_plan_tiles_the_window_exactly(self) -> None:
        bounds = _bounds()
        folds = rolling.fold_plan(bounds, 10)
        self.assertEqual(len(folds), 10)
        self.assertEqual(folds[0]["fitEnd"] - bounds["startInclusive"],
                         pd.Timedelta(days=rolling.WARMUP_DAYS))
        self.assertEqual(folds[-1]["evalEnd"], bounds["testEndExclusive"])
        for previous, following in zip(folds, folds[1:]):
            self.assertEqual(following["fitEnd"] - previous["fitEnd"],
                             pd.Timedelta(days=rolling.HORIZON_DAYS))
            self.assertEqual(following["valStart"], previous["fitEnd"])
        for fold in folds:
            self.assertGreater(fold["valStart"], bounds["startInclusive"])

    def test_fold_plan_refuses_to_peek_past_the_window(self) -> None:
        bounds = _bounds()
        with self.assertRaises(ValueError):
            rolling.fold_plan(bounds, 11)
        with self.assertRaises(ValueError):
            rolling.fold_plan(bounds, 0)

    def test_sign_test_p_values(self) -> None:
        self.assertAlmostEqual(rolling.sign_test_p(10, 10), 2 * 0.5 ** 10)
        self.assertEqual(rolling.sign_test_p(5, 10), 1.0)
        self.assertTrue(np.isnan(rolling.sign_test_p(0, 0)))
        self.assertLess(rolling.sign_test_p(9, 10), 0.05)
        self.assertGreater(rolling.sign_test_p(7, 10), 0.05, "7/10 不该被当成显著")

    def test_group_order_covers_every_candidate_set(self) -> None:
        self.assertEqual(set(rolling.GROUP_ORDER), {"staticOnly", "historyOnly", "opsOnly", "full"})


@unittest.skipUnless(HAVE_ARTIFACTS, "还没跑 features / train / evaluate")
class DatasetFacts(unittest.TestCase):
    """钉住数据事实与产物自洽——包括单位修复与两条负结论。"""

    @classmethod
    def setUpClass(cls) -> None:
        cls.frame = pd.read_pickle(common.FEATURES_PATH)
        with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
            cls.summary = json.load(handle)
        with open(common.TRAIN_METRICS_PATH, encoding="utf-8") as handle:
            cls.metrics = json.load(handle)
        with open(common.TEST_REPORT_PATH, encoding="utf-8") as handle:
            cls.report = json.load(handle)

    def test_timestamp_chain_and_labels_are_consistent(self) -> None:
        frame = self.frame
        self.assertTrue((frame["ended_at"] >= frame["started_at"]).all())
        self.assertTrue((frame["unplugged_at"] >= frame["ended_at"]).all())
        np.testing.assert_allclose(
            frame["over_min"].to_numpy(dtype=float),
            ((frame["unplugged_at"] - frame["ended_at"]) / np.timedelta64(1, "m")).to_numpy(dtype=float))
        self.assertTrue((frame["y_over"] == (frame["over_min"] >= common.OVERSTAY_THRESHOLD_MIN))
                        .all())
        self.assertTrue((frame["duration_min"] > 0).all())
        expected_bday = (frame["started_at"] + pd.Timedelta(
            hours=common.BUSINESS_OFFSET_HOURS)).dt.normalize()
        self.assertTrue((frame["business_date"] == expected_bday).all())

    def test_planned_hours_units_are_pinned(self) -> None:
        """单位回归事故现场：第一版把 target_value(Wh) 当 kWh 用，planned_hours 大了 1000 倍。"""
        self.assertTrue((self.frame["target_mode"] == "ENERGY").all())
        np.testing.assert_allclose(self.frame["planned_hours"].to_numpy(dtype=float),
                                   (self.frame["target_value"].to_numpy(dtype=float) / 1000.0
                                    / self.frame["rated_power_kw"].to_numpy(dtype=float)),
                                   rtol=1e-12)
        self.assertLess(float(self.frame["planned_hours"].max()), 24.0,
                        "planned_hours 又回到小时以上量级——单位混了")
        self.assertGreater(float(self.frame["planned_hours"].min()), 0.0)

    def test_split_counts_and_base_rates(self) -> None:
        self.assertEqual(self.frame["split"].value_counts().to_dict(),
                         {key: int(value) for key, value in self.summary["splits"].items()})
        self.assertEqual(self.summary["splits"], {"TRAIN": 79552, "VALIDATION": 20775,
                                                 "TEST": 19727, "EXCLUDED": 1485})
        self.assertEqual(self.summary["splitBaseRate"], {"TRAIN": 0.08934, "VALIDATION": 0.09011,
                                                        "TEST": 0.09104, "EXCLUDED": 0.09226})
        in_window = self.frame[self.frame["split"] != "EXCLUDED"]
        self.assertEqual(len(in_window), sum(int(self.summary["splits"][name])
                                             for name in ("TRAIN", "VALIDATION", "TEST")))
        self.assertLess(len(in_window), len(self.frame), "EXCLUDED 行必须留在表里当历史来源")
        self.assertEqual(int(self.report["risk"]["positives"]), int(self.summary["splitPositives"]["TEST"]))

    def test_zero_variance_columns_are_actually_constant(self) -> None:
        expected = {"transformer_kw": 360.0, "campaign_discount_cents": 300.0, "own_queue_wait_min": 5.0}
        dropped = self.summary["features"]["droppedConstant"]
        self.assertEqual(sorted(dropped), sorted(expected))
        for column in dropped:
            series = self.frame[column].dropna()
            self.assertLessEqual(series.nunique(), 1, column)
            self.assertAlmostEqual(float(series.iloc[0]), expected[column], places=5,
                                   msg=f"{column} 实际取值与报告解释不一致")

    def test_queue_facts_pinned(self) -> None:
        queue = self.summary["notes"]["queue"]
        self.assertEqual(queue["entries"], 36594)
        self.assertEqual(queue["unresolvedEntries"], 0, "数据末仍有未 resolve 队列记录时，NaT 过滤逻辑要重核")
        self.assertEqual(queue["statusMix"], {"SERVED": 22414, "ABANDONED": 10143, "CALL_EXPIRED": 4037})
        self.assertEqual(int(self.frame["from_queue"].sum()), queue["fromQueueRows"])
        self.assertTrue((self.frame["queue_open_at_start"] >= 0).all())

    def test_campaign_window_check_is_recorded(self) -> None:
        self.assertEqual(self.summary["notes"]["profiles"]["campaignWindowCheck"],
                         "all-tagged-started-at-inside-window")
        self.assertEqual(int(self.frame["has_campaign"].sum()),
                         self.summary["notes"]["profiles"]["campaignTagged"])

    def test_feature_list_keeps_the_leak_banned_and_the_planned(self) -> None:
        listed = set(self.summary["features"]["numeric"]) | set(self.summary["features"]["categorical"])
        banned = (set(features.FORBIDDEN_FEATURES) | set(common.NON_FEATURE_COLUMNS)
                  | set(features.IGNORED_COLUMNS))
        self.assertFalse(listed & banned)
        for group in self.summary["features"]["groups"].values():
            self.assertTrue(set(group) <= listed)
        self.assertEqual(set(self.summary["features"]["categorical"]), set(common.CATEGORICAL_FEATURES))
        self.assertEqual(self.summary["featureRows"], len(self.frame))
        self.assertEqual(self.summary["featureCount"], {"numeric": 45, "categorical": 10})

    def test_leak_audit_deviation_is_machine_zero(self) -> None:
        audit = self.summary["leakAudit"]
        self.assertEqual(len(audit["checked"]), len(features.AUDITED_FEATURES))
        self.assertEqual(audit["sampled"], 250)
        for name, delta in audit["maxAbsDiff"].items():
            self.assertLessEqual(delta, 1e-9, f"{name} 的 as-of 值两条路径不一致")

    def test_answer_side_spearman_keeps_the_fee_ban_honest(self) -> None:
        """停车费与占桩分钟强相关（0.442）——它是答案的马甲，钉住"相关但必须禁入"这句。"""
        self.assertGreater(self.summary["answerSideSpearman"]["parking_fee_cents"], 0.3)
        self.assertIn("parking_fee_cents", set(features.FORBIDDEN_FEATURES))

    def test_bundle_matches_feature_table_and_summary(self) -> None:
        bundle = evaluate.load_bundle()
        self.assertEqual(bundle["featuresSha256"], common.sha256_file(common.FEATURES_PATH))
        self.assertNotIn("derivedDatasetId", bundle, "本线没有派生数据集，模型包里不该冒出这个键")
        self.assertEqual(bundle["publishedBatchId"], common.EXPECTED_PUBLISHED_BATCH_ID)
        self.assertEqual(bundle["chosenSet"], self.metrics["chosenSet"])
        self.assertEqual(bundle["operatingPoint"]["threshold"], self.metrics["operatingPoint"]["threshold"])
        self.assertEqual(self.metrics["selectionRule"],
                         "四组候选特征集按 VALIDATION ROC AUC 择一发布；TEST 完全不参与选择")
        self.assertEqual(int(self.metrics["testRowsUntouched"]), int(self.report["testRows"]))
        self.assertTrue(self.metrics["operatingPoint"]["targetMet"])
        chosen = self.metrics["candidateValidation"][self.metrics["chosenSet"]]
        self.assertEqual(chosen["auc"], max(block["auc"]
                                            for block in self.metrics["candidateValidation"].values()),
                         "发布的不是 VALIDATION 上最好的那一组")
        self.assertEqual(self.metrics["disclaimer"], common.data_note())

    def test_bundle_refuses_a_tampered_feature_table(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            forged = Path(tmp) / "forged.pkl"
            forged.write_bytes(common.FEATURES_PATH.read_bytes() + b"\x00")
            with mock.patch.object(common, "FEATURES_PATH", forged):
                with self.assertRaises(common.BatchMismatch):
                    evaluate.load_bundle()

    def test_bundle_refuses_a_model_from_another_feature_table(self) -> None:
        bundle = joblib.load(common.BUNDLE_PATH)
        bundle["featuresSha256"] = "0" * 64
        with tempfile.TemporaryDirectory() as tmp:
            forged = Path(tmp) / "forged.joblib"
            joblib.dump(bundle, forged)
            with mock.patch.object(common, "BUNDLE_PATH", forged):
                with self.assertRaisesRegex(common.BatchMismatch, "成对发布"):
                    evaluate.load_bundle()

    def test_evaluation_report_is_internally_consistent(self) -> None:
        scoreboard = self.report["scoreboard"]
        self.assertEqual(scoreboard["model"]["auc"], round(self.report["risk"]["auc"], 4))
        self.assertTrue(scoreboard["model"]["deployable"])
        self.assertFalse(scoreboard["oracleActualDurationNONDEPLOYABLE"]["deployable"],
                         "oracle 必须标成不可部署，否则它就是个作弊基线")
        self.assertEqual(scoreboard["globalBaseRate"]["auc"], 0.5)  # 常数打分的尺子
        self.assertIsNone(scoreboard["plannedHoursOnly"]["logLoss"], "规则分不是概率，不许凑 logLoss")
        self.assertIsNotNone(scoreboard["model"]["logLoss"])
        self.assertGreater(self.report["risk"]["aucTrain"], self.report["risk"]["aucValidation"],
                           "TRAIN 不高于 VALIDATION 说明切分或打分有问题")
        head = self.report["headline"]
        model = self.report["risk"]["auc"]
        for key, baseline in (("vsBlendPct", "trainCellBlend"), ("vsUserPriorPct", "asOfUserPriorOnly"),
                              ("vsPlannedPct", "plannedHoursOnly")):
            stored = self.report["scoreboard"][baseline]["auc"]
            self.assertAlmostEqual(head[key] / 100.0 + 1.0, model / stored, delta=2e-4,
                                   msg=f"{key} 与 scoreboard 的比值对不上")
        beaten = all(model >= self.report["scoreboard"][name]["auc"]
                     for name in ("trainCellBlend", "asOfUserPriorOnly", "plannedHoursOnly"))
        self.assertEqual("跑赢了全部可部署基线" in head["conclusion"], beaten,
                         "主结论的措辞与实际排名不一致")

    def test_habit_negative_result_is_pinned(self) -> None:
        """"用户习惯没有场景之外的独立增量"——本线交付给运营的就是这句负结论。"""
        ablation = self.report["ablation"]
        self.assertGreater(ablation["staticOnly"], ablation["historyOnly"],
                           "TEST 上习惯组反超静态组了，README 与报告的全部措辞要连锅重审")
        self.assertGreater(ablation["staticOnly"], ablation["opsOnly"])
        self.assertAlmostEqual(ablation["full"] - ablation["staticOnly"], -0.0032, delta=0.004,
                               msg="full 与 staticOnly 的差距不再是 ±0.004 内的零——增量结论要重写")
        naive = self.report["scoreboard"]["naiveLastOverstay"]["auc"]
        self.assertLess(naive, 0.55, "朴素「上次超时」接近随机是本线核心论据之一")
        self.assertIn("historyOnly", self.report["valAblation"])
        self.assertLess(self.report["valAblation"]["historyOnly"],
                        self.report["valAblation"]["staticOnly"], "VAL 上习惯组也该输给静态组")

    def test_budget_ranking_and_frozen_point_agree_with_frame(self) -> None:
        test = self.frame[self.frame["split"] == "TEST"]
        days = test.groupby("business_date")["session_id"].count()
        rows = self.report["budgetRanking"]
        self.assertEqual([row["budget"] for row in rows], [0.05, 0.10, 0.15, 0.20, 0.25])
        for row in rows:
            ceiling = int(sum(max(1, math.ceil(float(row["budget"]) * n)) for n in days))
            for name, block in row.items():
                if name == "budget":
                    continue
                self.assertNotIn("oracle", name, "提醒预算表里混进了不可部署的 oracle")
                self.assertLessEqual(block["alerts"], ceiling, f"{name} 在 {row['budget']:.0%} 预算下超发")
                self.assertLessEqual(block["recall"], 1.0)
        self.assertGreater(rows[-1]["model"]["recall"], rows[0]["model"]["recall"])
        self.assertGreater(rows[2]["model"]["recall"], rows[2]["trainCellBlend"]["recall"])
        point = self.report["risk"]["frozenOperatingPoint"]
        self.assertEqual(point["alerts"], self.report["pile"]["flaggedRows"])
        self.assertEqual(point["note"], "阈值与规则在 VALIDATION 上定死，TEST 只验不改")

    def test_frozen_threshold_overshoot_is_reported_not_hidden(self) -> None:
        """并列分块让冻结阈值在 TEST 上越过了逐日 25% 预算——这条负事实必须留在产物里。"""
        self.assertGreater(self.report["budget"]["daysOverBudget"], 0,
                           "越预算天数归零了？要么并列问题真解决了，要么有人把负结论藏了")
        self.assertEqual(self.report["budget"]["days"], int(
            self.frame[self.frame["split"] == "TEST"]["business_date"].nunique()))

    def test_minutes_regression_beats_causal_baselines(self) -> None:
        rows = self.report["minutesRegression"]["rows"]
        self.assertIn("modelRegressor", rows)
        model = float(rows["modelRegressor"]["mae"])
        for name in ("trainMeanOverMin", "asOfUserMeanPrior", "trainCellMeanBlend"):
            self.assertLess(model, float(rows[name]["mae"]), f"模型没有赢过 {name}，回归结论要改")

    def test_calibration_and_breakdown_cover_the_test_window(self) -> None:
        table = self.report["calibration"]
        self.assertEqual(sum(row["n"] for row in table), int(self.report["testRows"]))
        self.assertEqual([row["predicted"] for row in table], sorted(r["predicted"] for r in table))
        for name in ("plannedHours", "userLastOver", "userHistoryPresence"):
            self.assertIn(name, self.report["breakdown"])


@unittest.skipUnless(HAVE_ROLLING, "还没跑 rolling")
class RollingArtifacts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        with open(common.ROLLING_SUMMARY_PATH, encoding="utf-8") as handle:
            cls.summary = json.load(handle)

    def test_every_round_is_reported_and_ordered(self) -> None:
        rounds = self.summary["roundsDetail"]
        self.assertEqual([row["round"] for row in rounds], list(range(len(rounds))))
        self.assertEqual(len(rounds), 10)
        for row in rounds:
            self.assertGreaterEqual(row["evalRows"], rolling.MIN_EVAL_ROWS)
            self.assertGreaterEqual(row["evalPositives"], rolling.MIN_EVAL_POSITIVES)
            self.assertLess(row["evalEnd"][:10], "2026-05-29", "评测窗越过了数据右端")
            self.assertIn(row["chosenSet"], rolling.GROUP_ORDER)
            self.assertEqual(row["evalDays"], rolling.HORIZON_DAYS)
            self.assertLess(row["alertRate"], 0.5, "告警率超过一半，阈值口径已经失效")
            self.assertIn("精确率≥", row["thresholdRule"])
        first, last = rounds[0], rounds[-1]
        self.assertLess(first["fitEnd"], last["fitEnd"])
        self.assertLess(first["userSessionsPriorMedian"], last["userSessionsPriorMedian"],
                        "用户历史必须逐轮变厚（中位 9 → 23 是本批事实）")

    def test_verdict_numbers_come_from_the_rounds(self) -> None:
        rounds = self.summary["roundsDetail"]
        mean_model = float(np.mean([row["auc"] for row in rounds]))
        self.assertAlmostEqual(self.summary["metrics"]["auc"]["mean"], round(mean_model, 4), places=4)
        trend = self.summary["trend"]
        self.assertEqual(trend["gainVsBlendPositiveRounds"],
                         sum(1 for row in rounds if row["aucGainVsBlendPct"] > 0))
        self.assertEqual(trend["gainVsPlannedPositiveRounds"],
                         sum(1 for row in rounds if row["aucGainVsPlannedPct"] > 0))
        self.assertEqual(trend["thresholdTargetMetRounds"],
                         sum(1 for row in rounds if row["thresholdTargetMet"]))
        self.assertEqual(trend["historyNonPositiveRounds"],
                         sum(1 for d in self.summary["historyMinusStaticByRound"] if d <= 0))
        self.assertEqual(trend["gainRounds"], len(rounds))
        best_cheap = max(self.summary["metrics"][key]["mean"] for key in
                         ("aucCellBlendBaseline", "aucUserPriorBaseline", "aucPlannedHoursOnly"))
        self.assertEqual(self.summary["verdict"]["modelBeatsCheapBaseline"], mean_model > best_cheap)
        self.assertIn(f"{self.summary['metrics']['auc']['mean']:.4f}", self.summary["verdict"]["headline"],
                      "headline 里的 AUC 不是十轮均值（被手改过？）")

    def test_habit_increment_is_nonpositive_in_every_round(self) -> None:
        deltas = self.summary["historyMinusStaticByRound"]
        self.assertEqual(len(deltas), len(self.summary["roundsDetail"]))
        self.assertLessEqual(max(deltas), 0.0,
                             "有轮次 historyOnly 反超 staticOnly——「习惯无独立增量」的结论措辞要连 README 一起改")
        self.assertTrue(all(abs(d) < 0.02 for d in self.summary["fullMinusStaticByRound"]),
                        "full 减 static 超出 ±0.02：跨轴增量的量级结论要重写")

    def test_metric_blocks_are_self_consistent(self) -> None:
        metrics = self.summary["metrics"]
        for name, block in metrics.items():
            self.assertLessEqual(block["min"], block["mean"], name)
            self.assertLessEqual(block["mean"], block["max"], name)
            self.assertGreaterEqual(block["std"], 0.0, name)
        self.assertEqual(self.summary["warmupDays"], rolling.WARMUP_DAYS)
        self.assertEqual(self.summary["rounds"], len(self.summary["roundsDetail"]))
        self.assertEqual(len(self.summary["valAucBySet"]), len(rolling.GROUP_ORDER))

    def test_round_files_are_the_source_of_truth(self) -> None:
        paths = sorted(common.ROLLING_ROUNDS_DIR.glob("round_*.json"))
        self.assertEqual(len(paths), len(self.summary["roundsDetail"]))
        for path in paths:
            with open(path, encoding="utf-8") as handle:
                record = json.load(handle)
            stored = next(row for row in self.summary["roundsDetail"] if row["round"] == record["round"])
            self.assertEqual(record["auc"], stored["auc"], f"{path.name} 与 summary 不一致")
            self.assertEqual(record["dataNote"], common.data_note(), f"{path.name} 少了口径声明")

    def test_reuse_of_the_blind_test_window_is_declared(self) -> None:
        self.assertIn("不是盲测指标", self.summary["caveat"],
                      "滚动折会复用 TEST 窗口这件事必须写在 caveat 里")


if __name__ == "__main__":
    unittest.main()
