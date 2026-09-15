"""留一站回测（LOSO）：藏起一站、只用其余 24 站重建需求场，预测被藏站的真实需求。

这是选址模型的**真盲测**：场模型能不能把需求外推到一个它没见过的位置？答案在本批是**否**——
Spearman 在所有吸引半径上为负（见 `backtest.md`）。脚本同时跑三个基线坐实这件事，并给机制诊断：

* ``field``           距离衰减需求场（本线主模型）；
* ``cityMean``        被藏站所在城市**其余 4 站**的需求均值——**不含任何空间/坐标信息**，只用"哪个城"；
* ``globalMean``      全网站均（常数）；
* ``random``          随机置换打分（秩相关的零线，跑 200 次取均值）。

机制诊断：本仿真把每城需求近似恒定地在城内 5 站间切分，"城内其余站之和 vs 本站"强负相关
（实测 Spearman≈−0.88），而站间距近于均匀、无真实需求梯度；于是靠邻居外推的需求场结构性地
**反预测**被藏站。这把预留括号里那句"真没人流量数据"从推测升级成了实证。

用法（仓库根目录）：python -m data_analysis.ml.siting.backtest
"""

from __future__ import annotations

import numpy as np
import pandas as pd
from scipy.stats import spearmanr

from . import common, field


def _rank_eval(pred: np.ndarray, actual: np.ndarray) -> dict:
    pred = np.asarray(pred, dtype=float)
    if np.allclose(pred, pred[0]):               # 常数打分：秩相关无定义，不喂给 spearman/corrcoef
        return {"spearman": None, "spearmanP": None, "pearson": None, "note": "常数打分，秩相关无定义"}
    rho = spearmanr(pred, actual)
    return {"spearman": round(float(rho.correlation), 4), "spearmanP": round(float(rho.pvalue), 5),
            "pearson": round(float(np.corrcoef(pred, actual)[0, 1]), 4)}


def loso_by_radius(intensity: pd.DataFrame) -> dict:
    coords = intensity[["latitude", "longitude"]].astype(float).to_numpy()
    demand = intensity["demand"].to_numpy(dtype=float)
    out = {}
    for radius in common.RADII_KM:
        predicted = field.loso_predict(coords, demand, radius_km=radius)
        out[str(radius)] = {"field": _rank_eval(predicted, demand),
                            "predictedMeanKw": None,
                            "n": int(len(demand))}
    return out


def baseline_evals(intensity: pd.DataFrame) -> dict:
    """不含坐标空间信息的平凡基线：cityMean / globalMean / random。"""
    rng = np.random.default_rng(common.SEED if hasattr(common, "SEED") else 20260915)
    demand = intensity["demand"].to_numpy(dtype=float)
    city = intensity["city_id"].to_numpy()

    city_mean = np.empty(len(demand))
    for c in np.unique(city):
        mask = city == c
        total, count = demand[mask].sum(), mask.sum()
        # 留一：被藏站用"本城其余站均值"预测，不能用它自己
        city_mean[mask] = (total - demand[mask]) / max(count - 1, 1)
    global_mean = np.full(len(demand), demand.mean())

    random_scores = []
    for _ in range(200):
        perm = rng.permutation(len(demand))
        random_scores.append(spearmanr(perm, demand).correlation)
    return {
        "cityMean": _rank_eval(city_mean, demand),
        "globalMean": _rank_eval(global_mean, demand),
        "random": {"spearman": round(float(np.mean(random_scores)), 4),
                   "spearmanAbsMax": round(float(np.max(np.abs(random_scores))), 4),
                   "note": "200 次随机置换的 Spearman 均值/绝对值最大——零信息的秩噪声带"},
    }


def mechanism_diagnostic(intensity: pd.DataFrame) -> dict:
    """需求场失败的可复算原因：城内零和切分 + 城市间近等大 + 两种需求口径互不一致。"""
    demand = intensity["demand"].to_numpy(dtype=float)
    city = intensity["city_id"].to_numpy()
    city_other = np.empty(len(demand))
    for c in np.unique(city):
        mask = city == c
        city_other[mask] = demand[mask].sum() - demand[mask]
    within = _rank_eval(city_other, demand)

    city_totals = intensity.groupby("city_id")["demand"].sum()
    coords = intensity[["latitude", "longitude"]].astype(float).to_numpy()
    D = field.distance_matrix(coords, coords)
    same = city[:, None] == city[None, :]
    iu = np.triu_indices(len(coords), 1)
    intra = D[iu][same[iu]]
    inter = D[iu][~same[iu]]
    return {
        "cityOtherSumVsOwnDemand": within,
        "cityTotals": {str(c): int(v) for c, v in city_totals.items()},
        "cityTotalSpread": round(float(city_totals.std() / city_totals.mean()), 4),
        "nearestNeighborKm": round(float(np.min(np.where(np.eye(len(coords), dtype=bool), np.inf, D), axis=1).mean()), 2),
        "intraCityPairKmP50": round(float(np.median(intra)), 2),
        "interCityPairKmP50": round(float(np.median(inter)), 0),
        "demandTwoViewsSpearman": round(float(
            spearmanr(intensity["demand"], intensity["charging_ticks"]).correlation), 4),
        "interpretation": ("城内其余站需求之和强烈负预测本站（本批≈−0.88）——仿真把近恒定的城市总量"
                           "在 5 站间切分；站间距均匀、无真实空间梯度。需求场靠邻居外推，继承了这个"
                           "零和结构 → 越半径越负。会话数与充电分钟两口径仅中等一致（见"
                           " demandTwoViewsSpearman），连'需求'本身都没有一把干净的尺子。"),
    }


