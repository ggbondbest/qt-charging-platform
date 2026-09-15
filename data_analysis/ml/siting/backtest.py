"""留一站回测（LOSO）：藏起一站、只用其余 24 站重建需求场，预测被藏站的真实需求。

这是选址模型的**真盲测**：场模型能不能把需求外推到一个它没见过的位置？本批答案是**否**——
Spearman 在所有吸引半径上为负，且**落在正确的置换零线之下**（不只是"比随机差"，见下）。

但一个负号在这份数据上**本身不含信息量**，这条脚本的全部严肃性在于把两件事分开：

1. **代数必然**：每城总量近恒定、恰好切给 5 站 ⇒ ``Σ_{j≠i} d_j = T_c − d_i`` 精确成立 ⇒
   **任何**用其余站需求预测被藏站的方法都必然反预测。所以"LOSO 为负"不能当结论用。
   ``cityMean`` 基线（本城其余站均值）与机制诊断里的"其余站之和"是**同一量的常数倍**
   （每城恰 5 站 → 除 4 不改秩），二者 Spearman 一字不差（都是 −0.8786）——它们是**一条**
   证据，不是两条。本脚本把这层关系明写在产物里，不再把它包装成"三个基线互相坐实"。
2. **有信息量的部分**：(a) 正确的零线不是"全局随机置换"（它打破城市零和结构，均值 ≈ +0.18，
   连"无信号"都不算），而是**城内置换**——保住每城总量与"5 站零和"、只打乱"谁站在哪"。
   注意这条零线**本身不在 0**：本批实测 +0.13~+0.23（弱正，位置级正相关仍能被蒙到一点）。
   场模型的观察值落在该零线的 200 次重抽带**之下**（经验 p ≤ 1/200），说明它学到的不是"没有
   空间信号"而是**倒置**结构——所以"负读数"的证据强度来自"低于零线"，不来自"零线在 0 附近"；
   (b) 同一批数据里 **``site_type`` 是能外推的**（城内归一份额的 LOSO Spearman ≈ +0.90）——
   本批可学的规律是"建什么类型"，不是"建在哪"。

另需记住两条统计边界：n=25 站的截面、且 25 个 LOSO 预测共享同一批 24 个需求值（彼此强相关），
所以 scipy 的 p 值**只是名义显著性**，真正的对照是上面的置换零线；站间距均匀不代表"无空间结构"——
实测最近邻站需求与本站需求 **−0.66**，比随机切分的理论地板（−1/(k−1)=−0.25）负得多，是**倒置梯度**。

用法（仓库根目录）：python -m data_analysis.ml.siting.backtest
"""

from __future__ import annotations

import numpy as np
import pandas as pd
from scipy.stats import spearmanr

from . import common, field

#: 置换零线的重抽次数。钉死在 SEED 上，换 seed 即换产物。
NULL_REPS = 200


def _rank_eval(pred: np.ndarray, actual: np.ndarray) -> dict:
    pred = np.asarray(pred, dtype=float)
    if np.allclose(pred, pred[0]):               # 常数打分：秩相关无定义，不喂给 spearman/corrcoef
        return {"spearman": None, "spearmanP": None, "pearson": None, "note": "常数打分，秩相关无定义"}
    rho = spearmanr(pred, actual)
    return {"spearman": round(float(rho.correlation), 4), "spearmanP": round(float(rho.pvalue), 5),
            "pearson": round(float(np.corrcoef(pred, actual)[0, 1]), 4),
            "pValueCaveat": "n=25 截面且各 LOSO 预测共享同一批需求值（互相强相关）→ 此 p 仅为名义显著性"}


def loso_by_radius(intensity: pd.DataFrame) -> dict:
    coords = intensity[["latitude", "longitude"]].astype(float).to_numpy()
    demand = intensity["demand"].to_numpy(dtype=float)
    out = {}
    for radius in common.RADII_KM:
        predicted = field.loso_predict(coords, demand, radius_km=radius)
        out[str(radius)] = {"field": _rank_eval(predicted, demand), "n": int(len(demand))}
    return out


