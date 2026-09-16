"""分配腿：把真实站·tick 需求流喂进 4 种容量分配策略，在压力容量网格上比较（只读、零新增数据）。

诚实前提（features 复核）：站变压器恒 360kW、站·tick 总负荷实测峰值 150.9kW —— **360kW 下容量从不
binding**，四种策略交付完全一致、无从比较。故真正的"全网功率分配"问题只在**收紧容量的反事实**里
成立：把同一批真实需求流的站级容量压到 100/125/150kW，看哪些 tick 会 binding、不同策略差在哪。

一个必须先讲清的不变式：单 tick 交付总功率 = ``min(cap, Σdemand)``，**与策略无关**——所以
``servedFraction`` 与 ``shortfallKw`` 在策略间恒等，本脚本会把这个"恒等"实测出来钉死。策略真正分化
的是**欠供怎么分**：``unfairnessGini``（全体参与者欠供份额的不均衡度，含被喂满的那些）、
``waitWeightedShortfall``（欠供按等待分钟加权）、``longestWaitStarvedPct``（等待最久的参与者反而被
欠得更多的 tick 占比——**配对敏感**，加权和巧合相等时它不会）。

同一份报告还给出**两两策略逐 tick 分配不同的 tick 数**（``_perTickAllocationDiffTicks``）：聚合指标
相等**不是**策略等价的证据，反过来也一样——不测就不知道"四个策略名"里有两条其实是同一条。本批实测
``greedy_fcfs`` 与 ``priority_wait`` 在**全部** binding tick 上逐 tick 一致（不同的 tick 数 = 0）：长表
组内行序本来就是"等待降序"（``session_id`` 随会话开始时间递增），于是"先来先服务"与"等待最久优先"是
同一个排序键。这一条由 ``session_order_check`` 从输入表里**实测**出来、由 ``_pair_verdicts`` 汇总成结论文字
写进产物（**换一批数据这句话可能整个反过来，所以它不能是句硬编码的话**）。真正分化的是 greedy 家族 vs
``proportional`` / ``maxmin``。

策略（`allocate.py`）：``greedy_fcfs`` 先来先服务、``proportional`` 比例公平、``maxmin`` max-min
水填充、``priority_wait`` 按会话已充电时长（等待代理）优先——最后这条在本批与第一条同序，故
"四种策略"在本批实际只有三种行为。

产物：outputs/ml_transformer_stress/policy_report.{json,md}（O_EXCL）。
用法（仓库根目录）：python -m data_analysis.ml.transformer.stress
"""

from __future__ import annotations

import json

import numpy as np
import pandas as pd

from . import common
from .allocate import POLICIES, allocate, policy_metrics, priority_order

#: 含 360 额定基线（预期从不 binding）与三档收紧容量。
CAPS = (common.TRANSFORMER_KW,) + common.STRESS_CAPS

#: 判"两个策略在这个 tick 上分配不同"的阈值——**只给绝对量、不给相对量**。
#: ``np.allclose`` 的默认 ``rtol=1e-5`` 配 100kW 级分配等于允许 1kW 的差；那样"不同的 tick 数"
#: 就成了容差的函数（把容差放宽到 2kW，本批的差异 tick 会凭空少几万个）。1e-9 kW = 一毫瓦，
#: 比任何真实功率都小六个数量级、只够吞掉浮点求和噪声（实测隐藏差 ≤1.4e-14kW），
#: 于是这个计数读起来就是"分配确实不一样"。写进产物，别让人猜判据。
PAIR_DIFF_ATOL_KW = 1e-9


