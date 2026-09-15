"""全网功率分配：三种容量约束下的分配策略 + 一套评估指标（纯函数，不碰数据层）。

背景（发布批实测，见 README）：站变压器恒 360kW、每站 3 桩、额定池最大 187kW、
实测站×tick 功率峰值 150.9kW —— **当前额定下容量从不 binding**。因此"过载"在本批
是诚实的负事实；本模块的工程量在**反事实收紧容量**（stress-cap 网格）下比较分配策略：
同样的真实需求流，喂给不同策略，交付电量/公平性/等待会差多少。

策略约定（全部向量化、确定性、逐 tick 独立）：
* ``allocate_greedy``        按给定顺序（默认先来先服务）逐台给满，给完为止；
* ``allocate_proportional``  比例公平：按需求份额缩放到容量内；
* ``allocate_maxmin``        max-min 公平（水填充）：先抬高最小需求者，逐层涨到饱和；
* ``allocate_priority``      优先级贪心：按优先级高者先给满（优先级 = 等待分钟等）。

需求与分配都用 kW 向量；``cap`` 是站级总容量。返回值与 demands 同形状且
``0 <= alloc <= demand``、``sum(alloc) <= cap``（测试钉死这两条不变式）。
"""

from __future__ import annotations

import numpy as np


def _clip(demands: np.ndarray, cap: float, alloc: np.ndarray) -> np.ndarray:
    alloc = np.clip(np.asarray(alloc, dtype=float), 0.0, None)
    return np.minimum(alloc, demands)


def allocate_greedy(demands: np.ndarray, cap: float, order: np.ndarray | None = None) -> np.ndarray:
    """按 ``order`` 给满：先来先服务的贪心。order=None 按索引序（稳定、可复跑）。"""
    demands = np.asarray(demands, dtype=float)
    alloc = np.zeros(len(demands))
    remaining = float(cap)
    for i in (np.arange(len(demands)) if order is None else np.asarray(order)):
        give = min(demands[i], remaining)
        alloc[i] = give
        remaining -= give
        if remaining <= 0:
            break
    return _clip(demands, cap, alloc)


def allocate_proportional(demands: np.ndarray, cap: float) -> np.ndarray:
    """比例公平：需求都乘同一因子 min(1, cap/Σdemand)。Σdemand=0 时返回全零。"""
    demands = np.asarray(demands, dtype=float)
    total = demands.sum()
    if total <= 0:
        return np.zeros(len(demands))
    return _clip(demands, cap, demands * min(1.0, float(cap) / total))


def allocate_maxmin(demands: np.ndarray, cap: float) -> np.ndarray:
    """max-min 公平（水填充）：均分抬升，需求先饱和者交出多余容量给未饱和者。

    小整数规模的显式分层循环（每站 3 桩），不用通用 LP——语义一眼可核、测试好钉。"""
    demands = np.asarray(demands, dtype=float)
    active = demands > 0
    alloc = np.zeros(len(demands))
    remaining = float(cap)
    while remaining > 1e-12 and active.any():
        level = remaining / active.sum()
        hit = active & (demands - alloc <= level)
        if hit.any():                      # 有人先饱和：给到需求、释放容量、活动集缩一圈
            alloc[hit] = demands[hit]
            remaining = float(cap) - alloc.sum()
            active &= ~hit
        else:                               # 无人饱和：均分后收尾
            alloc[active] += level
            remaining = 0.0
    return _clip(demands, cap, alloc)


def allocate_priority(demands: np.ndarray, cap: float, priority: np.ndarray) -> np.ndarray:
    """优先级贪心：priority 大者先给满。并列按索引序（稳定）。"""
    priority = np.asarray(priority, dtype=float)
    order = np.lexsort((np.arange(len(demands)), -priority))
    return allocate_greedy(demands, cap, order=order)


POLICIES = ("greedy_fcfs", "proportional", "maxmin", "priority_wait")


def allocate(policy: str, demands: np.ndarray, cap: float, *, wait_min: np.ndarray | None = None) -> np.ndarray:
    """统一入口；``priority_wait`` 策略需要 ``wait_min``（各桩已等分钟）。"""
    if policy == "greedy_fcfs":
        return allocate_greedy(demands, cap)
    if policy == "proportional":
        return allocate_proportional(demands, cap)
    if policy == "maxmin":
        return allocate_maxmin(demands, cap)
    if policy == "priority_wait":
        if wait_min is None:
            raise ValueError("priority_wait 需要 wait_min")
        return allocate_priority(demands, cap, wait_min)
    raise ValueError(f"未知策略 {policy!r}，可选 {POLICIES}")


def policy_metrics(demands: np.ndarray, alloc: np.ndarray, wait_min: np.ndarray | None = None) -> dict:
    """单 tick 指标：交付电量、欠供、公平（Gini）、等待加权欠供。"""
    demands = np.asarray(demands, dtype=float)
    alloc = np.asarray(alloc, dtype=float)
    shortfall = np.maximum(demands - alloc, 0.0)
    total_demand = demands.sum()
    served = np.divide(alloc.sum(), total_demand) if total_demand > 0 else 1.0
    share = np.divide(shortfall, demands, out=np.zeros_like(demands), where=demands > 0)
    nonzero = share[share > 0]
    gini = 0.0
    if len(nonzero) > 1:
        s = np.sort(nonzero)
        n = len(s)
        gini = float((2.0 * np.arange(1, n + 1) - n - 1).dot(s) / (n * s.sum()))
    metrics = {"servedFraction": round(float(served), 6),
               "shortfallKw": round(float(shortfall.sum()), 3),
               "unfairnessGini": round(gini, 4)}
    if wait_min is not None:
        w = np.clip(np.asarray(wait_min, dtype=float), 0.0, None)
        metrics["waitWeightedShortfall"] = round(float((shortfall * w).sum()), 3)
    return metrics
