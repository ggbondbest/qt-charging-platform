"""候选网格机会打分：在每城站点的包围盒内撒网格点，按"可吸收需求 ×（1−现有覆盖）"排序。

这是选址线的**工具**——确定性、可复跑、给出候选地块的机会排序。但它是同一个需求场模型打的分，
而那个模型的外推力已被 `backtest.py` 证伪，所以这些排序是**机械演示**、不是可信选址建议。
本脚本因此**强制**先有 `backtest.json` 才肯落盘（引用别人的结论，就得读得到那份结论），
并把回测读数原样写进 CSV 每一行的 `model_status` / `backtest_field_spearman` 两列——
CSV 会被单独拷走、单独打开，状态标记不能只活在 JSON 侧文件里。

准确的说法（勿写成"回测说没有信号"）：本批城市总量近恒定、恰在 5 站间零和切分，
"用其余站需求预测被藏站"**代数上必然**为负，故负号本身不含信息量；有信息量的是场模型落在
**城内置换零线**之下（它学的是倒置结构），以及同一批数据里 `site_type` 可外推。
详见 backtest.md 的三层结论。需求口径这里用
`demand_incl_unmet = 会话数 + ABANDONED 弃队数`——把"想充没充上"的未满足需求也算进可吸收面
（其中 476 条弃队 24h 内同人同站又充上了，属双计；`CALL_EXPIRED` 未并入，见 common 模块 docstring）。

用法（仓库根目录）：先 python -m data_analysis.ml.siting.backtest，再 python -m data_analysis.ml.siting.score
"""

from __future__ import annotations

import json

import pandas as pd

from . import common, field

#: 包围盒向外扩的经纬度缓冲（≈±9km）与网格步长（≈2.2km）。假设参数，写进产物。
PAD_DEG = 0.08
STEP_DEG = 0.02
TOP_N_PER_CITY = 5
#: 入榜的最小站距（km）：小于它就是在既有站旁边加建，不是"新地块"。
MIN_DIST_FROM_EXISTING_KM = 2.0


def read_backtest_verdict() -> dict:
    """引用回测结论就**必须**读得到它：缺文件即中止，不靠 docstring 里的记忆写免责声明。"""
    if not common.BACKTEST_JSON.exists():
        raise FileNotFoundError(
            f"缺少 {common.BACKTEST_JSON.name}：本脚本的免责声明与 CSV 状态列都从它取数，"
            "请先跑 python -m data_analysis.ml.siting.backtest")
    with open(common.BACKTEST_JSON, encoding="utf-8") as handle:
        report = json.load(handle)
    if report.get("publishedBatchId") != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch(
            f"{common.BACKTEST_JSON.name} 绑的批次 {report.get('publishedBatchId')!r} "
            f"!= 本线 {common.EXPECTED_PUBLISHED_BATCH_ID!r}，请先重跑 backtest")
    radius = str(common.CAPTURE_KM)
    by_radius = report.get("losobyRadius", {}).get(radius)
    nulls = report.get("permutationNulls", {}).get(radius, {}).get("nulls", {})
    if not by_radius or not nulls.get("inCity"):
        raise common.BatchMismatch(f"{common.BACKTEST_JSON.name} 缺 r={radius}km 的读数，请重跑 backtest")
    return {
        "fieldSpearmanAtCaptureKm": by_radius["field"]["spearman"],
        "inCityNullMean": nulls["inCity"]["nullMean"],
        "inCityShareAtOrBelowObserved": nulls["inCity"]["shareOfNullAtOrBelowObserved"],
        "siteTypeShareSpearman": report.get("siteTypeSignal", {}).get("losoOnWithinCityShare", {}).get("spearman"),
        "captureKm": common.CAPTURE_KM,
        "backtestFile": common.BACKTEST_JSON.name,
    }