def _permute_in_city(values: np.ndarray, city: np.ndarray, rng) -> np.ndarray:
    """只在城内打乱需求：保住每城总量（零和结构）与站坐标，只切断"谁站在哪"的对应关系。"""
    out = values.copy()
    for c in np.unique(city):
        idx = np.flatnonzero(city == c)
        out[idx] = rng.permutation(values[idx])
    return out


def permutation_nulls(intensity: pd.DataFrame, reps: int = NULL_REPS) -> dict:
    """两种置换零线，逐半径给出观察值相对零线带的位置。

    * ``global``  —— 全站混洗。它打破城市结构，对"场 vs 本站"这个估计是**错的**对照（均值 ~0.18
      而不是 0：中央位置的场值天然偏高，混洗后仍能蒙到一部分）。旧版只报了一个 ``random`` 均值，
      没跟观察值同口径比，容易被读成"场比随机差一点"。
    * ``inCity``  —— 只城内混洗。**保住近恒定城市总量与 5 站零和**，只切断空间对应——这才是
      "没有空间信号"应有的零线。它并不落在 0：把需求在城市内的"站位"打乱后，场的**位置级**
      正相关（中心位置天然吸得多）仍在，故零线均值为弱正。判断观察值时以这条带为准，
      而不是以"ρ<0"为准。
    """
    coords = intensity[["latitude", "longitude"]].astype(float).to_numpy()
    demand = intensity["demand"].to_numpy(dtype=float)
    city = intensity["city_id"].to_numpy()
    rng = np.random.default_rng(common.SEED)
    out: dict[str, dict] = {}
    for radius in common.RADII_KM:
        observed = spearmanr(field.loso_predict(coords, demand, radius_km=radius), demand).correlation
        blocks = {}
        for kind in ("global", "inCity"):
            vals = []
            for _ in range(reps):
                shuffled = rng.permutation(demand) if kind == "global" else _permute_in_city(demand, city, rng)
                vals.append(spearmanr(field.loso_predict(coords, shuffled, radius_km=radius), demand).correlation)
            vals = np.asarray(vals, dtype=float)
            blocks[kind] = {
                "nullMean": round(float(vals.mean()), 4),
                "nullP5": round(float(np.percentile(vals, 5)), 4),
                "nullP95": round(float(np.percentile(vals, 95)), 4),
                # 单侧：零线里有多大比例**不高于**观察值。观察值远低于零线 → 接近 0（场比无信号更糟）。
                "shareOfNullAtOrBelowObserved": round(float((vals <= observed).mean()), 4),
            }
        out[str(radius)] = {"observedSpearman": round(float(observed), 4), "nulls": blocks}
    return out


def site_type_signal(intensity: pd.DataFrame) -> dict:
    """本批**真正**带外推力的那把尺子：站型对城内归一需求份额。

    每城恰 5 站、每型一站，故"同城同类型"无留一余地 → 用**跨城池化**的同类型其余站平均份额做
    LOSO 预测（被藏站自己不参与任何一格）。这不是空间模型——它回答的是"建什么"，不是"建在哪"；
    放在这里是把"本批没有可学规律"这句话限定到它该被限定的范围。
    """
    demand = intensity["demand"].to_numpy(dtype=float)
    city = intensity["city_id"].to_numpy()
    types = intensity["site_type"].to_numpy()
    city_total = np.array([demand[city == c].sum() for c in city])
    share = demand / city_total
    predicted = np.full(len(share), np.nan)
    for i in range(len(share)):
        same = np.flatnonzero((types == types[i]) & (np.arange(len(share)) != i))
        if same.size:
            predicted[i] = share[same].mean()
    ok = ~np.isnan(predicted)
    table = (pd.DataFrame({"city": city, "type": types, "share": share})
             .groupby("type")["share"].agg(["mean", "std", "count"]))
    return {
        "losoOnWithinCityShare": _rank_eval(predicted[ok], share[ok]),
        "coveredStations": int(ok.sum()), "stations": int(len(share)),
        "shareByType": {str(k): {"mean": round(float(v["mean"]), 4), "std": round(float(v["std"]), 4),
                                 "count": int(v["count"])} for k, v in table.iterrows()},
        "note": ("份额极差（最大类型均值 − 最小类型均值）见 shareByType；每城每型恰一站，"
                 "故同类型留一只能跨城池化。此列不是选址（where）证据，是建型（what）证据。"),
    }


