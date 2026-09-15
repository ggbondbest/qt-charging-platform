"""变压器线单元测试（house 风格 unittest.TestCase，纯合成小数据，不读 388 万行遥测）。

跑法（仓库根目录，与 CI 里 ``data_analysis.ml.tests.test_delivery_safety`` 同一模式）：

    python -m unittest data_analysis.ml.transformer.tests.test_transformer -v

钉住的东西分三类：

* **分配不变式**：``0 ≤ alloc ≤ demand``、``Σalloc ≤ cap``（逐策略 × 逐需求向量 × 逐容量全对）；
  水填充/比例公平的解析解；FCFS 与优先级贪心的出队次序。
* **指标语义**（本 PR 审过、改过的两处，回归钉子写在这里）：Gini 在**全体参与者**上算——
  赢家通吃必须给出显著不均衡（旧实现只在欠供>0 的子集上算，恰好在那一刻归零）；
  ``longestWaitStarved`` 是配对敏感的，能区分两个加权和相等的策略。
* **帧装配**：``_shift_right`` 的 k=0 必须返回副本（np.save 对 views 去重会静默压掉列）；
  ``dropped`` 覆盖 head/tail/cross 三类且 usable 样本零 NaN；非特征列清单与帧列一致。
"""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

try:
    import numpy as np
    import pandas as pd

    from data_analysis.ml.transformer import allocate, common, features, stress

    STACK = all(importlib.util.find_spec(name) is not None for name in ("numpy", "pandas", "sklearn"))
except ImportError:  # 生成器 job：裸 python，无数据科学栈
    STACK = False


# ------------------------------------------------------------------ 分配不变式与解析解
# 刻意用裸 list：模块顶层不能依赖 numpy（CI 生成器 job 是裸 python，导入期就 NameError 的话
# skipUnless 根本没机会生效）。
DEMAND_CASES = [
    [10.0, 200.0, 5.0],
    [40.0, 40.0, 40.0],
    [0.0, 120.0, 30.0],
    [500.0],
    [0.0, 0.0, 0.0],
    [70.0, 10.0, 0.0, 25.0],
]
CAPS = [0.0, 5.0, 60.0, 100.0, 205.0]