def load_demands() -> pd.DataFrame:
    """读需求长表并**校验其哈希**。

    批次绑定只挡"换批次"，挡不住"这一份 parquet 被换过/写坏"：预测腿走
    ``train.load_matrix`` 的 featuresSha256 复核，分配腿过去只查存在性——两条腿的完整性纪律
    不对称，产出的 policy_report 却与预测腿同样"看起来正规"、同样绑着批次号。
    """
    if not common.DEMAND_LONG_PATH.exists():
        raise FileNotFoundError(f"缺少需求长表 {common.DEMAND_LONG_PATH}，请先跑 features")
    if not common.TICK_SUMMARY_PATH.exists():
        raise FileNotFoundError(f"缺少 {common.TICK_SUMMARY_PATH.name}，无法校验需求表完整性，请先跑 features")
    with open(common.TICK_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    expected = summary.get("demandLongSha256")
    actual = common.sha256_file(common.DEMAND_LONG_PATH)
    if not expected:
        # 缺键**不是免检**：哈希检查在"键不存在"上失败开放，等于这道闸形同虚设。
        raise common.BatchMismatch(
            f"{common.TICK_SUMMARY_PATH.name} 里没有 demandLongSha256，无法校验需求表完整性，"
            "请重跑 features（本脚本不接受『查不到就算通过』）")
    if expected != actual:
        raise common.BatchMismatch(
            f"需求长表哈希 {actual[:12]}… 与 features_summary 记录的 {expected[:12]}… 不一致，"
            "表被改过或写坏，请重跑 features")
    long = pd.read_parquet(common.DEMAND_LONG_PATH,
                           columns=["station_id", "tick_ts", "charger_id", "session_id",
                                    "demand_kw", "wait_min"])
    common.verify_batch()
    return long


def _tie_ranks(values: np.ndarray) -> np.ndarray:
    """并列取平均秩——秩相关的标准做法。"""
    return pd.Series(values).rank(method="average").to_numpy(dtype=float)


def session_order_check(long: pd.DataFrame) -> dict:
    """等待代理的排序键与"先来先服务"是不是同一把尺子——查输入表，不靠记忆断言。

    ``priority_wait`` 按等待降序给容量，``greedy_fcfs`` 按长表行序给容量（长表按 session_id 排序，
    组内行序即 id 序）。若 ``session_id`` 随会话开始时间非递减，两条策略必然同序。这是**批次性质**
    （换一批 id 与开始时间不同序的数据就会分开），所以测出来写进产物，而不是写在注释里。

    统计量是踩过坑才改成这样的：旧版用 ``argsort(argsort(x))`` 造秩，它**不平均并列**——
    全部开始时间相同时它也报 +1.0（稳定排序把并列按行序拆开，等于拿"待定"当"已证实"），
    而秩相关此时按定义无意义。现在把"判据本体"和"等级读数"分开：
    * ``idOrderMatchesStartOrder``：按 id 排好后开始时间是否非递减（这就是判据）；
    * ``spearmanTieAwareSessionIdVsStart``：并列感知的等级读数（退化时给 None，不给假 1.0）；
    * ``tiedStartSessionPct``：开始时间并列的会话占比——提醒"同序"里有多大一块是并列而非信息。
    """
    per_session = (long.assign(session_start=lambda d: pd.to_datetime(d["tick_ts"])
                               - pd.to_timedelta(d["wait_min"], unit="m"))
                   .groupby("session_id", as_index=False)["session_start"].min())
    starts = pd.to_datetime(per_session["session_start"]).astype("int64").to_numpy(dtype=float)
    n = int(len(starts))
    distinct = int(np.unique(starts).size)
    tied_pct = round(100.0 * (n - distinct) / n, 2) if n else None
    monotone = bool(n >= 2 and np.all(np.diff(starts) >= 0))
    rho = None if (n < 2 or distinct < 2) else round(
        float(np.corrcoef(_tie_ranks(np.arange(n)), _tie_ranks(starts))[0, 1]), 6)
    if n < 2:
        implication = (f"只有 {n} 个会话：单调性与秩相关都**无从判定**，既不能说同序也不能说不同序 "
                       "→ 以各容量档 _perTickAllocationDiffTicks 的实测计数为准")
    elif distinct < 2:
        implication = (f"全部 {n} 个会话开始时间**完全并列** → 等待代理在此退化成常数，两策略不可分辨"
                       "是数据退化造成的，不构成结构性结论")
    elif monotone:
        implication = (f"id 序与开始时间序一致（非递减检查通过，tie-aware Spearman={rho}）⇒ 组内行序"
                       " = 等待降序，priority_wait 与 greedy_fcfs 预期同分配；是否真的逐 tick 全等"
                       f"以 _perTickAllocationDiffTicks 为准（并列开始时间占 {tied_pct}%，"
                       "这部分同序不含信息）")
    else:
        implication = (f"id 序与开始时间序**不一致**（tie-aware Spearman={rho}、非递减检查失败）"
                       "⇒ 两条贪心策略的排序键确实不同，差异见各容量档 _perTickAllocationDiffTicks")
    return {
        "sessions": n,
        "distinctSessionStarts": distinct,
        "tiedStartSessionPct": tied_pct,
        "idOrderMatchesStartOrder": monotone if n >= 2 else None,
        "spearmanTieAwareSessionIdVsStart": rho,
        "implication": implication,
    }


def _tick_differs(left: np.ndarray, right: np.ndarray) -> bool:
    """两个策略在同一个 tick 上的分配算不算"不同"——判据集中在这一处，好让它能被单测。

    ``rtol=0.0`` 是这条线的要害：np.allclose 的默认 ``rtol=1e-5`` 配 100kW 级分配，会把 1kW 的真实
    差异判成"相同"（本函数有回归测试钉住这一点）。
    """
    return not np.allclose(left, right, rtol=0.0, atol=PAIR_DIFF_ATOL_KW)


def replay(long: pd.DataFrame, cap: float) -> dict:
    """在给定站级容量下回放全部真实需求流，按策略聚合指标。"""
    totals = long.groupby(["station_id", "tick_ts"], sort=False)["demand_kw"].transform("sum")
    max_tick = float(totals.max())
    total_ticks = int(long[["station_id", "tick_ts"]].drop_duplicates().shape[0])
    binding = long.loc[totals > cap + 1e-9]
    n = int(binding[["station_id", "tick_ts"]].drop_duplicates().shape[0])
    if n == 0:
        result = {policy: {"bindingTicks": 0, "servedFraction": None,
                           "meanShortfallKw": None, "unfairnessGini": None,
                           "waitWeightedShortfall": None, "longestWaitStarvedPct": None}
                  for policy in POLICIES}
        result["_bindingTicks"] = 0
        result["_totalTicks"] = total_ticks
        result["_maxTickKw"] = round(max_tick, 2)
        # 与有 binding 的分支**同一套顶层键**（此处一个 tick 都没测，故读数为 None 而非 0：
        # 0 会被读成"实测离差为零"，那是把"没测"冒充成"测了且通过"）。下游取数不必按分支分叉。
        result["_xpolicyDeliveredSpreadMeanKw"] = None
        result["_xpolicyDeliveredSpreadMaxKw"] = None
        result["_priorityOrderEqualsFcfsOrderTicks"] = 0
        result["_ticksWithVaryingWait"] = 0
        result["_perTickAllocationDiffTicks"] = {}
        result["_perTickAllocationDiffTicksExact"] = {}
        result["_perTickAllocationDiffMaxHiddenKw"] = {}
        result["_perTickDiffToleranceKw"] = PAIR_DIFF_ATOL_KW
        return result

    acc = {policy: {"servedFracSum": 0.0, "shortfallSum": 0.0, "giniSum": 0.0, "waitSum": 0.0,
                    "starveSum": 0.0}
           for policy in POLICIES}
    # 两两策略的**逐 tick 分配差异**计数：聚合指标相等不等于策略等价——必须先把它测出来，
    # 否则"两个数字一样"会被误读成"两条策略一样"（本 PR 早期版本就犯过这个错）。
    pairs = [(a, b) for i, a in enumerate(POLICIES) for b in POLICIES[i + 1:]]
    diff_counts = {f"{a}!={b}": 0 for a, b in pairs}
    # 同一件事的第二把尺：逐位精确相等（连 1e-9 容差都不给）。两把尺的差 = 纯浮点噪声的 tick 数，
    # 报出来才知道"容差在这里究竟吞掉了什么"——本批差额应为 0 或仅有浮点尘埃。
    exact_diff_counts = {f"{a}!={b}": 0 for a, b in pairs}
    # 被容差判成"相同"的那些 tick 里，最大的逐位差是多少（kW）。有这个读数，"容差吞掉 N 个 tick"
    # 才可被读者判成"吞掉的是浮点尘埃"还是"吞掉了真差异"——只报 N 是把判断留给人猜。
    hidden_max = {f"{a}!={b}": 0.0 for a, b in pairs}
    # 不变式的真正度量：同一个 tick 上跨策略的**实付总功率**极差（交付总量与策略无关 → 应≈0）。
    # 用未舍入的 alloc.sum()（kW）而不是 m["servedFraction"]：后者已被 policy_metrics 舍到 6 位，
    # 拿它测"钉死"的不变式，等于把测量分辨率封在 5e-7 以下——小于那个量的漏发根本测不出来。
    xpolicy_spread_sum = 0.0
    xpolicy_spread_max = 0.0
    # "priority_wait 的排序键 == greedy_fcfs 的行序" 的 tick 数：两条策略名会不会 collapses 成一条，
    # 不能靠读代码猜（那要连排序与建表顺序一起推），在这里逐 tick 实测。
    order_eq_ticks = 0
    varying_wait_ticks = 0
    counted = 0
    for (_station, _tick), part in binding.groupby(["station_id", "tick_ts"], sort=False):
        demands = part["demand_kw"].to_numpy(dtype=float)
        waits = part["wait_min"].to_numpy(dtype=float)
        counted += 1
        if np.unique(waits).size > 1:
            varying_wait_ticks += 1
        if np.array_equal(priority_order(waits), np.arange(len(waits))):
            order_eq_ticks += 1
        tick_delivered = []
        allocs: dict[str, np.ndarray] = {}
        for policy in POLICIES:
            alloc = allocate(policy, demands, cap, wait_min=waits)
            allocs[policy] = alloc
            m = policy_metrics(demands, alloc, wait_min=waits)
            a = acc[policy]
            a["servedFracSum"] += m["servedFraction"]
            a["shortfallSum"] += m["shortfallKw"]
            a["giniSum"] += m["unfairnessGini"]
            a["waitSum"] += m["waitWeightedShortfall"]
            a["starveSum"] += m["longestWaitStarved"]
            tick_delivered.append(float(alloc.sum()))
        for left, right in pairs:
            # 判据见 PAIR_DIFF_ATOL_KW：只用绝对容差，避免"不同的 tick 数"随相对容差漂移。
            if _tick_differs(allocs[left], allocs[right]):
                diff_counts[f"{left}!={right}"] += 1
            if not np.array_equal(allocs[left], allocs[right]):
                exact_diff_counts[f"{left}!={right}"] += 1
                if not _tick_differs(allocs[left], allocs[right]):
                    gap = float(np.abs(allocs[left] - allocs[right]).max())
                    hidden_max[f"{left}!={right}"] = max(hidden_max[f"{left}!={right}"], gap)
        spread = max(tick_delivered) - min(tick_delivered)
        xpolicy_spread_sum += spread
        xpolicy_spread_max = max(xpolicy_spread_max, spread)
    assert counted == n, f"binding tick 计数不一致：{counted} vs {n}"
    result: dict[str, dict] = {}
    for policy, a in acc.items():
        result[policy] = {
            "bindingTicks": counted,
            "servedFraction": round(a["servedFracSum"] / counted, 6),
            "meanShortfallKw": round(a["shortfallSum"] / counted, 3),
            "unfairnessGini": round(a["giniSum"] / counted, 4),
            "waitWeightedShortfall": round(a["waitSum"] / counted, 2),
            "longestWaitStarvedPct": round(100.0 * a["starveSum"] / counted, 2),
        }
    result["_bindingTicks"] = counted
    result["_totalTicks"] = total_ticks
    result["_maxTickKw"] = round(max_tick, 2)
    result["_xpolicyDeliveredSpreadMeanKw"] = round(xpolicy_spread_sum / counted, 12) if counted else None
    result["_xpolicyDeliveredSpreadMaxKw"] = round(xpolicy_spread_max, 12) if counted else None
    result["_priorityOrderEqualsFcfsOrderTicks"] = order_eq_ticks
    result["_ticksWithVaryingWait"] = varying_wait_ticks
    result["_perTickAllocationDiffTicks"] = diff_counts
    result["_perTickAllocationDiffTicksExact"] = exact_diff_counts
    # 刻意不 round(…, 12)：这些量本来就是 1e-14 级，舍到 12 位会把"吞掉了什么"舍成 0。
    result["_perTickAllocationDiffMaxHiddenKw"] = {k: float(f"{v:.3g}") for k, v in hidden_max.items()}
    result["_perTickDiffToleranceKw"] = PAIR_DIFF_ATOL_KW
    return result


def _pair_verdicts(per_cap: dict) -> dict:
    """把各容量档的逐 tick 差异计数**汇总成结论**——报告正文从这里生成，不再写死。

    `indistinguishable` 的含义是"在所有测到 binding 的容量档上、双方逐 tick 分配一次都没不同"，
    这是实测出来的；换一批数据（或换一档容量）它可能整个反过来，而硬编码的句子不会跟着变。
    """
    per_pair: dict[str, dict] = {}
    for cap in common.STRESS_CAPS:
        block = per_cap[str(cap)]
        exact = block.get("_perTickAllocationDiffTicksExact") or {}
        hidden = block.get("_perTickAllocationDiffMaxHiddenKw") or {}
        for pair, count in (block.get("_perTickAllocationDiffTicks") or {}).items():
            per_pair.setdefault(pair, {})[str(cap)] = {
                "bindingTicks": int(block["_bindingTicks"]), "diffTicks": int(count),
                "diffTicksExact": int(exact.get(pair, count))}
            if pair in hidden:
                per_pair[pair][str(cap)]["maxHiddenDiffKw"] = float(hidden[pair])
    verdicts = {}
    for pair, per in per_pair.items():
        binding = sum(v["bindingTicks"] for v in per.values())
        differ = sum(v["diffTicks"] for v in per.values())
        differ_exact = sum(v["diffTicksExact"] for v in per.values())
        verdicts[pair] = {"perCap": per, "bindingTicks": binding, "diffTicks": differ,
                          "diffTicksExact": differ_exact,
                          # 容差吞掉的 tick 数：>0 说明"不同"的判定真的依赖 1e-9kW 这条线，
                          # 报出来而不是让它藏在 allclose 的默认参数里。
                          "toleranceHiddenTicks": differ_exact - differ,
                          # 被容差判成"相同"的最大逐位差（kW）：0 表示两把尺完全一致。
                          "maxHiddenDiffKw": max([v.get("maxHiddenDiffKw", 0.0) for v in per.values()],
                                                 default=0.0),
                          "diffToleranceKw": PAIR_DIFF_ATOL_KW,
                          "indistinguishable": bool(binding > 0 and differ == 0)}
    return verdicts


def _behaviour_families(verdicts: dict) -> list[list[str]]:
    """按"实测不可分辨"把策略名聚成行为族（并查集的连通分量）。"""
    parent = {policy: policy for policy in POLICIES}

    def find(name: str) -> str:
        while parent[name] != name:
            parent[name] = parent[parent[name]]
            name = parent[name]
        return name

    for pair, verdict in verdicts.items():
        if verdict["indistinguishable"]:
            left, right = pair.split("!=")
            parent[find(left)] = find(right)
    families: dict[str, list[str]] = {}
    for policy in POLICIES:
        families.setdefault(find(policy), []).append(policy)
    return [sorted(group, key=POLICIES.index) for group in families.values()]


def _equivalence_statement(verdicts: dict, families: list[list[str]], order_check: dict) -> str:
    """结论句由实测拼装。三种情形（全都能分开 / 有对塌缩 / 一个 binding tick 都没测到）各说各话。"""
    measured = {pair: v for pair, v in verdicts.items() if v["bindingTicks"] > 0}
    if not measured:
        return ("没有任何容量档出现 binding tick → 本批**无从比较**策略行为，"
                "四策略同不同序这个问题在本批没有答案（不是『相同』，是『没测』）。")
    collapsed = sorted(f"{left} 与 {right}" for pair, v in measured.items()
                       if v["indistinguishable"] for left, right in [tuple(pair.split("!="))])
    tail = ("这一判据的输入侧证据：" + order_check["implication"])
    if not collapsed:
        return (f"实测：{len(measured)} 对策略在两两逐 tick 分配上**都**出现过不同 → "
                f"{len(POLICIES)} 个策略名在本批就是 {len(POLICIES)} 种行为。"
                f"（聚合指标相等≠策略等价，故以逐 tick 差异计数为准。）{tail}")
    return (f"实测：{'、'.join(collapsed)} 在所有 binding tick 上逐 tick 分配**一次都没不同**"
            f"（差异 tick 数 = 0）→ {len(POLICIES)} 个策略名在本批只有 {len(families)} 种行为"
            f"（行为族：{families}）。塌缩发生在排序键上、不在容量上：输入侧证据见下。"
            f"反过来说：只比聚合指标会同时漏掉两类误读（相等≠等价、不等价≠不同策略），"
            f"所以逐 tick 两两差异与排序键检查都要做。{tail}")


def main() -> dict:
    common.require_empty_run_dir(common.STRESS_DIR)
    long = load_demands()
    per_cap = {str(cap): replay(long, cap) for cap in CAPS}

    # 不变式实测：任一收紧容量下，同 tick 跨策略**实付总功率**的最大离差应≈0（交付总量与策略无关）。
    invariant_max = {}
    invariant_mean = {}
    for cap in common.STRESS_CAPS:
        block = per_cap[str(cap)]
        if block["_bindingTicks"] > 0:
            invariant_max[str(cap)] = block["_xpolicyDeliveredSpreadMaxKw"]
            invariant_mean[str(cap)] = block["_xpolicyDeliveredSpreadMeanKw"]

    # "策略名 != 策略行为"的实测：哪几对在逐 tick 上根本分不开、哪几对真的分开。
    order_check = session_order_check(long)
    verdicts = _pair_verdicts(per_cap)
    families = _behaviour_families(verdicts)
    equivalence = {
        "sessionOrderCheck": order_check,
        "pairVerdicts": verdicts,
        "behaviourFamilies": families,
        "distinctBehaviours": len(families),
        # 上面所有 diffTicks 的判据（绝对、无相对项）：没有这行，"0 个 tick 不同"是无法复现的断言。
        "perTickDiffToleranceKw": PAIR_DIFF_ATOL_KW,
        "perCap": {},
        "statement": _equivalence_statement(verdicts, families, order_check),
    }
    for cap in common.STRESS_CAPS:
        block = per_cap[str(cap)]
        binding = block["_bindingTicks"]
        diffs = block.get("_perTickAllocationDiffTicks") or {}
        equivalence["perCap"][str(cap)] = {
            "bindingTicks": binding,
            "ticksWherePriorityOrderEqualsRowOrder": block["_priorityOrderEqualsFcfsOrderTicks"],
            "ticksWithVaryingWait": block["_ticksWithVaryingWait"],
            "pairsWithZeroAllocationDifference": sorted(k for k, v in diffs.items() if v == 0),
            "diffShareOfBindingTicks": {k: (round(v / binding, 4) if binding else None)
                                        for k, v in diffs.items()},
        }

    rated_binding = per_cap[str(common.TRANSFORMER_KW)]["_bindingTicks"]
    report = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": common.read_source_manifest()["pipelineRunId"],
        "datasetId": common.DATASET_ID, "caps": list(CAPS), "policies": list(POLICIES),
        "perCap": per_cap,
        "invariant": {
            "statement": "单 tick 交付总功率 = min(cap, Σdemand)，与策略无关；故同 tick 各策略实付总功率"
                         "一致、欠供总量一致。策略分化在欠供**怎么分**："
                         "unfairnessGini（全体参与者份额，含被喂满者）、waitWeightedShortfall（加权和）、"
                         "longestWaitStarvedPct（配对敏感）。注意：聚合值相等不等于策略等价（反之『行为相同』"
                         "也可能藏在两个策略名下）——逐 tick 差异见各容量档 _perTickAllocationDiffTicks，"
                         "策略名与实际行为的对账见 policyEquivalence",
            "measuredOn": "未舍入的 alloc.sum()（kW）；早先用 round(...,6) 后的 servedFraction 测，"
                          "分辨率被压在 5e-7 以下，小于该量的漏发测不出来",
            "perTickCrossPolicyDeliveredMaxKw": invariant_max,
            "perTickCrossPolicyDeliveredMeanKw": invariant_mean,
        },
        "policyEquivalence": equivalence,
        "headline": {
            "ratedCapBindingTicks": per_cap[str(common.TRANSFORMER_KW)]["_bindingTicks"],
            "maxStationTickKw": per_cap[str(common.TRANSFORMER_KW)]["_maxTickKw"],
            "transformerKw": common.TRANSFORMER_KW,
            "note": (f"额定 {common.TRANSFORMER_KW:.0f}kW 下 binding tick={rated_binding}"
                     + ("（过载在本批是负事实），策略比较全部来自收紧容量的反事实" if rated_binding == 0 else
                        "，即额定容量本身就会过载——收紧容量档是**额外**的反事实，不再是唯一比较场所")),
        },
        "command": common.invocation("data_analysis.ml.transformer.stress"),
        "pythonVersions": common.dependency_versions(), "builtAt": common.stamp(),
        "disclaimer": common.data_note(),
    }
    common.write_new_json(common.STRESS_REPORT_PATH, report)
    common.write_new_text(common.STRESS_REPORT_MD, _markdown(report))

    print(f"[stress] 额定 {common.TRANSFORMER_KW:.0f}kW：binding tick="
          f"{report['headline']['ratedCapBindingTicks']} · 峰值 {report['headline']['maxStationTickKw']}kW")
    for cap in common.STRESS_CAPS:
        block = per_cap[str(cap)]
        print(f"[stress] cap={cap:.0f}kW binding={block['_bindingTicks']:,} "
              f"跨策略实付离差≤{invariant_max.get(str(cap),'n/a')}kW · "
              + " · ".join(f"{p}: gini={block[p]['unfairnessGini']}"
                           f" 饿等待最久={block[p]['longestWaitStarvedPct']}%" for p in POLICIES))
        print("[stress]   逐 tick 分配不同的 tick 数："
              + " · ".join(f"{k}={v:,}" for k, v in
                           block["_perTickAllocationDiffTicks"].items()))
    print(f"[stress] 策略名对账（由实测生成）：{len(POLICIES)} 个策略名 → "
          f"{equivalence['distinctBehaviours']} 种行为 · 不可分辨对="
          f"{[k for k, v in verdicts.items() if v['indistinguishable']] or '（无）'} · "
          + equivalence["sessionOrderCheck"]["implication"])
    print("[stress]   逐档：" + " · ".join(
        f"cap={c}: 等待有差异 {v['ticksWithVaryingWait']:,}/{v['bindingTicks']:,}"
        f" · priority序=行序 {v['ticksWherePriorityOrderEqualsRowOrder']:,}/{v['bindingTicks']:,}"
        for c, v in equivalence["perCap"].items()))
    print(f"[stress] -> {common.STRESS_REPORT_MD}")
    return report


