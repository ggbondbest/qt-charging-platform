"""构建 (事件×同城5候选) 长表:每次 STARTED 选址是一个事件,标签=用户实际选中的站。
纪律:所有历史特征严格"截至事件时刻之前"——日级聚合先右移 1 天再滚动,
用户×站点配对特征用 merge_asof(allow_exact_matches=False) 取严格更早的最近一条并携带"含该行"的累计计数,杜绝未来泄漏。
ABANDONED/FAILED 不进标签,只汇入站点侧滚动统计(弃单率、排队)。产物:outputs/ml_recommend/recommend_matrix.pkl + build_summary.json。

用法(仓库根目录):python -m data_analysis.ml.recommend.build_data
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import numpy as np
import pandas as pd

from . import common

CATEGORICAL = ["site_type", "membership", "segment", "vehicle_class"]
# 这些列留在表里做分组/溯源,但绝不能进特征(chosen_station_id 进特征=直接泄答案)
NON_FEATURE = {"event_id", "attempt_id", "cand_station_id", "label", "split", "user_id",
               "vehicle_id", "reference_time", "chosen_station_id", "city_id", "day_key",
               "day_shift", "opened_at"}


def _daily_wide(df: pd.DataFrame, value: str | None, aggfunc: str, stations: list[str],
                days: pd.DatetimeIndex) -> pd.DataFrame:
    """(day×station) 宽表;缺失日/站补 0。value=None 时数行数。"""
    if value is None:
        wide = df.groupby([df["day"], "station_id"]).size().unstack(fill_value=0).astype(float)
    else:
        wide = df.groupby([df["day"], "station_id"])[value].agg(aggfunc).unstack(fill_value=0).astype(float)
    return wide.reindex(index=days, columns=stations).fillna(0.0)


def _rolled(wide: pd.DataFrame) -> pd.DataFrame:
    """逐站 28 天滚动和,整体右移 1 天:第 d 行只含 d 之前的 28 天。"""
    return (wide.rolling(common.ROLLING_DAYS, min_periods=1).sum()
            .shift(1))


def station_rolling_features(att, sess, rev, queue, stations, days) -> pd.DataFrame:
    """站点日级滚动原料 → (station, day) 索引的比率特征,join 时按"事件前一日起算"对齐。"""
    started = att[att.outcome == "STARTED"]
    sums = {
        "attempts": _daily_wide(att, None, None, stations, days),
        "started": _daily_wide(started, None, None, stations, days),
        "abandon_nc": _daily_wide(att[att.failure_reason == "NO_AVAILABLE_CHARGER"],
                                  None, None, stations, days),
        "abandon_queue": _daily_wide(att[att.failure_reason == "QUEUE_PATIENCE"],
                                     None, None, stations, days),
        "sessions": _daily_wide(sess, None, None, stations, days),
        "energy_kwh": _daily_wide(sess, "energy_wh", "sum", stations, days) / 1000.0,
        "fee_cents": _daily_wide(sess, "total_fee_cents", "sum", stations, days),
        "rating_sum": _daily_wide(rev, "rating", "sum", stations, days),
        "rating_n": _daily_wide(rev, None, None, stations, days),
        "rating_low": _daily_wide(rev[rev.rating <= 2], None, None, stations, days),
        "queue_n": _daily_wide(queue, None, None, stations, days),
        "queue_pos": _daily_wide(queue, "position_at_join", "sum", stations, days),
    }
    roll = {k: _rolled(v) for k, v in sums.items()}
    feats = pd.DataFrame({
        "station_starts_r28": roll["started"].stack(),
        "station_attempts_r28": roll["attempts"].stack(),
        "station_fee_per_kwh_r28": (roll["fee_cents"] / roll["energy_kwh"].replace(0, np.nan)).stack(),
        "station_mean_rating_r28": (roll["rating_sum"] / roll["rating_n"].replace(0, np.nan)).stack(),
        "station_low_rating_share_r28": (roll["rating_low"] / roll["rating_n"].replace(0, np.nan)).stack(),
        "station_started_rate_r28": (roll["started"] / roll["attempts"].replace(0, np.nan)).stack(),
        "station_abandon_nc_rate_r28": (roll["abandon_nc"] / roll["attempts"].replace(0, np.nan)).stack(),
        "station_abandon_queue_rate_r28": (roll["abandon_queue"] / roll["attempts"].replace(0, np.nan)).stack(),
        "station_queue_mean_pos_r28": (roll["queue_pos"] / roll["queue_n"].replace(0, np.nan)).stack(),
        "station_reviews_n_r28": roll["rating_n"].stack(),
    })
    # stack 出来是 (day, station);join 键要 (station, day)
    feats = feats.reorder_levels([1, 0]).sort_index()
    feats.index.names = ["station_id", "day"]
    return feats


def hour_load_feature(sess: pd.DataFrame, stations: list[str],
                      days: pd.DatetimeIndex) -> pd.DataFrame:
    """(station, hour) 近 7 天(不含当日)同时段 START 次数。"""
    grid = sess.groupby(["station_id", "hour", "day"]).size()
    full = grid.rename("cnt").reset_index().set_index(["station_id", "hour", "day"])["cnt"]
    full = full.reindex(pd.MultiIndex.from_product([stations, range(24), days],
                                                    names=["station_id", "hour", "day"])).fillna(0.0)
    rolled = (full.groupby(level=["station_id", "hour"])
              .transform(lambda s: s.rolling(common.HOUR_LOOKBACK_DAYS, min_periods=1).sum().shift(1)))
    out = rolled.rename("station_hour_starts_prev7").reset_index()
    return out


def pair_asof(cand: pd.DataFrame, right: pd.DataFrame, right_time: str,
              cols: list[str]) -> pd.DataFrame:
    """逐候选站把历史流 merge_asof 到候选行:by=user_id、backward、拒绝恰好相等 → 只取严格早于事件时刻的最近一条。
    right 需含 user_id/station_id/<right_time>;携带的 cols 应为"含该行"的累计量,匹配到的最后一条即等于"t 之前全部"。"""
    parts = []
    for sid, block in cand.groupby("cand_station_id", sort=True):
        # user_id 是 by 键必须留着;station_id 是过滤残留,删掉防列名打架
        r = right[right.station_id == sid].sort_values(right_time).drop(columns="station_id")
        parts.append(pd.merge_asof(block.sort_values("reference_time"), r,
                                   left_on="reference_time", right_on=right_time,
                                   by="user_id", direction="backward", allow_exact_matches=False))
    merged = pd.concat(parts, ignore_index=True).set_index("row_key")
    return merged[cols + [right_time]].reindex(cand["row_key"].to_numpy()).reset_index(drop=True)


def main() -> int:
    common.ensure_out_dir()
    att = common.load_table("charging_attempts")
    sess = common.load_table("charging_sessions")
    rev = common.load_table("reviews")
    users = common.load_table("users")
    vehicles = common.load_table("vehicles")
    stations = common.load_table("stations")
    chargers = common.load_table("chargers")
    queue = common.load_table("queue_entries")

    att["attempted_at"] = pd.to_datetime(att["attempted_at"])
    att["day"] = att["attempted_at"].dt.normalize()
    att["started_flag"] = (att["outcome"] == "STARTED").astype(float)
    sess["started_at"] = pd.to_datetime(sess["started_at"])
    sess["day"] = sess["started_at"].dt.normalize()
    sess["hour"] = sess["started_at"].dt.hour
    rev["created_at"] = pd.to_datetime(rev["created_at"])
    rev["day"] = rev["created_at"].dt.normalize()
    queue["joined_at"] = pd.to_datetime(queue["joined_at"])
    queue["day"] = queue["joined_at"].dt.normalize()

    days = pd.DatetimeIndex(pd.date_range(att["day"].min(), att["day"].max(), freq="D"))
    station_ids = stations["station_id"].tolist()

    # ---- 事件:STARTED 的 attempt;候选=本城 5 站 ----
    events = att.loc[att.outcome == "STARTED",
                     ["attempt_id", "user_id", "vehicle_id", "station_id", "attempted_at"]].copy()
    events = events.rename(columns={"station_id": "chosen_station_id",
                                    "attempted_at": "reference_time", "attempt_id": "event_id"})
    city_of = stations.set_index("station_id")["city_id"]
    events["city_id"] = events["chosen_station_id"].map(city_of)
    events["hour"] = events["reference_time"].dt.hour
    events["day_key"] = events["reference_time"].dt.normalize()
    events["split"] = common.split_of(events["reference_time"])

    # ---- 用户全局历史(全部 attempt 流;当事行自身即"截至 t"口径)----
    a_sorted = att.sort_values("attempted_at").copy()
    a_sorted["pair_attempts_incl"] = a_sorted.groupby(["user_id", "station_id"]).cumcount() + 1
    a_sorted["pair_started_incl"] = a_sorted.groupby(["user_id", "station_id"])["started_flag"].cumsum()
    a_sorted["user_attempts_before"] = a_sorted.groupby("user_id").cumcount() + 1  # 含当事
    a_sorted["user_last_attempt"] = a_sorted.groupby("user_id")["attempted_at"].shift(1)
    ev = a_sorted.set_index("attempt_id").reindex(events["event_id"].to_numpy())
    # 当事 attempt 计入了自身:事件前的用户总量 = incl-1
    events["user_attempts_before"] = ev["user_attempts_before"].to_numpy() - 1
    events["user_days_since_last_any"] = (
        (events["reference_time"].to_numpy() - ev["user_last_attempt"].to_numpy())
        .astype("timedelta64[ns]").astype("float64") / 86400e9)

    cand = events.merge(stations[["station_id", "city_id", "site_type", "transformer_kw", "opened_at"]],
                       on="city_id", how="inner")
    cand["label"] = (cand["station_id"] == cand["chosen_station_id"]).astype(int)
    cand = cand.rename(columns={"station_id": "cand_station_id"})
    cand = cand.sort_values(["event_id", "cand_station_id"]).reset_index(drop=True)
    cand["row_key"] = np.arange(len(cand))
    assert len(cand) == 5 * len(events), "候选展开不是 5 倍,reshape 前提不成立"
    assert cand.groupby("event_id", sort=False)["label"].sum().eq(1).all(), "存在无正例事件"

    # ---- 静态与匹配 ----
    ch = chargers.groupby("station_id")["rated_power_kw"].agg(
        max_rated_kw="max", sum_rated_kw="sum", n_chargers="count")
    cand = cand.join(ch, on="cand_station_id")
    opened = stations.set_index("station_id")["opened_at"]
    cand["station_age_days"] = (
        (cand["reference_time"] - cand["cand_station_id"].map(opened)).dt.total_seconds() / 86400.0)
    veh = vehicles.set_index("vehicle_id")
    cand["vehicle_battery_kwh"] = cand["vehicle_id"].map(veh["battery_capacity_kwh"])
    cand["vehicle_max_charge_kw"] = cand["vehicle_id"].map(veh["max_charge_kw"])
    cand["vehicle_class"] = cand["vehicle_id"].map(veh["vehicle_class"])
    cand["eff_max_power_kw"] = np.minimum(cand["vehicle_max_charge_kw"], cand["max_rated_kw"])
    cand["power_surplus_kw"] = cand["max_rated_kw"] - cand["vehicle_max_charge_kw"]
    usr = users.set_index("user_id")
    cand["membership"] = cand["user_id"].map(usr["membership"])
    cand["segment"] = cand["user_id"].map(usr["segment"])
    cand["account_age_days"] = (
        (cand["reference_time"] - cand["user_id"].map(usr["registered_at"])).dt.total_seconds() / 86400.0)
    cand["day_of_week"] = cand["reference_time"].dt.dayofweek
    cand["is_weekend"] = (cand["day_of_week"] >= 5).astype(int)

    # ---- 站点滚动(事件前一日起算)----
    cand["day_shift"] = (cand["reference_time"] - pd.Timedelta(days=1)).dt.normalize()
    sdf = station_rolling_features(att, sess, rev, queue, station_ids, days).reset_index()
    cand = cand.merge(sdf, left_on=["cand_station_id", "day_shift"],
                      right_on=["station_id", "day"], how="left").drop(columns=["station_id", "day"])
    hlf = hour_load_feature(sess, station_ids, days).rename(
        columns={"station_id": "h_station", "hour": "h_hour", "day": "h_day"})
    cand = cand.merge(hlf, left_on=["cand_station_id", "hour", "day_shift"],
                      right_on=["h_station", "h_hour", "h_day"], how="left").drop(
        columns=["h_station", "h_hour", "h_day"])  # 特征 hour 原样保留

    # ---- 用户×候选站 配对历史(严格更早)----
    pair = pair_asof(cand, a_sorted, "attempted_at",
                     ["pair_attempts_incl", "pair_started_incl"])
    cand["pair_attempts_before"] = pair["pair_attempts_incl"].fillna(0.0).to_numpy()
    cand["pair_started_before"] = pair["pair_started_incl"].fillna(0.0).to_numpy()
    cand["pair_days_since_last_visit"] = (
        (cand["reference_time"] - pair["attempted_at"]).dt.total_seconds() / 86400.0).to_numpy()
    r_sorted = rev.sort_values("created_at").copy()
    rg = r_sorted.groupby(["user_id", "station_id"])
    r_sorted["rating_exp_mean"] = rg["rating"].cumsum() / (rg.cumcount() + 1)  # 含当行的扩张均值
    rating = pair_asof(cand, r_sorted, "created_at", ["rating_exp_mean"])
    cand["user_rating_given"] = rating["rating_exp_mean"].to_numpy()

    # ---- 忠诚度原料:该候选是否恰为用户最近光顾过的站 ----
    cand["last_visited_is_cand"] = (
        cand["pair_days_since_last_visit"]
        .eq(cand.groupby("event_id")["pair_days_since_last_visit"].transform("min"))
        & cand["pair_days_since_last_visit"].notna()).astype(int)

    for column in CATEGORICAL:
        cand[column] = pd.Categorical(cand[column].astype(str),
                                       categories=sorted(cand[column].astype(str).unique()))
    cand = cand.drop(columns=["row_key"])

    feature_columns = [c for c in cand.columns if c not in NON_FEATURE]
    # 守卫:特征只许数值或 category;datetime/object(如 opened_at)混进来直接拦下
    bad = [c for c in feature_columns
           if not (pd.api.types.is_numeric_dtype(cand[c]) or isinstance(cand[c].dtype, pd.CategoricalDtype))]
    if bad:
        raise RuntimeError(f"非数值特征混入: {bad}")
    summary = {
        "datasetId": common.DATASET_ID,
        "events": int(len(events)),
        "candidateRows": int(len(cand)),
        "splitEvents": events["split"].value_counts().to_dict(),
        "splitRowCounts": cand["split"].value_counts().to_dict(),
        "featureColumns": feature_columns,
        "categorical": CATEGORICAL,
        "rollingDays": common.ROLLING_DAYS,
        "hourLookbackDays": common.HOUR_LOOKBACK_DAYS,
        "splits": {"trainEndExclusive": str(common.TRAIN_END_EXCLUSIVE.date()),
                   "validEndExclusive": str(common.VALID_END_EXCLUSIVE.date()),
                   "testEndExclusive": str(common.TEST_END_EXCLUSIVE.date())},
        "warmupNaNShare": {c: round(float(cand[c].isna().mean()), 4)
                           for c in feature_columns if cand[c].isna().any()},
    }
    # 两件套都走 tmp+os.replace(负荷线复审同款纪律):中断只留 .tmp,不留半截表/半截 JSON;
    # summary 最后替换——下游若以 summary 为准,看到的永远是对应一份完整表的摘要。
    tmp_table = Path(str(common.LONG_TABLE) + ".tmp")
    cand.to_pickle(tmp_table)
    os.replace(tmp_table, common.LONG_TABLE)
    tmp_summary = Path(str(common.SUMMARY_PATH) + ".tmp")
    with open(tmp_summary, "w", encoding="utf-8") as handle:
        json.dump(summary, handle, ensure_ascii=False, indent=2)
    os.replace(tmp_summary, common.SUMMARY_PATH)
    print(f"events={len(events)} rows={len(cand)} -> {common.LONG_TABLE}")
    print(json.dumps({k: summary[k] for k in ("splitEvents", "splits")}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
