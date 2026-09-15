"""特征层：一次会话在 ``started_at`` 一刻能看到的一切，看不到此后发生的任何事。

决策时点写死成一个值：``started_at``。所有 as-of 量以它为查询时刻、一律"严格早于"。本线的
核心难点比第七线多一维——**历史的两条可见时间轴**：

  · **次数类**（该用户/该站到此为止一共开过几场充电）：一条会话在 ``started_at`` 就宣告存在，
    可见时间轴 = ``started_at``；
  · **超占数值类**（该用户历史上占桩多久、超时率多少）：一次会话"充完后占不占、占多久"要等
    **拔枪**那一刻才成为事实。可见时间轴 = ``unplugged_at``，但查询时刻仍是本行的
    ``started_at``——用户上一单忘了挪车这件事，系统只有在他回来拔枪之后才知道。
    两条轴一旦混用（拿 started 轴查超占值），就会把"还没拔枪的上一次"当成已知，这是本线
    最隐蔽的一处未来信息。

其余口径：

  · **站点/桩/用户画像**（车型、会员、站点类型、接口、厂商）都是决策时点早已存在的属性，天然可用；
    ``registered_at`` 只以"开场时账号已注册多少天"的形式进特征。
  · **日历/天气**：周末与场景事件是计划量取当日；天气**只取 T−1 日及更早**的城×日聚合
    （``weather_hourly`` 的小时行按北京日历日聚合后移位，今天的天气在决策时点不可得）。
  · **排队侧**：本站近 2 小时/近 24 小时的加入数、当前在排人数（加入累计−离开累计）、
    历史弃队率——全部按 ``joined_at``/``resolved_at`` 严格早于查询时刻；"本会话是否排队而来"
    与其排队位次、被叫到开始充电的等待时长在 ``started_at`` 已是事实，可用。
  · **campaign 标记**：建表时核对每个 campaign_id 的窗口都覆盖对应会话的 ``started_at``，
    所以"有没有促销、折扣多少"是决策时点已知量。

禁入名单 ``FORBIDDEN_FEATURES`` 封死一切 ``ended_at`` 之后才存在的列（含费用六兄弟——其中
``parking_fee_cents`` 与标签的 Spearman 高达 0.44，**就是按超时时长收的钱**，是全表最直接的
答案马甲）与 ``status``/``stop_reason``/``end_soc_pct``/能量列。``verify_no_future_leak`` 用
**全表布尔掩码**独立重算 ``AUDITED_FEATURES`` 里 12 个跨轴 as-of 量（不复用排序/二分/
merge_asof/pivot-shift 代码路径），逐行要求 0 偏差。

用法（仓库根目录）：python -m data_analysis.ml.overstay.features
"""

from __future__ import annotations

import random

import numpy as np
import pandas as pd

from . import common

#: 事后 / 身份 / 结局列：留在表里做分组、溯源、评价与诊断，绝不进特征。
FORBIDDEN_FEATURES = frozenset({
    "session_id", "attempt_id", "user_id", "vehicle_id", "station_id", "charger_id", "city_id",
    "started_at", "ended_at", "unplugged_at", "business_date", "split",
    "over_min", "y_over", "duration_min",
    # —— ended_at 之后才存在的"答案马甲" ——
    "status", "stop_reason", "end_soc_pct", "energy_wh", "grid_energy_wh",
    "electricity_fee_cents", "service_fee_cents", "parking_fee_cents", "discount_cents",
    "total_fee_cents", "grid_cost_cents",
    # —— 身份与原始时间戳：只允许作为 as-of 键或以差值形式进入 ——
    "registered_at", "campaign_id", "queue_id", "queue_status", "called_at", "joined_at",
    "resolved_at",
})

#: 明确"读过但不用"的量，逐条留原因——换批次时一定有人问。dict 而非 frozenset（第七线教训：
#: 写成 ``frozenset({…dict…})`` 会只留键、把原因悄悄丢掉）。
IGNORED_COLUMNS = {
    "demand_multiplier": "仿真侧计划系数（第六线实测单变量 AUC 0.4932，等于噪声），不进任何候选集",
    "target_mode": "全表恒为 ENERGY，无信息量；留在表里以便换批次时复核这句话",
    "hour_block": "查表基线与 cell 引擎的分桶辅助列，与 hour 完全共线，不另进特征",
    "home_city_id": "users 的常驻城与本表站点城重合度高；城的信息已经由 site_type/站历史承载",
    "acquisition_channel": "users 画像的第三类，与 segment/membership 同源冗余，未纳入候选",
    "status(queue_entries)": "排队终态（SERVED/ABANDONED/CALL_EXPIRED）是 resolve 之后才有的事实，"
                             "只按 resolved_at 聚合成站级历史弃队率，绝不作为行级特征",
}

#: 收缩强度。用户 p50 只有 18 场会话，α 太大会把个体差异糊掉；站/格样本多，α 取中。
ALPHA_USER = 15.0
ALPHA_USER_STATION = 10.0
ALPHA_STATION = 20.0
ALPHA_CELL = 10.0
ALPHA_QUEUE = 20.0

WEATHER_LAG_COLUMNS = ("temp_c_mean", "temp_c_max", "humidity_mean", "rainfall_mm_sum")


# --------------------------------------------------------------------------- 样本与标签


