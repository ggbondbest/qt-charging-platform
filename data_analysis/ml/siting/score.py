"""候选网格机会打分：在每城站点的包围盒内撒网格点，按"可吸收需求 ×（1−现有覆盖）"排序。

这是选址线的**工具**——确定性、可复跑、给出候选地块的机会排序。但必须先读 `backtest.py` 的结论：
本批 LOSO 回测显示需求场外推力为**负**（连 cityMean 都为负），所以这些打分是**机械演示**，
不构成可信的选址建议；它要等到有真实人流/POI 需求面（本批没有）才具备决策意义。需求口径这里
用 `demand_incl_unmet = 会话数 + ABANDONED 弃队数`——把"想充没充上"的未满足需求也算进可吸收面。

用法（仓库根目录）：python -m data_analysis.ml.siting.score
"""

from __future__ import annotations

import pandas as pd

from . import common, field

#: 包围盒向外扩的经纬度缓冲（≈±9km）与网格步长（≈2.2km）。假设参数，写进产物。
PAD_DEG = 0.08
STEP_DEG = 0.02
TOP_N_PER_CITY = 5


def main() -> dict:
    common.require_empty_run_dir(common.OUT_DIR, extra_allowed=(common.BACKTEST_JSON.name,
                                                                common.BACKTEST_MD.name))
    common.verify_batch()
    intensity = common.station_intensity()

    rows = []
    for city, grp in intensity.groupby("city_id"):
        # 只拿本城站点当服务源：跨城站 >1000km、decay≈0，纳入只会把 coverage 分母摊到全网需求、
        # 把"这个候选点被本城现有站覆盖了多小比例"稀释成近 0——按城口径才解释得通。
        served_coords = grp[["latitude", "longitude"]].astype(float).to_numpy()
        demand = grp["demand_incl_unmet"].to_numpy(dtype=float)
        lat = grp["latitude"].astype(float)
        lon = grp["longitude"].astype(float)
        cand = field.grid_candidates(lat.min() - PAD_DEG, lat.max() + PAD_DEG,
                                     lon.min() - PAD_DEG, lon.max() + PAD_DEG, STEP_DEG)
        score = field.score_candidates(cand, served_coords, demand, radius_km=common.CAPTURE_KM)
        city_rows = pd.DataFrame({"city_id": city, "candidate_lat": cand[:, 0],
                                  "candidate_lon": cand[:, 1], "absorb": score["absorb"],
                                  "coverage": score["coverage"], "opportunity": score["opportunity"]})
        # 标注：该候选点离最近现有站多远（>2km 才算"新地块"，否则是在既有站旁边加建）。
        dmin = field.distance_matrix(cand, served_coords).min(axis=1)
        city_rows["nearest_station_km"] = dmin
        rows.append(city_rows)

    grid = pd.concat(rows, ignore_index=True)
    ranked = (grid[grid["nearest_station_km"] > 2.0]
              .sort_values("opportunity", ascending=False)
              .groupby("city_id", group_keys=False).head(TOP_N_PER_CITY).reset_index(drop=True))
    for column in ["candidate_lat", "candidate_lon", "absorb", "coverage", "opportunity", "nearest_station_km"]:
        ranked[column] = ranked[column].round(4)
    common.write_new_csv(common.OPPORTUNITIES_CSV, ranked)

    summary = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": common.read_source_manifest()["pipelineRunId"],
        "datasetId": common.DATASET_ID,
        "status": "NOT-BACKTEST-VALIDATED",
        "warning": ("候选打分是确定性工具演示；本批 LOSO 回测证伪了需求场的空间外推力"
                    "（Spearman 全负，见 backtest.md）——在有真实人流/POI 需求面前，这些排序"
                    "不应被当作可信选址建议。"),
        "demandBasis": "demand_incl_unmet = 会话数 + ABANDONED 弃队数（未满足需求代理）",
        "gridParams": {"padDeg": PAD_DEG, "stepDeg": STEP_DEG,
                       "captureKm": common.CAPTURE_KM, "topNPerCity": TOP_N_PER_CITY,
                       "minDistFromExistingKm": 2.0},
        "candidatePoints": int(len(grid)), "rankedRows": int(len(ranked)),
        "abandonedInDemand": int(intensity["abandoned"].sum()),
        "topCandidates": ranked.to_dict(orient="records"),
        "command": common.invocation("data_analysis.ml.siting.score"),
        "pythonVersions": common.dependency_versions(), "builtAt": common.stamp(),
        "disclaimer": common.data_note(),
    }
    common.write_new_json(common.SCORE_JSON, summary)

    print(f"[score] 候选点 {len(grid):,} · 距现有站>2km 且入榜 {len(ranked)} 行 -> {common.OPPORTUNITIES_CSV.name}")
    for rec in summary["topCandidates"]:
        print(f"[score]   {rec['city_id']} ({rec['candidate_lat']},{rec['candidate_lon']}) "
              f"opp={rec['opportunity']:.1f} absorb={rec['absorb']:.1f} cov={rec['coverage']:.2f} "
              f"nearest={rec['nearest_station_km']:.1f}km")
    print("[score] 提醒：本产物 NOT-BACKTEST-VALIDATED（需求场空间外推在本批为负）")
    return summary


if __name__ == "__main__":
    main()