def main() -> dict:
    common.require_empty_run_dir(common.OUT_DIR)
    common.verify_batch()
    intensity = common.station_intensity()
    demand = intensity["demand"].to_numpy(dtype=float)

    by_radius = loso_by_radius(intensity)
    baselines = baseline_evals(intensity)
    mechanism = mechanism_diagnostic(intensity)

    all_field_negative = all(by_radius[str(r)]["field"]["spearman"] < 0 for r in common.RADII_KM)
    best_radius = max(by_radius, key=lambda r: by_radius[r]["field"]["spearman"])
    best_rho = by_radius[best_radius]["field"]["spearman"]

    report = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": common.read_source_manifest()["pipelineRunId"],
        "datasetId": common.DATASET_ID, "stations": int(len(demand)),
        "obsDays": int(intensity["obs_days"].iloc[0]),
        "abandonedTotal": int(intensity["abandoned"].sum()),
        "losobyRadius": by_radius, "baselines": baselines, "mechanism": mechanism,
        "verdict": {
            "fieldNegativeEverywhere": bool(all_field_negative),
            "cityMeanAlsoNegative": bool(baselines["cityMean"]["spearman"] is not None
                                         and baselines["cityMean"]["spearman"] < 0),
            "bestFieldSpearman": best_rho, "bestRadiusKm": float(best_radius),
            "headline": (
                "需求场在全部吸引半径上 Spearman 为负（场模型甚至比纯随机更差）；"
                "连**不含坐标**的 cityMean 基线也是负值（"
                f"{baselines['cityMean']['spearman']:+.3f}）——说明问题不在空间模型本身，"
                "而是本批数据里没有任何可从邻居推断'哪站忙'的信号：每城总量近恒定、在 5 站间"
                "零和切分，任何用其余站需求预测被藏站的方法都**结构性反预测**。"
                "这把预留时那句'真没人流量数据'升级成了实证。"
                if all_field_negative else
                "部分半径下需求场为正，值得进一步做候选打分"),
        },
        "command": common.invocation("data_analysis.ml.siting.backtest"),
        "pythonVersions": common.dependency_versions(), "builtAt": common.stamp(),
        "disclaimer": common.data_note(),
    }
    common.write_new_json(common.BACKTEST_JSON, report)
    common.write_new_text(common.BACKTEST_MD, _markdown(report))

    print(f"[backtest] 站 {len(demand)} · 观测 {report['obsDays']} 天 · 弃队 {report['abandonedTotal']:,}")
    for r in common.RADII_KM:
        blk = by_radius[str(r)]["field"]
        print(f"[backtest] field r={r:4.1f}km  spearman={blk['spearman']:+.3f} p={blk['spearmanP']:.3f}")
    print(f"[backtest] 基线 cityMean spearman={baselines['cityMean']['spearman']:+.3f} · "
          f"globalMean {baselines['globalMean']['spearman']} · random {baselines['random']['spearman']:+.3f}")
    print(f"[backtest] 机制 cityOtherSum~own {mechanism['cityOtherSumVsOwnDemand']['spearman']:+.2f}")
    print(f"[backtest] 结论：{'场模型处处为负（选址前提被证伪）' if all_field_negative else '部分为正'} -> {common.BACKTEST_MD}")
    return report


def _markdown(report: dict) -> str:
    lines = ["# 新站选址 · 留一站回测（LOSO）报告", "", f"> {report['disclaimer']}", ""]
    lines.append(f"**结论**：{report['verdict']['headline']}")
    lines.append("")
    lines.append(f"- 站数 {report['stations']} · 观测天数 {report['obsDays']} · 网络弃队 {report['abandonedTotal']:,}")
    lines.append("")
    lines.append("## 需求场 vs 真实需求（留一站，秩相关）")
    lines.append("| 吸引半径 km | Spearman | p | Pearson |")
    lines.append("| ---: | ---: | ---: | ---: |")
    for r in [str(x) for x in common.RADII_KM]:
        blk = report["losobyRadius"][r]["field"]
        lines.append(f"| {r} | {blk['spearman']:+.3f} | {blk['spearmanP']:.3f} | {blk['pearson']:+.3f} |")
    lines.append("")
    lines.append("## 基线（不含空间信息作对照）")
    lines.append("| 打分 | Spearman |")
    lines.append("| --- | ---: |")
    lines.append(f"| field(最优半径 {report['verdict']['bestRadiusKm']}km) | {report['verdict']['bestFieldSpearman']:+.3f} |")
    lines.append(f"| cityMean（只用'哪个城'） | {report['baselines']['cityMean']['spearman']:+.3f} |")
    lines.append(f"| random（零信息秩噪声） | {report['baselines']['random']['spearman']:+.3f} |")
    mech = report["mechanism"]
    lines.append("")
    lines.append("## 机制诊断")
    lines.append(f"- 城内其余站之和 vs 本站需求 Spearman = **{mech['cityOtherSumVsOwnDemand']['spearman']:+.2f}**（近零和切分）")
    lines.append(f"- 各城总量：{mech['cityTotals']}（归一化离散 {mech['cityTotalSpread']}）")
    lines.append(f"- 站最近邻均值 {mech['nearestNeighborKm']}km · 城内站距 p50 {mech['intraCityPairKmP50']}km · 跨城 p50 {mech['interCityPairKmP50']:.0f}km")
    lines.append(f"- 会话数 vs 充电分钟两口径 Spearman = {mech['demandTwoViewsSpearman']}")
    lines.append(f"- {mech['interpretation']}")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    main()