def load_sessions() -> tuple[pd.DataFrame, dict]:
    """读会话事实表并现场构建标签。结构断言全部针对"决策时点之后"的三时间戳链路。"""
    frame = common.load_clean_table("charging_sessions")
    for column in ("started_at", "ended_at", "unplugged_at"):
        frame[column] = pd.to_datetime(frame[column])
    assert frame["session_id"].is_unique and frame["attempt_id"].is_unique, "会话主键重复"
    assert (frame["started_at"].notna() & frame["ended_at"].notna()
            & frame["unplugged_at"].notna()).all(), "存在三时间戳缺失的会话，标签口径不成立"
    started, ended, unplugged = (frame[c].to_numpy() for c in
                                 ("started_at", "ended_at", "unplugged_at"))
    assert (ended >= started).all(), "存在 ended 早于 started 的会话"
    assert (unplugged >= ended).all(), "存在 unplugged 早于 ended 的会话（over_min 不可能为负）"
    frame["duration_min"] = (ended - started) / np.timedelta64(1, "m")
    frame["over_min"] = (unplugged - ended) / np.timedelta64(1, "m")
    assert (frame["over_min"] >= 0).all() and (frame["duration_min"] > 0).all()
    frame["y_over"] = (frame["over_min"] >= common.OVERSTAY_THRESHOLD_MIN).astype("int64")
    frame["business_date"] = (frame["started_at"] + pd.Timedelta(
        hours=common.BUSINESS_OFFSET_HOURS)).dt.normalize()
    notes = {"sessions": int(len(frame)), "users": int(frame["user_id"].nunique()),
             "stations": int(frame["station_id"].nunique()),
             "chargers": int(frame["charger_id"].nunique()),
             "statusCounts": {str(k): int(v) for k, v in frame["status"].value_counts().items()},
             "label": {"thresholdMin": common.OVERSTAY_THRESHOLD_MIN,
                       "positives": int(frame["y_over"].sum()),
                       "baseRate": round(float(frame["y_over"].mean()), 5),
                       "overMinutes": {"p50": float(frame["over_min"].median()),
                                       "p90": float(frame["over_min"].quantile(0.9)),
                                       "p99": float(frame["over_min"].quantile(0.99)),
                                       "max": float(frame["over_min"].max())}}}
    frame = frame.sort_values("started_at", kind="stable").reset_index(drop=True)
    return frame, notes


# --------------------------------------------------------------------------- 画像与静态属性


