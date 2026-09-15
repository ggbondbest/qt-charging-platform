"""从遥测装配站×5 分钟 tick 的负荷帧（只读聚合，零新增数据）。

产出两样：

* ``tick_features.pkl``——预测腿的模型输入。每个 (站, tick) 一行：
  目标 ``total_kw_next`` = 下一 tick 站级总充电功率；特征只用**当 tick 及更早**的量
  （``load_lag1`` 是当 tick 实测、``load_lag2..12`` 是更早、``load_mean_1h``/``load_max_1h``
  是含当 tick 的回溯 12-tick 窗口），日历量（tod/dow 正余弦）按定义无泄漏。
  不可进样本的行统一标在 ``dropped``（三类，逐类计数冻进 summary 的 ``droppedRows``）：
  **head** 滞后窗口不全（前 ``LAG_TAIL-1`` 个 tick）、**tail** 目标越出栅格右端、
  **cross** 目标落到别的段或段外（1-tick purge）。
* ``tick_demands.parquet``——分配腿的逐桩需求长表：每个 (站, tick, 充电中桩) 一行，
  ``demand_kw`` 取该桩实测充电功率、``wait_min`` 取该会话到当 tick 的已充电时长
  （priority_wait 的等待代理）。压力回放按站×tick 聚成需求向量。

诚实口径：站变压器恒 360kW，本批实测站×tick 总负荷峰值远低于它——"过载从未发生"是这里
顺手核出的负事实，冻进 summary 供 README 与报告引用。装配走"站×tick 宽矩阵 + shift(axis=1)"：
滞后即列方向平移，天然只用过去列，杜绝逐组 shift 的 dtype 对齐坑。

用法（仓库根目录）：python -m data_analysis.ml.transformer.features
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from . import common

#: 滞后阶数：lag1..lagLAG_TAIL（12 个 tick = 1 小时）。回溯窗口同宽。
LAG_TAIL = 12


def _naive_utc(series: pd.Series) -> pd.Series:
    """遥测时间戳 → UTC naive（与发布批其它表同基准）。"""
    return pd.to_datetime(series, utc=True).dt.tz_localize(None)


def load_telemetry() -> pd.DataFrame:
    """读遥测，裁成本线要的列，时刻转 naive UTC。"""
    tel = common.load_clean_table(
        "charger_telemetry",
        ["charger_id", "station_id", "recorded_at", "state", "power_kw", "session_id"])
    tel["recorded_at"] = _naive_utc(tel["recorded_at"])
    tel["power_kw"] = pd.to_numeric(tel["power_kw"], errors="coerce")
    return tel


def _wide(values: pd.DataFrame, value: str, aggfunc, tick_axis: pd.DatetimeIndex,
          stations) -> pd.DataFrame:
    """把 (站, tick) 长表透视为宽矩阵（行=站、列=tick、对齐到完整栅格），缺格 fill 0。"""
    wide = values.pivot_table(index="station_id", columns="tick_ts", values=value, aggfunc=aggfunc)
    wide = wide.reindex(index=list(stations), columns=tick_axis).fillna(0.0)
    wide.columns.name = None
    wide.index.name = None
    return wide.astype("float64")


def _shift_right(matrix: np.ndarray, k: int) -> np.ndarray:
    """列方向右移 k（t 处取 t-k，头部补 NaN）。k=0 也**必须返回副本**：返回入参本身会让
    ``load`` 与 ``load_lag1..3`` 是同一块 buffer，``np.save`` 对 views 去重后 tick 帧只剩 1/4 大小
    （真实数值仍对，但"每站×每 tick 稠密"的帧被压缩成看不见的共享列，下游改一列会串改四列）。

    ``k >= 列数`` 时整列都是 NaN——不加这个守卫，``matrix[:, :ncol-k]`` 会变成负切片，
    numpy 直接抛 broadcast 错（真实栅格 51,840 列永远碰不到，短栅格的合成用例必然撞到）。
    """
    out = np.full_like(matrix, np.nan)
    ncol = matrix.shape[1]
    if k >= ncol:
        return out
    out[:, k:] = matrix[:, :ncol - k]
    return out


def _shift_left(matrix: np.ndarray, k: int) -> np.ndarray:
    """列方向左移 k（t 处取 t+k，尾部补 NaN）——下一 tick 目标。同 ``_shift_right`` 的越界守卫。"""
    out = np.full_like(matrix, np.nan)
    ncol = matrix.shape[1]
    if k >= ncol:
        return out
    out[:, :ncol - k] = matrix[:, k:]
    return out


def build_tick_frame(tel: pd.DataFrame) -> pd.DataFrame:
    """稠密站×tick 总负荷 + 当 tick 及更早的滞后特征 + 下一 tick 目标。

    宽矩阵 (站×tick) 上做 numpy 行优先 ravel，用 ``np.repeat``/``np.tile`` 显式对齐坐标——
    刻意不用 ``stack()``：滞后/rolling 列在边界是 NaN，而 ``stack`` 默认丢 NaN 会让各列长度不齐。
    """
    charging = tel[(tel["state"] == "CHARGING") & tel["power_kw"].notna() & (tel["power_kw"] > 0)]
    charging = charging.rename(columns={"recorded_at": "tick_ts"})
    tick_axis = pd.DatetimeIndex(np.sort(tel["recorded_at"].unique()))
    stations = np.sort(charging["station_id"].unique())

    load = _wide(charging, "power_kw", "sum", tick_axis, stations).to_numpy()   # (S,T)
    active = _wide(charging, "charger_id", "count", tick_axis, stations).to_numpy()
    nrow, ncol = load.shape

    idx_station = np.repeat(stations, ncol)
    idx_tick = np.tile(tick_axis, nrow)
    long = pd.DataFrame({"station_id": idx_station, "tick_ts": idx_tick,
                         "total_kw": load.ravel(), "n_active": active.ravel()})
    long["total_kw_next"] = _shift_left(load, 1).ravel()
    for k in range(1, LAG_TAIL + 1):                       # lag1 = 当 tick；lag_k = 更早第 k-1 tick
        long[f"load_lag{k}"] = _shift_right(load, k - 1).ravel()
    wide = pd.DataFrame(load)                              # 回溯窗口只在列方向，rolling 安全
    roll = wide.T.rolling(LAG_TAIL, min_periods=LAG_TAIL)
    long["load_mean_1h"] = roll.mean().T.to_numpy().ravel()
    long["load_max_1h"] = roll.max().T.to_numpy().ravel()

    ts = long["tick_ts"]
    minutes = ts.dt.hour * 60 + ts.dt.minute
    long["tod_sin"] = np.sin(2 * np.pi * minutes / 1440.0)
    long["tod_cos"] = np.cos(2 * np.pi * minutes / 1440.0)
    long["dow_sin"] = np.sin(2 * np.pi * ts.dt.dayofweek / 7.0)
    long["dow_cos"] = np.cos(2 * np.pi * ts.dt.dayofweek / 7.0)

    long["tick_index"] = np.tile(np.arange(ncol), nrow).astype("int32")
    long["split"] = common.assign_split(long["tick_ts"]).to_numpy()

    # 不可进样本的三类行，合成一列 ``dropped``（旧名 dropped_last 只覆盖了后两类，且文档声称覆盖
    # 前一类而代码没做——栅格起点若贴住 split 起点，带 NaN 滞后的头部行就会静默进 TRAIN）：
    #   head  滞后窗口不全（lag12 / 1h 回溯需要前面 LAG_TAIL-1 个 tick）
    #   tail  目标越出栅格右端
    #   cross 目标落到别的段或段外（1-tick purge）
    head_incomplete = (long["tick_index"] < LAG_TAIL - 1).to_numpy()
    next_index = long["tick_index"] + 1
    next_ts = tick_axis.to_numpy()[np.clip(next_index, 0, ncol - 1)]
    next_split = common.assign_split(pd.Series(next_ts, dtype="datetime64[ns]")).to_numpy()
    tail_out = (next_index >= ncol).to_numpy()
    cross_split = next_split != long["split"].to_numpy()
    long["dropped"] = head_incomplete | tail_out | cross_split
    drop_reasons = {"headIncompleteLag": int(head_incomplete.sum()),
                    "tailOutOfGrid": int(tail_out.sum()),
                    "crossSplitPurged": int(cross_split.sum())}

    stations_meta = common.load_clean_table(
        "stations", ["station_id", "city_id", "site_type", "transformer_kw"]).set_index("station_id")
    for column in ["city_id", "site_type"]:
        long[column] = long["station_id"].map(stations_meta[column])
    long["transformer_kw"] = long["station_id"].map(stations_meta["transformer_kw"]).astype("float64")
    assert long["transformer_kw"].notna().all(), "站缺 transformer_kw，容量口径不成立"
    frame = long.reset_index(drop=True)
    return frame, drop_reasons


def build_demand_long(tel: pd.DataFrame) -> pd.DataFrame:
    """分配腿：逐 (站, tick, 充电中桩) 需求长表 + 会话已充电时长（等待代理）。"""
    charging = tel[(tel["state"] == "CHARGING") & tel["power_kw"].notna() & (tel["power_kw"] > 0)]
    charging = charging.rename(columns={"recorded_at": "tick_ts"})
    charging = charging.sort_values(["session_id", "tick_ts"], kind="stable")
    first = charging.groupby("session_id", as_index=False)["tick_ts"].min().rename(
        columns={"tick_ts": "session_start"})
    long = charging[["station_id", "tick_ts", "charger_id", "session_id", "power_kw"]].rename(
        columns={"power_kw": "demand_kw"}).merge(first, on="session_id", how="left", validate="many_to_one")
    long["wait_min"] = (long["tick_ts"] - long["session_start"]).dt.total_seconds().to_numpy() / 60.0
    return long.drop(columns=["session_start"]).reset_index(drop=True)


def numeric_feature_columns() -> list[str]:
    return ([f"load_lag{k}" for k in range(1, LAG_TAIL + 1)]
            + ["load_mean_1h", "load_max_1h", "tod_sin", "tod_cos", "dow_sin", "dow_cos"])


def main() -> dict:
    common.require_empty_run_dir(common.OUT_DIR)
    common.verify_batch()
    tel = load_telemetry()
    frame, drop_reasons = build_tick_frame(tel)
    demands = build_demand_long(tel)

    peak = float(frame["total_kw"].max())
    over_ticks = int((frame["total_kw"] > frame["transformer_kw"]).sum())
    quant = frame["total_kw"].quantile([0.5, 0.99, 1.0]).to_dict()

    feature_cols = numeric_feature_columns()
    usable = frame[~frame["dropped"] & frame["split"].isin(["TRAIN", "VALIDATION", "TEST"])]
    split_counts = usable["split"].value_counts().to_dict()
    # 假不变式的正面守卫：进了样本的行，一个 NaN 特征都不许有（历史上靠"栅格头部恰好落 EXCLUDED"
    # 侥幸成立，现在由 dropped 的 head 分支保证，并在此处钉死）。
    assert usable[feature_cols].notna().all().all(), "usable 样本里仍有 NaN 特征，滞后头部护栏失效"

    common.write_new_pickle(common.TICK_FEATURES_PATH, frame)
    common.write_new_parquet(common.DEMAND_LONG_PATH, demands)

    manifest = common.read_source_manifest()
    summary = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": manifest["pipelineRunId"],
        "datasetId": common.DATASET_ID,
        "sampleUnit": "station × 5-minute tick",
        "decisionTime": "当 tick 起点（用当 tick 及更早的实测负荷，预测下一 tick）",
        "label": {"name": "total_kw_next",
                  "definition": "下一 tick（+300s）站级 CHARGING 总功率（kW）"},
        "features": {"numeric": feature_cols, "categorical": [], "groups": {"all": feature_cols}},
        "tickRows": int(len(frame)), "stations": int(frame["station_id"].nunique()),
        "ticksPerStation": int(frame["tick_index"].max() + 1),
        "usableBySplit": {k: int(v) for k, v in split_counts.items()},
        "loadCapCheck": {
            "transformerKw": common.TRANSFORMER_KW,
            "gridTicks": int(len(frame)),
            "chargingTicks": int((frame["n_active"] > 0).sum()),
            "maxStationTickKw": round(peak, 3),
            "medianKw": round(float(quant[0.5]), 3),
            "p99Kw": round(float(quant[0.99]), 3),
            "ticksOverCap": over_ticks,
            "note": ("口径分两把：gridTicks 是稠密站×tick 全格（空闲格计 0kW），chargingTicks 是"
                     "至少一桩在充的格。medianKw 是稠密口径（含空闲），峰值/越限判定两口径同为 0。"),
            "headline": ("当前 360kW 额定下站×tick 总负荷从不越限（过载是负事实）；"
                         "分配策略的区分度只在收紧容量的压力测试里" if over_ticks == 0 else
                         "存在越限 tick，需真实限流"),
        },
        "droppedRows": drop_reasons,
        "demandLongRows": int(len(demands)),
        "featuresSha256": common.sha256_file(common.TICK_FEATURES_PATH),
        "demandLongSha256": common.sha256_file(common.DEMAND_LONG_PATH),
        "boundaries": {k: v.isoformat() for k, v in
                       common.split_boundaries(manifest).items()},
        "command": common.invocation("data_analysis.ml.transformer.features"),
        "pythonVersions": common.dependency_versions(),
        "builtAt": common.stamp(),
        "ignoredColumns": common.IGNORED_COLUMNS,
        "disclaimer": common.data_note(),
    }
    common.write_new_json(common.TICK_SUMMARY_PATH, summary)

    print(f"[features] 站×tick {len(frame):,} 行 · {summary['stations']} 站 × {summary['ticksPerStation']:,} tick")
    print(f"[features] 落段可用 {summary['usableBySplit']}")
    print(f"[features] 负荷峰值 {peak:.1f}kW · p99 {quant[0.99]:.1f}kW · 越 360kW 的 tick = {over_ticks}")
    print(f"[features] 分配需求长表 {len(demands):,} 行 -> {common.DEMAND_LONG_PATH.name}")
    print(f"[features] -> {common.TICK_SUMMARY_PATH}")
    return summary


if __name__ == "__main__":
    main()