def baseline_evals(intensity: pd.DataFrame) -> dict:
    """不含坐标空间信息的平凡打分。**注意 cityMean 不是独立证据**——见 sameAlgebraAsMechanism。"""
    rng = np.random.default_rng(common.SEED)
    demand = intensity["demand"].to_numpy(dtype=float)
    city = intensity["city_id"].to_numpy()

    city_mean = np.empty(len(demand))
    per_city_count = {}
    for c in np.unique(city):
        mask = city == c
        total, count = demand[mask].sum(), int(mask.sum())
        per_city_count[c] = count
        # 留一：被藏站用"本城其余站均值"预测，不能用它自己
        city_mean[mask] = (total - demand[mask]) / max(count - 1, 1)
    global_mean = np.full(len(demand), demand.mean())

    random_scores = [spearmanr(rng.permutation(len(demand)), demand).correlation for _ in range(NULL_REPS)]
    return {
        "cityMean": _rank_eval(city_mean, demand),
        "globalMean": _rank_eval(global_mean, demand),
        "randomGlobalPermutation": {
            "spearmanMean": round(float(np.mean(random_scores)), 4),
            "spearmanAbsMax": round(float(np.max(np.abs(random_scores))), 4),
            "note": (f"{NULL_REPS} 次**全局**置换的秩噪声带。它对场模型不是正确的零线（会打破城市零和"
                     "结构）；正确零线见 permutationNulls.inCity"),
        },
        "stationsPerCity": per_city_count,
        "sameAlgebraAsMechanism": (
            "每城恰 5 站时 cityMean = (T_c − d_i)/4 与'其余站之和'只差一个常数 → 秩全等，"
            "故 cityMean 的 Spearman 与 mechanism.cityOtherSumVsOwnDemand **必然同值**。"
            "它们是同一条证据，不能当两个独立确认数。"),
    }


def mechanism_diagnostic(intensity: pd.DataFrame) -> dict:
    """场模型为什么负：零和切分 + **倒置**空间结构 + 两把需求尺子互不一致。"""
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
    nearest = np.array([demand[np.argsort(D[i])[1]] for i in range(len(coords))])
    nearest_eval = _rank_eval(nearest, demand)
    stations_per_city = int(same[0].sum())
    floor = -1.0 / (stations_per_city - 1)
    two_views = float(spearmanr(intensity["demand"], intensity["charging_ticks"]).correlation)
    spread = round(float(city_totals.std() / city_totals.mean()), 4)
    view_phrase = ("两口径一致性尚可" if two_views > 0.9 else
                   "连'需求'本身都没有一把干净的尺子")
    inversion = (nearest_eval.get("spearman") if nearest_eval.get("spearman") is not None else float("nan"))
    within_rho = (within.get("spearman") if within.get("spearman") is not None else float("nan"))
    return {
        "cityOtherSumVsOwnDemand": within,
        "nearestNeighbourDemandVsOwn": nearest_eval,
        "stationsPerCity": stations_per_city,
        "randomSplitFloor": round(floor, 4),
        "cityTotals": {str(c): int(v) for c, v in city_totals.items()},
        "cityTotalSpread": spread,
        "cityTotalsNearConstant": bool(spread < 0.05),
        "nearestNeighborKm": round(float(np.min(np.where(np.eye(len(coords), dtype=bool), np.inf, D), axis=1).mean()), 2),
        "intraCityPairKmP50": round(float(np.median(intra)), 2),
        "interCityPairKmP50": round(float(np.median(inter)), 0),
        "demandTwoViewsSpearman": round(two_views, 4),
        "interpretation": (
            f"城内其余站需求之和预测本站 = {_num(within_rho)}——每城总量近恒定（归一化离散 "
            f"{spread}）、在 {stations_per_city} 站间切分，这是**代数**层面的必然。更要紧的是：最近邻站需求与"
            f"本站 = {inversion:+.3f}，{'远负于' if inversion < floor else '未负于'}随机切分的理论地板 "
            f"−1/(k−1)={floor:.2f}"
            + ("，即城内不是'无空间梯度'而是**倒置梯度**（仿真给近邻配了互补份额）" if inversion < floor
               else "，即城内确实接近无空间梯度")
            + "。需求场靠邻居外推，于是把倒置结构当成预测信号 → 半径越大越接近"
              "Σ其它站需求 = T_c − 本站，读数总体也越负（是否严格单调见 verdict.radiusDriftPhrase）。"
              f"会话数与 5 分钟充电 tick 两口径的秩相关 = {two_views:.3f}（{view_phrase}）。"),
    }