def attach_profiles(frame: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """车辆/站点/桩/用户画像 + campaign 覆盖核对。全部是决策时点已存在的属性。"""
    vehicles = common.load_clean_table("vehicles", columns=["vehicle_id", "vehicle_class",
                                                            "battery_capacity_kwh", "max_charge_kw"])
    stations = common.load_clean_table("stations", columns=["station_id", "city_id", "site_type",
                                                            "transformer_kw"])
    chargers = (common.load_clean_table("chargers", columns=["charger_id", "station_id",
                                                             "connector_type", "rated_power_kw",
                                                             "manufacturer", "model"])
                # station_id 两侧同名会静默变成 _x/_y 把下游全砸了——改名后当作一致性核对列用。
                .rename(columns={"station_id": "charger_station_id", "model": "charger_model"}))
    users = common.load_clean_table("users", columns=["user_id", "segment", "membership",
                                                      "registered_at"])
    rows_before = len(frame)
    frame = frame.merge(vehicles, on="vehicle_id", how="left", validate="many_to_one")
    frame = frame.merge(stations, on="station_id", how="left", validate="many_to_one")
    frame = frame.merge(chargers, on="charger_id", how="left", validate="many_to_one")
    frame = frame.merge(users, on="user_id", how="left", validate="many_to_one")
    assert len(frame) == rows_before and frame["vehicle_class"].notna().all()
    assert (frame["station_id"] == frame["charger_station_id"]).all(), \
        "桩所属站与会话所属站不一致（换批次要先查这条）"
    frame = frame.drop(columns=["charger_station_id"])
    assert frame["segment"].notna().all() and frame["site_type"].notna().all()
    registered = pd.to_datetime(frame["registered_at"])
    frame["user_age_days"] = ((frame["started_at"] - registered).dt.total_seconds() / 86400.0)
    assert (frame["user_age_days"] >= 0).all(), "存在注册之前就开始的会话（profile 时间线矛盾）"
    frame = frame.drop(columns=["registered_at"])

    campaigns = common.load_clean_table("campaigns", columns=["campaign_id", "starts_at",
                                                              "ends_at", "discount_cents"])
    tagged = frame["campaign_id"].notna()
    check = (frame.loc[tagged, ["session_id", "campaign_id", "started_at"]]
             .merge(campaigns, on="campaign_id", how="left", validate="many_to_one"))
    assert check["starts_at"].notna().all(), "会话引用了 campaigns 表里没有的 campaign_id"
    inside = ((check["started_at"] >= check["starts_at"]) & (check["started_at"] < check["ends_at"]))
    assert inside.all(), (f"{int((~inside).sum())} 条带 campaign 标记的会话开始时刻不在活动窗口内——"
                          "『标记在决策时点已知』的前提被破坏，has_campaign 必须停用并重新核口径")
    frame = frame.merge(campaigns[["campaign_id", "discount_cents"]]
                        .rename(columns={"discount_cents": "campaign_discount_cents"}),
                        on="campaign_id", how="left", validate="many_to_one")
    frame["has_campaign"] = tagged.astype("int64")
    # 计划充电时长：target_value 是用户在起充前设定的 ENERGY 目标，**单位是 Wh**（target_mode 恒为
    # ENERGY；本表取值 500–77,573 Wh），除以 1000 折成 kWh 再除以桩额定功率 kW 才是小时数——
    # 这是决策时点可算的 oracle（真实充电时长）的**可部署替身**，两者对照在评测报告第 3 节。
    frame["planned_hours"] = frame["target_value"] / 1000.0 / frame["rated_power_kw"].astype("float64")
    # 单位保险：真实小时数上界 = 77.6 kWh / 7 kW ≈ 11.1 h；若哪天 Wh/kW 又搞混，这里直接炸。
    assert (frame["planned_hours"] <= 24.0).all(), "planned_hours 超出 24h——疑似 target_value 单位口径变了"
    notes = {"profileCoverage": {c: round(float(frame[c].notna().mean()), 4)
                                 for c in ("vehicle_class", "site_type", "connector_type",
                                           "segment", "membership", "campaign_discount_cents")},
             "campaignTagged": int(tagged.sum()),
             "campaignWindowCheck": "all-tagged-started-at-inside-window",
             "vehicleClasses": {str(k): int(v) for k, v in frame["vehicle_class"].value_counts().items()},
             "siteTypes": {str(k): int(v) for k, v in frame["site_type"].value_counts().items()}}
    return frame, notes


# --------------------------------------------------------------------------- 日历与天气（T−1 日因果聚合）


def build_day_context() -> pd.DataFrame:
    """城×北京日历日的上下文表：日历事实 + 天气日聚合。聚合本身不含因果判断，
    "哪天能读到"在 ``attach_calendar_and_weather`` 里做（本线只读 T−1 日及更早）。"""
    calendar = common.load_clean_table("calendar")
    calendar["business_date"] = pd.to_datetime(calendar["business_date"])
    weather = common.load_clean_table("weather_hourly", columns=["city_id", "recorded_at",
                                                                 "temperature_c", "humidity_pct",
                                                                 "weather", "rainfall_mm"])
    weather["recorded_at"] = pd.to_datetime(weather["recorded_at"])
    weather["business_date"] = (weather["recorded_at"] + pd.Timedelta(
        hours=common.BUSINESS_OFFSET_HOURS)).dt.normalize()
    assert weather["temperature_c"].notna().all(), "天气表存在缺失气温的小时"
    grouped = weather.groupby(["city_id", "business_date"], observed=True).agg(
        temp_c_mean=("temperature_c", "mean"),
        temp_c_min=("temperature_c", "min"),
        temp_c_max=("temperature_c", "max"),
        humidity_mean=("humidity_pct", "mean"),
        rainfall_mm_sum=("rainfall_mm", "sum"),
        rain_hours=("rainfall_mm", lambda s: int((s > 0).sum())),
        weather_mode=("weather", lambda s: str(s.mode().iloc[0])),
    ).reset_index().rename(columns={"weather_mode": "weather"})
    context = calendar.merge(grouped, on=["city_id", "business_date"], how="left",
                             validate="one_to_one")
    assert context["weather"].notna().all(), "存在没有天气聚合的城-日"
    context["is_weekend"] = context["is_weekend"].astype("int64")
    return context


def _lagged_prev(ordered: pd.DataFrame, column: str, keys: pd.MultiIndex) -> np.ndarray:
    """城×日矩阵沿日期轴 shift(1) 后按 (城, 日) 取值。pivot 保证 shift **绝不跨城**。"""
    matrix = ordered.pivot(index="business_date", columns="city_id", values=column).sort_index()
    previous = matrix.shift(1)
    flat = previous.stack()
    flat.index = flat.index.swaplevel().set_names(["city_id", "business_date"])
    return flat.reindex(keys).to_numpy(dtype="float64")


def attach_calendar_and_weather(frame: pd.DataFrame, context: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """当日计划量（周末/场景）+ T−1 日天气聚合。今天的天气列根本不存在于合并过程。"""
    ordered = context.sort_values(["city_id", "business_date"], kind="stable").reset_index(drop=True)
    keys = pd.MultiIndex.from_arrays([ordered["city_id"].to_numpy(),
                                      ordered["business_date"].to_numpy()],
                                     names=["city_id", "business_date"])
    day = ordered[["city_id", "business_date", "is_weekend", "scenario_event",
                   "demand_multiplier"]].copy()
    for column in WEATHER_LAG_COLUMNS:
        day[f"{column}_prev"] = _lagged_prev(ordered, column, keys)
    weather_matrix = (ordered.assign(_code=ordered["weather"].astype("category"))
                      .pivot(index="business_date", columns="city_id", values="_code").sort_index())
    flat = weather_matrix.shift(1).stack().astype("object")
    flat.index = flat.index.swaplevel().set_names(["city_id", "business_date"])
    day["weather_prev"] = flat.reindex(keys).to_numpy(dtype=object)
    day["rain_prev"] = np.where(day["rainfall_mm_sum_prev"].isna(), np.nan,
                                (day["rainfall_mm_sum_prev"] > 0).astype(float))

    rows_before = len(frame)
    frame = frame.merge(day, on=["city_id", "business_date"], how="left", validate="many_to_one")
    assert len(frame) == rows_before, "城-日上下文关联改变了行数（网格不稠密或有重复）"
    starts = frame["started_at"]
    frame["hour"] = starts.dt.hour.astype("float64")
    frame["day_of_week"] = starts.dt.dayofweek.astype("float64")
    frame["hour_sin"] = np.sin(2 * np.pi * frame["hour"] / 24.0)
    frame["hour_cos"] = np.cos(2 * np.pi * frame["hour"] / 24.0)
    frame["hour_block"] = (frame["hour"] // common.HOUR_BLOCK_WIDTH).astype("int64")
    notes = {
        "weatherRule": "天气只取 T−1 日的城×日聚合，今天的天气不进合并；shift 在日×城矩阵上进行，绝不跨城",
        "coverage": {c: round(float(frame[c].notna().mean()), 4)
                     for c in ("weather_prev", "temp_c_max_prev", "rainfall_mm_sum_prev",
                               "scenario_event", "is_weekend", "user_age_days")},
        "sameDayWeatherNotMerged": True,
    }
    return frame, notes


# --------------------------------------------------------------------------- 用户历史（跨时间轴的 as-of）


def _query(frame: pd.DataFrame, key_column: str) -> pd.DataFrame:
    """给 ``AsOfCounts`` 用的最小查询表：实体键 + 查询时刻（本行会话的 started_at）。"""
    return pd.DataFrame({"__query_time": pd.to_datetime(frame["started_at"]),
                         key_column: frame[key_column].to_numpy()})


def time_gap(later, earlier, scale: float) -> np.ndarray:
    """两组时间戳之差除以 ``scale``（天 / 分钟）；任一侧 NaT 则该行 NaN。

    刻意走 Series 的 timedelta 算术而不是 int64 减法：``strict_last`` 对"从来没有过"的行给 NaT，
    而 NaT 转 int64 是 ``-9223372036854775808``，与当刻相减会溢出——这种边界不该用溢出行为来兜。
    """
    base = pd.Series(pd.to_datetime(np.asarray(later, dtype="datetime64[ns]")))
    past = pd.Series(pd.to_datetime(np.asarray(earlier, dtype="datetime64[ns]")))
    gap = (base - past).dt.total_seconds()
    assert len(gap) == len(base)
    return (gap / scale).to_numpy(dtype="float64")


def attach_user_history(frame: pd.DataFrame, events: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """这个用户"到此为止"的充电史：场次计数（started 轴）+ 超占历史（unplugged 轴）。

    两条时间轴各走各的引擎，**查询时刻统一是 started_at**：
      · 计数引擎的事件时刻 = ``started_at``——上一场会话开始的一刻它就存在了；
      · 超占引擎的事件时刻 = ``unplugged_at``——上一场占没占、占多久，要等他拔枪才知道。
    收缩先验是"截至查询时刻之前的全量超占率/均值"（事件按拔枪可见性计），无历史取 0.0。
    """
    unplugged = pd.to_datetime(events["unplugged_at"])
    flags = events["y_over"].to_numpy(dtype=float)
    minutes = events["over_min"].to_numpy(dtype=float)

    prior_counts, prior_sums = common.global_prior_at(frame["started_at"], unplugged, flags)
    rate_prior = np.where(prior_counts > 0, prior_sums / np.maximum(prior_counts, 1.0), 0.0)
    mean_counts, mean_sums = common.global_prior_at(frame["started_at"], unplugged, minutes)
    mean_prior = np.where(mean_counts > 0, mean_sums / np.maximum(mean_counts, 1.0), 0.0)

    helper = events.assign(_flag=flags, _minutes=minutes,
                          _user_station=events["user_id"].astype("string") + "|"
                          + events["station_id"].astype("string"))
    query_user = _query(frame, "user_id")
    count_engine = common.AsOfCounts(helper, key="user_id", stamps="started_at")
    flag_engine = common.AsOfCounts(helper, key="user_id", stamps="unplugged_at", value="_flag")
    minute_engine = common.AsOfCounts(helper, key="user_id", stamps="unplugged_at", value="_minutes")
    us_flag_engine = common.AsOfCounts(helper, key="_user_station", stamps="unplugged_at",
                                       value="_flag")

    sessions_prior, _ = count_engine.cumulative(query_user, "user_id")
    frame["user_sessions_prior"] = sessions_prior
    frame["user_sessions_7d"] = count_engine.window(query_user, "user_id", 7.0)[0]
    frame["user_sessions_30d"] = count_engine.window(query_user, "user_id", 30.0)[0]
    flag_counts, flag_sums = flag_engine.cumulative(query_user, "user_id", "_flag")
    frame["user_over_rate_prior"] = common.smoothed(flag_counts, flag_sums, ALPHA_USER, rate_prior)
    w_counts, w_sums = flag_engine.window(query_user, "user_id", 30.0, "_flag")
    frame["user_over_rate_30d"] = common.smoothed(w_counts, w_sums, ALPHA_USER, rate_prior)
    m_counts, m_sums = minute_engine.cumulative(query_user, "user_id", "_minutes")
    frame["user_over_mean_prior"] = common.smoothed(m_counts, m_sums, ALPHA_USER, mean_prior)
    query_us = pd.DataFrame({"__query_time": pd.to_datetime(frame["started_at"]),
                             "_user_station": frame["user_id"].astype("string").to_numpy() + "|"
                             + frame["station_id"].astype("string").to_numpy()})
    us_counts, us_sums = us_flag_engine.cumulative(query_us, "_user_station", "_flag")
    frame["us_over_rate_prior"] = common.smoothed(us_counts, us_sums, ALPHA_USER_STATION, rate_prior)
    frame["us_joint_sessions_prior"] = us_counts

    last = common.strict_last(frame, "started_at", "user_id",
                              helper[["user_id", "unplugged_at", "_flag", "_minutes"]],
                              "unplugged_at", ["_flag", "_minutes"])
    frame["user_last_over"] = last["_flag"].to_numpy(dtype="float64")
    frame["user_last_over_min"] = last["_minutes"].to_numpy(dtype="float64")
    frame["user_hours_since_prev_unplug"] = time_gap(
        pd.to_datetime(frame["started_at"]).to_numpy(), last["__last_time"].to_numpy(), 3_600.0)
    prev_start = common.strict_last(frame, "started_at", "user_id",
                                    helper[["user_id", "started_at"]], "started_at", [])
    frame["user_days_since_prev_session"] = time_gap(
        pd.to_datetime(frame["started_at"]).to_numpy(),
        prev_start["__last_time"].to_numpy(), 86_400.0)
    notes = {"shrinkageTarget": {
        "rule": "先验 = 截至本行 started_at 之前**已拔枪**的全量超占率/均值，无历史取 0.0",
        "ratePriorRange": [round(float(rate_prior.min()), 5), round(float(rate_prior.max()), 5)],
        "meanPriorRange": [round(float(mean_prior.min()), 2), round(float(mean_prior.max()), 2)]},
        "rowsWithNoUserHistory": int((sessions_prior == 0).sum()),
        "userStationJointPairs": int(helper["_user_station"].nunique())}
    return frame, notes


# --------------------------------------------------------------------------- 站点/格历史与排队运营量


def attach_station_history(frame: pd.DataFrame, events: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """这个站"到此为止"的超占文化与车流：同样两条轴（计数=started、超占=unplugged）。"""
    unplugged = pd.to_datetime(events["unplugged_at"])
    flags = events["y_over"].to_numpy(dtype=float)
    minutes = events["over_min"].to_numpy(dtype=float)
    prior_counts, prior_sums = common.global_prior_at(frame["started_at"], unplugged, flags)
    rate_prior = np.where(prior_counts > 0, prior_sums / np.maximum(prior_counts, 1.0), 0.0)
    mean_counts, mean_sums = common.global_prior_at(frame["started_at"], unplugged, minutes)
    mean_prior = np.where(mean_counts > 0, mean_sums / np.maximum(mean_counts, 1.0), 0.0)

    helper = events.assign(_flag=flags, _minutes=minutes,
                          _station_cell=events["station_id"].astype("string") + "|"
                          + (events["started_at"].dt.hour // common.HOUR_BLOCK_WIDTH).astype("string"))
    query_station = _query(frame, "station_id")
    count_engine = common.AsOfCounts(helper, key="station_id", stamps="started_at")
    flag_engine = common.AsOfCounts(helper, key="station_id", stamps="unplugged_at", value="_flag")
    minute_engine = common.AsOfCounts(helper, key="station_id", stamps="unplugged_at",
                                      value="_minutes")
    cell_flag_engine = common.AsOfCounts(helper, key="_station_cell", stamps="unplugged_at",
                                         value="_flag")

    frame["station_sessions_prior"] = count_engine.cumulative(query_station, "station_id")[0]
    frame["station_sessions_7d"] = count_engine.window(query_station, "station_id", 7.0)[0]
    frame["station_sessions_30d"] = count_engine.window(query_station, "station_id", 30.0)[0]
    counts, sums = flag_engine.cumulative(query_station, "station_id", "_flag")
    frame["station_over_rate_prior"] = common.smoothed(counts, sums, ALPHA_STATION, rate_prior)
    w_counts, w_sums = flag_engine.window(query_station, "station_id", 30.0, "_flag")
    frame["station_over_rate_30d"] = common.smoothed(w_counts, w_sums, ALPHA_STATION, rate_prior)
    m_counts, m_sums = minute_engine.cumulative(query_station, "station_id", "_minutes")
    frame["station_over_mean_prior"] = common.smoothed(m_counts, m_sums, ALPHA_STATION, mean_prior)
    dw_counts, dw_sums = minute_engine.window(query_station, "station_id", 30.0, "_minutes")
    frame["station_over_minutes_per_day_30d"] = dw_sums / 30.0

    query_cell = pd.DataFrame({"__query_time": pd.to_datetime(frame["started_at"]),
                              "_station_cell": frame["station_id"].astype("string").to_numpy() + "|"
                              + frame["hour_block"].astype("string").to_numpy()})
    c_counts, c_sums = cell_flag_engine.cumulative(query_cell, "_station_cell", "_flag")
    frame["cell_over_rate_prior"] = common.smoothed(c_counts, c_sums, ALPHA_CELL, rate_prior)
    frame["cell_sessions_prior"] = c_counts

    over_events = helper[helper["y_over"] == 1][["station_id", "unplugged_at"]]
    last_over = common.strict_last(frame, "started_at", "station_id", over_events,
                                   "unplugged_at", [])
    frame["station_days_since_last_over"] = time_gap(
        pd.to_datetime(frame["started_at"]).to_numpy(),
        last_over["__last_time"].to_numpy(), 86_400.0)
    notes = {"cellKey": f"station_id × (started_at.hour // {common.HOUR_BLOCK_WIDTH})",
             "cells": int(helper["_station_cell"].nunique()),
             "stationOverRatePriorSpread": {
                 "min": round(float(frame["station_over_rate_prior"].min()), 4),
                 "p50": round(float(frame["station_over_rate_prior"].median()), 4),
                 "max": round(float(frame["station_over_rate_prior"].max()), 4)}}
    return frame, notes


def attach_queue_ops(frame: pd.DataFrame) -> tuple[pd.DataFrame, dict, pd.DataFrame]:
    """排队侧运营量（站级 as-of，全部严格早于 started_at）+ 本会话自己的排队属性。

    "当前在排人数" = 已加入累计 − 已离开累计：每条队列记录的 ``resolved_at`` 是它真实离场的时刻，
    在 t 之前离开的在 t 时刻已经不排了，这条差值是决策时点系统本来就看得见的量（与第七线
    "报修数−已恢复数"同一手法）。**未离场的记录 ``resolved_at`` 为 NaT**：转成 int64 是
    ``-9223372036854775808``，会被 ``searchsorted`` 当成"早于一切"——所以 resolve 侧引擎一律
    只吃非空行；还挂在队里的记录继续留在 joins 侧被计入在排人数，这才是决策时点的真相。
    本会话自己的排队属性只在"排队而来"的行上非空。
    """
    entries = common.load_clean_table("queue_entries", columns=["queue_id", "user_id",
                                                                "station_id", "joined_at",
                                                                "called_at", "resolved_at",
                                                                "position_at_join", "status",
                                                                "session_id"])
    for column in ("joined_at", "called_at", "resolved_at"):
        entries[column] = pd.to_datetime(entries[column])
    assert entries["joined_at"].notna().all(), "存在没有加入时刻的排队记录"
    known = entries[entries["resolved_at"].notna()]
    assert (known["resolved_at"] >= known["joined_at"]).all(), "存在离开早于加入的排队记录"
    called = known[known["called_at"].notna()]
    assert (called["called_at"] <= called["resolved_at"]).all(), "被叫时刻晚于离场时刻"
    assert set(entries["status"].unique()) <= {"SERVED", "ABANDONED", "CALL_EXPIRED"}, \
        "排队状态出现未登记的取值，弃队率口径要重核"
    helper = entries.assign(_one=1.0)
    resolved_helper = known.assign(_one=1.0)
    abandoned = resolved_helper[resolved_helper["status"] == "ABANDONED"]

    query_station = _query(frame, "station_id")
    join_engine = common.AsOfCounts(helper, key="station_id", stamps="joined_at", value="_one")
    resolve_engine = common.AsOfCounts(resolved_helper, key="station_id", stamps="resolved_at",
                                       value="_one")
    abandon_engine = common.AsOfCounts(abandoned, key="station_id", stamps="resolved_at",
                                       value="_one")

    frame["queue_joined_2h"] = join_engine.window(query_station, "station_id", 2.0 / 24.0, "_one")[0]
    frame["queue_joined_24h"] = join_engine.window(query_station, "station_id", 1.0, "_one")[0]
    joins_total, _ = join_engine.cumulative(query_station, "station_id", "_one")
    resolves_total, _ = resolve_engine.cumulative(query_station, "station_id", "_one")
    frame["queue_open_at_start"] = np.maximum(joins_total - resolves_total, 0.0)
    abandons_total, _ = abandon_engine.cumulative(query_station, "station_id", "_one")
    prior_counts, prior_sums = common.global_prior_at(frame["started_at"],
                                                      known["resolved_at"],
                                                      (known["status"] == "ABANDONED")
                                                      .to_numpy(dtype=float))
    abandon_prior = np.where(prior_counts > 0, prior_sums / np.maximum(prior_counts, 1.0), 0.0)
    frame["station_abandon_share_prior"] = common.smoothed(
        resolves_total, abandons_total, ALPHA_QUEUE, abandon_prior)

    own = entries[entries["session_id"].notna()][["session_id", "user_id", "position_at_join",
                                                  "called_at"]].copy()
    assert own["session_id"].is_unique, "同一条会话挂了多条排队记录，『本会话来自排队』无定义"
    merged = frame[["session_id", "user_id", "started_at"]].merge(own, on="session_id",
                                                                  how="left", suffixes=("", "_queue"))
    assert len(merged) == len(frame)
    paired = merged[merged["called_at"].notna()]
    assert (paired["user_id"] == paired["user_id_queue"]).all(), "排队记录与会话的用户对不上"
    wait = (frame["started_at"].to_numpy() - merged["called_at"].to_numpy()) / np.timedelta64(1, "m")
    from_queue = merged["called_at"].notna().to_numpy()
    frame["from_queue"] = from_queue.astype("int64")
    frame["own_position_at_join"] = merged["position_at_join"].to_numpy(dtype="float64")
    frame["own_queue_wait_min"] = np.where(from_queue, np.maximum(wait, 0.0), np.nan)
    if from_queue.any():
        assert (wait[from_queue] >= -1e-9).all(), \
            "排队而来却被叫到之后才开始的会话存在负等待——called_at 与 started_at 的时间线矛盾"
    notes = {"entries": int(len(entries)), "unresolvedEntries": int(entries["resolved_at"].isna().sum()),
             "fromQueueRows": int(frame["from_queue"].sum()),
             "statusMix": {str(k): int(v) for k, v in entries["status"].value_counts().items()},
             "openAtStart": {"p50": float(frame["queue_open_at_start"].median()),
                             "p95": float(frame["queue_open_at_start"].quantile(0.95)),
                             "max": float(frame["queue_open_at_start"].max())},
             "abandonPriorRange": [round(float(abandon_prior.min()), 5),
                                   round(float(abandon_prior.max()), 5)],
             "ownWaitMinutes": {"p50": float(pd.Series(frame.loc[from_queue, "own_queue_wait_min"]).median()),
                                "p95": float(pd.Series(frame.loc[from_queue, "own_queue_wait_min"]).quantile(0.95))}}
    return frame, notes, entries


# --------------------------------------------------------------------------- 泄漏审计


AUDITED_FEATURES = ("user_over_rate_prior", "user_over_rate_30d", "user_over_mean_prior",
                    "user_sessions_30d", "us_over_rate_prior", "station_over_rate_prior",
                    "station_over_minutes_per_day_30d", "cell_over_rate_prior",
                    "user_last_over", "station_abandon_share_prior", "queue_open_at_start",
                    "temp_c_max_prev", "rainfall_mm_sum_prev")


def verify_no_future_leak(frame: pd.DataFrame, events: pd.DataFrame, entries: pd.DataFrame,
                          context: pd.DataFrame, sample: int = 250,
                          seed: int = common.SEED) -> dict:
    """随机抽若干会话，用**全表布尔掩码暴力重算** ``AUDITED_FEATURES``，要求逐条 0 偏差。

    审计刻意走另一条代码路径（不排序、不二分、不 merge_asof、不复用 ``AsOfCounts``/
    ``global_prior_at``/pivot-shift）。重点盯跨轴语义：``user_last_over`` 这类量必须同时满足
    "拔枪早于本场开始"（可见性）与"是该用户最晚的一条"（严格最后），一条都松不得。
    """
    rng = random.Random(seed)
    rows = sorted(rng.sample(range(len(frame)), min(sample, len(frame))))
    ev_unplugged = pd.to_datetime(events["unplugged_at"])
    ev_flags = events["y_over"].to_numpy(dtype=float)
    ev_minutes = events["over_min"].to_numpy(dtype=float)
    events_by_user = {k: part for k, part in events.groupby("user_id", observed=True)}
    events_by_station = {k: part for k, part in events.groupby("station_id", observed=True)}
    cell_by = {k: part for k, part in events.groupby(
        [events["station_id"], events["started_at"].dt.hour // common.HOUR_BLOCK_WIDTH],
        observed=True)}
    entries_by_station = {k: part for k, part in entries.groupby("station_id", observed=True)}
    context_by_city = {k: part for k, part in context.groupby("city_id", observed=True)}

    worst = {name: 0.0 for name in AUDITED_FEATURES}
    empty_events = events.iloc[0:0]
    for position in rows:
        row = frame.iloc[position]
        stamp = pd.to_datetime(row["started_at"])
        day_start = stamp - pd.Timedelta(days=30)

        user_part = events_by_user.get(row["user_id"], empty_events)
        u_unplugged = pd.to_datetime(user_part["unplugged_at"])
        u_flags = user_part["y_over"].to_numpy(dtype=float)
        u_minutes = user_part["over_min"].to_numpy(dtype=float)
        u_unplug_before = (u_unplugged < stamp).to_numpy()
        u_unplug_30d = u_unplug_before & (u_unplugged >= day_start).to_numpy()
        u_started = pd.to_datetime(user_part["started_at"])
        u_start_30d = ((u_started < stamp) & (u_started >= day_start)).to_numpy()

        station_part = events_by_station.get(row["station_id"], empty_events)
        s_unplugged = pd.to_datetime(station_part["unplugged_at"])
        s_flags = station_part["y_over"].to_numpy(dtype=float)
        s_minutes = station_part["over_min"].to_numpy(dtype=float)
        s_before = (s_unplugged < stamp).to_numpy()
        s_30d = s_before & (s_unplugged >= day_start).to_numpy()
        us_before = s_before & (station_part["user_id"].to_numpy() == row["user_id"])

        cell_part = cell_by.get((row["station_id"],
                                 int(row["hour"]) // common.HOUR_BLOCK_WIDTH), empty_events)
        c_before = (pd.to_datetime(cell_part["unplugged_at"]) < stamp).to_numpy()

        global_before = (ev_unplugged < stamp).to_numpy()
        global_rate = float(ev_flags[global_before].mean()) if global_before.any() else 0.0
        global_mean = float(ev_minutes[global_before].mean()) if global_before.any() else 0.0

        here_entries = entries_by_station.get(row["station_id"], entries.iloc[0:0])
        joined_before = (pd.to_datetime(here_entries["joined_at"]) < stamp).to_numpy()
        resolved_before = (pd.to_datetime(here_entries["resolved_at"]) < stamp).to_numpy()
        # 弃队率的分子分母都走 resolved 轴（与引擎一致），不是 joined 轴——两轴混用正是本审计要抓的错
        abandoned_before = (here_entries["status"].to_numpy() == "ABANDONED") & resolved_before
        all_before = (pd.to_datetime(entries["resolved_at"]) < stamp).to_numpy()
        all_flags = (entries["status"].to_numpy() == "ABANDONED").astype(float)[all_before]
        abandon_prior = float(all_flags.mean()) if all_before.any() else 0.0

        city_part = context_by_city.get(row["city_id"], context.iloc[0:0])
        prev_day = (stamp + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
                    - pd.Timedelta(days=1)).normalize()
        prev_row = city_part[city_part["business_date"] == prev_day]

        truth = {
            "user_over_rate_prior": (float(u_flags[u_unplug_before].sum())
                                     + ALPHA_USER * global_rate)
            / (int(u_unplug_before.sum()) + ALPHA_USER),
            "user_over_rate_30d": (float(u_flags[u_unplug_30d].sum()) + ALPHA_USER * global_rate)
            / (int(u_unplug_30d.sum()) + ALPHA_USER),
            "user_over_mean_prior": (float(u_minutes[u_unplug_before].sum())
                                     + ALPHA_USER * global_mean)
            / (int(u_unplug_before.sum()) + ALPHA_USER),
            "user_sessions_30d": float(u_start_30d.sum()),
            "us_over_rate_prior": (float(s_flags[us_before].sum())
                                   + ALPHA_USER_STATION * global_rate)
            / (int(us_before.sum()) + ALPHA_USER_STATION),
            "station_over_rate_prior": (float(s_flags[s_before].sum()) + ALPHA_STATION * global_rate)
            / (int(s_before.sum()) + ALPHA_STATION),
            "station_over_minutes_per_day_30d": float(s_minutes[s_30d].sum()) / 30.0,
            "cell_over_rate_prior": (float(cell_part["y_over"].to_numpy(dtype=float)[c_before].sum())
                                     + ALPHA_CELL * global_rate)
            / (int(c_before.sum()) + ALPHA_CELL),
            "user_last_over": np.nan,  # 下面单独算：必须同时"拔枪早于本场开始"且"是该用户最晚一条"
            "station_abandon_share_prior": (float(abandoned_before.sum())
                                            + ALPHA_QUEUE * abandon_prior)
            / (float(resolved_before.sum()) + ALPHA_QUEUE),
            "queue_open_at_start": max(float(joined_before.sum()) - float(resolved_before.sum()), 0.0),
            "temp_c_max_prev": np.nan if len(prev_row) == 0 else float(prev_row["temp_c_max"].iloc[0]),
            "rainfall_mm_sum_prev": np.nan if len(prev_row) == 0
            else float(prev_row["rainfall_mm_sum"].iloc[0]),
        }
        if u_unplug_before.any():
            latest = u_unplugged[u_unplug_before].max()
            truth["user_last_over"] = float(u_flags[(u_unplugged == latest).to_numpy()][-1])
        for name, expected in truth.items():
            got = float(row[name])
            if pd.isna(expected) and pd.isna(got):
                continue
            worst[name] = max(worst[name], abs(got - float(expected)))
    audit = {"sampled": len(rows), "checked": list(AUDITED_FEATURES),
             "maxAbsDiff": {k: float(v) for k, v in worst.items()},
             "method": "全表布尔掩码独立重算（与向量路径不共用排序/二分/merge_asof/pivot-shift 代码）"}
    offenders = {k: v for k, v in audit["maxAbsDiff"].items() if v > 1e-9}
    if offenders:
        raise AssertionError(f"as-of 特征与暴力重算不一致（未来信息泄漏）：{offenders}")
    return audit


# --------------------------------------------------------------------------- 特征名单


#: 候选集划分的归属规则（前缀 + 显式名单，顺序即优先级）。"历史"是本线的重头戏：用户/站/格
#: 三条 as-of 轴；"运营"是排队侧；其余画像与时钟归"静态"。
HISTORY_PREFIXES = ("user_", "us_", "station_over_", "station_sessions_",
                    "station_days_since_last_over", "cell_over_", "cell_sessions_")
OPS_PREFIXES = ("queue_", "station_abandon_", "from_queue", "own_")


def candidate_sets(numeric: list[str], categorical: list[str]) -> dict[str, list[str]]:
    """四组候选特征集：只画像/时钟/天气、只跨轴历史、只排队运营、全部。

    刻意把"只画像/时钟/天气"单列一组：侦察实测 staticOnly 就能到 TEST AUC 0.78——**车型×站型×
    时段的结构**是本线的主干信号；"只历史"组回答"用户习惯还值多少增量"，两组之差就是本线真正
    要交付给运营的那句话。
    """
    def is_history(column: str) -> bool:
        return column.startswith(HISTORY_PREFIXES)

    def is_ops(column: str) -> bool:
        return column.startswith(OPS_PREFIXES)

    history_numeric = [c for c in numeric if is_history(c)]
    ops_numeric = [c for c in numeric if is_ops(c) and not is_history(c)]
    static_numeric = [c for c in numeric if not is_history(c) and not is_ops(c)]
    groups = {
        "staticOnly": sorted(set(static_numeric) | set(categorical)),
        "historyOnly": sorted(set(history_numeric)),
        "opsOnly": sorted(set(ops_numeric)),
        "full": sorted(set(numeric) | set(categorical)),
    }
    universe = set(numeric) | set(categorical)
    for name, columns in groups.items():
        extra = set(columns) - universe
        assert not extra, f"候选集 {name} 引用了不在特征名单里的列：{sorted(extra)}"
    assert groups["staticOnly"] and groups["historyOnly"] and groups["opsOnly"], "有空候选集"
    assert set(groups["full"]) == universe
    banned = set(common.NON_FEATURE_COLUMNS) | set(FORBIDDEN_FEATURES) | set(IGNORED_COLUMNS)
    assert not (set(groups["staticOnly"]) & banned)
    return groups


def pick_features(frame: pd.DataFrame) -> dict[str, list[str]]:
    """选特征：数值 / 类别两份名单 + 四组候选集。零方差列剔除并留痕，禁入列出现即报错。"""
    banned = set(common.NON_FEATURE_COLUMNS) | set(FORBIDDEN_FEATURES) | set(IGNORED_COLUMNS)
    categorical = [c for c in common.CATEGORICAL_FEATURES if c in frame.columns]
    numeric = [column for column in frame.columns
               if column not in banned and column not in categorical
               and (pd.api.types.is_bool_dtype(frame[column])
                     or pd.api.types.is_numeric_dtype(frame[column]))]
    constants = [c for c in numeric if frame[c].dropna().nunique() <= 1]
    numeric = [c for c in numeric if c not in constants]
    leaked = (set(numeric) | set(categorical)) & banned
    assert not leaked, f"禁入列出现在特征名单里：{sorted(leaked)}"
    assert len(categorical) == len(common.CATEGORICAL_FEATURES), \
        f"类别特征缺列：{set(common.CATEGORICAL_FEATURES) - set(categorical)}"
    return {"numeric": numeric, "categorical": categorical, "droppedConstant": constants,
            "groups": candidate_sets(numeric, categorical)}


# --------------------------------------------------------------------------- 主流程


def build() -> tuple[pd.DataFrame, dict]:
    """全流程但不落盘：``rolling.py`` 靠它在内存里重建同一张表。"""
    manifest = common.verify_batch()
    bounds = common.split_boundaries(manifest)
    frame, label_notes = load_sessions()
    frame, profile_notes = attach_profiles(frame)
    context = build_day_context()
    frame, calendar_notes = attach_calendar_and_weather(frame, context)
    # 事件流 = **全部**会话（含窗口外的 EXCLUDED 行）：早于窗口的会话是窗口内会话合法的"过去"。
    events = frame.copy()
    frame, user_notes = attach_user_history(frame, events)
    frame, station_notes = attach_station_history(frame, events)
    frame, queue_notes, entries = attach_queue_ops(frame)
    frame = frame.sort_values(["started_at", "session_id"], kind="stable").reset_index(drop=True)
    frame["split"] = common.assign_split(frame["started_at"], bounds)
    label_notes["splits"] = {name: int((frame["split"] == name).sum())
                             for name in ("TRAIN", "VALIDATION", "TEST", "EXCLUDED")}
    label_notes["splitBaseRate"] = {name: round(float(frame.loc[frame["split"] == name, "y_over"].mean()), 5)
                                    for name in ("TRAIN", "VALIDATION", "TEST", "EXCLUDED")}
    label_notes["splitPositives"] = {name: int(frame.loc[frame["split"] == name, "y_over"].sum())
                                     for name in ("TRAIN", "VALIDATION", "TEST", "EXCLUDED")}
    features = pick_features(frame)
    source_rows = {name: int(len(common.load_clean_table(name, columns=[column])))
                   for name, column in _SOURCE_ROW_PROBES.items()}
    notes = {"label": label_notes, "profiles": profile_notes, "calendar": calendar_notes,
             "userHistory": user_notes, "stationHistory": station_notes, "queue": queue_notes}
    return frame, {"bounds": bounds, "features": features, "notes": notes,
                   "manifest": manifest, "context": context, "events": events,
                   "queueEntries": entries, "sourceRows": source_rows}


#: 换批次时先核对这些表的体量（各取一列的最省读法），再谈特征。
_SOURCE_ROW_PROBES = {
    "charging_sessions": "session_id",
    "queue_entries": "queue_id",
    "vehicles": "vehicle_id",
    "users": "user_id",
    "stations": "station_id",
    "chargers": "charger_id",
}


def main() -> dict:
    common.require_empty_run_dir(common.OUT_DIR)
    frame, built = build()
    features, notes = built["features"], built["notes"]
    audit = verify_no_future_leak(frame, built["events"], built["queueEntries"], built["context"])
    common.write_new_pickle(common.FEATURES_PATH, frame)

    # 答案侧相关核对：把禁入名单的理由用当批数据的活数字钉死（每换一次批次都会重新算一遍）。
    from scipy import stats
    answer_side = {column: round(float(stats.spearmanr(frame[column].to_numpy(dtype=float),
                                                       frame["over_min"].to_numpy(dtype=float)
                                                       ).statistic), 3)
                   for column in ("parking_fee_cents", "energy_wh", "end_soc_pct", "total_fee_cents")}

    summary = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": built["manifest"].get("pipelineRunId"),
        "datasetId": common.DATASET_ID,
        "modelId": common.MODEL_ID,
        "featuresSha256": common.sha256_file(common.FEATURES_PATH),
        "sourceTables": common.SOURCE_TABLES,
        "sourceRows": built["sourceRows"],
        "label": ("y_over = (unplugged_at − ended_at) ≥ 30 分钟；over_min = 该分钟数（回归目标）；"
                  "决策时点 = started_at"),
        "sampleUnit": "一次充电会话（charging_sessions 一行）",
        "decisionTime": "会话 started_at 一刻；此后发生的一切（含本场 ended/unplugged/费用）不可见",
        "scope": notes["label"],
        "splits": notes["label"]["splits"],
        "splitBaseRate": notes["label"]["splitBaseRate"],
        "splitPositives": notes["label"]["splitPositives"],
        "splitDays": {name: int(frame.loc[frame["split"] == name, "business_date"].nunique())
                      for name in ("TRAIN", "VALIDATION", "TEST")},
        "boundaries": {k: str(v) for k, v in built["bounds"].items()},
        "answerSideSpearman": answer_side,
        "features": features,
        "featureCount": {"numeric": len(features["numeric"]),
                         "categorical": len(features["categorical"])},
        "constantWhy": {name: "样本域内取值唯一（非缺失），无信息量"
                          for name in features["droppedConstant"]},
        "leakAudit": audit,
        "notes": notes,
        "featureRows": int(len(frame)),
        "disclaimer": common.data_note(),
    }
    common.write_new_json(common.BUILD_SUMMARY_PATH, summary)
    print(f"[overstay] 特征表 {len(frame):,} 行会话（TRAIN "
          f"{summary['splits']['TRAIN']:,} / VAL {summary['splits']['VALIDATION']:,} / "
          f"TEST {summary['splits']['TEST']:,} / EXCLUDED {summary['splits']['EXCLUDED']:,}）")
    print(f"[overstay] 基础率 TRAIN={summary['splitBaseRate']['TRAIN']} "
          f"VAL={summary['splitBaseRate']['VALIDATION']} TEST={summary['splitBaseRate']['TEST']}")
    print(f"[overstay] 答案侧相关（Spearman vs over_min）：{answer_side}")
    print(f"[overstay] 特征 {len(features['numeric'])} 数值 + {len(features['categorical'])} 类别，"
          f"零方差剔除 {features['droppedConstant']}")
    print(f"[overstay] 候选集 {[(k, len(v)) for k, v in features['groups'].items()]}")
    print(f"[overstay] 泄漏审计 抽样 {audit['sampled']} 行 × {len(audit['checked'])} 个量，"
          f"maxAbsDiff = {max(audit['maxAbsDiff'].values()):.1e}")
    print(f"[overstay] -> {common.FEATURES_PATH}")
    return summary


if __name__ == "__main__":
    main()
