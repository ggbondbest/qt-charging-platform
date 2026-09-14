"""第六线（插枪启动失败）的回归测试。

分两类：
  · 纯逻辑测试——批次核对、切分边界、独占写入、as-of 口径（尤其是**同刻不可见**这条）、收缩公式、
    特征名单闸门、折计划、符号检验：不依赖任何产物，秒级；
  · 产物测试——需要本地先跑过 build_data / train / evaluate（``data_analysis/outputs/`` 在 .gitignore 内，
    所以 CI 与刚 clone 的人这里自动 skip，而不是假装通过）。

依赖 sklearn 的部分（train / rolling 里的分组与阈值逻辑）单独用 HAVE_SKLEARN 兜住：
建表侧 build_data 只依赖 numpy/pandas，即使没装 sklearn 也应该能验。

用法（仓库根目录）：python -m unittest discover -s data_analysis/ml/attempt/tests -t .
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import numpy as np
import pandas as pd

from data_analysis.ml.attempt import build_data, common

try:  # train / rolling 顶层引用 sklearn；缺依赖时这两块的测试整体跳过而不是报错
    from data_analysis.ml.attempt import train as attempt_train

    HAVE_SKLEARN = True
except ImportError:  # pragma: no cover
    attempt_train = None
    HAVE_SKLEARN = False

ARTIFACTS = (common.MATRIX_PATH, common.BUILD_SUMMARY_PATH, common.BUNDLE_PATH,
             common.TRAIN_METRICS_PATH, common.TEST_REPORT_PATH)
HAVE_ARTIFACTS = all(path.exists() for path in ARTIFACTS)


def _bounds() -> dict[str, pd.Timestamp]:
    """与 manifest 等价的边界，但不读盘（纯逻辑测试要能在任何机器上跑）；
    两者是否仍然一致由 ``BatchAndSplits.test_hardcoded_bounds_still_match_the_manifest`` 把守。"""
    shift = pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
    return {"startInclusive": pd.Timestamp("2025-12-02") - shift,
            "trainEndExclusive": pd.Timestamp("2026-03-30") - shift,
            "validationEndExclusive": pd.Timestamp("2026-04-29") - shift,
            "testEndExclusive": pd.Timestamp("2026-05-29") - shift}


def _tiny_history() -> pd.DataFrame:
    """一台桩 6 次尝试：开头两条同刻并列，第 5/6 条已跨出 30 天窗口——覆盖 as-of 全部边界。"""
    base = pd.Timestamp("2026-01-01")
    stamps = [base, base, base + pd.Timedelta(days=1), base + pd.Timedelta(days=1, hours=5),
              base + pd.Timedelta(days=40), base + pd.Timedelta(days=40, minutes=1)]
    return pd.DataFrame({"charger_id": ["A"] * 6,
                         "attempted_at": pd.to_datetime(stamps),
                         "y_tech": [0, 1, 0, 0, 0, 0]})


class BatchAndSplits(unittest.TestCase):
    def test_verify_batch_fails_closed_on_other_batch(self) -> None:
        with self.assertRaises(common.BatchMismatch):
            common.verify_batch({"publishedBatchId": "analytics-" + "0" * 32})
        self.assertEqual(common.verify_batch({"publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID})
                         ["publishedBatchId"], common.EXPECTED_PUBLISHED_BATCH_ID)

    def test_missing_batch_field_is_a_mismatch_too(self) -> None:
        with self.assertRaises(common.BatchMismatch):
            common.verify_batch({"datasetId": common.DATASET_ID})

    def test_verify_batch_accepts_the_bound_batch_on_disk(self) -> None:
        manifest = common.read_manifest()
        self.assertEqual(common.verify_batch(manifest)["publishedBatchId"],
                         common.EXPECTED_PUBLISHED_BATCH_ID)
        self.assertEqual(manifest["mlSplits"]["usable"], True)

    def test_hardcoded_bounds_still_match_the_manifest(self) -> None:
        """纯逻辑测试用的手抄边界必须与真实 manifest 同步，否则折计划测试是假的。"""
        self.assertEqual(_bounds(), common.split_boundaries(common.read_manifest()))

    def test_dataset_id_is_the_directory_not_the_upstream_simulation_id(self) -> None:
        """两个 id 不是一回事：DATASET_ID 是本地重切目录名，manifest 的 datasetId 是上游仿真集 id。
        这条测试存在的意义就是提醒：换批次时别把两者当成一个字段去比。"""
        manifest = common.read_manifest()
        self.assertEqual(common.DATASET_ID, common.CLEAN_DIR.parent.name)
        self.assertNotEqual(manifest["datasetId"], common.DATASET_ID)
        self.assertEqual(manifest["businessTimezone"], "Asia/Shanghai")

    def test_boundaries_are_business_calendar_days_converted_to_utc(self) -> None:
        bounds = common.split_boundaries(common.read_manifest())
        self.assertEqual((bounds["trainEndExclusive"] - bounds["startInclusive"]).days, 118)
        self.assertEqual((bounds["validationEndExclusive"] - bounds["trainEndExclusive"]).days, 30)
        self.assertEqual((bounds["testEndExclusive"] - bounds["validationEndExclusive"]).days, 30)
        # 边界日是北京时间 00:00，即 UTC 前一天 16:00：直接用 UTC 零点会整体错 8 小时
        self.assertEqual(bounds["trainEndExclusive"].strftime("%Y-%m-%d %H:%M"), "2026-03-29 16:00")

    def test_assign_split_boundary_ownership(self) -> None:
        bounds = _bounds()
        stamps = pd.to_datetime([bounds["startInclusive"] - pd.Timedelta(seconds=1),
                                 bounds["startInclusive"],
                                 bounds["trainEndExclusive"] - pd.Timedelta(seconds=1),
                                 bounds["trainEndExclusive"],
                                 bounds["validationEndExclusive"],
                                 bounds["testEndExclusive"],
                                 bounds["testEndExclusive"] + pd.Timedelta(seconds=1)])
        self.assertEqual(list(common.assign_split(stamps, bounds)),
                         ["EXCLUDED", "TRAIN", "TRAIN", "VALIDATION", "TEST", "EXCLUDED", "EXCLUDED"])

    def test_load_table_rejects_missing_columns_instead_of_dropping_them(self) -> None:
        """列裁剪请求失败必须报错：静默丢列会把"这列全是 NaN"伪装成"这列不存在"。"""
        with self.assertRaises(KeyError) as caught:
            common.load_table("chargers", columns=["charger_id", "definitely_not_a_column"])
        self.assertIn("definitely_not_a_column", str(caught.exception))

    def test_load_table_column_pruning_keeps_every_row(self) -> None:
        pruned = common.load_table("chargers", columns=["charger_id", "manufacturer"])
        self.assertEqual(set(pruned.columns), {"charger_id", "manufacturer"})
        self.assertEqual(len(pruned), len(common.load_table("chargers")))


class FrozenWrites(unittest.TestCase):
    def test_require_empty_run_dir_refuses_existing_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            original = common.OUT_DIR
            common.OUT_DIR = Path(tmp)
            try:
                common.require_empty_run_dir()          # 空目录放行
                common.write_new_json(common.OUT_DIR / "a.json", {"x": 1})
                with self.assertRaises(FileExistsError):
                    common.require_empty_run_dir()
                common.require_empty_run_dir(extra_allowed=("a.json",))   # 已批准的续跑文件放行
            finally:
                common.OUT_DIR = original

    def test_write_new_helpers_are_exclusive(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "x.json"
            common.write_new_json(path, {"a": 1})
            with self.assertRaises(FileExistsError):
                common.write_new_json(path, {"a": 2})
            pickle_path = Path(tmp) / "y.pkl"
            frame = pd.DataFrame({"c": [1, 2]})
            common.write_new_pickle(pickle_path, frame)
            with self.assertRaises(FileExistsError):
                common.write_new_pickle(pickle_path, frame)
            blob = Path(tmp) / "z.bin"
            common.write_new_bytes(blob, b"1")
            with self.assertRaises(FileExistsError):
                common.write_new_bytes(blob, b"2")
            self.assertEqual(pd.read_pickle(pickle_path)["c"].tolist(), [1, 2])   # 首次写入没被截断
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), {"a": 1})


class AsOfSemantics(unittest.TestCase):
    """本线最容易出错的地方：一切「截至此刻」的量都必须只看过去，且同刻算还不知道。"""

    def test_cumulative_excludes_same_instant_events(self) -> None:
        frame = _tiny_history()
        engine = build_data.AsOfCounts(frame, key="charger_id", stamps="attempted_at", value="y_tech")
        query = build_data.query_frame(frame, "charger_id")
        counts, sums = engine.cumulative(query, "charger_id", "y_tech")
        # 前两条同刻：互相都看不到对方；第 3 条看到 2 次尝试、其中 1 次失败
        self.assertEqual(list(counts), [0.0, 0.0, 2.0, 3.0, 4.0, 5.0])
        self.assertEqual(list(sums), [0.0, 0.0, 1.0, 1.0, 1.0, 1.0])

    def test_window_is_half_open_on_both_ends(self) -> None:
        frame = _tiny_history()
        engine = build_data.AsOfCounts(frame, key="charger_id", stamps="attempted_at", value="y_tech")
        query = build_data.query_frame(frame, "charger_id")
        counts, sums = engine.window(query, "charger_id", 30.0, "y_tech")
        # day40 的两行看不到 day1 的事件（30 天下沿已经越过）
        self.assertEqual(list(counts), [0.0, 0.0, 2.0, 3.0, 0.0, 1.0])
        self.assertEqual(list(sums), [0.0, 0.0, 1.0, 1.0, 0.0, 0.0])

    def test_unknown_entity_reads_as_zero_history(self) -> None:
        frame = _tiny_history()
        engine = build_data.AsOfCounts(frame, key="charger_id", stamps="attempted_at", value="y_tech")
        query = pd.DataFrame({"__query_time": pd.to_datetime(["2026-02-01"]), "charger_id": ["ZZZ"]})
        counts, sums = engine.cumulative(query, "charger_id", "y_tech")
        self.assertEqual(list(counts), [0.0])
        self.assertEqual(list(sums), [0.0])

    def test_strict_last_never_returns_the_same_instant_row(self) -> None:
        """同刻泄漏回归：模拟器在 attempted_at 当刻翻桩态，含同刻关联会造出 100% 假特征。"""
        frame = pd.DataFrame({"charger_id": ["A", "A", "A"],
                              "attempted_at": pd.to_datetime(["2026-01-01 00:00:00",
                                                               "2026-01-01 00:05:00",
                                                               "2026-01-01 00:05:00"]),
                              "y_tech": [0, 1, 0]})
        got = build_data.strict_last(frame, "attempted_at", "charger_id", frame, "attempted_at",
                                     ["y_tech"])
        self.assertTrue(np.isnan(got["y_tech"].iloc[0]))          # 第一条没有过去
        self.assertEqual(float(got["y_tech"].iloc[1]), 0.0)       # 看到 00:00 那条
        self.assertEqual(float(got["y_tech"].iloc[2]), 0.0)       # 同刻那条按「还不知道」处理
        self.assertEqual(pd.Timestamp(got["__last_time"].iloc[1]), pd.Timestamp("2026-01-01 00:00:00"))
        self.assertLess(pd.Timestamp(got["__last_time"].iloc[2]), pd.Timestamp("2026-01-01 00:05:00"))

    def test_strict_last_preserves_row_order_and_index(self) -> None:
        frame = pd.DataFrame({"charger_id": ["B", "A", "B"],
                              "attempted_at": pd.to_datetime(["2026-01-02", "2026-01-03",
                                                               "2026-01-09"]),
                              "state": [7, 8, 9]}).set_index(pd.Index([101, 102, 103]))
        got = build_data.strict_last(frame, "attempted_at", "charger_id", frame, "attempted_at",
                                     ["state"])
        self.assertEqual(list(got.index), [101, 102, 103])        # 行序与原表一致（断言行对齐靠它）
        self.assertTrue(np.isnan(got["state"].iloc[0]))
        self.assertTrue(np.isnan(got["state"].iloc[1]))            # A 没有更早的记录
        self.assertEqual(float(got["state"].iloc[2]), 7.0)

    def test_smoothed_shrinks_toward_causal_prior(self) -> None:
        out = build_data.smoothed(np.array([0.0, 10.0, 100.0]), np.array([0.0, 5.0, 50.0]),
                                  20.0, np.array([0.04, 0.04, 0.04]))
        self.assertAlmostEqual(out[0], 0.04, places=12)            # 无样本 → 完全收向先验
        self.assertAlmostEqual(out[1], (5 + 0.8) / 30.0, places=12)
        self.assertAlmostEqual(out[2], (50 + 0.8) / 120.0, places=12)

    def test_global_causal_rate_matches_brute_force(self) -> None:
        """off-by-one 回归：审计曾在 charger_fail_rate_prior 上抓到 1.86e-4 偏差，根因就是这个。"""
        rng = np.random.default_rng(common.SEED)
        n = 400
        stamps = pd.to_datetime(pd.Timestamp("2026-01-01")
                                + pd.to_timedelta(rng.integers(0, 500_000, n), unit="s"))
        y = rng.integers(0, 2, n).astype(float)
        got = build_data.global_causal_rate(y, pd.Series(stamps))
        array = stamps.to_numpy(dtype="datetime64[ns]").astype("int64")
        for index in rng.choice(n, size=30, replace=False):
            earlier = y[array < array[index]]
            expected = float(earlier.mean()) if len(earlier) else 0.0
            self.assertAlmostEqual(got[index], expected, places=12)

    def test_global_causal_rate_hides_same_instant_rows_and_starts_at_zero(self) -> None:
        stamps = pd.to_datetime(["2026-01-01 00:00:00", "2026-01-01 00:00:00",
                                 "2026-01-01 00:01:00"])
        self.assertEqual(list(build_data.global_causal_rate(np.array([1.0, 1.0, 0.0]), stamps)),
                         [0.0, 0.0, 1.0])


class FeatureGate(unittest.TestCase):
    @staticmethod
    def _synthetic_matrix() -> pd.DataFrame:
        rows = len(common.CATEGORICAL_FEATURES)
        frame = pd.DataFrame({name: ["x"] * rows for name in common.CATEGORICAL_FEATURES},
                             index=range(rows))
        frame["hour_local"] = np.arange(rows, dtype="int16")
        frame["charger_fail_rate_prior"] = np.linspace(0.01, 0.05, rows)
        frame["tel_last_state_code"] = np.arange(rows, dtype="float64")
        frame["constant_col"] = 3.0                                 # 样本域内唯一 → 应被剔除并留痕
        frame["flag_col"] = np.array([True, False] * rows)[:rows]
        for name in ("attempt_id", "y_tech", "outcome", "failure_reason", "charger_id",
                     "attempted_at", "tech_reason_code", "city_id", "session_id", "queue_id"):
            frame[name] = np.arange(rows, dtype="float64")
        frame["business_date"] = pd.to_datetime(["2026-01-01"] * rows)
        return frame

    def test_pick_features_drops_constants_and_bans_answer_columns(self) -> None:
        picked = build_data.pick_features(self._synthetic_matrix())
        for column in ("hour_local", "charger_fail_rate_prior", "tel_last_state_code", "flag_col"):
            self.assertIn(column, picked["numeric"])
        self.assertEqual(picked["droppedConstant"], ["constant_col"])
        self.assertNotIn("constant_col", picked["numeric"])
        self.assertEqual(picked["categorical"], list(common.CATEGORICAL_FEATURES))
        banned = (set(common.NON_FEATURE_COLUMNS) | build_data.FORBIDDEN_FEATURES
                  | build_data.IGNORED_COLUMNS)
        self.assertFalse((set(picked["numeric"]) | set(picked["categorical"])) & banned)

    def test_pick_features_refuses_a_missing_categorical(self) -> None:
        frame = self._synthetic_matrix().drop(columns=["manufacturer"])
        with self.assertRaises(AssertionError):
            build_data.pick_features(frame)

    def test_ban_lists_cover_everything_that_can_carry_the_answer(self) -> None:
        banned = set(common.NON_FEATURE_COLUMNS) | set(build_data.FORBIDDEN_FEATURES)
        for column in ("y_tech", "outcome", "failure_reason", "tech_reason_code", "charger_id",
                       "attempt_id", "attempted_at", "queue_id", "session_id", "city_id",
                       "home_city_id", "recorded_at", "expires_at"):
            self.assertIn(column, banned, f"{column} 必须被禁止入特征")

    def test_ignored_columns_are_declared_not_silently_absent(self) -> None:
        """severity / status / 成本一类列要写进 IGNORED_COLUMNS，换批次时才知道是谁判断过不用。"""
        for column in ("severity", "status", "energy_wh", "grid_cost_cents", "hour",
                       "battery_capacity_kwh"):
            self.assertIn(column, build_data.IGNORED_COLUMNS)
        self.assertFalse(build_data.IGNORED_COLUMNS & set(common.CATEGORICAL_FEATURES))


@unittest.skipUnless(HAVE_SKLEARN, "没有 sklearn，跳过 train / rolling 侧测试")
class ModelRules(unittest.TestCase):
    def test_asof_grouping_keeps_ticket_and_telemetry_columns_in_the_asof_group(self) -> None:
        """回归：token 写成复数 ``_tickets`` 时，charger_ticket_open_now 会漏进静态组，消融口径就假了。"""
        numeric = ["charger_fail_rate_prior", "charger_ticket_open_now",
                   "charger_days_since_last_ticket", "station_tickets_30d", "tel_last_state_code",
                   "charger_age_days", "user_age_days", "hour_local", "demand_multiplier"]
        asof, other = attempt_train.split_asof(numeric)
        for column in ("charger_ticket_open_now", "charger_days_since_last_ticket",
                       "station_tickets_30d", "tel_last_state_code", "charger_fail_rate_prior"):
            self.assertIn(column, asof, f"{column} 是截至此刻的统计量，不能算静态")
        for column in ("charger_age_days", "user_age_days", "hour_local", "demand_multiplier"):
            self.assertIn(column, other, f"{column} 是前置可得的静态/计划量，不该进 as-of 组")
        self.assertEqual(sorted(asof + other), sorted(numeric))

    def test_candidate_sets_partition_and_only_asof_group_drops_categoricals(self) -> None:
        numeric = ["charger_fail_rate_prior", "charger_ticket_open_now", "hour_local", "user_age_days"]
        categorical = ["manufacturer"]
        sets = attempt_train.candidate_sets(numeric, categorical)
        self.assertEqual(sets["full"], (numeric, categorical))
        self.assertEqual(sets["asOfOnly"][1], [])
        self.assertFalse(set(sets["asOfOnly"][0]) & set(sets["staticOnly"][0]))
        self.assertEqual(len(sets["asOfOnly"][0]) + len(sets["staticOnly"][0]), len(numeric))

    def test_design_encodes_booleans_and_missing_categories(self) -> None:
        frame = pd.DataFrame({"a": pd.array([True, False], dtype="boolean"), "b": [1.5, np.nan],
                              "cat": ["X", None]})
        matrix, mask = attempt_train.design(frame, ["a", "b"], ["cat"])
        self.assertEqual(mask.tolist(), [False, False, True])
        self.assertEqual(matrix["a"].tolist(), [1.0, 0.0])
        self.assertTrue(pd.api.types.is_numeric_dtype(matrix["a"]))
        self.assertEqual(list(matrix["cat"].astype(str)), ["X", "MISSING"])
        self.assertTrue(np.isnan(matrix["b"].iloc[1]))              # NaN 交给 HGBT 原生处理
        with self.assertRaises(KeyError):
            attempt_train.design(frame, ["nope"], ["cat"])

    def test_pick_threshold_reports_the_honest_fallback(self) -> None:
        rng = np.random.default_rng(7)
        y = np.r_[np.ones(40), np.zeros(960)].astype(int)
        noise = np.clip(rng.normal(0.03, 0.02, 1000), 0.0, 1.0)    # 分数与标签几乎无关 → 达不到目标
        point = attempt_train.pick_threshold(noise, y)
        self.assertFalse(point["targetMet"])
        self.assertIn("退回", point["rule"])
        good = np.where(y == 1, 0.9, 0.01)                         # 可分 → 达标
        met = attempt_train.pick_threshold(good, y)
        self.assertTrue(met["targetMet"])
        self.assertGreaterEqual(met["precision"], attempt_train.PRECISION_TARGET)
        self.assertGreater(met["alertRate"], 0.0)
        self.assertGreater(met["recall"], 0.0)

    def test_pick_threshold_refuses_a_run_with_too_few_alerts(self) -> None:
        # 只有 40 行：最低档也只圈出 20 条告警，达不到 MIN_FLAGGED，宁可报错也不给假阈值
        prob = np.linspace(0.0, 1.0, 40)
        y = (prob > 0.9).astype(int)
        self.assertLess(int((prob >= np.quantile(prob, 0.5)).sum()), attempt_train.MIN_FLAGGED)
        with self.assertRaises(RuntimeError):
            attempt_train.pick_threshold(prob, y)

    def test_baseline_blend_falls_back_to_global_rate_for_unknown_keys(self) -> None:
        train = pd.DataFrame({"charger_id": ["A"] * 40 + ["B"] * 40,
                              "charger_model": ["M1"] * 40 + ["M2"] * 40,
                              "station_id": ["S1"] * 40 + ["S2"] * 40,
                              "y_tech": [1] * 8 + [0] * 72})
        baselines = attempt_train.fit_baselines(train)
        query = pd.DataFrame({"charger_id": ["A", "B", "ZZZ"], "charger_model": ["M1", "M2", "MX"],
                              "station_id": ["S1", "S2", "SZ"]})
        scores = attempt_train.predict_baseline(baselines, query)
        global_rate = float(train["y_tech"].mean())
        self.assertEqual(len(scores), 3)
        self.assertAlmostEqual(float(scores[2]), global_rate, places=12)   # 三级全缺 → 回落全局率
        self.assertGreater(float(scores[0]), float(scores[1]))             # 坏桩 A 高于零失败的 B
        self.assertGreater(float(scores[0]), global_rate)                  # 收缩后仍高于全局率

    def test_baseline_blend_survives_an_unordered_query(self) -> None:
        train = pd.DataFrame({"charger_id": ["A", "A", "B"] * 20,
                              "charger_model": ["M1", "M2", "M1"] * 20,
                              "station_id": ["S1", "S2", "S2"] * 20,
                              "y_tech": [1, 0, 0] * 20})
        baselines = attempt_train.fit_baselines(train)
        frame = pd.DataFrame({"charger_id": ["B", "A", "A"], "charger_model": ["M1", "M1", "M2"],
                              "station_id": ["S2", "S1", "S1"]})
        forward = attempt_train.predict_baseline(baselines, frame)
        reversed_scores = attempt_train.predict_baseline(
            baselines, frame.iloc[::-1].reset_index(drop=True))
        self.assertEqual([round(float(v), 12) for v in forward],
                         [round(float(v), 12) for v in reversed_scores][::-1])


@unittest.skipUnless(HAVE_SKLEARN, "没有 sklearn，跳过 rolling 侧测试")
class FoldPlan(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        from data_analysis.ml.attempt import rolling

        cls.rolling = rolling

    def test_folds_tile_the_window_without_overlap(self) -> None:
        bounds = _bounds()
        folds = self.rolling.fold_plan(bounds, 10)
        self.assertEqual(len(folds), 10)
        self.assertEqual(folds[0]["fitEnd"],
                         bounds["startInclusive"] + pd.Timedelta(days=self.rolling.WARMUP_DAYS))
        self.assertEqual(folds[-1]["evalEnd"], bounds["testEndExclusive"])   # 十轮正好铺满 178 天
        for previous, current in zip(folds, folds[1:]):
            self.assertEqual(current["fitEnd"], previous["evalEnd"])         # 评测窗互不重叠
            self.assertEqual(current["valStart"],
                             current["fitEnd"] - pd.Timedelta(days=self.rolling.VALIDATION_DAYS))
            self.assertLess(current["valStart"], current["fitEnd"])          # 验证段在拟合窗尾部

    def test_refuses_more_rounds_than_the_window_holds(self) -> None:
        with self.assertRaises(ValueError) as caught:
            self.rolling.fold_plan(_bounds(), 12)
        self.assertIn("最多支持", str(caught.exception))

    def test_refuses_nonsensical_round_counts(self) -> None:
        with self.assertRaises(ValueError):
            self.rolling.fold_plan(_bounds(), 0)

    def test_sign_test_p_known_values(self) -> None:
        self.assertAlmostEqual(self.rolling.sign_test_p(10, 10), 2.0 / 1024.0, places=9)
        self.assertAlmostEqual(self.rolling.sign_test_p(5, 10), 1.0, places=9)      # 完全不偏 → 概率 1
        self.assertAlmostEqual(self.rolling.sign_test_p(0, 4), 2.0 / 16.0, places=9)
        self.assertTrue(np.isnan(self.rolling.sign_test_p(0, 0)))

    def test_fold_cuts_leave_no_overlap_between_fit_and_eval(self) -> None:
        """评测段必须完全晚于拟合段——泄漏最常见的形态就是差一天的边界重叠。"""
        bounds = _bounds()
        fold = self.rolling.fold_plan(bounds, 10)[0]
        stamps = pd.to_datetime([bounds["startInclusive"], fold["valStart"] - pd.Timedelta(hours=1),
                                 fold["fitEnd"] - pd.Timedelta(hours=1), fold["fitEnd"],
                                 fold["evalEnd"] - pd.Timedelta(hours=1)])
        frame = pd.DataFrame({"attempted_at": stamps})
        train = frame[frame["attempted_at"] < fold["valStart"]]
        val = frame[(frame["attempted_at"] >= fold["valStart"]) & (frame["attempted_at"] < fold["fitEnd"])]
        ev = frame[(frame["attempted_at"] >= fold["fitEnd"]) & (frame["attempted_at"] < fold["evalEnd"])]
        self.assertEqual(len(train) + len(val) + len(ev), len(frame))   # 三段无缝、互斥铺满
        self.assertLess(train["attempted_at"].max(), ev["attempted_at"].min())
        self.assertLess(val["attempted_at"].max(), ev["attempted_at"].min())


@unittest.skipUnless(HAVE_ARTIFACTS, "本地产物未生成（outputs/ 在 .gitignore 内），跳过")
class Artifacts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        def load(path: Path) -> dict:
            with open(path, encoding="utf-8") as handle:
                return json.load(handle)

        cls.summary = load(common.BUILD_SUMMARY_PATH)
        cls.train = load(common.TRAIN_METRICS_PATH)
        cls.report = load(common.TEST_REPORT_PATH)
        cls.frame = pd.read_pickle(common.MATRIX_PATH)

    def test_exclusion_ledger_adds_up(self) -> None:
        scope = self.summary["scope"]
        ledger = scope["excluded"]
        numeric = [value for value in ledger.values() if isinstance(value, int)]
        self.assertEqual(len(numeric), 4, "排除台账应有四块并且都能核对")
        self.assertEqual(sum(numeric) + scope["sampleRows"], scope["totalAttempts"])
        self.assertAlmostEqual(scope["baseRate"], scope["positives"] / scope["sampleRows"], places=4)
        self.assertEqual(scope["sampleRows"], int(len(self.frame)))
        self.assertEqual(sum(self.summary["splits"].values()), scope["sampleRows"])

    def test_matrix_is_disjoint_from_line_five_sample(self) -> None:
        """与第五线的硬切割：本线样本里没有排队关联行，且每行都已选定桩。"""
        self.assertTrue(self.frame["queue_id"].isna().all())
        self.assertTrue(self.frame["charger_id"].notna().all())
        self.assertTrue(self.frame["attempt_id"].is_unique)
        self.assertEqual(int(self.frame["outcome"].isin(["STARTED", "FAILED"]).sum()), int(len(self.frame)))
        excluded = [value for value in self.summary["scope"]["excluded"].values()
                    if isinstance(value, int)]
        self.assertEqual(sum(excluded),
                         self.summary["scope"]["totalAttempts"] - self.summary["scope"]["sampleRows"])

    def test_label_matches_outcome_and_reason_exactly(self) -> None:
        tech = set(common.TECHNICAL_FAILURE_REASONS)
        self.assertEqual(set(self.frame.loc[self.frame["y_tech"] == 1, "failure_reason"]) - tech, set())
        self.assertEqual(int((self.frame["outcome"] == "FAILED").sum()), int(self.frame["y_tech"].sum()))
        self.assertEqual(self.summary["scope"]["positives"], int(self.frame["y_tech"].sum()))
        self.assertEqual(sum(self.summary["scope"]["reasonMix"].values()), int(self.frame["y_tech"].sum()))
        self.assertFalse(set(self.frame.loc[self.frame["y_tech"] == 0, "failure_reason"].dropna()) & tech)
        self.assertEqual(sorted(float(value) for value in self.frame["tech_reason_code"].dropna().unique()),
                         [0.0, 1.0, 2.0])

    def test_split_columns_are_ordered_in_time(self) -> None:
        parts = {name: pd.to_datetime(self.frame.loc[self.frame["split"] == name, "attempted_at"])
                 for name in ("TRAIN", "VALIDATION", "TEST")}
        self.assertLess(parts["TRAIN"].max(), parts["VALIDATION"].min())
        self.assertLess(parts["VALIDATION"].max(), parts["TEST"].min())
        bounds = {key: pd.Timestamp(value) for key, value in self.summary["boundaries"].items()}
        self.assertGreaterEqual(parts["TRAIN"].min(), bounds["startInclusive"])
        self.assertLess(parts["TEST"].max(), bounds["testEndExclusive"])
        self.assertEqual(self.summary["splits"]["TRAIN"], self.train["trainRows"])
        self.assertEqual(self.summary["splits"]["VALIDATION"], self.train["validationRows"])
        self.assertEqual(self.summary["splits"]["TEST"], self.train["testRowsUntouched"])

    def test_feature_lists_match_the_matrix_and_ban_answer_columns(self) -> None:
        features = self.summary["features"]
        banned = set(common.NON_FEATURE_COLUMNS) | set(build_data.FORBIDDEN_FEATURES)
        self.assertFalse((set(features["numeric"]) | set(features["categorical"])) & banned)
        self.assertEqual(self.summary["featureCount"]["numeric"], len(features["numeric"]))
        self.assertEqual(self.summary["featureCount"]["categorical"], len(features["categorical"]))
        self.assertEqual(features["categorical"], list(common.CATEGORICAL_FEATURES))
        for column in features["numeric"] + features["categorical"]:
            self.assertIn(column, self.frame.columns)

    @unittest.skipUnless(HAVE_SKLEARN, "分组逻辑在 train.py 里，需要 sklearn")
    def test_candidate_sets_cover_the_feature_list_exactly_once(self) -> None:
        numeric = set(self.summary["features"]["numeric"])
        sets = attempt_train.candidate_sets(self.summary["features"]["numeric"],
                                           self.summary["features"]["categorical"])
        self.assertEqual(set(sets["full"][0]), numeric)
        self.assertEqual(set(sets["asOfOnly"][0]) | set(sets["staticOnly"][0]), numeric)
        self.assertFalse(set(sets["asOfOnly"][0]) & set(sets["staticOnly"][0]))
        self.assertGreater(len(sets["asOfOnly"][0]), 5)
        self.assertGreater(len(sets["staticOnly"][0]), 5)

    def test_dropped_constants_are_really_constant(self) -> None:
        self.assertGreaterEqual(len(self.summary["features"]["droppedConstant"]), 1)
        for column in self.summary["features"]["droppedConstant"]:
            self.assertLessEqual(self.frame[column].dropna().nunique(), 1,
                                 f"{column} 被当作零方差剔除，实际却有多个取值")
            self.assertIn(column, self.summary["constantWhy"])

    def test_leak_audit_is_zero_deviation_and_covers_the_risky_columns(self) -> None:
        audit = self.summary["leakAudit"]
        self.assertGreaterEqual(audit["sampled"], 100)
        for name, delta in audit["maxAbsDiff"].items():
            self.assertEqual(delta, 0.0, f"{name} 与独立暴力重算不一致")
        for column in ("charger_fail_rate_prior", "charger_ticket_open_now", "tel_last_state_code",
                       "charger_prev_attempt_failed", "tel_offline_rows_24h"):
            self.assertIn(column, audit["checked"])

    def test_telemetry_state_is_not_a_deterministic_predictor_of_failure(self) -> None:
        """同刻泄漏的产物级回归：含同刻关联时「上一条状态=AVAILABLE」对正类命中率是 100%。"""
        rates = self.summary["notes"]["telemetry"]["lastStateFailureRate"]
        self.assertGreater(len(rates), 3)
        self.assertLess(max(rates.values()), 0.10,
                        f"某个桩态的失败率高达 {max(rates.values())}，几乎肯定是同刻泄漏回来了")
        base = self.summary["scope"]["baseRate"]
        for state, rate in rates.items():
            self.assertLess(abs(rate - base), 0.05, f"{state} 的失败率 {rate} 偏离基础率过多")

    def test_asof_counts_are_non_negative_and_restart_at_first_attempt(self) -> None:
        self.assertGreaterEqual(self.frame["charger_n_prior"].min(), 0)
        first = self.frame.groupby("charger_id")["attempted_at"].transform("min")
        opening = (self.frame["attempted_at"] == first).to_numpy()
        self.assertTrue((self.frame["charger_n_prior"].to_numpy()[opening] == 0).all())
        self.assertGreater(self.frame["charger_n_prior"].to_numpy()[~opening].min(), 0)
        self.assertTrue((self.frame["charger_prev_attempt_failed"].isna().to_numpy()[opening]).all())

    def test_shrinkage_target_is_the_causal_rate_not_the_table_rate(self) -> None:
        """零历史行的比率必须等于「截至该行之前的全量经验率」，不是全表标签率、也不是常数 0.5。"""
        frame = self.frame.sort_values(["attempted_at", "attempt_id"], kind="stable").reset_index(drop=True)
        priors = build_data.global_causal_rate(frame["y_tech"].to_numpy(dtype=float),
                                              frame["attempted_at"])
        zero = (frame["charger_n_prior"] == 0).to_numpy()
        np.testing.assert_allclose(frame["charger_fail_rate_prior"].to_numpy()[zero], priors[zero],
                                   rtol=0.0, atol=1e-12)
        table_rate = float(frame["y_tech"].mean())
        self.assertGreater(abs(float(np.mean(priors)) - table_rate), 0.0, "先验不该退化成一个常数")
        self.assertLess(priors[-1], table_rate + 0.05)

    def test_train_selected_on_validation_only(self) -> None:
        self.assertIn(self.train["chosenSet"], self.train["candidateValidation"])
        self.assertIn("VALIDATION ROC AUC", self.train["selectionRule"])
        self.assertEqual(self.train["testRowsUntouched"], int((self.frame["split"] == "TEST").sum()))
        self.assertEqual(self.train["publishedBatchId"], common.EXPECTED_PUBLISHED_BATCH_ID)
        self.assertEqual(self.train["matrixSha256"], self.summary["matrixSha256"])
        self.assertEqual(self.train["matrixSha256"], common.sha256_file(common.MATRIX_PATH))
        self.assertEqual(self.train["bundleSha256"], common.sha256_file(common.BUNDLE_PATH))
        for block in self.train["candidateValidation"].values():
            self.assertGreater(block["auc"], 0.5)
        self.assertEqual(self.train["operatingPoint"]["targetMet"],
                         self.report["risk"]["frozenOperatingPoint"]["targetMet"])
        self.assertEqual(self.train["operatingPoint"]["threshold"],
                         self.report["risk"]["frozenOperatingPoint"]["threshold"])
        self.assertGreater(self.train["baselineValidation"]["asOfChargerPriorOnly"]["auc"], 0.5)

    def test_baselines_are_weaker_than_the_shipped_model_on_test(self) -> None:
        risk = self.report["risk"]
        self.assertGreater(risk["auc"], risk["aucLookupBaseline"])
        self.assertGreater(risk["auc"], risk["aucAsOfPrior"])
        self.assertGreater(risk["prAuc"], risk["prAucLookupBaseline"])
        self.assertGreater(risk["liftAt5pct"], 1.0)
        self.assertEqual(self.report["valAblation"][self.report["chosenSet"]],
                         self.train["candidateValidation"][self.train["chosenSet"]]["auc"])

    def test_report_numbers_are_internally_consistent(self) -> None:
        risk = self.report["risk"]
        self.assertEqual(self.report["testRows"], int((self.frame["split"] == "TEST").sum()))
        point = risk["frozenOperatingPoint"]
        self.assertGreater(point["alertRate"], 0.0)
        self.assertIn("rule", point)
        self.assertAlmostEqual(risk["aucGainVsLookupPct"],
                               100.0 * (risk["auc"] / risk["aucLookupBaseline"] - 1.0), places=0)
        self.assertEqual(self.report["invariants"]["testStartsAtValidationEnd"], True)
        self.assertEqual(self.report["invariants"]["testEndsBeforeWindowEnd"], True)
        self.assertEqual(self.report["invariants"]["attemptIdUniqueInTest"], True)
        self.assertEqual(self.report["invariants"]["scoreRangeLegal"], True)
        self.assertLessEqual(self.report["invariants"]["testRowsWithUnseenCharger"], 0)
        shares = [block["share"] for block in self.report["reasons"]["perReason"].values()]
        self.assertAlmostEqual(sum(shares), 1.0, places=3)
        self.assertEqual(sum(block["count"] for block in self.report["reasons"]["perReason"].values()),
                         self.report["reasons"]["positives"])
        self.assertEqual(self.report["leakAudit"]["maxAbsDiff"], self.summary["leakAudit"]["maxAbsDiff"])
        for row in self.report["calibration"]:
            self.assertGreater(row["n"], 0)
        self.assertEqual(self.report["publishedBatchId"], common.EXPECTED_PUBLISHED_BATCH_ID)

    def test_reason_conclusion_matches_the_measured_separation(self) -> None:
        """只有「占比接近均匀 + 类内不可分」同时成立，才允许写「不发原因分类器」。"""
        reasons = self.report["reasons"]
        if "只发二分类风险分" in reasons["conclusion"]:
            self.assertLess(reasons["maxShareDeviationFromUniform"], 0.06)
            self.assertLess(reasons["aucSpreadWithinPositives"], 0.05)
        else:
            self.assertTrue(reasons["maxShareDeviationFromUniform"] >= 0.06
                            or reasons["aucSpreadWithinPositives"] >= 0.05)

    def test_every_published_number_carries_the_disclaimer(self) -> None:
        for payload in (self.summary, self.train, self.report):
            self.assertEqual(payload["disclaimer"], common.simulated_note())
        text = common.TEST_REPORT_MD.read_text(encoding="utf-8")
        self.assertIn("不代表真实运营数据表现", text)
        self.assertIn(common.EXPECTED_PUBLISHED_BATCH_ID, text)
        # 建表侧留下的口径说明也要能查到，否则换批次的人不知道哪些量是"严格早于"
        notes = json.dumps(self.summary["notes"], ensure_ascii=False)
        self.assertIn("严格早于", notes)
        self.assertIn("只看过去", notes)


@unittest.skipUnless(HAVE_ARTIFACTS and HAVE_SKLEARN, "需要先跑 rolling.py")
class RollingArtifacts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not common.ROLLING_SUMMARY_PATH.exists():
            raise unittest.SkipTest("滚动研究尚未运行")
        with open(common.ROLLING_SUMMARY_PATH, encoding="utf-8") as handle:
            cls.summary = json.load(handle)
        cls.rounds = sorted(cls.summary["roundsDetail"], key=lambda row: row["round"])

    def test_round_files_match_the_summary(self) -> None:
        self.assertEqual(len(self.rounds), self.summary["rounds"])
        for row in self.rounds:
            path = common.ROLLING_ROUNDS_DIR / f"round_{row['round']:02d}.json"
            self.assertTrue(path.exists(), f"缺少 {path}")
            with open(path, encoding="utf-8") as handle:
                self.assertEqual(json.load(handle)["auc"], row["auc"])

    def test_round_windows_tile_strictly_forward(self) -> None:
        for row in self.rounds:
            self.assertGreater(pd.Timestamp(row["evalEnd"]), pd.Timestamp(row["fitEnd"]))
            self.assertGreaterEqual(row["evalPositives"], 30, "评测窗正样本太少，AUC 不可信")
            self.assertGreaterEqual(row["evalRows"], 500)
            self.assertGreaterEqual(row["validationRows"], 500)
            self.assertIn(row["chosenSet"], row["valAucBySet"])
        for previous, current in zip(self.rounds, self.rounds[1:]):
            self.assertEqual(pd.Timestamp(current["fitEnd"]), pd.Timestamp(previous["evalEnd"]))
            self.assertGreater(pd.Timestamp(current["fitEnd"]), pd.Timestamp(previous["fitEnd"]))

    def test_aggregates_match_the_per_round_detail(self) -> None:
        for name in ("auc", "aucLookupBaseline", "aucAsOfPriorBaseline", "prAuc"):
            values = np.array([row[name] for row in self.rounds], dtype=float)
            block = self.summary["metrics"][name]
            self.assertAlmostEqual(block["mean"], float(values.mean()), places=3)
            self.assertAlmostEqual(block["min"], float(values.min()), places=4)
            self.assertAlmostEqual(block["max"], float(values.max()), places=4)
            self.assertEqual(block["first"], values[0])
            self.assertEqual(block["last"], values[-1])
        self.assertEqual(sum(self.summary["chosenSetCounts"].values()), self.summary["rounds"])
        self.assertEqual(self.summary["trend"]["gainPositiveRounds"],
                         sum(1 for row in self.rounds if row["aucGainVsLookupPct"] > 0))

    def test_asof_history_grew_but_the_pile_level_prior_did_not_become_useful(self) -> None:
        """这条研究要回答的问题：历史变多以后，桩级经验率是否变强。当前批次的结论是「几乎没有」。"""
        trend = self.summary["trend"]
        self.assertLess(trend["chargerHistoryMedianPriorNFirst"],
                        trend["chargerHistoryMedianPriorNLast"])
        drift = trend["priorAucLastThreeMean"] - trend["priorAucFirstThreeMean"]
        self.assertLess(abs(drift), 0.03, f"桩级先验随历史漂移 {drift:+.4f}，需要重新解释")
        metrics = self.summary["metrics"]
        self.assertGreater(metrics["auc"]["mean"], metrics["aucAsOfPriorBaseline"]["mean"])
        self.assertGreater(metrics["aucGainVsLookupPct"]["mean"], 0.0)

    def test_choosing_by_validation_auc_never_picked_the_asof_only_group(self) -> None:
        counts = self.summary["chosenSetCounts"]
        self.assertEqual(counts.get("asOfOnly", 0), 0, counts)
        self.assertEqual(sum(counts.values()), self.summary["rounds"])
        for row in self.rounds:
            best = max(row["valAucBySet"], key=lambda key: row["valAucBySet"][key])
            self.assertEqual(row["chosenSet"], best, f"第 {row['round']} 轮选组与验证段 AUC 不一致")

    def test_threshold_rule_is_never_loosened_to_reach_the_target(self) -> None:
        """阈值口径不许被改动去凑达标：未达标轮次必须带着「退回」这句话。"""
        for row in self.rounds:
            if row["thresholdTargetMet"]:
                self.assertNotIn("退回", row["thresholdRule"])
            else:
                self.assertIn("退回", row["thresholdRule"])
                self.assertLess(row["evalPrecision"] or 0.0, self.summary["precisionTarget"])
        self.assertLessEqual(self.summary["trend"]["thresholdTargetMetRounds"], self.summary["rounds"])

    def test_caveat_and_disclaimer_are_present(self) -> None:
        self.assertIn("不是盲测指标", self.summary["caveat"])
        self.assertEqual(self.summary["disclaimer"], common.simulated_note())
        text = common.ROLLING_SUMMARY_MD.read_text(encoding="utf-8")
        self.assertIn("不代表真实运营数据表现", text)
        self.assertIn("逐轮明细", text)


if __name__ == "__main__":
    unittest.main()