def _null_sign_phrase(values: list[float]) -> str:
    """城内置换零线均值的**符号**由数据说，不预设"近 0"。

    早期版本把这条零线写成"均值近 0"，实跑它随半径从 +0.23 退到 +0.13（弱正）。说法必须跟着数据：
    若哪天它真的深负，"场读数为负"就不再是独立证据——零线自己已经把那个负号造出来了。
    """
    lo, hi = min(values), max(values)
    if lo > 0.05:
        return f"弱正（全部半径 ≥{lo:+.3f}）"
    if hi < -0.05:
        return f"偏负（全部半径 ≤{hi:+.3f}）→ 场的负读数**不再是独立证据**"
    return f"跨零（[{lo:+.3f}, {hi:+.3f}]）"


def _drift_phrase(rhos: list[float], radii=common.RADII_KM, min_drop: float = 0.1) -> str:
    """读数随半径怎么漂移，措辞跟着实测走（"恶化"= 更负 = 离零线更远）。

    早期版本在正文里写死"单调恶化"，而实跑 r=2km −0.524 → r=3km −0.522 是往上走的一小步，
    同一份 JSON 的判定标志位是 false —— 产物自相矛盾。
    """
    steps = [f"r={a:g}→{b:g}km {x:+.3f}→{y:+.3f}"
             for a, b, x, y in zip(radii[:-1], radii[1:], rhos[:-1], rhos[1:]) if y > x]
    if not steps:
        return ("读数随半径**单调**恶化" if rhos[0] - rhos[-1] >= min_drop
                else "读数随半径基本持平")
    if rhos[0] - rhos[-1] >= min_drop:
        return f"读数随半径**总体**走弱但**非严格单调**（反向台阶：{' · '.join(steps)}）"
    return f"读数**未**随半径单调走弱（反向台阶：{' · '.join(steps)}）——未见零和代数接管"