def _markdown(report: dict) -> str:
    lines = ["# 全网功率分配 · 策略反事实压力回放", "", f"> {report['disclaimer']}", ""]
    headline = report["headline"]
    lines.append(f"- 实测站·tick 峰值 **{headline['maxStationTickKw']}kW** · 变压器额定 "
                 f"{headline['transformerKw']:.0f}kW → {headline['note']}")
    lines.append(f"- 不变式：同一 tick 上跨策略**实付总功率**最大离差 "
                 f"{report['invariant']['perTickCrossPolicyDeliveredMaxKw']} kW"
                 f"（≈0 = 交付总量与策略无关；策略只在欠供怎么分上分化。测量口径："
                 f"{report['invariant']['measuredOn']}）")
    eq = report["policyEquivalence"]
    lines.append("")
    lines.append("## 策略名 ≠ 策略行为（逐 tick 对账）")
    lines.append(f"- {eq['statement']}")
    chk = eq["sessionOrderCheck"]
    lines.append(f"- 排序键检查：会话数 {chk['sessions']:,} · 不同开始时间 {chk['distinctSessionStarts']:,} 个"
                 f"（并列占 {chk['tiedStartSessionPct']}%）· id 序是否即开始时间序："
                 f"**{chk['idOrderMatchesStartOrder']}** · tie-aware Spearman = "
                 f"{chk['spearmanTieAwareSessionIdVsStart']} → {chk['implication']}")
    lines.append(f"- 行为族（同族 = 逐 tick 分配一次都没不同）：{eq['behaviourFamilies']}")
    lines.append("- 逐容量档（下面策略对的键名写作 ``a!=b``，其计数为 0 就意味着这两条**完全一致**）：")
    for cap, blk in eq["perCap"].items():
        lines.append(f"  - cap {cap} kW：binding {blk['bindingTicks']:,} tick · 组内等待有差异 "
                     f"{blk['ticksWithVaryingWait']:,} · priority 序 = 行序 "
                     f"{blk['ticksWherePriorityOrderEqualsRowOrder']:,} · 逐 tick 完全一致的策略对 "
                     f"{blk['pairsWithZeroAllocationDifference'] or '（无）'} · "
                     f"不同占比 " + "，".join(f"`{k}` {v}" for k, v in blk["diffShareOfBindingTicks"].items()))
    for cap in [str(c) for c in report["caps"]]:
        block = report["perCap"][cap]
        lines.append("")
        lines.append(f"## 容量 {cap} kW（binding tick {block['_bindingTicks']:,} / {block['_totalTicks']:,}）")
        if block["_bindingTicks"] == 0:
            lines.append("_容量高于全部站·tick 峰值，策略无从分化。_")
            continue
        lines.append("| 策略 | servedFraction | meanShortfallKw | 不公平Gini | 等待加权欠供 | 等待最久反被欠更多 |")
        lines.append("| --- | ---: | ---: | ---: | ---: | ---: |")
        for policy in report["policies"]:
            m = block[policy]
            lines.append(f"| {policy} | {m['servedFraction']} | {m['meanShortfallKw']} | "
                         f"{m['unfairnessGini']} | {m['waitWeightedShortfall']} | "
                         f"{m['longestWaitStarvedPct']}% |")
        diffs = block.get("_perTickAllocationDiffTicks") or {}
        if diffs:
            lines.append("")
            exact = block.get("_perTickAllocationDiffTicksExact") or {}
            hidden_mag = block.get("_perTickAllocationDiffMaxHiddenKw") or {}
            hidden = sum(exact.get(k, v) - v for k, v in diffs.items())
            note = (f"（判据：逐位差 >{PAIR_DIFF_ATOL_KW:g}kW 即算不同；容差吞掉 {hidden:,} 个 tick，"
                    f"其中最大逐位差 {max(hidden_mag.values(), default=0.0):g}kW —— 远小于任何真实功率，"
                    "是浮点尘埃而非漏发）" if hidden else
                    f"（判据：逐位差 >{PAIR_DIFF_ATOL_KW:g}kW；与『逐位精确不等』计数完全一致）")
            lines.append("逐 tick 分配**不同**的 tick 数（**聚合指标相等 ≠ 策略等价**）："
                         + " · ".join(f"`{k}` {v:,}" for k, v in diffs.items()) + note)
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    main()
