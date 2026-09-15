"""变压器线纯单元测试：分配策略不变式 + 手工水填充 + tick 帧滞后/目标/边界（合成小数据）。

刻意不读 388 万行真实遥测——那些数字由脚本入口的一次实跑产出并冻进 outputs/；这里只钉
"代码逻辑对不对"：分配的四条不变式、max-min 水填充与比例公平的解析解、以及 build_tick_frame
的滞后只用过去、目标严格 t+1、1-tick purge 与跨段丢弃。
"""

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from data_analysis.ml.transformer import allocate, features


# ------------------------------------------------------------------ 分配不变式与解析解
DEMAND_CASES = [
    np.array([10.0, 200.0, 5.0]),
    np.array([40.0, 40.0, 40.0]),
    np.array([0.0, 120.0, 30.0]),
    np.array([500.0]),
    np.array([0.0, 0.0, 0.0]),
]
CAPS = [0.0, 5.0, 60.0, 100.0, 205.0]


@pytest.mark.parametrize("demands", DEMAND_CASES)
@pytest.mark.parametrize("cap", CAPS)
@pytest.mark.parametrize("policy", allocate.POLICIES)
def test_allocate_invariants(policy, demands, cap):
    wait = np.array([5.0, 1.0, 3.0])[:len(demands)] if len(demands) <= 3 else np.arange(len(demands), dtype=float)
    alloc = allocate.allocate(policy, demands, cap, wait_min=wait)
    assert np.all(alloc >= -1e-9), (policy, demands, cap, alloc)
    assert np.all(alloc <= demands + 1e-9), (policy, demands, cap, alloc)
    assert alloc.sum() <= cap + 1e-9, (policy, demands, cap, alloc)


def test_maxmin_waterfill_hand_case():
    alloc = allocate.allocate_maxmin(np.array([10.0, 200.0, 5.0]), 100.0)
    assert np.allclose(alloc, [10.0, 85.0, 5.0])


def test_proportional_equal_demands_are_equal():
    alloc = allocate.allocate_proportional(np.array([40.0, 40.0, 40.0]), 60.0)
    assert np.allclose(alloc, [20.0, 20.0, 20.0])


def test_greedy_fcfs_orders_by_index():
    alloc = allocate.allocate_greedy(np.array([30.0, 30.0, 30.0]), 40.0)
    assert alloc[0] == 30.0 and alloc[1] == 10.0 and alloc[2] == 0.0


def test_priority_uses_wait_order():
    demands = np.array([30.0, 30.0])
    # 桩 1 优先级更高 → 先给满
    alloc = allocate.allocate_priority(demands, 40.0, priority=np.array([0.0, 10.0]))
    assert alloc[1] == 30.0 and alloc[0] == 10.0


def test_priority_wait_greedy_equals_reverse_index_when_wait_is_reverse():
    demands = np.array([30.0, 30.0, 30.0])
    # 等待时长恰好是索引的逆序 → priority_wait 应等价于"从尾到头"的贪心
    alloc = allocate.allocate("priority_wait", demands, 40.0, wait_min=np.array([0.0, 1.0, 2.0]))
    assert alloc[2] == 30.0 and alloc[1] == 10.0 and alloc[0] == 0.0


# ------------------------------------------------------------------ policy_metrics
def test_served_fraction_is_policy_invariant_under_binding():
    demands = np.array([20.0, 50.0, 80.0])   # Σ=150 > cap
    wait = np.array([3.0, 1.0, 2.0])
    fractions = set()
    for policy in allocate.POLICIES:
        alloc = allocate.allocate(policy, demands, 40.0, wait_min=wait)
        fractions.add(round(allocate.policy_metrics(demands, alloc, wait)["servedFraction"], 6))
    assert len(fractions) == 1, fractions        # 交付 min(cap,Σd) 与策略无关


def test_proportional_zero_gini():
    demands = np.array([30.0, 30.0, 30.0])
    m = allocate.policy_metrics(demands, allocate.allocate_proportional(demands, 30.0))
    assert m["unfairnessGini"] == 0.0            # 每人欠供份额相同


