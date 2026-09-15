"""分配腿：把真实站·tick 需求流喂进 4 种容量分配策略，在压力容量网格上比较（只读、零新增数据）。

诚实前提（features 复核）：站变压器恒 360kW、站·tick 总负荷实测峰值 150.9kW —— **360kW 下容量从不
binding**，四种策略交付完全一致、无从比较。故真正的"全网功率分配"问题只在**收紧容量的反事实**里
成立：把同一批真实需求流的站级容量压到 100/125/150kW，看哪些 tick 会 binding、不同策略差在哪。

一个必须先讲清的不变式：单 tick 交付总功率 = ``min(cap, Σdemand)``，**与策略无关**——所以
``servedFraction`` 与 ``shortfallKw`` 在策略间恒等，本脚本会把这个"恒等"实测出来钉死。策略真正分化
的是**欠供怎么分**：``unfairnessGini``（欠供份额的不均衡度）与 ``waitWeightedShortfall``（谁在等）。

策略（`allocate.py`）：``greedy_fcfs`` 先来先服务、``proportional`` 比例公平、``maxmin`` max-min
水填充、``priority_wait`` 按会话已充电时长（等待代理）优先。

产物：outputs/ml_transformer_stress/policy_report.{json,md}（O_EXCL）。
用法（仓库根目录）：python -m data_analysis.ml.transformer.stress
"""

from __future__ import annotations

import pandas as pd

from . import common
from .allocate import POLICIES, allocate, policy_metrics

#: 含 360 额定基线（预期从不 binding）与三档收紧容量。
CAPS = (common.TRANSFORMER_KW,) + common.STRESS_CAPS


def load_demands() -> pd.DataFrame:
    if not common.DEMAND_LONG_PATH.exists():
        raise FileNotFoundError(f"缺少需求长表 {common.DEMAND_LONG_PATH}，请先跑 features")
    long = pd.read_parquet(common.DEMAND_LONG_PATH,
                           columns=["station_id", "tick_ts", "charger_id", "demand_kw", "wait_min"])
    common.verify_batch()
    return long


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
                           "waitWeightedShortfall": None} for policy in POLICIES}
        result["_bindingTicks"] = 0
        result["_totalTicks"] = total_ticks
        result["_maxTickKw"] = round(max_tick, 2)
        return result

    acc = {policy: {"servedFracSum": 0.0, "shortfallSum": 0.0, "giniSum": 0.0, "waitSum": 0.0}
           for policy in POLICIES}
    # 不变式的真正度量：同一个 tick 上，跨策略的 servedFraction 极差（交付总量与策略无关 → 应≈0）。
    xpolicy_spread_sum = 0.0
    xpolicy_spread_max = 0.0
    counted = 0
    for (_station, _tick), part in binding.groupby(["station_id", "tick_ts"], sort=False):
        demands = part["demand_kw"].to_numpy(dtype=float)
        waits = part["wait_min"].to_numpy(dtype=float)
        counted += 1
        tick_served = []
        for policy in POLICIES:
            alloc = allocate(policy, demands, cap, wait_min=waits)
            m = policy_metrics(demands, alloc, wait_min=waits)
            a = acc[policy]
            a["servedFracSum"] += m["servedFraction"]
            a["shortfallSum"] += m["shortfallKw"]
            a["giniSum"] += m["unfairnessGini"]
            a["waitSum"] += m["waitWeightedShortfall"]
            tick_served.append(m["servedFraction"])
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
        }
    result["_bindingTicks"] = counted
    result["_totalTicks"] = total_ticks
    result["_maxTickKw"] = round(max_tick, 2)
    result["_xpolicyServedSpreadMean"] = round(xpolicy_spread_sum / counted, 9) if counted else None
    result["_xpolicyServedSpreadMax"] = round(xpolicy_spread_max, 9) if counted else None
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

    report = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": common.read_source_manifest()["pipelineRunId"],
        "datasetId": common.DATASET_ID, "caps": list(CAPS), "policies": list(POLICIES),
        "perCap": per_cap,
        "invariant": {
            "statement": "单 tick 交付总功率 = min(cap, Σdemand)，与策略无关；故同 tick 各策略 "
                         "servedFraction 一致、欠供总量一致，策略只在公平性维度（Gini / 等待加权欠供）分化",
            "perTickCrossPolicyServedFractionMaxSpread": invariant_max,
            "perTickCrossPolicyServedFractionMeanSpread": invariant_mean,
        },
        "headline": {
            "ratedCapBindingTicks": per_cap[str(common.TRANSFORMER_KW)]["_bindingTicks"],
            "maxStationTickKw": per_cap[str(common.TRANSFORMER_KW)]["_maxTickKw"],
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
              + " · ".join(f"{p}: gini={block[p]['unfairnessGini']}" for p in POLICIES))
    print(f"[stress] -> {common.STRESS_REPORT_MD}")
    return report


def _markdown(report: dict) -> str:
    lines = ["# 全网功率分配 · 策略反事实压力回放", "", f"> {report['disclaimer']}", ""]
    lines.append(f"- 额定 {report['headline']['maxStationTickKw']}kW 峰值下，360kW 变压器 binding tick = "
                 f"{report['headline']['ratedCapBindingTicks']}（{report['headline']['note']}）")
    lines.append(f"- 不变式：同一 tick 上跨策略 ``servedFraction`` 最大离差 "
                 f"{report['invariant']['perTickCrossPolicyServedFractionMaxSpread']}（≈0 = 交付总量与策略无关；"
                 f"Gini 也测不出'谁被欠供'，区分度在等待加权欠供）")
    for cap in [str(c) for c in report["caps"]]:
        block = report["perCap"][cap]
        lines.append("")
        lines.append(f"## 容量 {cap} kW（binding tick {block['_bindingTicks']:,} / {block['_totalTicks']:,}）")
        if block["_bindingTicks"] == 0:
            lines.append("_容量高于全部站·tick 峰值，策略无从分化。_")
            continue
        lines.append("| 策略 | servedFraction | meanShortfallKw | 不公平Gini | 等待加权欠供 |")
        lines.append("| --- | ---: | ---: | ---: | ---: |")
        for policy in report["policies"]:
            m = block[policy]
            lines.append(f"| {policy} | {m['servedFraction']} | {m['meanShortfallKw']} | "
                         f"{m['unfairnessGini']} | {m['waitWeightedShortfall']} |")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    main()
