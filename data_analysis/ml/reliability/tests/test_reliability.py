"""第七线（桩级次日可靠性预警）的回归测试。

分两层：
  · **纯逻辑层**——批次/派生集核对的 fail-closed、按日整块切分、as-of 严格早于（尤其"同一天互相
    不可见"这条本线最要命的边界）、两种收缩先验公式、禁入名单闸门、候选集划分、运营阈值与逐日预算、
    折计划铺满、符号检验：不读任何产物，秒级；
  · **产物层**——需要本地先跑过 build_panel / features / train / evaluate / rolling。
    ``data_analysis/outputs/`` 在 .gitignore 内，所以 CI 与刚 clone 的人这里自动 skip，而不是假装通过；
    派生数据集 ``data_analysis/derived/`` 是入仓文件，跑过 build_panel 就有。

产物层刻意钉住三条**事实**（不是愿望），都是本线开发过程中真出过问题的地方：
  · 面板稠密：桩数 × 天数 = 行数，0 尝试的日子必为 0 失败（不稠密则"30 天窗口"的分母不成立）；
  · lag1 方向为正：P(今日失败日 | 昨天坏) > P(… | 昨天没坏)——早期一次探针用错了
    ``DataFrame.shift`` 的轴（默认 axis=0＝上一台桩，不是昨天），得出过反号结论并写进了注释；
  · 四个零方差列在样本域内真的只有一个取值，且取值与报告里的解释一致。

用法（仓库根目录）：python -m unittest discover -s data_analysis/ml/reliability/tests -t .
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

from data_analysis.ml.reliability import common, features

try:  # train / evaluate / rolling 顶层引用 sklearn；缺依赖时这两块的测试整体跳过而不是报错
    from data_analysis.ml.reliability import evaluate, rolling, train

    HAVE_SKLEARN = True
except ImportError:  # pragma: no cover
    evaluate = rolling = train = None
    HAVE_SKLEARN = False

HAVE_DERIVED = (common.DERIVED_MANIFEST_PATH.exists() and common.PANEL_CSV.exists()
                and common.CONTEXT_CSV.exists())
ARTIFACTS = (common.FEATURES_PATH, common.BUILD_SUMMARY_PATH, common.BUNDLE_PATH,
             common.TRAIN_METRICS_PATH, common.TEST_REPORT_PATH)
HAVE_ARTIFACTS = HAVE_DERIVED and HAVE_SKLEARN and all(path.exists() for path in ARTIFACTS)
HAVE_ROLLING = HAVE_SKLEARN and common.ROLLING_SUMMARY_PATH.exists()


def _bounds() -> dict[str, pd.Timestamp]:
    """与 manifest 等价的边界，但不读盘（纯逻辑测试要能在任何机器上跑）；
    两者是否仍一致由 ``Splits.test_hardcoded_bounds_still_match_the_manifest`` 把守。"""
    shift = pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
    return {"startInclusive": pd.Timestamp("2025-12-02") - shift,
            "trainEndExclusive": pd.Timestamp("2026-03-30") - shift,
            "validationEndExclusive": pd.Timestamp("2026-04-29") - shift,
            "testEndExclusive": pd.Timestamp("2026-05-29") - shift}


def _tiny_panel() -> pd.DataFrame:
    """两台桩 × 六天的稠密面板；A 的失败日 0,1,1,1,0,1，B 的 1,1,0,0,0,0。"""
    start = pd.Timestamp("2026-01-01")
    flags = {"A": [0, 1, 1, 1, 0, 1], "B": [1, 1, 0, 0, 0, 0]}
    return pd.DataFrame([{"charger_id": charger,
                          "business_date": start + pd.Timedelta(days=index),
                          "day_start_utc": start + pd.Timedelta(days=index), "fail_flag": flag}
                         for charger in ("A", "B") for index, flag in enumerate(flags[charger])])


def _tiny_feature_frame() -> pd.DataFrame:
    """给 ``pick_features`` 用的最小表：8 个类别列齐、含当日量、含零方差列、含被忽略列。"""
    frame = _tiny_panel().rename(columns={"fail_flag": "y_fail"})
    frame["attempts_on_day"] = list(range(1, 13))
    frame["tech_fails_on_day"] = frame["y_fail"]
    frame["users_on_day"] = 1
    frame["started_on_day"] = 1
    frame["prev_day_fail"] = [0.0, 0.0, 1.0, 1.0, 0.0, 1.0] * 2
    frame["attempts_per_day_30d"] = [float(v) for v in list(range(1, 13))]
    frame["charger_tickets_30d"] = [0.0, 1.0, 0.0, 2.0, 0.0, 0.0] * 2
    frame["tel_offline_prev_day"] = [0.0, 0.0, 3.0, 0.0, 0.0, 1.0] * 2
    frame["rated_power_kw"] = [60.0, 120.0] * 6
    frame["constant_column"] = 7
    frame["demand_multiplier"] = 1.0
    for name, value in (("site_type", "MALL"), ("scenario_event", "NONE"), ("weather_prev", "SUNNY"),
                        ("connector_type", "DC"), ("manufacturer", "V1"), ("charger_model", "M1")):
        frame[name] = value
    frame["last_state_code"] = 0.0
    frame["is_weekend"] = 0
    return frame


class BatchAndDerivedGates(unittest.TestCase):
    """两道闸门都必须 fail closed：批次错配、派生集被改过一个字节。"""

    def test_verify_batch_rejects_other_batch_and_missing_field(self) -> None:
        with self.assertRaises(common.BatchMismatch):
            common.verify_batch({"publishedBatchId": "analytics-" + "0" * 32})
        with self.assertRaises(common.BatchMismatch):
            common.verify_batch({"datasetId": common.DATASET_ID})
        self.assertEqual(common.verify_batch({"publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID})
                         ["publishedBatchId"], common.EXPECTED_PUBLISHED_BATCH_ID)

    def test_derived_manifest_must_declare_the_same_source_batch(self) -> None:
        with self.assertRaises(common.DerivedMismatch):
            common.verify_derived({"sourceBatchId": "analytics-" + "1" * 32, "files": []})

    @unittest.skipUnless(HAVE_DERIVED, "还没跑 build_panel，派生集不在")
    def test_verify_derived_accepts_the_dataset_on_disk(self) -> None:
        manifest = common.verify_derived()
        self.assertEqual(manifest["sourceBatchId"], common.EXPECTED_PUBLISHED_BATCH_ID)
        self.assertEqual({entry["name"] for entry in manifest["files"]},
                         {common.PANEL_NAME + ".csv", common.CONTEXT_NAME + ".csv"})

    @unittest.skipUnless(HAVE_DERIVED, "还没跑 build_panel，派生集不在")
    def test_verify_derived_refuses_missing_file(self) -> None:
        manifest = common.read_derived_manifest()
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(common, "DERIVED_DIR", Path(tmp)):
            with self.assertRaisesRegex(common.DerivedMismatch, "缺文件"):
                common.verify_derived(manifest)

    @unittest.skipUnless(HAVE_DERIVED, "还没跑 build_panel，派生集不在")
    def test_verify_derived_refuses_one_byte_change(self) -> None:
        """派生 CSV 是入仓的普通文件：手改一字节、checkout 截断，都必须被哈希挡住。"""
        manifest = json.loads(json.dumps(common.read_derived_manifest()))
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / manifest["files"][0]["name"]
            target.write_bytes(common.PANEL_CSV.read_bytes() + b"# tampered\n")
            with mock.patch.object(common, "DERIVED_DIR", Path(tmp)):
                with self.assertRaisesRegex(common.DerivedMismatch, "sha256"):
                    common.verify_derived(manifest)

    @unittest.skipUnless(HAVE_DERIVED, "还没跑 build_panel，派生集不在")
    def test_verify_derived_refuses_row_count_drift(self) -> None:
        """哈希对得上但行数不符（例如 manifest 被手改）也要拒。"""
        manifest = json.loads(json.dumps(common.read_derived_manifest()))
        manifest["files"][0]["rows"] += 1
        with self.assertRaisesRegex(common.DerivedMismatch, "行"):
            common.verify_derived(manifest)

    def test_load_panel_refuses_a_missing_derived_dataset(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(common, "DERIVED_DIR", Path(tmp)):
            with self.assertRaises(common.DerivedMismatch):
                common.load_panel()

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


class Splits(unittest.TestCase):
    def test_hardcoded_bounds_still_match_the_manifest(self) -> None:
        self.assertEqual(_bounds(), common.split_boundaries(common.read_source_manifest()))

    def test_boundaries_are_business_days_minus_eight_hours(self) -> None:
        bounds = common.split_boundaries(common.read_source_manifest())
        self.assertEqual((bounds["trainEndExclusive"] - bounds["startInclusive"]).days, 118)
        self.assertEqual((bounds["validationEndExclusive"] - bounds["trainEndExclusive"]).days, 30)
        self.assertEqual((bounds["testEndExclusive"] - bounds["validationEndExclusive"]).days, 30)
        self.assertEqual((bounds["testEndExclusive"] - bounds["startInclusive"]).days, 178)
        self.assertEqual(bounds["trainEndExclusive"].strftime("%Y-%m-%d %H:%M"), "2026-03-29 16:00")

    def test_split_is_whole_day_blocks_and_boundaries_belong_to_next_segment(self) -> None:
        bounds = _bounds()
        stamps = [bounds["startInclusive"] - pd.Timedelta(days=1), bounds["startInclusive"],
                  bounds["trainEndExclusive"] - pd.Timedelta(hours=1), bounds["trainEndExclusive"],
                  bounds["validationEndExclusive"], bounds["testEndExclusive"] - pd.Timedelta(hours=1),
                  bounds["testEndExclusive"]]
        self.assertEqual(common.assign_split(stamps, bounds).tolist(),
                         ["EXCLUDED", "TRAIN", "TRAIN", "VALIDATION", "TEST", "TEST", "EXCLUDED"])

    def test_same_day_chargers_never_straddle_two_segments(self) -> None:
        """本线样本是"某天某桩"：同一天必须整块落同一段，否则未来就被切进过去。"""
        bounds = _bounds()
        days = pd.date_range(bounds["startInclusive"], bounds["testEndExclusive"], freq="D")
        stamps = pd.Series([day for day in days for _ in range(3)])
        grouped = (pd.DataFrame({"day": stamps, "split": common.assign_split(stamps, bounds)})
                   .groupby("day")["split"].nunique())
        self.assertTrue((grouped == 1).all())


class AsOfSemantics(unittest.TestCase):
    def test_cumulative_is_strictly_earlier_and_same_day_invisible(self) -> None:
        """同刻并列＝同一天：某桩今天的尝试绝不能算进"昨天的历史"。"""
        base = pd.Timestamp("2026-01-01")
        events = pd.DataFrame({"charger_id": ["A"] * 5,
                               "day_start_utc": [base, base, base + pd.Timedelta(days=1),
                                                 base + pd.Timedelta(days=1),
                                                 base + pd.Timedelta(days=2)],
                               "_attempts": [2.0, 3.0, 4.0, 5.0, 6.0]})
        engine = common.AsOfCounts(events, key="charger_id", stamps="day_start_utc", value="_attempts")
        query = pd.DataFrame({"charger_id": ["A", "A", "A", "A", "B"],
                              "__query_time": pd.to_datetime([base, base + pd.Timedelta(days=1),
                                                              base + pd.Timedelta(days=2),
                                                              base + pd.Timedelta(days=9),
                                                              base + pd.Timedelta(days=9)])})
        counts, sums = engine.cumulative(query, "charger_id", value="_attempts")
        self.assertEqual(counts.tolist(), [0.0, 2.0, 4.0, 5.0, 0.0])
        self.assertEqual(sums.tolist(), [0.0, 5.0, 14.0, 20.0, 0.0])
        plain_counts, plain_sums = engine.cumulative(query, "charger_id")
        self.assertEqual(plain_counts.tolist(), counts.tolist())
        self.assertEqual(plain_sums.tolist(), sums.tolist(),
                         "不写 value 时拿到的仍是引擎构造时绑定的取值列——参数只是自述，不是重选")
        with self.assertRaises(AssertionError):  # 传一个不一致的列名必须被拒，不能静默忽略
            engine.cumulative(query, "charger_id", value="_fails")
        counter = common.AsOfCounts(events, key="charger_id", stamps="day_start_utc")
        two, also_two = counter.cumulative(query, "charger_id")
        self.assertEqual(two.tolist(), also_two.tolist())  # 纯计数时二者相同
        self.assertEqual(two.tolist(), [0.0, 2.0, 4.0, 5.0, 0.0])

    def test_window_is_half_open_on_both_ends(self) -> None:
        base = pd.Timestamp("2026-01-01")
        events = pd.DataFrame({"charger_id": ["A"] * 4,
                               "day_start_utc": [base, base + pd.Timedelta(days=10),
                                                 base + pd.Timedelta(days=10),
                                                 base + pd.Timedelta(days=31)]})
        engine = common.AsOfCounts(events, key="charger_id", stamps="day_start_utc")
        query = pd.DataFrame({"charger_id": ["A", "A"],
                              "__query_time": pd.to_datetime([base + pd.Timedelta(days=40)] * 2)})
        counts, _ = engine.window(query, "charger_id", days=30)
        self.assertEqual(counts.tolist(), [3.0, 3.0])  # 第 0 天已出窗；第 40 天（同刻）不可见

    def test_strict_last_hides_the_same_instant_and_gives_nat_otherwise(self) -> None:
        base = pd.Timestamp("2026-01-01")
        left = pd.DataFrame({"charger_id": ["A", "A", "B"],
                             "day_start_utc": [base + pd.Timedelta(days=1),
                                               base + pd.Timedelta(days=2),
                                               base + pd.Timedelta(days=5)]})
        events = pd.DataFrame({"charger_id": ["A", "A", "A"],
                               "day_start_utc": [base, base + pd.Timedelta(days=1),
                                                 base + pd.Timedelta(days=2)],
                               "fail_flag": [0, 1, 0]})
        got = common.strict_last(left, "day_start_utc", "charger_id", events, "day_start_utc",
                                 ["fail_flag"])
        self.assertEqual(len(got), 3)
        self.assertEqual(float(got["fail_flag"].iloc[0]), 0.0)  # 同刻（同一天）那条看不见 → 看见的是 base
        self.assertEqual(float(got["fail_flag"].iloc[1]), 1.0)  # 昨天那条看得见
        self.assertTrue(pd.isna(got["fail_flag"].iloc[2]))      # B 从来没事件 → NaN，不是 0 也不是负哨兵
        self.assertTrue(pd.isna(got["__last_time"].iloc[2]))
        self.assertEqual(got["__last_time"].iloc[0], base)      # 拿到的确实是"严格早于"的最后一条

    def test_strict_last_refuses_null_join_key(self) -> None:
        left = pd.DataFrame({"charger_id": ["A"], "day_start_utc": [pd.Timestamp("2026-01-02")]})
        events = pd.DataFrame({"charger_id": [None], "day_start_utc": [pd.Timestamp("2026-01-01")],
                               "fail_flag": [1]})
        with self.assertRaises(AssertionError):
            common.strict_last(left, "day_start_utc", "charger_id", events, "day_start_utc",
                               ["fail_flag"])

    def test_smoothed_formula_matches_its_definition(self) -> None:
        """两条观测的经验率都是 20%，先验 50%：样本少的那条必须被拽得更靠近先验。"""
        got = common.smoothed(np.array([0.0, 5.0, 100.0]), np.array([0.0, 1.0, 20.0]),
                              alpha=15.0, priors=np.array([0.5, 0.5, 0.5]))
        self.assertAlmostEqual(got[0], (0.0 + 15 * 0.5) / (0.0 + 15))     # 无观测 → 完全是先验
        self.assertAlmostEqual(got[1], (1.0 + 15 * 0.5) / (5.0 + 15))
        self.assertAlmostEqual(got[2], (20.0 + 15 * 0.5) / (100.0 + 15))  # 观测多 → 先验被冲淡
        self.assertAlmostEqual(got[0], 0.5)
        self.assertGreater(got[1], got[2], "同为 20% 经验率，5 次观测的那条该更靠近先验")
        self.assertGreater(got[2], 0.2, "但也不能真的等于经验率：α 就摆在那儿")

    def test_global_causal_ratio_is_attempt_weighted_and_strictly_causal(self) -> None:
        """与逐个"截至 t 之前"的暴力循环对齐；同时钉住它不同于按行等权的那一句。"""
        stamps = pd.Series(pd.to_datetime(["2026-01-01", "2026-01-01", "2026-01-02", "2026-01-03",
                                           "2026-01-03", "2026-01-05"]))
        nums = np.array([1.0, 2.0, 0.0, 3.0, 1.0, 0.0])
        dns = np.array([10.0, 20.0, 5.0, 4.0, 6.0, 1.0])
        got = common.global_causal_ratio(nums, dns, stamps)
        times = stamps.to_numpy()
        want = [0.0 if dns[times < stamp].sum() == 0
                else nums[times < stamp].sum() / dns[times < stamp].sum() for stamp in times]
        np.testing.assert_allclose(got, want)
        self.assertAlmostEqual(got[0], 0.0, msg="无历史必须是 0.0，不能是 NaN 也不是全表率")
        self.assertAlmostEqual(got[2], 3.0 / 30.0)  # 同刻两行互相不可见：1/01 的两条都算"今天"
        row_weighted = common.global_causal_rate((nums > 0).astype(float), stamps)
        self.assertFalse(np.allclose(got[2:], row_weighted[2:]),
                         "两种先验必须真的不同（尝试加权 vs 按行等权），否则这条测试只是装饰")

    def test_global_causal_rate_no_history_is_zero(self) -> None:
        stamps = pd.Series(pd.to_datetime(["2026-01-01", "2026-01-01", "2026-01-02"]))
        self.assertEqual(common.global_causal_rate(np.array([1.0, 0.0, 1.0]), stamps).tolist(),
                         [0.0, 0.0, 0.5])


class MetricHelpers(unittest.TestCase):
    def test_lift_brier_mae_perfect_and_degenerate_cases(self) -> None:
        y = np.array([1, 0, 1, 0, 1, 0, 1, 0, 1, 0])
        self.assertEqual(common.lift_at(y, y.astype(float), 0.5), 2.0)  # Top5 命中 4/5，基础率 0.4
        self.assertEqual(common.brier(y, y.astype(float)), 0.0)
        self.assertEqual(common.mae(np.array([1.0, 2.0]), np.array([3.0, 2.0])), 1.0)
        self.assertTrue(np.isnan(common.lift_at(np.zeros(5, dtype=int), np.arange(5), 0.4)),
                        "全负样本时 lift 没有定义，必须是 NaN 而不是 0")

    def test_calibration_bins_are_exhaustive_and_count_preserving(self) -> None:
        rng = np.random.default_rng(0)
        prob = rng.random(500)
        table = common.calibration_bins((prob > 0.8).astype(int), prob, bins=10)
        self.assertEqual(int(table["n"].sum()), 500)
        self.assertEqual(len(table), 10)
        self.assertTrue((table["predicted"].diff().dropna() >= 0).all(), "桶必须按预测概率升序")
        self.assertLess(float(table["predicted"].iloc[0]), float(table["predicted"].iloc[-1]))


class FeatureGuards(unittest.TestCase):
    def test_same_day_exposure_columns_are_banned(self) -> None:
        banned = {"attempts_on_day", "users_on_day", "started_on_day", "tech_fails_on_day", "fail_flag"}
        self.assertTrue(banned <= set(features.FORBIDDEN_FEATURES))
        self.assertTrue(banned <= set(common.NON_FEATURE_COLUMNS))

    def test_as_of_yesterday_columns_are_not_banned(self) -> None:
        """昨天的 as-of 是正牌信号；把它们当禁入列会把本线唯一有用的东西删掉。"""
        for column in ("prev_day_fail", "prev_day_attempts", "last_state_code", "last_online",
                       "fail_streak_before", "attempts_per_day_30d", "charger_fail_rate_prior",
                       "temp_c_max_prev", "charger_tickets_30d", "tel_offline_prev_day"):
            self.assertNotIn(column, set(features.FORBIDDEN_FEATURES), column)
            self.assertNotIn(column, set(common.NON_FEATURE_COLUMNS), column)

    def test_ignored_columns_all_carry_a_reason(self) -> None:
        self.assertIn("demand_multiplier", set(features.IGNORED_COLUMNS))
        self.assertIn("status", set(features.IGNORED_COLUMNS))
        self.assertTrue(all(reason for reason in features.IGNORED_COLUMNS.values()),
                        "声明了读过但不用的列却没写原因")

    def test_pick_features_drops_constants_and_banned_columns(self) -> None:
        picked = features.pick_features(_tiny_feature_frame())
        listed = set(picked["numeric"]) | set(picked["categorical"])
        for column in ("attempts_on_day", "tech_fails_on_day", "users_on_day", "started_on_day",
                       "y_fail", "constant_column", "demand_multiplier", "charger_id",
                       "business_date", "day_start_utc"):
            self.assertNotIn(column, listed, column)
        self.assertIn("constant_column", picked["droppedConstant"])
        self.assertIn("prev_day_fail", picked["groups"]["historyOnly"])
        self.assertEqual(set(picked["categorical"]), set(common.CATEGORICAL_FEATURES))
        for columns in picked["groups"].values():
            self.assertFalse(set(columns) & (set(features.FORBIDDEN_FEATURES)
                                             | set(common.NON_FEATURE_COLUMNS)
                                             | set(features.IGNORED_COLUMNS)))

    def test_pick_features_refuses_a_banned_column_named_as_categorical(self) -> None:
        """数值列靠"排除法"挡住，类别列靠名单——所以真正的闸门是这道断言：
        只要有人把当日量写进 ``CATEGORICAL_FEATURES``，建表就必须当场失败，而不是静默泄题。"""
        frame = _tiny_feature_frame()
        with mock.patch.object(common, "CATEGORICAL_FEATURES",
                               list(common.CATEGORICAL_FEATURES) + ["tech_fails_on_day"]):
            with self.assertRaises(AssertionError):
                features.pick_features(frame)

    def test_candidate_sets_partition_and_are_nonempty(self) -> None:
        numeric = ["prev_day_fail", "attempts_per_day_30d", "charger_tickets_30d", "rated_power_kw",
                   "tel_offline_prev_day"]
        categorical = ["manufacturer", "last_state_code"]
        groups = features.candidate_sets(numeric, categorical)
        self.assertEqual(set(groups["full"]), set(numeric) | set(categorical))
        self.assertIn("prev_day_fail", groups["historyOnly"])
        self.assertIn("attempts_per_day_30d", groups["historyOnly"])
        self.assertNotIn("attempts_per_day_30d", groups["staticOnly"])  # 因果用量属于历史，不是身份
        self.assertIn("charger_tickets_30d", groups["opsOnly"])
        self.assertIn("rated_power_kw", groups["staticOnly"])
        self.assertIn("last_state_code", groups["opsOnly"])              # 遥测最后状态是运营信号
        self.assertNotIn("last_state_code", groups["staticOnly"])
        self.assertTrue(all(groups.values()))
        # 当日量万一被人塞进数值名单，candidate_sets 自己就得拒绝（它落进 staticOnly → 撞上禁入闸门）
        with self.assertRaises(AssertionError):
            features.candidate_sets(numeric + ["attempts_on_day"], categorical)

    def test_fail_streak_before_excludes_today(self) -> None:
        panel = _tiny_panel()
        streak = features.fail_streak_before(panel, panel[["charger_id", "business_date"]].copy())
        # A 的失败日 0,1,1,1,0,1 → 不含今天的连跑 0,0,1,2,3,0；B 的 1,1,0,0,0,0 → 0,1,2,0,0,0
        self.assertEqual(streak.tolist()[:6], [0.0, 0.0, 1.0, 2.0, 3.0, 0.0])
        self.assertEqual(streak.tolist()[6:], [0.0, 1.0, 2.0, 0.0, 0.0, 0.0])
        self.assertEqual(int(streak[4]), 3, "第 4 天自己没坏，但昨天为止连坏 3 天——这正是本线的信号")
        self.assertEqual(int(streak[1]), 0, "第 1 天自己坏了却拿到 0：说明它没被算进「此前」")

    def test_fail_streak_before_is_capped(self) -> None:
        panel = _tiny_panel()
        panel["fail_flag"] = 1
        streak = features.fail_streak_before(panel, panel[["charger_id", "business_date"]].copy(), cap=2)
        self.assertLessEqual(int(streak.max()), 2)
        self.assertEqual(streak.tolist(), [0.0, 1.0, 2.0, 2.0, 2.0, 2.0] * 2)

    def test_lagged_never_crosses_cities(self) -> None:
        """pivot 成 日×城 再 shift：B 城首日不能吃 A 城末日的值，rolling 也不能吃上一城的尾巴。"""
        days = pd.date_range("2026-01-01", periods=12)
        ordered = pd.DataFrame({"city_id": ["A"] * 12 + ["B"] * 7,
                                "business_date": list(days) + list(days[5:]),
                                "temp_c_max": [10.0] * 12 + [99.0] * 7})
        keys = pd.MultiIndex.from_arrays([ordered["city_id"].to_numpy(),
                                          ordered["business_date"].to_numpy()],
                                         names=["city_id", "business_date"])
        out = features._lagged(ordered, "temp_c_max", keys, rolling=True)
        previous = pd.Series(out["temp_c_max_prev"])
        self.assertTrue(np.isnan(previous.iloc[12]), "B 城首日必须没有'昨天'，不能吃 A 城末日的 10℃")
        self.assertEqual(previous.iloc[13], 99.0)
        self.assertTrue(np.isnan(previous.iloc[0]))  # A 城首日同样没有昨天
        baseline = pd.Series(out["temp_c_max_mean_30d"])
        self.assertTrue(np.isnan(baseline.iloc[13]), "min_periods=10：B 城只有 1 天历史，均值必须为空")
        self.assertEqual(baseline.iloc[11], 10.0, "A 城第 11 天已攒满 10 个有效观测，该出均值了")

    def test_time_gap_is_nat_safe(self) -> None:
        later = pd.to_datetime(["2026-01-03", "2026-01-03"])
        earlier = pd.to_datetime(["2026-01-01", None])
        gap_days = features.time_gap(later, earlier, 86_400.0)
        self.assertAlmostEqual(gap_days[0], 2.0)
        self.assertTrue(np.isnan(gap_days[1]))
        self.assertTrue((gap_days[~np.isnan(gap_days)] >= 0).all(), "不能出现 int64 溢出的负哨兵")

    def test_prev_beijing_day(self) -> None:
        stamp = pd.Timestamp("2026-01-05 16:00")  # UTC naive = 北京 1/6 零点
        self.assertEqual(features.prev_beijing_day(stamp), pd.Timestamp("2026-01-05"))

    def test_audited_feature_list_covers_the_four_families(self) -> None:
        audited = set(features.AUDITED_FEATURES)
        self.assertEqual(len(audited), 17)
        self.assertFalse(audited & (set(features.FORBIDDEN_FEATURES) | set(common.NON_FEATURE_COLUMNS)))
        for family in ("attempts_sum_30d", "charger_fail_rate_prior", "prev_day_fail",
                       "fail_streak_before", "charger_tickets_30d", "charger_ticket_open_at_start",
                       "tel_days_missing_30d", "tel_offline_prev_day", "last_state_code",
                       "temp_c_max_prev", "temp_c_max_mean_30d"):
            self.assertIn(family, audited, family)


@unittest.skipUnless(HAVE_SKLEARN, "没装 sklearn")
class ThresholdAndDesignLogic(unittest.TestCase):
    def test_daily_alert_cap_is_per_day_share_not_a_floor(self) -> None:
        frame = pd.DataFrame({"business_date": pd.to_datetime(["2026-01-01"] * 75 + ["2026-01-02"] * 40),
                              "charger_id": [f"C{i}" for i in range(75)] + [f"D{i}" for i in range(40)]})
        cap = train.daily_alert_cap(frame)
        self.assertEqual(sorted(int(value) for value in cap), [10, 19])  # ceil(25%×75)=19、ceil(25%×40)=10
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
        self.assertLess(point["precision"], train.PRECISION_MULTIPLE * float(y.mean()))
        self.assertIn("无档满足，退回最高精确率档（如实标注，不假装达标）", point["rule"])
        self.assertGreaterEqual(point["alerts"], train.MIN_FLAGGED)

    def test_pick_threshold_refuses_when_no_tier_has_enough_alerts(self) -> None:
        rng = np.random.default_rng(1)
        with self.assertRaises(RuntimeError):  # 40 行：任何档都凑不出 30 条告警
            train.pick_threshold(rng.random(40), (rng.random(40) < 0.3).astype(int))

    def test_design_keeps_nan_and_marks_categoricals(self) -> None:
        frame = pd.DataFrame({"a": [1.0, np.nan, 3.0], "b": ["x", None, "y"], "c": [True, False, True]})
        matrix, mask = train.design(frame, ["a", "c"], ["b"])
        self.assertTrue(np.isnan(matrix["a"].iloc[1]), "NaN 必须留给 HGBT 原生处理，不能被填掉")
        self.assertEqual(mask.tolist(), [False, False, True])
        self.assertEqual(str(matrix["b"].dtype), "category")
        self.assertEqual(list(matrix["b"].cat.categories), ["MISSING", "x", "y"])
        self.assertEqual(int(matrix["c"].iloc[0]), 1)
        self.assertEqual(list(matrix.columns), ["a", "c", "b"])
        with self.assertRaises(KeyError):
            train.design(frame, ["a", "nope"], ["b"])

    def test_split_group_rejects_unknown_columns(self) -> None:
        with self.assertRaises(AssertionError):
            train.split_group(["ghost"], ["a"], ["b"])
        self.assertEqual(train.split_group(["b", "a"], ["a"], ["b"]), (["a"], ["b"]))

    def test_metrics_block_reports_the_advertised_keys(self) -> None:
        y = np.array([1, 0, 1, 0, 1, 0, 1, 0])
        block = train.metrics_block(y, y.astype(float) * 0.9 + 0.05)
        for key in ("auc", "prAuc", "logLoss", "brier", "liftAt5pct", "liftAt10pct", "liftAt20pct"):
            self.assertIn(key, block)
        self.assertEqual(block["auc"], 1.0)
        self.assertGreater(block["liftAt20pct"], 1.0)

    def test_baseline_scores_use_only_the_declared_oracle_column(self) -> None:
        """六档基线里只有一档用了当日量，且它就是标注不可部署的那档。"""
        frame = pd.DataFrame({"charger_id": ["A"] * 4, "charger_model": ["M"] * 4,
                              "station_id": ["S"] * 4, "charger_fail_rate_prior": [0.1] * 4,
                              "attempts_per_day_30d": [2.0] * 4, "attempts_on_day": [9.0] * 4,
                              "prev_day_fail": [np.nan, 1.0, 0.0, 1.0]})
        baselines = {"global": pd.DataFrame({"rate": [0.2]}),
                     "charger": pd.DataFrame({"charger_id": [], "rate": []}),
                     "charger_model": pd.DataFrame({"charger_model": [], "rate": []}),
                     "station": pd.DataFrame({"station_id": [], "rate": []})}
        scores = train.baseline_scores(frame, baselines)
        self.assertEqual(set(scores), {"globalBaseRate", "trainLookupBlend", "asOfChargerPriorOnly",
                                       "expectedVolumeOnly", "oracleActualVolumeNONDEPLOYABLE",
                                       "naiveLag1Alert"})
        self.assertEqual(scores["oracleActualVolumeNONDEPLOYABLE"].tolist(), [9.0] * 4)
        self.assertEqual(scores["expectedVolumeOnly"].tolist(), [2.0] * 4)
        self.assertEqual(scores["globalBaseRate"].tolist(), [0.2] * 4)
        self.assertEqual([round(float(value), 12) for value in scores["trainLookupBlend"]], [0.2] * 4,
                         "三档查表全空时必须干净回落到全局率")
        self.assertEqual(scores["naiveLag1Alert"].tolist(), [0.0, 1.0, 0.0, 1.0])
        self.assertTrue(np.isnan(frame["prev_day_fail"].iloc[0]),
                        "lag1 的缺失是「没有昨天」，向量里必须是 NaN；填 0 由 baseline_scores 显式负责")

    def test_predict_baseline_falls_back_to_global_rate_and_prefers_bad_chargers(self) -> None:
        train_frame = pd.DataFrame({"charger_id": ["A"] * 20 + ["B"] * 20,
                                    "charger_model": ["M"] * 40, "station_id": ["S"] * 40,
                                    "y_fail": [1, 0, 1, 0] * 5 + [0] * 20})
        baselines = train.fit_baselines(train_frame)
        global_rate = float(baselines["global"]["rate"].iloc[0])
        self.assertAlmostEqual(global_rate, 0.25)
        query = pd.DataFrame({"charger_id": ["Z", "A", "Z"], "charger_model": ["NOPE", "M", "NOPE"],
                              "station_id": ["N", "S", "N"]})
        scores = train.predict_baseline(baselines, query)
        self.assertAlmostEqual(float(scores[0]), global_rate, places=9)  # 三档全查不到 → 回落全局率
        self.assertAlmostEqual(float(scores[2]), global_rate, places=9)
        self.assertGreater(float(scores[1]), global_rate, "A 的 TRAIN 失败率高于全局，查表必须往上走")

    def test_smoothed_rate_shrinks_small_samples_toward_the_prior(self) -> None:
        frame = pd.DataFrame({"charger_id": ["A"] * 40 + ["B"] * 2,
                              "y_fail": [1, 0, 1, 0] * 10 + [1, 1]})
        table = train._smoothed_rate(frame[["charger_id"]], frame["y_fail"], alpha=train.BASELINE_ALPHA)
        values = dict(zip(table["charger_id"], table["rate"]))
        self.assertAlmostEqual(values["A"], (20 + train.BASELINE_ALPHA * train.NEUTRAL_PRIOR) / 70.0)
        self.assertLess(values["B"], 1.0, "两台观测就断定 100% 会坏，是查表基线最该防的过拟合")
        self.assertGreater(values["B"], values["A"], "B 两次全坏，收缩后仍应高于 A")


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
            self.assertEqual(following["valStart"], previous["fitEnd"])  # 验证段接上一轮的拟合尾
            self.assertEqual(following["fitEnd"] - following["valStart"],
                             pd.Timedelta(days=rolling.VALIDATION_DAYS))
        for fold in folds:
            self.assertGreater(fold["valStart"], bounds["startInclusive"], "验证段跑到窗口左边之外了")

    def test_fold_plan_refuses_to_peek_past_the_window(self) -> None:
        bounds = _bounds()
        with self.assertRaises(ValueError):
            rolling.fold_plan(bounds, 11)
        with self.assertRaises(ValueError):
            rolling.fold_plan(bounds, 0)

    def test_sign_test_p_values(self) -> None:
        self.assertAlmostEqual(rolling.sign_test_p(10, 10), 2 * 0.5 ** 10)          # 十战全胜
        self.assertAlmostEqual(rolling.sign_test_p(9, 10),
                               2 * sum(math.comb(10, i) for i in range(2)) / 2.0 ** 10)
        self.assertEqual(rolling.sign_test_p(5, 10), 1.0)                           # 完全对称 → 无证据
        self.assertTrue(np.isnan(rolling.sign_test_p(0, 0)))
        self.assertLess(rolling.sign_test_p(9, 10), 0.05)
        self.assertGreater(rolling.sign_test_p(7, 10), 0.05, "7/10 不该被当成显著")

    def test_group_order_covers_every_candidate_set(self) -> None:
        self.assertEqual(set(rolling.GROUP_ORDER), {"staticOnly", "historyOnly", "opsOnly", "full"})


@unittest.skipUnless(HAVE_ARTIFACTS, "还没跑 build_panel / features / train / evaluate")
class DatasetFacts(unittest.TestCase):
    """钉住数据事实与产物自洽：稠密面板、lag1 为正、零方差列真的零方差、报告不自我矛盾。"""

    @classmethod
    def setUpClass(cls) -> None:
        cls.panel = common.load_panel()
        cls.frame = pd.read_pickle(common.FEATURES_PATH)
        cls.manifest = common.read_derived_manifest()
        with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
            cls.summary = json.load(handle)
        with open(common.TRAIN_METRICS_PATH, encoding="utf-8") as handle:
            cls.metrics = json.load(handle)
        with open(common.TEST_REPORT_PATH, encoding="utf-8") as handle:
            cls.report = json.load(handle)

    def test_panel_is_a_dense_charger_day_grid(self) -> None:
        chargers = int(self.panel["charger_id"].nunique())
        days = int(self.panel["business_date"].nunique())
        self.assertEqual((chargers, days), (75, 180))
        self.assertEqual(len(self.panel), chargers * days, "面板不稠密：'30 天窗口'的分母就不成立")
        self.assertFalse(self.panel.duplicated(subset=["charger_id", "business_date"]).any())
        full = pd.MultiIndex.from_product(
            [self.panel["charger_id"].unique(), self.panel["business_date"].unique()],
            names=["charger_id", "business_date"])
        self.assertTrue(full.equals(pd.MultiIndex.from_frame(
            self.panel[["charger_id", "business_date"]]).sort_values()))
        self.assertEqual(len(common.load_context()), int(self.panel["city_id"].nunique()) * days)

    def test_zero_attempt_days_carry_zero_failures(self) -> None:
        idle = self.panel[self.panel["attempts_on_day"] == 0]
        self.assertGreater(len(idle), 0, "一个 0 尝试的日子都没有，那这条排除规则就是空谈")
        self.assertTrue((idle["tech_fails_on_day"] == 0).all())
        self.assertFalse(idle["fail_flag"].astype(bool).any())
        self.assertTrue((self.panel["tech_fails_on_day"] <= self.panel["attempts_on_day"]).all())

    def test_exposure_gradient_is_steep_enough_to_matter(self) -> None:
        """common 模块文档说"当日尝试 1–2 次 1.9%、20 次以上 52.9%"——那句是禁入名单的全部理由。"""
        busy = self.panel[self.panel["attempts_on_day"] >= 20]
        quiet = self.panel[self.panel["attempts_on_day"].between(1, 2)]
        self.assertGreater(len(busy), 0, "样本里根本没有 20 次以上的日子，那句话无从核对")
        self.assertGreater(busy["fail_flag"].mean(), 10 * max(quiet["fail_flag"].mean(), 1e-9),
                           "暴露度梯度没这么陡了，禁入名单的理由要重写")

    def test_panel_totals_reconcile_with_the_derivation_manifest(self) -> None:
        scope = self.manifest["attemptScope"]
        self.assertEqual(int(self.panel["attempts_on_day"].sum()), int(scope["inScopeRows"]))
        self.assertEqual(int(self.panel["tech_fails_on_day"].sum()), int(scope["techFailures"]))
        self.assertLess(int(scope["techFailures"]), int(scope["inScopeRows"]))
        for phrase in ("outcome", "非排队关联", "charger_id 非空"):
            self.assertIn(phrase, scope["scopeRule"],
                          "口径说明没交代清楚，换批次时没人能复核这句话")

    def test_sample_drops_exactly_the_zero_attempt_rows(self) -> None:
        label = self.summary["scope"]
        self.assertEqual(int(label["panelRows"]) - int(label["droppedZeroAttemptRows"]), int(label["rows"]))
        self.assertEqual(len(self.frame), int(label["rows"]))
        self.assertLess(len(self.frame), len(self.panel))
        self.assertEqual(int(label["droppedPositives"]), 0)
        self.assertTrue((self.frame["attempts_on_day"] > 0).all())
        self.assertEqual(int(label["chargers"]), int(self.frame["charger_id"].nunique()))

    def test_lag1_dependence_is_positive(self) -> None:
        """反号过一次就永远钉住：昨天坏 → 今天更容易坏。"""
        trained = self.frame[self.frame["split"] == "TRAIN"]
        p1 = float(trained.loc[trained["prev_day_fail"] == 1, "y_fail"].mean())
        p0 = float(trained.loc[trained["prev_day_fail"] == 0, "y_fail"].mean())
        self.assertGreater(p1, p0, "lag1 方向变了，features/train 里的注释要重新核")
        self.assertGreater(p1 / p0, 1.5, "倍差没了的话，「昨天坏」这个信号就不值得单独成组")

    def test_fail_streak_is_monotone_on_real_data(self) -> None:
        buckets = (self.frame.assign(b=pd.cut(self.frame["fail_streak_before"], [-1, 0, 1, 2, 20]))
                   .groupby("b", observed=True)["y_fail"].mean())
        values = [float(value) for _, value in buckets.items()]
        self.assertGreaterEqual(len(values), 4)
        self.assertEqual(values, sorted(values), f"连跑长度与失败率不再单调：{values}")

    def test_label_agrees_with_the_panel_counts(self) -> None:
        self.assertTrue((self.frame["y_fail"].to_numpy()
                         == (self.frame["tech_fails_on_day"] > 0).to_numpy()).all())
        self.assertTrue((self.frame["y_fail"] == self.frame["fail_flag"]).all())
        in_window = self.frame[self.frame["split"] != "EXCLUDED"]
        self.assertEqual(int(in_window["y_fail"].sum()),
                         sum(int(self.summary["splitPositives"][name])
                             for name in ("TRAIN", "VALIDATION", "TEST")))
        self.assertEqual(int(self.frame["y_fail"].sum()), int(self.summary["scope"]["positives"]))
        self.assertLess(len(in_window), len(self.frame),
                        "特征表本该保留窗口外的 EXCLUDED 行供复核，两数相等说明被提前丢了")
        self.assertEqual(int((self.frame["split"] == "EXCLUDED").sum()),
                         int(self.summary["splits"]["EXCLUDED"]))
        self.assertEqual(self.frame["split"].value_counts().to_dict(),
                         {key: int(value) for key, value in self.summary["splits"].items()})
        self.assertEqual(int(self.report["risk"]["positives"]), int(self.summary["splitPositives"]["TEST"]))
        self.assertEqual(self.summary["label"],
                         "y_fail = 该桩当日 tech_fails_on_day ≥ 1；y_fails = 当日技术失败次数（回归目标）")

    def test_zero_variance_columns_are_actually_constant(self) -> None:
        expected = {"transformer_kw": 360.0, "heatwave_prev": 0.0, "days_since_last_observed": 1.0,
                    "tel_gap_hours_at_start": 1.0 / 12.0}
        dropped = self.summary["features"]["droppedConstant"]
        self.assertEqual(sorted(dropped), sorted(expected))
        for column in dropped:
            series = self.frame[column].dropna()
            self.assertLessEqual(series.nunique(), 1, column)
            self.assertAlmostEqual(float(series.iloc[0]), expected[column], places=5,
                                   msg=f"{column} 的实际取值与报告里的解释不一致，注释要改")
        self.assertEqual(self.summary["constantWhy"],
                         {name: "样本域内取值唯一（非缺失），无信息量" for name in dropped})

    def test_feature_list_keeps_the_leak_banned_and_the_planned(self) -> None:
        listed = set(self.summary["features"]["numeric"]) | set(self.summary["features"]["categorical"])
        banned = (set(features.FORBIDDEN_FEATURES) | set(common.NON_FEATURE_COLUMNS)
                  | set(features.IGNORED_COLUMNS))
        self.assertFalse(listed & banned)
        for group in self.summary["features"]["groups"].values():
            self.assertTrue(set(group) <= listed)
        self.assertEqual(set(self.summary["features"]["groups"]), set(rolling.GROUP_ORDER))
        self.assertEqual(set(self.summary["features"]["categorical"]), set(common.CATEGORICAL_FEATURES))
        self.assertGreater(len(self.summary["features"]["numeric"]), 50)
        self.assertEqual(self.summary["featureRows"], len(self.frame))

    def test_leak_audit_deviation_is_machine_zero(self) -> None:
        values = self.summary["leakAudit"]["maxAbsDiff"]
        self.assertEqual(len(values), len(features.AUDITED_FEATURES))
        self.assertEqual(self.summary["leakAudit"]["sampled"], 250)
        for name, delta in values.items():
            self.assertLessEqual(delta, 1e-9, f"{name} 的 as-of 值两条路径不一致")

    def test_bundle_matches_feature_table_and_summary(self) -> None:
        bundle = evaluate.load_bundle()
        self.assertEqual(bundle["featuresSha256"], common.sha256_file(common.FEATURES_PATH))
        self.assertEqual(bundle["derivedDatasetId"], common.DERIVED_DATASET_ID)
        self.assertEqual(bundle["publishedBatchId"], common.EXPECTED_PUBLISHED_BATCH_ID)
        self.assertEqual(bundle["chosenSet"], self.metrics["chosenSet"])
        self.assertEqual(bundle["operatingPoint"]["threshold"], self.metrics["operatingPoint"]["threshold"])
        self.assertEqual(bundle["numericFeatures"] + bundle["categoricalFeatures"],
                         self.metrics["numericFeatures"] + self.metrics["categoricalFeatures"])
        self.assertEqual(self.metrics["selectionRule"],
                         "四组候选特征集按 VALIDATION ROC AUC 择一发布；TEST 完全不参与选择")
        self.assertEqual(int(self.metrics["testRowsUntouched"]), int(self.report["testRows"]))
        self.assertTrue(self.metrics["operatingPoint"]["targetMet"])
        self.assertGreaterEqual(self.metrics["operatingPoint"]["precision"],
                                train.PRECISION_MULTIPLE * self.summary["splitBaseRate"]["VALIDATION"] - 1e-9)
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
                with self.assertRaises(common.DerivedMismatch):
                    evaluate.load_bundle()

    def test_bundle_refuses_a_model_from_another_feature_table(self) -> None:
        """模型与特征表必须成对发布：只换其中一个，配对检查就得拦住。"""
        bundle = joblib.load(common.BUNDLE_PATH)
        bundle["featuresSha256"] = "0" * 64
        with tempfile.TemporaryDirectory() as tmp:
            forged = Path(tmp) / "forged.joblib"
            joblib.dump(bundle, forged)
            with mock.patch.object(common, "BUNDLE_PATH", forged):
                with self.assertRaisesRegex(common.DerivedMismatch, "成对发布"):
                    evaluate.load_bundle()

    def test_evaluation_invariants_are_all_true(self) -> None:
        invariants = self.report["invariants"]
        for key in ("testStartsAtValidationEnd", "testEndsBeforeWindowEnd", "chargerDayUniqueInTest",
                    "everyRowHasAttempts", "scoreRangeLegal", "labelMatchesPanelCounts"):
            self.assertTrue(invariants[key], key)
        self.assertEqual(invariants["testDays"], 30)
        self.assertEqual(invariants["testRowsWithUnseenCharger"], 0,
                         "TEST 里出现了 TRAIN 没见过的桩：查表基线在那几行只能回落全局率")
        self.assertEqual(invariants["chargerShareOfTestSeenInTrain"], 1.0)
        self.assertEqual(int(self.report["testRows"]), int(self.summary["splits"]["TEST"]))

    def test_evaluation_report_is_internally_consistent(self) -> None:
        scoreboard = self.report["scoreboard"]
        self.assertEqual(scoreboard["model"]["auc"], round(self.report["risk"]["auc"], 4))
        self.assertTrue(scoreboard["model"]["deployable"])
        self.assertFalse(scoreboard["oracleActualVolumeNONDEPLOYABLE"]["deployable"],
                         "oracle 必须标成不可部署，否则它就是个作弊基线")
        self.assertEqual(scoreboard["globalBaseRate"]["auc"], 0.5)  # 常数打分的尺子
        self.assertGreater(scoreboard["naiveLag1Alert"]["auc"], 0.5,
                           "lag1 的 AUC 应大于 0.5（正依赖），报告与数据不一致时先看这条")
        self.assertIsNone(scoreboard["naiveLag1Alert"]["logLoss"], "规则分不是概率，不许凑 logLoss")
        self.assertIsNotNone(scoreboard["model"]["logLoss"])
        self.assertGreater(self.report["risk"]["aucTrain"], self.report["risk"]["aucValidation"],
                           "TRAIN 不高于 VALIDATION 说明切分或打分有问题（过拟合方向反了）")
        for name, value in self.report["rankAgreement"].items():
            self.assertGreater(value, 0.5, f"模型与 {name} 的秩相关掉到 0.5 以下，报告第 3 节措辞要改")

    def test_inspection_budget_table_is_blind_to_the_answer(self) -> None:
        rows = self.report["budgetRanking"]
        self.assertEqual([row["budget"] for row in rows], list(evaluate.BUDGETS))
        names = {key for row in rows for key in row if key != "budget"}
        self.assertIn("model", names)
        self.assertNotIn("oracleActualVolumeNONDEPLOYABLE", names,
                         "巡检预算表里混进了不可部署的 oracle，就是在给运营看作弊数字")
        days = int(self.report["invariants"]["testDays"])
        chargers = int(self.summary["scope"]["chargers"])
        for row in rows:
            budget = float(row["budget"])
            ceiling = days * math.ceil(budget * chargers)  # 逐日 Top-k 的绝对上限
            for name, block in row.items():
                if name == "budget":
                    continue
                self.assertLessEqual(block["alerts"], ceiling, f"{name} 在 {budget:.0%} 预算下超发告警")
                self.assertLessEqual(block["recall"], 1.0)
                self.assertLessEqual(block["precision"], 1.0)
        self.assertGreater(rows[-1]["model"]["recall"], rows[0]["model"]["recall"],
                           "预算翻了 5 倍召回却不涨，排序就没意义")

    def test_breakdown_buckets_are_exhaustive(self) -> None:
        test_rows = int(self.report["testRows"])
        for name in ("weekend", "halfWindow", "prevDayFail", "expectedVolume"):
            total = sum(row["n"] for row in self.report["breakdown"][name])
            self.assertEqual(total, test_rows, f"{name} 的分桶没有覆盖全部 TEST 行")
        self.assertEqual(len(self.report["breakdown"]["expectedVolume"]), 5)
        self.assertEqual(len(self.report["breakdown"]["prevDayFail"]), 2)
        for row in self.report["breakdown"]["expectedVolume"]:
            self.assertGreaterEqual(row["n"], evaluate.MIN_BUCKET_ROWS)
            self.assertIsNotNone(row["auc"], f"{row['bucket']} 档内 AUC 为空：切片太小，归因结论要重写")

    def test_calibration_table_covers_test(self) -> None:
        table = self.report["calibration"]
        self.assertEqual(len(table), 10)
        self.assertEqual(sum(row["n"] for row in table), int(self.report["testRows"]))
        self.assertEqual([row["predicted"] for row in table], sorted(row["predicted"] for row in table))

    def test_count_regression_conclusion_matches_its_numbers(self) -> None:
        rows = self.report["countRegression"]["rows"]
        self.assertEqual(sorted(rows), ["expectedVolumeTimesPrior", "modelRegressor", "trainMeanFails"])
        self.assertIsNone(rows["trainMeanFails"]["auc"], "常数打分没有 AUC，必须留空而不是 0.5")
        beats_both = rows["modelRegressor"]["mae"] < min(rows["trainMeanFails"]["mae"],
                                                        rows["expectedVolumeTimesPrior"]["mae"])
        self.assertEqual("模型在次数尺度上也赢了两条基线" in self.report["countRegression"]["conclusion"],
                         beats_both, "回归结论与 MAE 数字不一致（自动生成的措辞被手改过？）")

    def test_headline_percentages_match_the_scoreboard(self) -> None:
        model = self.report["risk"]["auc"]
        head = self.report["headline"]
        for key, baseline in (("vsLookupPct", "trainLookupBlend"), ("vsPriorPct", "asOfChargerPriorOnly"),
                              ("vsVolumePct", "expectedVolumeOnly")):
            stored = self.report["scoreboard"][baseline]["auc"]
            self.assertAlmostEqual(head[key] / 100.0 + 1.0, model / stored, delta=2e-4,
                                   msg=f"{key} 与 scoreboard 的比值对不上")
        beaten = model >= max(self.report["scoreboard"]["trainLookupBlend"]["auc"],
                              self.report["scoreboard"]["asOfChargerPriorOnly"]["auc"])
        self.assertEqual("跑赢了全部可部署基线" in head["conclusion"], beaten,
                         "主结论的措辞与实际排名不一致")

    def test_operating_point_and_budget_compliance_agree(self) -> None:
        point = self.report["risk"]["frozenOperatingPoint"]
        budget = self.report["budget"]
        self.assertEqual(budget["days"], self.report["invariants"]["testDays"])
        self.assertLessEqual(budget["meanAlertsPerDay"], train.MAX_ALERT_SHARE * 75 + 1,
                             "平均每日告警数超过了自我设定的 25% 日预算")
        self.assertGreaterEqual(point["alerts"], train.MIN_FLAGGED)
        self.assertIn(f"{train.MAX_ALERT_SHARE:.0%}", point["rule"])
        self.assertEqual(point["note"], "阈值与规则在 VALIDATION 上定死，TEST 只验不改")


@unittest.skipUnless(HAVE_ROLLING, "还没跑 rolling")
class RollingArtifacts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        with open(common.ROLLING_SUMMARY_PATH, encoding="utf-8") as handle:
            cls.summary = json.load(handle)

    def test_every_round_is_reported_and_ordered(self) -> None:
        rounds = self.summary["roundsDetail"]
        self.assertEqual([row["round"] for row in rounds], list(range(len(rounds))))
        self.assertGreaterEqual(len(rounds), 10)
        for row in rounds:
            self.assertGreaterEqual(row["evalRows"], rolling.MIN_EVAL_ROWS)
            self.assertGreaterEqual(row["evalPositives"], rolling.MIN_EVAL_POSITIVES)
            self.assertLess(row["evalEnd"][:10], "2026-05-29", "评测窗越过了数据右端")
            self.assertIn(row["chosenSet"], rolling.GROUP_ORDER)
            self.assertEqual(row["evalDays"], rolling.HORIZON_DAYS)
            self.assertLess(row["alertRate"], 0.5, "评测窗告警率超过一半，阈值口径已经失效")
        first, last = rounds[0], rounds[-1]
        self.assertLess(first["fitEnd"], last["fitEnd"])
        self.assertLess(first["chargerDaysPriorMedian"], last["chargerDaysPriorMedian"],
                        "面板应该越滚越厚：每台桩的历史天数中位数必须变多")

    def test_verdict_numbers_come_from_the_rounds(self) -> None:
        rounds = self.summary["roundsDetail"]
        mean_model = float(np.mean([row["auc"] for row in rounds]))
        self.assertAlmostEqual(self.summary["metrics"]["auc"]["mean"], round(mean_model, 4), places=4)
        trend = self.summary["trend"]
        self.assertEqual(trend["gainVsPriorPositiveRounds"],
                         sum(1 for row in rounds if row["aucGainVsPriorPct"] > 0))
        self.assertEqual(trend["gainVsLookupPositiveRounds"],
                         sum(1 for row in rounds if row["aucGainVsLookupPct"] > 0))
        self.assertEqual(trend["gainRounds"], len(rounds))
        best_cheap = max(self.summary["metrics"]["aucAsOfPriorBaseline"]["mean"],
                         self.summary["metrics"]["aucLookupBaseline"]["mean"])
        self.assertEqual(self.summary["verdict"]["modelBeatsCheapBaseline"], mean_model > best_cheap)
        self.assertIn(f"{self.summary['metrics']['auc']['mean']:.4f}",
                      self.summary["verdict"]["headline"], "headline 里的 AUC 不是十轮均值（被手改过？）")
        self.assertEqual(self.summary["chosenSetCounts"],
                         {key: sum(1 for row in rounds if row["chosenSet"] == key)
                          for key in rolling.GROUP_ORDER
                          if any(row["chosenSet"] == key for row in rounds)})

    def test_metric_blocks_are_self_consistent(self) -> None:
        metrics = self.summary["metrics"]
        for name, block in metrics.items():
            self.assertLessEqual(block["min"], block["mean"], name)
            self.assertLessEqual(block["mean"], block["max"], name)
            self.assertGreaterEqual(block["std"], 0.0, name)
        self.assertEqual(self.summary["warmupDays"], rolling.WARMUP_DAYS)
        self.assertEqual(self.summary["rounds"], len(self.summary["roundsDetail"]))
        self.assertEqual(len(self.summary["valAucBySet"]), len(rolling.GROUP_ORDER))
        for key, values in self.summary["valAucBySet"].items():
            self.assertEqual(len(values), len(self.summary["roundsDetail"]), key)

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