def main() -> dict:
    common.require_empty_run_dir(common.OUT_DIR, extra_allowed=(common.BACKTEST_JSON.name,
                                                                common.BACKTEST_MD.name))
    common.verify_batch()
    backtest = read_backtest_verdict()
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
    ranked = (grid[grid["nearest_station_km"] > MIN_DIST_FROM_EXISTING_KM]
              .sort_values("opportunity", ascending=False)
              .groupby("city_id", group_keys=False).head(TOP_N_PER_CITY).reset_index(drop=True))
    for column in ["candidate_lat", "candidate_lon", "absorb", "coverage", "opportunity", "nearest_station_km"]:
        ranked[column] = ranked[column].round(4)
    # 状态写进表体：CSV 单独流传时，读它的人也要能立刻看到"这不是可信建议"以及凭什么。
    ranked["model_status"] = "NOT-BACKTEST-VALIDATED"
    ranked["backtest_field_spearman"] = backtest["fieldSpearmanAtCaptureKm"]
    ranked["backtest_incity_null_spearman"] = backtest["inCityNullMean"]
    common.write_new_csv(common.OPPORTUNITIES_CSV, ranked)

    summary = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": common.read_source_manifest()["pipelineRunId"],
        "datasetId": common.DATASET_ID,
        "status": "NOT-BACKTEST-VALIDATED",
        "backtestReadback": backtest,
        "warning": ("候选打分与 backtest 用的是同一个需求场模型，其留一站外推力在本批为负、且低于"
                    "城内置换零线（读数见 backtestReadback，随产物一起写进了 CSV 每一行）——"
                    "在有真实人流/POI 需求面前，这些排序不应被当作可信选址建议。"
                    "负号的含义请读 backtest.md 的三层结论：城市总量近恒定的零和切分使"
                    "'用邻居外推'代数上必然为负，故结论不是'没有空间信号'，而是"
                    "'本批的可学规律在站型（site_type 份额 LOSO "
                    f"{backtest['siteTypeShareSpearman']}），不在空间位置'。"),
        "demandBasis": "demand_incl_unmet = 会话数 + ABANDONED 弃队数（未满足需求代理；"
                       "其中 476 条 24h 内已复充 = 双计，CALL_EXPIRED 未并入）",
        "gridParams": {"padDeg": PAD_DEG, "stepDeg": STEP_DEG,
                       "captureKm": common.CAPTURE_KM, "topNPerCity": TOP_N_PER_CITY,
                       "minDistFromExistingKm": MIN_DIST_FROM_EXISTING_KM},
        "candidatePoints": int(len(grid)), "rankedRows": int(len(ranked)),
        "abandonedInDemand": int(intensity["abandoned"].sum()),
        "sourceTablesSha256": common.source_table_digests(),
        "topCandidates": ranked.to_dict(orient="records"),
        "command": common.invocation("data_analysis.ml.siting.score"),
        "pythonVersions": common.dependency_versions(), "builtAt": common.stamp(),
        "disclaimer": common.data_note(),
    }
    common.write_new_json(common.SCORE_JSON, summary)

    print(f"[score] 候选点 {len(grid):,} · 距现有站>{MIN_DIST_FROM_EXISTING_KM:g}km 且入榜 {len(ranked)} 行 "
          f"-> {common.OPPORTUNITIES_CSV.name}")
    for rec in summary["topCandidates"]:
        print(f"[score]   {rec['city_id']} ({rec['candidate_lat']},{rec['candidate_lon']}) "
              f"opp={rec['opportunity']:.1f} absorb={rec['absorb']:.1f} cov={rec['coverage']:.2f} "
              f"nearest={rec['nearest_station_km']:.1f}km")
    print(f"[score] 回测读数（r={common.CAPTURE_KM}km）：field spearman={backtest['fieldSpearmanAtCaptureKm']} "
          f"vs 城内置换零线 {backtest['inCityNullMean']} -> 已写进 CSV 状态列")
    print("[score] 提醒：本产物 NOT-BACKTEST-VALIDATED（需求场空间外推在本批为负、"
          "且低于城内置换零线 → 排序是机械演示，不是可信选址建议）")
    return summary


if __name__ == "__main__":
    main()