def _wait(n: int):
    base = np.array([5.0, 1.0, 3.0, 8.0, 2.0])
    return np.tile(base, n // len(base) + 1)[:n]


@unittest.skipUnless(STACK, "numpy/pandas/scikit-learn not installed")
class AllocationInvariants(unittest.TestCase):
    def test_every_policy_respects_box_and_cap(self):
        for policy in allocate.POLICIES:
            for case in DEMAND_CASES:
                demands = np.asarray(case, dtype=float)
                for cap in CAPS:
                    with self.subTest(policy=policy, demands=case, cap=cap):
                        alloc = allocate.allocate(policy, demands, cap, wait_min=_wait(len(demands)))
                        self.assertTrue(np.all(alloc >= -1e-9), (policy, case, cap, alloc))
                        self.assertTrue(np.all(alloc <= demands + 1e-9), (policy, case, cap, alloc))
                        self.assertLessEqual(float(alloc.sum()), cap + 1e-9)

    def test_maxmin_waterfill_hand_case(self):
        alloc = allocate.allocate_maxmin(np.array([10.0, 200.0, 5.0]), 100.0)
        self.assertTrue(np.allclose(alloc, [10.0, 85.0, 5.0]))

    def test_maxmin_saturates_small_demands_first(self):
        alloc = allocate.allocate_maxmin(np.array([5.0, 5.0, 300.0]), 60.0)
        self.assertTrue(np.allclose(alloc, [5.0, 5.0, 50.0]))

    def test_proportional_equal_demands_are_equal(self):
        alloc = allocate.allocate_proportional(np.array([40.0, 40.0, 40.0]), 60.0)
        self.assertTrue(np.allclose(alloc, [20.0, 20.0, 20.0]))

    def test_proportional_zero_demand_returns_zeros(self):
        alloc = allocate.allocate_proportional(np.array([0.0, 0.0]), 50.0)
        self.assertTrue(np.allclose(alloc, [0.0, 0.0]))

    def test_greedy_fcfs_orders_by_index(self):
        alloc = allocate.allocate_greedy(np.array([30.0, 30.0, 30.0]), 40.0)
        self.assertEqual(list(alloc), [30.0, 10.0, 0.0])

    def test_priority_uses_wait_order(self):
        alloc = allocate.allocate_priority(np.array([30.0, 30.0]), 40.0, priority=np.array([0.0, 10.0]))
        self.assertEqual(list(alloc), [10.0, 30.0])

    def test_priority_wait_greedy_equals_reverse_index_when_wait_is_reverse(self):
        alloc = allocate.allocate("priority_wait", np.array([30.0, 30.0, 30.0]), 40.0,
                                  wait_min=np.array([0.0, 1.0, 2.0]))
        self.assertEqual(list(alloc), [0.0, 10.0, 30.0])

    def test_priority_ties_break_by_index(self):
        alloc = allocate.allocate_priority(np.array([30.0, 30.0]), 30.0, priority=np.array([7.0, 7.0]))
        self.assertEqual(list(alloc), [30.0, 0.0])

    def test_unknown_policy_rejected(self):
        with self.assertRaises(ValueError):
            allocate.allocate("magic", np.array([1.0]), 1.0)

    def test_priority_wait_requires_wait(self):
        with self.assertRaises(ValueError):
            allocate.allocate("priority_wait", np.array([1.0, 2.0]), 1.0)


@unittest.skipUnless(STACK, "numpy/pandas/scikit-learn not installed")
class PolicyMetrics(unittest.TestCase):
    def test_served_fraction_is_policy_invariant_under_binding(self):
        demands = np.array([20.0, 50.0, 80.0])           # Σ=150 > cap
        wait = np.array([3.0, 1.0, 2.0])
        fractions = {round(allocate.policy_metrics(
            demands, allocate.allocate(p, demands, 40.0, wait_min=wait), wait)["servedFraction"], 6)
            for p in allocate.POLICIES}
        self.assertEqual(len(fractions), 1, fractions)    # 交付 min(cap,Σd) 与策略无关

    def test_gini_is_zero_under_proportional(self):
        demands = np.array([30.0, 30.0, 30.0])
        metrics = allocate.policy_metrics(demands, allocate.allocate_proportional(demands, 30.0))
        self.assertEqual(metrics["unfairnessGini"], 0.0)

    def test_gini_flags_winner_take_all(self):
        """回归钉子：一桩给满、其余全饿，是**最**不均衡，不是"完全公平"。

        旧实现只在欠供>0 的子集上算 Gini——幸存者份额相等，这里会报 0.0。
        两参与者时 (0,1) 的 Gini = 0.5（该形状的理论最大值）；三参与者 (0,1,1) = 1/3。
        """
        two = allocate.policy_metrics(np.array([50.0, 50.0]), allocate.allocate_greedy(
            np.array([50.0, 50.0]), 50.0))
        self.assertAlmostEqual(two["unfairnessGini"], 0.5)
        demands = np.array([100.0, 100.0, 100.0])
        metrics = allocate.policy_metrics(demands, allocate.allocate_greedy(demands, 100.0))  # [100,0,0]
        self.assertGreater(metrics["unfairnessGini"], 0.3, metrics)
        # 完全公平的反面对照：份额全等 → 0
        fair = allocate.policy_metrics(demands, allocate.allocate_proportional(demands, 100.0))
        self.assertEqual(fair["unfairnessGini"], 0.0)

    def test_gini_ignores_idle_chargers(self):
        """没需求的桩（share 无定义）不参与不均衡度：单桩在充且被喂满 → 0。"""
        metrics = allocate.policy_metrics(np.array([10.0, 0.0, 0.0]), np.array([10.0, 0.0, 0.0]))
        self.assertEqual(metrics["unfairnessGini"], 0.0)

    def test_wait_weighted_only_when_wait_given(self):
        demands = np.array([50.0, 50.0])
        alloc = allocate.allocate_greedy(demands, 30.0)
        self.assertNotIn("waitWeightedShortfall", allocate.policy_metrics(demands, alloc))
        self.assertIn("waitWeightedShortfall",
                      allocate.policy_metrics(demands, alloc, wait_min=np.array([1.0, 5.0])))

    def test_longest_wait_starved_sees_what_aggregates_cannot(self):
        """两个策略**交付份额与欠供总量全等**（不变式），逐 tick 分配却相反——这一列要能分辨。

        桩 0 等 1 分钟、桩 1 等 9 分钟，需求各 50、容量 50：
        FCFS 喂先来的桩 0 → 等最久的桩 1 挨饿（=1）；priority_wait 喂桩 1 → 没人挨饿（=0）。
        （真实批里更狠：``waitWeightedShortfall`` 在策略间也相等，只有这一列动。）
        """
        demands = np.array([50.0, 50.0])
        wait = np.array([1.0, 9.0])
        fcfs_alloc = allocate.allocate("greedy_fcfs", demands, 50.0, wait_min=wait)
        prio_alloc = allocate.allocate("priority_wait", demands, 50.0, wait_min=wait)
        self.assertFalse(np.allclose(fcfs_alloc, prio_alloc))
        fcfs = allocate.policy_metrics(demands, fcfs_alloc, wait)
        prio = allocate.policy_metrics(demands, prio_alloc, wait)
        self.assertEqual(fcfs["servedFraction"], prio["servedFraction"])
        self.assertEqual(fcfs["shortfallKw"], prio["shortfallKw"])
        self.assertEqual(fcfs["longestWaitStarved"], 1.0)
        self.assertEqual(prio["longestWaitStarved"], 0.0)

    def test_longest_wait_starved_zero_for_single_participant(self):
        demands = np.array([50.0, 0.0])
        metrics = allocate.policy_metrics(demands, allocate.allocate_greedy(demands, 10.0),
                                          wait_min=np.array([3.0, 1.0]))
        self.assertEqual(metrics["longestWaitStarved"], 0.0)

    def test_greedy_and_priority_genuinely_differ_on_a_real_binding_shape(self):
        """greedy 与 priority 在这份数据上**不**等价（早期版本把聚合相等误读成重合）。"""
        demands = np.array([60.0, 20.0, 40.0])
        wait = np.array([1.0, 30.0, 15.0])
        fcfs = allocate.allocate("greedy_fcfs", demands, 70.0, wait_min=wait)
        prio = allocate.allocate("priority_wait", demands, 70.0, wait_min=wait)
        self.assertFalse(np.allclose(fcfs, prio))
        self.assertTrue(np.allclose(fcfs, [60.0, 10.0, 0.0]))
        self.assertTrue(np.allclose(prio, [10.0, 20.0, 40.0]))   # 等最久的先给满，剩下的按等待降序


# ------------------------------------------------------------------ 宽矩阵位移
@unittest.skipUnless(STACK, "numpy/pandas/scikit-learn not installed")
class ShiftHelpers(unittest.TestCase):
    def test_shift_right_at_zero_returns_a_copy(self):
        """k=0 必须是副本：返回入参本身会让 load 与 load_lag1 共用 buffer，
        ``np.save`` 对 views 去重后帧被静默压缩，改一列串改四列。"""
        matrix = np.arange(12.0).reshape(3, 4)
        right = features._shift_right(matrix, 0)
        self.assertIsNot(right, matrix)
        self.assertFalse(np.shares_memory(right, matrix))
        self.assertTrue(np.allclose(right, matrix))

    def test_shift_left_and_right_place_nan_correctly(self):
        matrix = np.arange(12.0).reshape(3, 4)
        left = features._shift_left(matrix, 1)
        self.assertTrue(np.isnan(left[:, -1]).all())
        self.assertTrue(np.allclose(left[:, :-1], matrix[:, 1:]))
        right = features._shift_right(matrix, 2)
        self.assertTrue(np.isnan(right[:, :2]).all())
        self.assertTrue(np.allclose(right[:, 2:], matrix[:, :-2]))

    def test_shift_right_does_not_share_output_columns(self):
        matrix = np.arange(12.0).reshape(3, 4)
        one = features._shift_right(matrix, 1)
        two = features._shift_right(matrix, 2)
        self.assertFalse(np.shares_memory(one, two))
        one[:, 3] = -1.0                              # 改副本不许污染源或兄弟列
        self.assertEqual(matrix[0, 3], 3.0)
        self.assertEqual(two[0, 3], 1.0)


# ------------------------------------------------------------------ 合成站×tick 帧
def _telemetry(n_ticks: int, station_b_gap_at: int | None = None) -> pd.DataFrame:
    """两站 × ``n_ticks`` 个 5 分钟 tick，功率可手算；``station_b_gap_at`` 让 B 站在某 tick 全空。"""
    start = pd.Timestamp("2025-01-01T00:00:00")
    rows = []
    for i in range(n_ticks):
        t = start + pd.Timedelta(minutes=5 * i)
        rows.append(("A", "a1", t, "CHARGING", f"s1_{i}", 10.0 * (i + 1)))
        if i >= 2:
            rows.append(("A", "a2", t, "CHARGING", "s2", 5.0))
        if station_b_gap_at != i:
            rows.append(("B", "b1", t, "CHARGING", "s3", 30.0))
    rows.append(("A", "a3", start, "AVAILABLE", None, 0.0))     # 不计入负荷
    tel = pd.DataFrame(rows, columns=["station_id", "charger_id", "recorded_at", "state",
                                      "session_id", "power_kw"])
    tel["recorded_at"] = pd.to_datetime(tel["recorded_at"], utc=True).dt.tz_localize(None)
    return tel


def _stations_frame() -> pd.DataFrame:
    return pd.DataFrame({"station_id": ["A", "B"], "city_id": ["C1", "C1"],
                         "site_type": ["MALL", "STREET"], "transformer_kw": [360.0, 360.0]})


def _build(tel, split_fn=None):
    """在 patch 掉 clean 表与落段函数的前提下装配帧（不碰发布数据集）。"""
    def fake_clean(name, columns=None):
        assert name == "stations", name
        frame = _stations_frame()
        return frame[columns] if columns else frame

    fallback = lambda stamps: pd.Series("TEST", index=pd.Series(stamps).index)  # noqa: E731
    with mock.patch.object(features.common, "load_clean_table", fake_clean), \
            mock.patch.object(features.common, "assign_split", split_fn or fallback):
        return features.build_tick_frame(tel)


@unittest.skipUnless(STACK, "numpy/pandas/scikit-learn not installed")
class TickFrame(unittest.TestCase):
    def test_lag_and_next(self):
        frame, _ = _build(_telemetry(6))
        station_a = frame[frame["station_id"] == "A"].sort_values("tick_ts")
        # 站 A 逐 tick 总负荷 = a1(10·(i+1)) + a2(自 i>=2 记 5)
        self.assertTrue(np.allclose(station_a["total_kw"].to_numpy(), [10, 20, 35, 45, 55, 65]))
        self.assertEqual(station_a["total_kw_next"].iloc[0], 20.0)
        self.assertTrue(np.isnan(station_a["total_kw_next"].iloc[-1]))
        # lag1 = 当 tick（决策时点已知）；lag2 = 上一 tick
        self.assertTrue(np.allclose(station_a["load_lag1"].to_numpy(), station_a["total_kw"].to_numpy()))
        self.assertEqual(station_a["load_lag2"].iloc[1], 10.0)
        self.assertTrue(np.isnan(station_a["load_lag2"].iloc[0]))

    def test_frame_shape_and_drop_reasons(self):
        """短栅格（6 tick < 滞后窗口）：每一行都因 head 被丢，tail 单独计数，跨段为 0。"""
        frame, reasons = _build(_telemetry(6))
        self.assertIn("dropped", frame.columns)
        self.assertNotIn("dropped_last", frame.columns)
        self.assertEqual(set(reasons), {"headIncompleteLag", "tailOutOfGrid", "crossSplitPurged"})
        self.assertEqual(len(frame), 12)                                  # 2 站 × 6 tick
        self.assertTrue(bool(frame["dropped"].all()))
        self.assertEqual(reasons["headIncompleteLag"], 12)
        self.assertEqual(reasons["tailOutOfGrid"], 2)
        self.assertEqual(reasons["crossSplitPurged"], 0)
        # 三类是并集，不是求和：dropped 行数 ≤ 三类计数之和
        self.assertLessEqual(int(frame["dropped"].sum()), sum(reasons.values()))

    def test_head_guard_drops_incomplete_lag_rows(self):
        """栅格头部（滞后窗口不全）必须被 dropped 覆盖——旧名 dropped_last 只覆盖 tail/cross。"""
        n = features.LAG_TAIL + 6
        frame, reasons = _build(_telemetry(n))
        head = frame["tick_index"] < features.LAG_TAIL - 1
        self.assertEqual(reasons["headIncompleteLag"], int(head.sum()))
        self.assertTrue(bool(frame.loc[head, "dropped"].all()))
        usable = frame[~frame["dropped"]]
        self.assertGreater(len(usable), 0)
        cols = features.numeric_feature_columns()
        self.assertTrue(bool(usable[cols].notna().all().all()), "usable 里仍有 NaN 特征")

    def test_tail_reason_counted(self):
        frame, reasons = _build(_telemetry(features.LAG_TAIL + 3))
        last = frame["tick_index"] == frame.groupby("station_id")["tick_index"].transform("max")
        self.assertEqual(int(last.sum()), int(frame["station_id"].nunique()))   # 每站恰好一行
        self.assertEqual(reasons["tailOutOfGrid"], int(last.sum()))
        self.assertTrue(bool(frame.loc[last, "dropped"].all()))
        self.assertTrue(bool(np.isnan(frame.loc[last, "total_kw_next"]).all()))
        # 目标缺失只发生在栅格右端：其余行的 total_kw_next 全部有值
        self.assertFalse(bool(frame.loc[~last, "total_kw_next"].isna().any()))

    def test_cross_split_rows_are_purged(self):
        """目标落到别的段 → 该行被丢（1-tick purge 的边界纪律）。

        落段函数按**时间戳值**分块（每 4 个 tick 一段、TRAIN/TEST 交替）：它必须是 stamp 的纯函数，
        因为 build_tick_frame 对当 tick 与下一 tick 分别调用它，两次传入的索引顺序不同。
        """
        def split_by_block(stamps):
            stamps = pd.Series(stamps)
            tick_no = (stamps.dt.hour * 60 + stamps.dt.minute) // 5
            return pd.Series(np.where((tick_no // 4) % 2 == 0, "TRAIN", "TEST"), index=stamps.index)

        n = features.LAG_TAIL + 6                                      # 18 tick：跨界在 3/7/11/15
        frame, reasons = _build(_telemetry(n), split_fn=split_by_block)
        self.assertEqual(reasons["crossSplitPurged"], 8)                # 2 站 × 4 个跨界 tick
        self.assertTrue(bool(frame.loc[frame["tick_index"].isin([3, 7, 11, 15]), "dropped"].all()))
        usable = frame[~frame["dropped"]]
        self.assertEqual(sorted(usable["tick_index"].unique().tolist()), [12, 13, 14, 16])
        self.assertTrue(bool(usable[features.numeric_feature_columns()].notna().all().all()))

    def test_non_feature_columns_cover_the_frame(self):
        """漂移守卫：帧的每一列要么进特征清单，要么在 NON_FEATURE_COLUMNS 里登记过。

        历史上 ``business_date`` 被登记成一个根本不存在的列，而 ``dropped_last`` 改了名却没人核对。
        """
        frame, _ = _build(_telemetry(features.LAG_TAIL + 2))
        declared = set(features.numeric_feature_columns()) | set(common.NON_FEATURE_COLUMNS)
        strays = [column for column in frame.columns if column not in declared]
        self.assertEqual(strays, [], f"未登记的非特征列：{strays}")
        for column in common.NON_FEATURE_COLUMNS:                       # 清单里的列也得真的存在
            self.assertIn(column, frame.columns, f"NON_FEATURE_COLUMNS 登记了不存在的列 {column}")

    def test_transformer_kw_missing_raises(self):
        tel = _telemetry(4)

        def fake_clean(name, columns=None):
            frame = _stations_frame().assign(transformer_kw=np.nan)
            return frame[columns] if columns else frame

        with mock.patch.object(features.common, "load_clean_table", fake_clean):
            with self.assertRaises(AssertionError):
                features.build_tick_frame(tel)

    def test_numeric_feature_columns_no_future(self):
        cols = features.numeric_feature_columns()
        self.assertNotIn("total_kw", cols)
        self.assertNotIn("total_kw_next", cols)
        self.assertIn("load_lag1", cols)
        self.assertIn("load_mean_1h", cols)
        self.assertEqual(len(cols), len(set(cols)))


@unittest.skipUnless(STACK, "numpy/pandas/scikit-learn not installed")
class DemandLong(unittest.TestCase):
    def test_wait_min_starts_at_zero_and_grows_within_session(self):
        tel = _telemetry(5)
        long = features.build_demand_long(tel)
        self.assertTrue(bool((long["demand_kw"] > 0).all()))
        self.assertTrue(bool((long["wait_min"] >= 0).all()))
        station_a2 = long[long["charger_id"] == "a2"].sort_values("tick_ts")
        self.assertEqual(float(station_a2["wait_min"].iloc[0]), 0.0)      # 会话首 tick 已等 0 分钟
        self.assertTrue(bool((np.diff(station_a2["wait_min"].to_numpy()) == 5.0).all()))

    def test_idle_ticks_excluded(self):
        """B 站在缺口 tick 上没有 CHARGING 行 → 需求长表按"在充"口径，不含空闲格。"""
        gap = 3
        long = features.build_demand_long(_telemetry(6, station_b_gap_at=gap))
        start = pd.Timestamp("2025-01-01T00:00:00")
        gap_ts = start + pd.Timedelta(minutes=5 * gap)
        self.assertEqual(len(long[(long["station_id"] == "B") & (long["tick_ts"] == gap_ts)]), 0)


@unittest.skipUnless(STACK, "numpy/pandas/scikit-learn not installed")
class StressReplay(unittest.TestCase):
    """分配腿**入口**要真跑一次：stress.py 曾漏 `import numpy`（ruff F821），只在有 binding tick 时
    才炸，而旧测试全停在 allocate 层——漏得干干净净。这里连 main() 的落盘与两份报告一起过。"""

    @staticmethod
    def _long(waits=(30.0, 10.0), demands=(60.0, 55.0)):
        rows = [{"station_id": "A", "tick_ts": pd.Timestamp("2026-01-01 00:00:00"),
                 "charger_id": f"c{i}", "session_id": f"SES-{i:08d}",
                 "demand_kw": d, "wait_min": w}
                for i, (d, w) in enumerate(zip(demands, waits))]
        return pd.DataFrame(rows)

    def test_binding_tick_yields_invariant_and_pair_diffs(self):
        block = stress.replay(self._long(), 60.0)                 # Σ=115 > 60
        self.assertEqual(block["_bindingTicks"], 1)
        self.assertEqual(block["_xpolicyServedSpreadMax"], 0.0)   # 交付总量与策略无关
        # 行序就是等待降序（SES-00000000 等 30 分钟排在前）→ 两条贪心策略逐 tick 全等
        self.assertEqual(block["_perTickAllocationDiffTicks"]["greedy_fcfs!=priority_wait"], 0)
        self.assertEqual(block["_priorityOrderEqualsFcfsOrderTicks"], 1)
        self.assertEqual(block["_ticksWithVaryingWait"], 1)
        for policy in allocate.POLICIES:
            self.assertIn("longestWaitStarvedPct", block[policy])

    def test_non_binding_branch_carries_the_same_keys(self):
        """从不 binding 的容量档也必须给出同一套键，否则下游取数要按分支分叉。"""
        block = stress.replay(self._long(), 1e6)
        self.assertEqual(block["_bindingTicks"], 0)
        self.assertEqual(block["_perTickAllocationDiffTicks"], {})
        for policy in allocate.POLICIES:
            self.assertEqual(set(block[policy]), {"bindingTicks", "servedFraction", "meanShortfallKw",
                                                 "unfairnessGini", "waitWeightedShortfall",
                                                 "longestWaitStarvedPct"})

    def test_priority_genuinely_differs_when_row_order_is_not_wait_order(self):
        block = stress.replay(self._long(waits=(10.0, 30.0)), 60.0)
        self.assertEqual(block["_priorityOrderEqualsFcfsOrderTicks"], 0)
        self.assertEqual(block["_perTickAllocationDiffTicks"]["greedy_fcfs!=priority_wait"], 1)

    def test_session_order_check_reads_the_table_instead_of_assuming(self):
        check = stress.session_order_check(self._long())
        self.assertEqual(check["sessions"], 2)
        self.assertAlmostEqual(check["spearmanSessionIdRankVsStartRank"], 1.0, places=9)
        self.assertIn("同序", check["implication"])
        swapped = self._long()
        swapped["session_id"] = ["SES-00000001", "SES-00000000"]   # id 与开始时间反序
        self.assertIn("排序键确实不同", stress.session_order_check(swapped)["implication"])

    def test_main_writes_both_reports_then_freezes_the_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            with mock.patch.object(stress.common, "STRESS_DIR", base), \
                    mock.patch.object(stress.common, "STRESS_REPORT_PATH", base / "policy_report.json"), \
                    mock.patch.object(stress.common, "STRESS_REPORT_MD", base / "policy_report.md"), \
                    mock.patch.object(stress.common, "verify_batch", lambda manifest=None: {}), \
                    mock.patch.object(stress.common, "read_source_manifest",
                                      lambda: {"pipelineRunId": "test-run"}), \
                    mock.patch.object(stress, "load_demands", self._long):
                report = stress.main()
                with self.assertRaises(FileExistsError):          # 就地重跑必须被拒
                    stress.main()
                written = json.loads((base / "policy_report.json").read_text(encoding="utf-8"))
                text = (base / "policy_report.md").read_text(encoding="utf-8")
        cap100 = str(float(common.STRESS_CAPS[0]))
        block = report["policyEquivalence"]["perCap"][cap100]
        self.assertEqual(block["bindingTicks"], 1)
        self.assertEqual(block["pairsWithZeroAllocationDifference"], ["greedy_fcfs!=priority_wait"])
        self.assertEqual(written["policyEquivalence"]["sessionOrderCheck"]["sessions"], 2)
        self.assertIn("策略名 ≠ 策略行为", text)
        self.assertIn("聚合指标相等 ≠ 策略等价", text)


if __name__ == "__main__":
    unittest.main()
