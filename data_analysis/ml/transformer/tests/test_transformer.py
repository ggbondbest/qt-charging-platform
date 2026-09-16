"""变压器线单元测试（house 风格 unittest.TestCase，纯合成小数据，不读 388 万行遥测）。

跑法（仓库根目录，与 CI 里 ``data_analysis.ml.tests.test_delivery_safety`` 同一模式）：

    python -m unittest data_analysis.ml.transformer.tests.test_transformer -v

钉住的东西分三类：

* **分配不变式**：``0 ≤ alloc ≤ demand``、``Σalloc == min(cap, Σdemand)``（上界**与**下界，逐策略 ×
  逐需求向量 × 逐容量全对；旧版只有 ``Σalloc ≤ cap``，一只漏发的分配器能整片绿灯）；
  水填充/比例公平的解析解；FCFS 与优先级贪心的出队次序；``allocate()`` 对脏输入拒算。
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

    def test_delivery_equals_min_cap_and_total_demand(self):
        """交付总量必须**正好**是 min(cap, Σdemand)——上界加下界一起钉。

        旧测试只有 ``Σalloc <= cap`` 与逐元素上下界，一只系统性漏发 1% 的分配器能整片绿灯；
        跨策略 servedFraction 相等那条也只测"彼此一致"，不测"等于该交付的量"。
        """
        for policy in allocate.POLICIES:
            for case in DEMAND_CASES:
                demands = np.asarray(case, dtype=float)
                for cap in CAPS:
                    with self.subTest(policy=policy, demands=case, cap=cap):
                        alloc = allocate.allocate(policy, demands, cap, wait_min=_wait(len(demands)))
                        self.assertAlmostEqual(float(alloc.sum()), min(cap, float(demands.sum())), delta=1e-9)

    def test_allocate_rejects_dirty_inputs(self):
        """``allocate()`` 的输入闸：负/NaN/inf 需求、负容量、长度不符都要**拒算**。

        回归钉子：负需求会击穿"0 <= alloc <= demand"——``_clip`` 把 ``[0, demand]`` 当区间，
        demand 为负时取到负值，``greedy([-5,10], cap=7)`` 曾给出 ``[-5,10]``（某桩实抽 10kW > 容量）。
        真实长表实测无负值/无 NaN，所以这是**契约**问题不是当期数据问题——契约要靠拒算守住。
        """
        wait = np.array([1.0, 2.0])
        with self.assertRaises(ValueError):
            allocate.allocate("greedy_fcfs", np.array([-5.0, 10.0]), 7.0, wait_min=wait)
        with self.assertRaises(ValueError):
            allocate.allocate("proportional", np.array([np.nan, 10.0]), 7.0, wait_min=wait)
        with self.assertRaises(ValueError):
            allocate.allocate("maxmin", np.array([np.inf, 10.0]), 7.0, wait_min=wait)
        with self.assertRaises(ValueError):
            allocate.allocate("greedy_fcfs", np.array([5.0, 10.0]), -1.0, wait_min=wait)
        with self.assertRaises(ValueError):
            allocate.allocate("greedy_fcfs", np.array([5.0, 10.0]), 7.0, wait_min=np.array([1.0]))
        with self.assertRaises(ValueError):
            allocate.allocate("priority_wait", np.array([5.0, 10.0]), 7.0, wait_min=np.array([1.0, np.nan]))
        # 二维输入也曾让"逐桩需求"变成矩阵广播，静默给出形状不同的分配
        with self.assertRaises(ValueError):
            allocate.allocate("greedy_fcfs", np.array([[5.0, 10.0]]), 7.0)

    def test_maxmin_waterfill_matches_closed_form(self):
        """与解析式 ``min(d_i, L)``（L 由 Σ min(d,L)=cap 决定）对账：独立参考实现，不看代码抄答案。"""
        rng = np.random.default_rng(11)
        for _ in range(200):
            demands = np.round(rng.uniform(0, 200, int(rng.integers(1, 9))), 1)
            cap = float(rng.uniform(0, demands.sum() * 1.2))
            lo, hi = 0.0, float(cap)
            for _ in range(200):                      # 二分水位线
                mid = (lo + hi) / 2.0
                if np.minimum(demands, mid).sum() < cap:
                    lo = mid
                else:
                    hi = mid
            want = np.minimum(demands, (lo + hi) / 2.0)
            got = allocate.allocate_maxmin(demands, cap)
            with self.subTest(demands=demands.tolist(), cap=cap):
                self.assertTrue(np.allclose(got, want, atol=1e-3), (got, want))

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
        """没需求的桩（share 无定义）不参与不均衡度——而且这条断言**有区分力**。

        旧版此处只用了一个单桩在充的例子：参与者数 = 1，Gini 对任何实现都返回 0，
        把闲置桩错误地算进去也照样通过（恒真测试）。这里改成：3 个参与者份额 (0, .5, 1)
        的正确值 = 0.4444；若把第 4 个闲置桩（份额被 where 置 0）一起算，值会变成 0.5833。
        两个数不同，才真把"参与者集合"这件事钉住。
        """
        demands = np.array([10.0, 10.0, 10.0, 0.0])
        alloc = np.array([10.0, 5.0, 0.0, 0.0])
        self.assertAlmostEqual(allocate.policy_metrics(demands, alloc)["unfairnessGini"], 0.4444, places=4)
        # 对照：单参与者（含闲置桩）无从谈不均衡
        self.assertEqual(allocate.policy_metrics(np.array([10.0, 0.0, 0.0]),
                                                 np.array([10.0, 0.0, 0.0]))["unfairnessGini"], 0.0)

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
        self.assertLess(block["_xpolicyDeliveredSpreadMaxKw"], 1e-9)   # 交付总量与策略无关（浮点意义下）
        # 行序就是等待降序（SES-00000000 等 30 分钟排在前）→ 两条贪心策略逐 tick 全等
        self.assertEqual(block["_perTickAllocationDiffTicks"]["greedy_fcfs!=priority_wait"], 0)
        self.assertEqual(block["_priorityOrderEqualsFcfsOrderTicks"], 1)
        self.assertEqual(block["_ticksWithVaryingWait"], 1)
        for policy in allocate.POLICIES:
            self.assertIn("longestWaitStarvedPct", block[policy])

    def test_spread_is_measured_below_the_rounding_floor(self):
        """不变式的测量**不能**被指标舍入限住分辨率。

        旧版拿 ``policy_metrics`` 里 round(...,6) 后的 servedFraction 求极差，于是小于 5e-7
        （150kW tick 上约 0.1W）的跨策略漏发永远测不出 0 以外的值。现改测未舍入的 alloc.sum()，
        所以这里注入一个只漏 1e-9 相对量的分配器，它也必须被抓出来。
        """
        original = allocate.allocate_proportional

        def leaky(demands, cap):
            return original(demands, cap) * (1.0 - 1e-9)

        with mock.patch.object(allocate, "allocate_proportional", leaky):
            leaked = stress.replay(self._long(), 60.0)
        self.assertGreater(leaked["_xpolicyDeliveredSpreadMaxKw"], 0.0)
        self.assertLess(leaked["_xpolicyDeliveredSpreadMaxKw"], 1e-4)   # 确实是"小于舍入地板"的量

    def test_non_binding_branch_carries_the_same_keys(self):
        """从不 binding 的容量档也必须给出同一套**顶层**键，否则下游取数要按分支分叉。

        旧版这个测试只比对每策略的子字典——而缺的键（``_xpolicyDeliveredSpread*Kw``）在顶层，
        于是测试名字承诺的东西恰好没测：非 binding 分支真的少了两个键也照样绿。
        """
        loose = stress.replay(self._long(), 1e6)
        binding = stress.replay(self._long(), 60.0)
        self.assertEqual(loose["_bindingTicks"], 0)
        self.assertEqual(loose["_perTickAllocationDiffTicks"], {})
        self.assertEqual(set(loose), set(binding))               # 顶层键集一致
        self.assertIsNone(loose["_xpolicyDeliveredSpreadMaxKw"])  # 没测≠测出 0
        for policy in allocate.POLICIES:
            self.assertEqual(set(loose[policy]), set(binding[policy]))
            self.assertEqual(set(loose[policy]), {"bindingTicks", "servedFraction", "meanShortfallKw",
                                                 "unfairnessGini", "waitWeightedShortfall",
                                                 "longestWaitStarvedPct"})

    def test_priority_genuinely_differs_when_row_order_is_not_wait_order(self):
        block = stress.replay(self._long(waits=(10.0, 30.0)), 60.0)
        self.assertEqual(block["_priorityOrderEqualsFcfsOrderTicks"], 0)
        self.assertEqual(block["_perTickAllocationDiffTicks"]["greedy_fcfs!=priority_wait"], 1)

    def _verdicts(self, diffs):
        block = {"_bindingTicks": 10, "_perTickAllocationDiffTicks": dict(diffs),
                 "_priorityOrderEqualsFcfsOrderTicks": 10, "_ticksWithVaryingWait": 10}
        per_cap = {str(cap): block for cap in common.STRESS_CAPS}
        verdicts = stress._pair_verdicts(per_cap)
        return verdicts, stress._behaviour_families(verdicts)

    def test_equivalence_statement_is_generated_from_measurement(self):
        """结论句必须跟着实测翻转——回归钉子：旧版把"greedy ≡ priority、不同 tick 数 = 0"写死在正文里。

        写死的句子在换一批数据（id 序与开始时间不同序）后会**反过来**说假话，而产物看起来一样正规。
        """
        order_check = {"implication": "（证据略）"}
        collapsed_diffs = {"greedy_fcfs!=priority_wait": 0, "greedy_fcfs!=proportional": 7,
                           "greedy_fcfs!=maxmin": 3, "proportional!=maxmin": 5,
                           "proportional!=priority_wait": 7, "maxmin!=priority_wait": 3}
        verdicts, families = self._verdicts(collapsed_diffs)
        self.assertTrue(verdicts["greedy_fcfs!=priority_wait"]["indistinguishable"])
        self.assertEqual(len(families), 3)
        text = stress._equivalence_statement(verdicts, families, order_check)
        self.assertIn("greedy_fcfs 与 priority_wait", text)
        self.assertIn("一次都没不同", text)
        # 全部分开：同一套代码必须换一句话，而不是继续宣称塌缩
        open_diffs = {k: (1 if k == "greedy_fcfs!=priority_wait" else 2) for k in collapsed_diffs}
        verdicts, families = self._verdicts(open_diffs)
        text = stress._equivalence_statement(verdicts, families, order_check)
        self.assertNotIn("一次都没不同", text)
        self.assertIn("4 种行为", text)
        self.assertEqual(len(families), 4)
        # 一个 binding tick 都没测到：既不能说相同、也不能说不同
        block = {"_bindingTicks": 0, "_perTickAllocationDiffTicks": {},
                 "_priorityOrderEqualsFcfsOrderTicks": 0, "_ticksWithVaryingWait": 0}
        verdicts = stress._pair_verdicts({str(c): block for c in common.STRESS_CAPS})
        text = stress._equivalence_statement(verdicts, stress._behaviour_families(verdicts), order_check)
        self.assertIn("没测", text)

    def test_pair_diff_ruling_is_absolute_only(self):
        """回归钉子：np.allclose 的**默认相对容差**会把真实差异判成"相同"。

        旧写法 `allclose(a, b, atol=1e-9)` 还带着 rtol=1e-5——100kW 量级的分配上，判据实际是
        |a−b| ≤ 1e-9 + 1e-3 kW，也就是**1 瓦以内的差异被算作相同**，"不同的 tick 数"于是成了
        容差的函数（换个 numpy 默认值就换个结论）。现在只准绝对项 1e-9kW（一毫瓦）。
        """
        base = np.array([100.0, 50.0])
        half_watt = np.array([100.0 + 5e-4, 50.0])          # 差 0.5 瓦：真差异，不是浮点尘埃
        self.assertTrue(stress._tick_differs(base, half_watt))
        # 旧判据（带默认 rtol）在这里回答"相同"——0.5 瓦被 1e-5×100kW=1 瓦 的相对项吞掉了
        self.assertTrue(np.allclose(base, half_watt, atol=1e-9))
        dust = np.array([100.0 + 1e-12, 50.0])              # 差 1e-12：求和噪声，不计为差异
        self.assertFalse(stress._tick_differs(base, dust))
        self.assertFalse(np.array_equal(base, dust))        # 但精确尺会记下来 → 两把尺的差额可见

    def test_pair_verdicts_report_both_rulers(self):
        """容差吞掉的 tick 数必须自己说出来（toleranceHiddenTicks），不能只留一个计数。"""
        block = {"_bindingTicks": 10, "_priorityOrderEqualsFcfsOrderTicks": 10,
                 "_ticksWithVaryingWait": 10,
                 "_perTickAllocationDiffTicks": {"greedy_fcfs!=priority_wait": 0},
                 "_perTickAllocationDiffTicksExact": {"greedy_fcfs!=priority_wait": 4},
                 "_perTickAllocationDiffMaxHiddenKw": {"greedy_fcfs!=priority_wait": 1.4e-14}}
        verdict = stress._pair_verdicts({str(c): dict(block) for c in common.STRESS_CAPS})
        pair = verdict["greedy_fcfs!=priority_wait"]
        self.assertEqual(pair["diffTicks"], 0)
        self.assertEqual(pair["diffTicksExact"], 12)
        self.assertEqual(pair["toleranceHiddenTicks"], 12)
        # 吞掉的量级也要报出来：只报"吞了 12 个 tick"，读者无从判断那是尘埃还是真差异
        self.assertAlmostEqual(pair["maxHiddenDiffKw"], 1.4e-14)
        self.assertEqual(pair["diffToleranceKw"], stress.PAIR_DIFF_ATOL_KW)
        self.assertTrue(pair["indistinguishable"])          # 仍按物理上无意义的 1e-9kW 判"不可分辨"

    def test_session_order_check_reads_the_table_instead_of_assuming(self):
        check = stress.session_order_check(self._long())
        self.assertEqual(check["sessions"], 2)
        self.assertTrue(check["idOrderMatchesStartOrder"])
        self.assertAlmostEqual(check["spearmanTieAwareSessionIdVsStart"], 1.0, places=9)
        self.assertIn("预期同分配", check["implication"])
        swapped = self._long()
        swapped["session_id"] = ["SES-00000001", "SES-00000000"]   # id 与开始时间反序
        reverse = stress.session_order_check(swapped)
        self.assertFalse(reverse["idOrderMatchesStartOrder"])
        self.assertIn("不一致", reverse["implication"])

    def test_session_order_check_does_not_claim_from_degenerate_data(self):
        """退化输入**不许**被读成结论：旧版单会话给 NaN 却照样输出"排序键确实不同"。

        更要紧的是全并列：``argsort(argsort(x))`` 造的秩会把并列按行序拆开，于是它也报 +1.0
        ——把"没信息"当成"已证实同序"。现在并列占比要显式写出来，且退化时读数给 None。
        """
        single = stress.session_order_check(self._long().head(1))
        self.assertEqual(single["sessions"], 1)
        self.assertIsNone(single["idOrderMatchesStartOrder"])
        self.assertIsNone(single["spearmanTieAwareSessionIdVsStart"])
        self.assertIn("无从判定", single["implication"])
        self.assertNotIn("确实不同", single["implication"])
        # 多 tick 同一会话：按 session_id 去重，不能把 4 行数成 4 个会话
        many = self._long().loc[self._long().index.repeat(4)].reset_index(drop=True)
        many["tick_ts"] = pd.to_datetime([pd.Timestamp("2026-01-01 00:00:00")] * 4
                                         + [pd.Timestamp("2026-01-01 00:05:00")] * 4)
        many["session_id"] = ["SES-00000000"] * 4 + ["SES-00000001"] * 4
        self.assertEqual(stress.session_order_check(many)["sessions"], 2)
        # 开始时间完全并列
        tied = self._long()
        tied["wait_min"] = [10.0, 10.0]
        check = stress.session_order_check(tied)
        self.assertIsNone(check["spearmanTieAwareSessionIdVsStart"])
        self.assertIn("完全并列", check["implication"])

    def test_load_demands_refuses_when_the_digest_key_is_absent(self):
        """哈希检查不能在"键不存在"上失败开放——那等于这道闸形同虚设。"""
        with tempfile.TemporaryDirectory() as tmp:
            real = json.loads(stress.common.TICK_SUMMARY_PATH.read_text(encoding="utf-8"))
            stripped = {k: v for k, v in real.items() if k != "demandLongSha256"}
            path = Path(tmp) / "features_summary.json"
            path.write_text(json.dumps(stripped), encoding="utf-8")
            with mock.patch.object(stress.common, "TICK_SUMMARY_PATH", path):
                with self.assertRaises(common.BatchMismatch):
                    stress.load_demands()

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