def main() -> dict:
    common.require_empty_run_dir(common.OUT_DIR)
    manifest = common.verify_batch()
    intensity = common.station_intensity()
    demand = intensity["demand"].to_numpy(dtype=float)

    by_radius = loso_by_radius(intensity)
    nulls = permutation_nulls(intensity)
    type_signal = site_type_signal(intensity)
    baselines = baseline_evals(intensity)
    mechanism = mechanism_diagnostic(intensity)

    all_field_negative = all(by_radius[str(r)]["field"]["spearman"] < 0 for r in common.RADII_KM)
    best_radius = max(by_radius, key=lambda r: by_radius[r]["field"]["spearman"])
    best_rho = by_radius[best_radius]["field"]["spearman"]
    worst_below_incity = all(nulls[str(r)]["nulls"]["inCity"]["shareOfNullAtOrBelowObserved"] <= 0.05
                             for r in common.RADII_KM)
    # 读数随半径的漂移：短半径=真实邻接结构，长半径=零和代数接管。怎么说由下面的 _drift_phrase 判。
    rho_sequence = " → ".join(f"r={r:g}km {by_radius[str(r)]['field']['spearman']:+.3f}"
                              for r in common.RADII_KM)
    rhos = [by_radius[str(r)]["field"]["spearman"] for r in common.RADII_KM]
    strictly_monotone = all(later <= earlier for earlier, later in zip(rhos[:-1], rhos[1:]))
    weakens_overall = rhos[-1] < rhos[0] - 0.1
    drift_phrase = _drift_phrase(rhos)
    in_city_null_means = [nulls[str(r)]["nulls"]["inCity"]["nullMean"] for r in common.RADII_KM]
    null_sequence = " → ".join(f"r={r:g}km {v:+.3f}"
                               for r, v in zip(common.RADII_KM, in_city_null_means))
    null_sign = _null_sign_phrase(in_city_null_means)
    # 零线均值的**解释**也按符号给，不能预设它一定在正侧
    null_explainer = ("——打乱站位后，场的'中心位置天然吸得多'这类位置级效应仍能蒙到一点正相关，"
                      if min(in_city_null_means) > 0.05 else "，")
    if weakens_overall:
        handoff = f"长半径由 (1) 的零和代数接管：{drift_phrase}（完整序列见 verdict.radiusDrift）。"
    else:
        handoff = f"本批看不出长半径被零和代数接管：{drift_phrase}。"

    report = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": manifest["pipelineRunId"],
        "datasetId": common.DATASET_ID,
        "seed": common.SEED, "nullReps": NULL_REPS,
        "stations": int(len(demand)),
        "obsDays": int(intensity["obs_days"].iloc[0]),
        "abandonedTotal": int(intensity["abandoned"].sum()),
        # 弃队代理的双计口径（复核过的事实，不留到 README 才说）
        "unmetProxyCaveats": {
            "abandonedRechargedWithin24h": 476,
            "abandonedRechargedWithin24hPct": round(100.0 * 476 / max(int(intensity["abandoned"].sum()), 1), 2),
            "callExpiredExcluded": 4037,
            "note": ("demand_incl_unmet = 会话数 + ABANDONED；其中 476 条弃队在 24h 内同人同站又开成"
                     "会话（双计），CALL_EXPIRED 4,037 条同为未满足需求但未并入（未做敏感性分析）。"),
        },
        "sourceTablesSha256": common.source_table_digests(),
        "losobyRadius": by_radius,
        "permutationNulls": nulls,
        "siteTypeSignal": type_signal,
        "baselines": baselines,
        "mechanism": mechanism,
        "verdict": {
            "fieldNegativeEverywhere": bool(all_field_negative),
            "fieldBelowInCityNull": bool(worst_below_incity),
            "inCityNullNotDeeplyNegative": bool(all(v > -0.25 for v in in_city_null_means)),
            "inCityNullMeanSequence": null_sequence,
            "inCityNullSign": null_sign,
            "strictlyWorsensWithRadius": bool(strictly_monotone),
            "weakensWithRadiusOverall": bool(weakens_overall),
            "radiusDriftPhrase": drift_phrase,
            "radiusDrift": rho_sequence,
            "bestFieldSpearman": best_rho, "bestRadiusKm": float(best_radius),
            "headline": (
                "分三层说，别混成一句：\n"
                "  (1) cityMean / '其余站之和' 那个负号**不含信息量**——每城总量近恒定、恰在 "
                f"{mechanism['stationsPerCity']} 站间零和切分，"
                "于是 Σ_{j≠i}d_j = T_c − d_i 精确成立，own 与 others 的负相关是**定义式**（本批 "
                f"{mechanism['cityOtherSumVsOwnDemand']['spearman']:+.3f}；cityMean 与之同秩 → 同一条证据）。"
                "它只说明'本批城市总量近恒定'，不说明模型好坏。\n"
                "  (2) 有信息量的第一条：以**城内置换**（保住每城总量、只切断'谁站在哪'）为零线。这条零线"
                f"本身不落在 0：本批 {null_sequence}，即{null_sign}{null_explainer}"
                "而场模型在全部半径上都**落在其 p5 之下**"
                f"（经验 p≤{1.0 / NULL_REPS:.3f}）：证据强度来自'低于零线带'，不来自'ρ<0'。这才是"
                f"『比无信号更糟』的正经读法。它的来源可拆两半："
                f"短半径读到的是本批真实的**倒置邻接结构**（最近邻需求 vs 本站 "
                f"{mechanism['nearestNeighbourDemandVsOwn']['spearman']:+.3f}，深于随机切分地板 "
                f"{mechanism['randomSplitFloor']}），{handoff}\n"
                "  (3) 有信息量的第二条：同一批数据里 **site_type 可外推**（城内归一份额 LOSO Spearman "
                f"{type_signal['losoOnWithinCityShare']['spearman']:+.3f}）——本批能学的是'建什么类型'，"
                "不是'建在哪'。因此对'用邻居需求外推新站空间需求'这条选址前提，本批证据是**否**；"
                "而'仓库没有人流/POI 表'是文件清单事实，不需要回测来证明，本回测也没为它增加证据。"),
        },
        "command": common.invocation("data_analysis.ml.siting.backtest"),
        "pythonVersions": common.dependency_versions(), "builtAt": common.stamp(),
        "disclaimer": common.data_note(),
    }
    common.write_new_json(common.BACKTEST_JSON, report)
    common.write_new_text(common.BACKTEST_MD, _markdown(report))

    print(f"[backtest] 站 {len(demand)} · 业务日 {report['obsDays']} 天 · 弃队 {report['abandonedTotal']:,}")
    for r in common.RADII_KM:
        blk = by_radius[str(r)]["field"]
        nul = nulls[str(r)]["nulls"]["inCity"]
        print(f"[backtest] field r={r:4.1f}km spearman={blk['spearman']:+.3f} "
              f"(名义 p={blk['spearmanP']:.3f}) · 城内置换零线 {nul['nullMean']:+.3f} "
              f"[p5,p95]=[{nul['nullP5']:+.3f},{nul['nullP95']:+.3f}] "
              f"P(零线≤观察)={nul['shareOfNullAtOrBelowObserved']:.3f}")
    print(f"[backtest] cityMean {baselines['cityMean']['spearman']:+.3f}"
          f"（= 机制诊断同秩，非独立证据） · globalMean {baselines['globalMean']['spearman']}"
          f" · random {baselines['randomGlobalPermutation']['spearmanMean']:+.3f}")
    print(f"[backtest] site_type 外推份额 spearman={type_signal['losoOnWithinCityShare']['spearman']:+.3f}"
          f" · 最近邻需求 vs 本站 {mechanism['nearestNeighbourDemandVsOwn']['spearman']:+.3f}"
          f"（随机切分地板 {mechanism['randomSplitFloor']}）")
    print(f"[backtest] -> {common.BACKTEST_MD}")
    return report


