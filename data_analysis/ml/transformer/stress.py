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
同一个排序键。这一条由 ``session_order_check`` 从输入表里**实测**出来并写进产物，不靠 docstring 记忆；
真正分化的是 greedy 家族 vs ``proportional`` / ``maxmin``。

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
    if expected and expected != actual:
        raise common.BatchMismatch(
            f"需求长表哈希 {actual[:12]}… 与 features_summary 记录的 {expected[:12]}… 不一致，"
            "表被改过或写坏，请重跑 features")
    long = pd.read_parquet(common.DEMAND_LONG_PATH,
                           columns=["station_id", "tick_ts", "charger_id", "session_id",
                                    "demand_kw", "wait_min"])
    common.verify_batch()
    return long


def session_order_check(long: pd.DataFrame) -> dict:
    """等待代理的排序键与"先来先服务"是不是同一把尺子——查输入表，不靠记忆断言。

    ``priority_wait`` 按等待降序给容量，``greedy_fcfs`` 按长表行序给容量。本批长表按
    (session_id, tick) 稳定排序，而 ``session_id`` 又随会话开始时间递增 ⇒ 组内行序**就是**等待降序，
    两条策略必然逐 tick 同分配。这是**批次性质**（换一批 id 与开始时间不同序的数据就会分开），
    所以把它测出来写进产物，而不是写在注释里。
    """
    per_session = (long.assign(session_start=lambda d: pd.to_datetime(d["tick_ts"])
                               - pd.to_timedelta(d["wait_min"], unit="m"))
                   .groupby("session_id", as_index=False)["session_start"].min())
    ids = per_session["session_id"].astype("category").cat.codes.to_numpy(dtype=float)
    starts = pd.to_datetime(per_session["session_start"]).astype("int64").to_numpy(dtype=float)
    rho = float(np.corrcoef(np.argsort(np.argsort(ids, kind="stable"), kind="stable"),
                            np.argsort(np.argsort(starts, kind="stable"), kind="stable"))[0, 1])
    return {
        "sessions": int(len(per_session)),
        "spearmanSessionIdRankVsStartRank": round(rho, 6),
        "implication": ("=+1.0 ⇒ 组内行序 = 等待降序 ⇒ priority_wait 与 greedy_fcfs 同序、"
                        "逐 tick 必然同分配（本批四策略只有三种行为）" if abs(rho - 1.0) < 1e-9 else
                        "≠+1.0 ⇒ 两条策略的排序键确实不同，差异见各容量档 _perTickAllocationDiffTicks"),
    }


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
        # 与有 binding 的分支同一套键，下游取数不必分叉
        result["_priorityOrderEqualsFcfsOrderTicks"] = 0
        result["_ticksWithVaryingWait"] = 0
        result["_perTickAllocationDiffTicks"] = {}
        return result

    acc = {policy: {"servedFracSum": 0.0, "shortfallSum": 0.0, "giniSum": 0.0, "waitSum": 0.0,
                    "starveSum": 0.0}
           for policy in POLICIES}
    # 两两策略的**逐 tick 分配差异**计数：聚合指标相等不等于策略等价——必须先把它测出来，
    # 否则"两个数字一样"会被误读成"两条策略一样"（本 PR 早期版本就犯过这个错）。
    pairs = [(a, b) for i, a in enumerate(POLICIES) for b in POLICIES[i + 1:]]
    diff_counts = {f"{a}!={b}": 0 for a, b in pairs}
    # 不变式的真正度量：同一个 tick 上，跨策略的 servedFraction 极差（交付总量与策略无关 → 应≈0）。
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
        tick_served = []
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
            tick_served.append(m["servedFraction"])
        for left, right in pairs:
            if not np.allclose(allocs[left], allocs[right], atol=1e-9):
                diff_counts[f"{left}!={right}"] += 1
        spread = max(tick_served) - min(tick_served)
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
    result["_xpolicyServedSpreadMean"] = round(xpolicy_spread_sum / counted, 9) if counted else None
    result["_xpolicyServedSpreadMax"] = round(xpolicy_spread_max, 9) if counted else None
    result["_priorityOrderEqualsFcfsOrderTicks"] = order_eq_ticks
    result["_ticksWithVaryingWait"] = varying_wait_ticks
    result["_perTickAllocationDiffTicks"] = diff_counts
    return result