def test_wait_weighted_only_when_wait_given():
    demands = np.array([50.0, 50.0])
    alloc = allocate.allocate_greedy(demands, 30.0)
    assert "waitWeightedShortfall" not in allocate.policy_metrics(demands, alloc)
    assert "waitWeightedShortfall" in allocate.policy_metrics(demands, alloc, wait_min=np.array([1.0, 5.0]))


# ------------------------------------------------------------------ 宽矩阵位移
def test_shift_left_and_right_place_nan_correctly():
    m = np.arange(12.0).reshape(3, 4)             # 3 站 × 4 tick
    left = features._shift_left(m, 1)             # t 处取 t+1
    assert np.isnan(left[:, -1]).all()
    assert np.allclose(left[:, :-1], m[:, 1:])
    right = features._shift_right(m, 2)           # t 处取 t-2
    assert np.isnan(right[:, :2]).all()
    assert np.allclose(right[:, 2:], m[:, :-2])


# ------------------------------------------------------------------ 合成站×tick 帧
def _synthetic_telemetry() -> pd.DataFrame:
    """两站 × 每站 3 个 5 分钟 tick，构造可手算的 CHARGING 功率，外加 AVAILABLE 空档。"""
    ticks = pd.to_datetime(
        ["2025-01-01T00:00:00Z", "2025-01-01T00:05:00Z", "2025-01-01T00:10:00Z",
         "2025-01-01T00:15:00Z", "2025-01-01T00:20:00Z", "2025-01-01T00:25:00Z"]).tz_localize(None)
    rows = []
    # 站 A：桩 a1 功率 10,20,30,40,50,60；桩 a2 只在部分 tick 充电
    for i, t in enumerate(ticks):
        rows.append(("A", "a1", t, "CHARGING", "s1", 10.0 * (i + 1)))
        if i >= 2:
            rows.append(("A", "a2", t, "CHARGING", "s2", 5.0))
        rows.append(("B", "b1", t, "CHARGING", "s3", 30.0))
    # 掺一条 AVAILABLE（不计入负荷）
    rows.append(("A", "a3", ticks[0], "AVAILABLE", None, 0.0))
    tel = pd.DataFrame(rows, columns=["station_id", "charger_id", "recorded_at", "state", "session_id", "power_kw"])
    return tel


def test_build_tick_frame_lag_and_next(monkeypatch):
    tel = _synthetic_telemetry()

    def fake_clean(name, columns=None):
        stations = pd.DataFrame({"station_id": ["A", "B"], "city_id": ["C1", "C1"],
                                 "site_type": ["MALL", "STREET"], "transformer_kw": [360.0, 360.0]})
        return stations[columns]

    # 落段全设 EXCLUDED（边界无关，只看帧装配本身）：assign_split 走真实 manifest，这里 monkeypatch。
    monkeypatch.setattr(features.common, "load_clean_table", fake_clean)
    monkeypatch.setattr(features.common, "assign_split",
                        lambda stamps: pd.Series("TEST", index=pd.Series(stamps).index))
    frame = features.build_tick_frame(tel)
    a = frame[frame["station_id"] == "A"].sort_values("tick_ts")
    # 站 A 逐 tick 总负荷 = a1(10·(i+1)) + a2(自 i>=2 记 5)：10,20,35,45,55,65
    assert np.allclose(a["total_kw"].to_numpy(), [10, 20, 35, 45, 55, 65])
    # 目标 = 下一 tick：第一行 next=20；最后一行 next=NaN 且 dropped_last
    assert a["total_kw_next"].iloc[0] == 20.0
    assert np.isnan(a["total_kw_next"].iloc[-1]) and bool(a["dropped_last"].iloc[-1])
    # lag1 = 当 tick（决策时点已知）；lag2 = 上一 tick
    assert np.allclose(a["load_lag1"].to_numpy(), a["total_kw"].to_numpy())
    assert a["load_lag2"].iloc[1] == 10.0
    assert np.isnan(a["load_lag2"].iloc[0])


def test_numeric_feature_columns_no_future():
    cols = features.numeric_feature_columns()
    assert "total_kw" not in cols and "total_kw_next" not in cols   # 目标与其现值不作特征
    assert "load_lag1" in cols and "load_mean_1h" in cols