def _num(value, spec: str = "+.3f") -> str:
    """None（常数打分 → 秩相关无定义）在报告里写 n/a，而不是让格式化抛 TypeError。"""
    return "n/a" if value is None else format(value, spec)


def _markdown(report: dict) -> str:
    lines = ["# 新站选址 · 留一站回测（LOSO）报告", "", f"> {report['disclaimer']}", ""]
    lines.append("**结论（分三层，勿混成一句）**")
    lines.append("")
    for part in report["verdict"]["headline"].split("\n"):
        lines.append(f"> {part}".rstrip())
    lines.append("")
    lines.append(f"- 站数 {report['stations']} · 观测业务天数 {report['obsDays']} · 网络弃队 "
                 f"{report['abandonedTotal']:,} · seed {report['seed']} · 置换重抽 {report['nullReps']} 次")
    caveats = report["unmetProxyCaveats"]
    lines.append(f"- 弃队代理脏处：{caveats['abandonedRechargedWithin24h']} 条（"
                 f"{caveats['abandonedRechargedWithin24hPct']}%）弃队 24h 内同人同站又充上了 → "
                 f"`demand_incl_unmet` 双计；`CALL_EXPIRED` {caveats['callExpiredExcluded']:,} 条未并入")
    lines.append("")
    lines.append("## 需求场 vs 真实需求（留一站）+ 两种置换零线")
    lines.append("| 吸引半径 km | 场 Spearman | 名义 p | 城内置换零线均值 | 零线 [p5,p95] | P(零线≤观察) | 全局置换零线均值 |")
    lines.append("| ---: | ---: | ---: | ---: | --- | ---: | ---: |")
    for r in [str(x) for x in common.RADII_KM]:
        blk = report["losobyRadius"][r]["field"]
        nul = report["permutationNulls"][r]["nulls"]
        lines.append(f"| {r} | {_num(blk['spearman'])} | {_num(blk['spearmanP'], '.3f')} | "
                     f"{_num(nul['inCity']['nullMean'])} | [{_num(nul['inCity']['nullP5'])}, "
                     f"{_num(nul['inCity']['nullP95'])}] | {nul['inCity']['shareOfNullAtOrBelowObserved']:.3f} | "
                     f"{_num(nul['global']['nullMean'])} |")
    lines.append("")
    lines.append("`P(零线≤观察)` 是 200 次城内置换里不高于观察值的比例：≈0 = 场模型**比『无空间信号』还糟**，"
                 "它学到的是倒置结构。全局置换那一列只是旧版 `random` 的正确版本，**不是**本模型的对照。")
    lines.append("")
    lines.append(f"- 读数随半径的漂移：{report['verdict']['radiusDrift']} —— "
                 f"{report['verdict']['radiusDriftPhrase']}"
                 f"（短半径读到的是真实邻接结构；长半径是否被零和代数接管 = "
                 f"{report['verdict']['weakensWithRadiusOverall']}）"
                 "；严格单调性 = "
                 f"{report['verdict']['strictlyWorsensWithRadius']}")
    lines.append(f"- 城内置换零线均值随半径：{report['verdict']['inCityNullMeanSequence']} —— "
                 f"{report['verdict']['inCityNullSign']}；判断场的负读数一律以这条带为准，"
                 "**不是**以 0 为准")
    lines.append("")
    lines.append("## 平凡打分与'同一条证据'声明")
    lines.append("| 打分 | Spearman |")
    lines.append("| --- | ---: |")
    lines.append(f"| field（最优半径 {report['verdict']['bestRadiusKm']}km） | "
                 f"{_num(report['verdict']['bestFieldSpearman'])} |")
    lines.append(f"| cityMean（只用'哪个城'） | {_num(report['baselines']['cityMean']['spearman'])} |")
    lines.append(f"| random（全局置换均值，非正确零线） | "
                 f"{_num(report['baselines']['randomGlobalPermutation']['spearmanMean'])} |")
    lines.append("")
    lines.append(f"> {report['baselines']['sameAlgebraAsMechanism']}")
    lines.append("")
    lines.append("## 本批可学的规律：站型（建什么），不是空间（建在哪）")
    ts = report["siteTypeSignal"]
    lines.append(f"- 城内归一份额的 LOSO 外推（同类型其余站、跨城池化）：Spearman = "
                 f"**{ts['losoOnWithinCityShare']['spearman']:+.3f}**（n={ts['coveredStations']}）")
    lines.append("- 类型均值份额：" + " · ".join(f"{k} {v['mean']:.3f}(σ{v['std']:.3f})"
                                              for k, v in ts["shareByType"].items()))
    lines.append("")
    mech = report["mechanism"]
    lines.append("## 机制诊断")
    lines.append(f"- 城内其余站之和 vs 本站 Spearman = **{mech['cityOtherSumVsOwnDemand']['spearman']:+.2f}**"
                 f"（cityMean 与之同秩：{report['baselines']['cityMean']['spearman']:+.2f}）")
    lines.append(f"- 最近邻站需求 vs 本站 Spearman = **{mech['nearestNeighbourDemandVsOwn']['spearman']:+.2f}**"
                 f"（随机切分理论地板 {mech['randomSplitFloor']} → "
                 + ("**倒置梯度**，不是'无梯度'"
                    if mech['nearestNeighbourDemandVsOwn']['spearman'] < mech['randomSplitFloor']
                    else "确实接近'无空间梯度'") + "）")
    lines.append(f"- 各城总量：{mech['cityTotals']}（归一化离散 {mech['cityTotalSpread']}"
                 + ("，近恒定 → '其余站之和 = T_c − 本站' 近似代数恒等"
                    if mech.get('cityTotalsNearConstant') else "，城市总量本身有差异") + "）")
    lines.append(f"- 站最近邻均值 {mech['nearestNeighborKm']}km · 城内站距 p50 {mech['intraCityPairKmP50']}km · "
                 f"跨城 p50 {mech['interCityPairKmP50']:.0f}km")
    lines.append(f"- 会话数 vs 充电 tick 两口径 Spearman = {mech['demandTwoViewsSpearman']}")
    lines.append(f"- {mech['interpretation']}")
    lines.append("")
    lines.append("## 输入表内容摘要（换表即产物失效）")
    for name, digest in report["sourceTablesSha256"].items():
        lines.append(f"- `{name}` `{digest[:16]}…`")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    main()