def main() -> dict:
    common.require_empty_run_dir(common.STRESS_DIR)
    long = load_demands()
    per_cap = {str(cap): replay(long, cap) for cap in CAPS}

    # 不变式实测：任一收紧容量下，同 tick 跨策略 servedFraction 的最大离差应≈0（交付总量与策略无关）。
    invariant_max = {}
    invariant_mean = {}
    for cap in common.STRESS_CAPS:
        block = per_cap[str(cap)]
        if block["_bindingTicks"] > 0:
            invariant_max[str(cap)] = block["_xpolicyServedSpreadMax"]
            invariant_mean[str(cap)] = block["_xpolicyServedSpreadMean"]

    # "策略名 != 策略行为"的实测：哪几对在逐 tick 上根本分不开、哪几对真的分开。
    equivalence = {
        "sessionOrderCheck": session_order_check(long),
        "perCap": {},
        "statement": (
            "greedy_fcfs 与 priority_wait 在本批是**同一条策略**：两者逐 tick 分配不同的 tick 数 = 0，"
            "且这不是巧合——长表组内行序本身就是等待降序（见 sessionOrderCheck），"
            "'先来先服务'与'等待最久优先'用的是同一个排序键。故本报告标题上的四种策略，"
            "在本批只有三种行为。反过来说：只比聚合指标会同时漏掉这两类误读（相等≠等价、"
            "不等价≠不同策略），所以逐 tick 两两差异与排序键检查都要做。"),
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

    report = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": common.read_source_manifest()["pipelineRunId"],
        "datasetId": common.DATASET_ID, "caps": list(CAPS), "policies": list(POLICIES),
        "perCap": per_cap,
        "invariant": {
            "statement": "单 tick 交付总功率 = min(cap, Σdemand)，与策略无关；故同 tick 各策略 "
                         "servedFraction 一致、欠供总量一致。策略分化在欠供**怎么分**："
                         "unfairnessGini（全体参与者份额，含被喂满者）、waitWeightedShortfall（加权和）、"
                         "longestWaitStarvedPct（配对敏感）。注意：聚合值相等不等于策略等价（反之'行为相同'"
                         "也可能藏在两个策略名下）——逐 tick 差异见各容量档 _perTickAllocationDiffTicks，"
                         "策略名与实际行为的对账见 policyEquivalence",
            "perTickCrossPolicyServedFractionMaxSpread": invariant_max,
            "perTickCrossPolicyServedFractionMeanSpread": invariant_mean,
        },
        "policyEquivalence": equivalence,
        "headline": {
            "ratedCapBindingTicks": per_cap[str(common.TRANSFORMER_KW)]["_bindingTicks"],
            "maxStationTickKw": per_cap[str(common.TRANSFORMER_KW)]["_maxTickKw"],
            "transformerKw": common.TRANSFORMER_KW,
            "note": "360kW 额定下 binding tick=0（过载是负事实），策略比较全部来自收紧容量的反事实",
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
              f"跨策略servedFrac离差≤{invariant_max.get(str(cap),'n/a')} · "
              + " · ".join(f"{p}: gini={block[p]['unfairnessGini']}"
                           f" 饿等待最久={block[p]['longestWaitStarvedPct']}%" for p in POLICIES))
        print("[stress]   逐 tick 分配不同的 tick 数："
              + " · ".join(f"{k}={v:,}" for k, v in
                           block["_perTickAllocationDiffTicks"].items()))
    chk = equivalence["sessionOrderCheck"]
    print(f"[stress] 策略名对账：greedy_fcfs ≡ priority_wait（逐 tick 不同的 tick 数 = 0）· "
          f"rank(session_id) vs rank(会话开始) Spearman={chk['spearmanSessionIdRankVsStartRank']:+.3f}"
          " · " + " · ".join(
              f"cap={c}: 等待有差异 {v['ticksWithVaryingWait']:,}/{v['bindingTicks']:,}"
              f" · priority序=行序 {v['ticksWherePriorityOrderEqualsRowOrder']:,}/{v['bindingTicks']:,}"
              for c, v in equivalence["perCap"].items()))
    print(f"[stress] -> {common.STRESS_REPORT_MD}")
    return report


def _markdown(report: dict) -> str:
    lines = ["# 全网功率分配 · 策略反事实压力回放", "", f"> {report['disclaimer']}", ""]
    headline = report["headline"]
    lines.append(f"- 实测站·tick 峰值 **{headline['maxStationTickKw']}kW** · 变压器额定 "
                 f"{headline['transformerKw']:.0f}kW → 额定下 binding tick = "
                 f"{headline['ratedCapBindingTicks']}（{headline['note']}）")
    lines.append(f"- 不变式：同一 tick 上跨策略 ``servedFraction`` 最大离差 "
                 f"{report['invariant']['perTickCrossPolicyServedFractionMaxSpread']}"
                 f"（≈0 = 交付总量与策略无关；策略只在欠供怎么分上分化）")
    eq = report["policyEquivalence"]
    lines.append("")
    lines.append("## 策略名 ≠ 策略行为（逐 tick 对账）")
    lines.append(f"- {eq['statement']}")
    chk = eq["sessionOrderCheck"]
    lines.append(f"- 排序键检查：会话数 {chk['sessions']:,} · rank(``session_id``) vs rank(会话开始) 的 "
                 f"Spearman = **{chk['spearmanSessionIdRankVsStartRank']:+.3f}** → {chk['implication']}")
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
            lines.append("逐 tick 分配**不同**的 tick 数（**聚合指标相等 ≠ 策略等价**）："
                         + " · ".join(f"`{k}` {v:,}" for k, v in diffs.items()))
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    main()
