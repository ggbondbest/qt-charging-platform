"""特征层（第九线）：一台桩在**当日北京零点之前一刻**能看到的一切，看不到此后发生的任何事。

决策时点写死成每行一个值：``day_start_utc``（= 北京日历日 − 8h，UTC naive）。面板是稠密的
75 桩 × 178 日（12,000+ 行），标签 ``y_ticket7`` = ``[D, D+6]`` 北京日内出现 ≥1 张新工单。

四族特征、四条可见性时间轴（本线最容易出错的地方）：

  · **工单历史**——事件时刻 = ``reported_at``（报修那一刻工单才存在）。计数/窗口/最近一次
    都严格早于 ``day_start_utc``。"上一张修了多久"特殊：它要等 ``restored_at``（修完那一刻
    时长才成为事实），所以走**第二条修复轴**（strict_last on restored），仍是"只知过去"。
  · **使用负荷**——会话按 ``ended_at`` 可见（结束了才知道充了多少电、是否异常收尾）。
  · **启动尝试**——按 ``attempted_at`` 可见；技术失败 = 三个硬故障码（与第六/七线同口径）。
  · **遥测健康**——5 分钟栅格先聚成**桩×北京日**日表，再整体滞后：当行只读 D−1 日（及更早的
    7 日窗）——今天的遥测在今天的决策时点不存在。

日历/天气与第八线同款：周末与场景事件是计划量取当日；天气**只取 T−1 日及更早**的城×日聚合。

禁入名单 ``FORBIDDEN_FEATURES`` 封死：工单终态与成本（``status``/``severity``/
``labor_cost_cents``/``parts_cost_cents``）、**未来那张工单**的 ``fault_type`` 与四个原始时刻、
以及一切行级事件列马甲。``verify_no_future_leak`` 用全表布尔掩码独立重算
``AUDITED_FEATURES`` 里 17 个跨轴量（不复用排序/二分/merge_asof/pivot-shift 代码路径），
逐行要求 0 偏差——含本线独有的三条**前向标签量**（y_ticket7/tickets_next7d/label_end）的
窗口算术核对。

用法（仓库根目录）：python -m data_analysis.ml.health.features
"""

from __future__ import annotations

import random

import numpy as np
import pandas as pd

from . import common

#: 事后 / 身份 / 结局列：一律禁入特征。工单四时刻与终态列是答案本身或其马甲；
#: 行级事件列在面板里本不该出现，进名单纯属防御——哪天聚合代码写错把原始列带进表，这里当场炸。
FORBIDDEN_FEATURES = frozenset({
    # —— 身份与原始时间轴 ——
    "ticket_id", "reported_at", "accepted_at", "work_started_at", "restored_at",
    "attempt_id", "session_id", "user_id", "vehicle_id", "queue_id", "reservation_id",
    "attempted_at", "started_at", "ended_at", "unplugged_at", "recorded_at",
    "commissioned_at", "opened_at",
    # —— 工单终态与成本：修复之后才存在 ——
    "status", "severity", "fault_type", "labor_cost_cents", "parts_cost_cents",
    # —— 行级事件属性：只允许以聚合量进入 ——
    "outcome", "failure_reason", "stop_reason", "state", "power_kw", "online",
    "meter_wh", "energy_wh", "grid_energy_wh", "interval_seconds",
    "target_mode", "target_value", "campaign_id",
    "electricity_fee_cents", "service_fee_cents", "parking_fee_cents", "discount_cents",
    "total_fee_cents", "grid_cost_cents",
    # —— 站级身份/选址与经营列 ——
    "station_name", "latitude", "longitude", "rent_daily_cents",
})

#: 明确"读过但不用"的量，逐条留原因——换批次时一定有人问。
IGNORED_COLUMNS = {
    "demand_multiplier": "日历计划系数（第八线实测单变量 AUC 0.4932，等于噪声），不进任何候选集",
    "grid_energy_wh": "遥测/会话的电网侧电量，与桩级表码增量同源，已取 meter_wh 一路",
    "grid_cost_cents": "成本列，与设备健康无因果通路；留在 IGNORED 供换批复核",
    "queue_id": "工单/尝试表里的排队外键：排队是站级拥堵现象，本线动作是单桩维护，不引入",
    "reservation_id": "预约外键同上；预约失败（EXPIRED/CANCELLED）不是设备故障",
    "latitude": "站坐标——选址类信息，新站选址是预留项本期不做；城市结构已由 site_type 承载",
    "longitude": "同 latitude",
}

#: 技术失败码（第六/七线同口径的三码）；排队/用户侧失败不算设备病。
TECH_FAIL_REASONS = frozenset({"CONNECTOR_HANDSHAKE", "APP_TIMEOUT", "AUTH_FAILED"})

#: 工单故障类型全集（实测四类），历史哑变量按这个顺序展开。
FAULT_TYPES = ("COMMUNICATION", "CONNECTOR", "POWER_MODULE", "COOLING")

#: 收缩强度（单位=天，先验是"每桩每工单日率"）：桩级 20 天、站级 15 天、失败率 20 次。
ALPHA_CHARGER = 20.0
ALPHA_STATION = 15.0
ALPHA_ATTEMPT = 20.0

WEATHER_LAG_COLUMNS = ("temp_c_mean", "temp_c_max", "humidity_mean", "rainfall_mm_sum")

MINUTES_PER_SLOT = 5.0  # interval_seconds 恒为 300，构造时断言。


# --------------------------------------------------------------------------- 事件源装载


def load_tickets() -> tuple[pd.DataFrame, dict]:
    """维修工单台账：标签源 + 历史源。断言全部针对状态机时间链。"""
    frame = common.load_clean_table("maintenance_tickets")
    for column in ("reported_at", "accepted_at", "work_started_at", "restored_at"):
        frame[column] = pd.to_datetime(frame[column])
    assert frame["ticket_id"].is_unique, "工单主键重复"
    assert frame["charger_id"].notna().all() and frame["station_id"].notna().all()
    assert (frame["reported_at"] <= frame["accepted_at"]).all(), "受理早于报修"
    assert (frame["accepted_at"] <= frame["work_started_at"]).all(), "开工早于受理"
    known = frame[frame["restored_at"].notna()]
    assert (known["work_started_at"] <= known["restored_at"]).all(), "修复早于开工"
    assert set(frame["fault_type"].unique()) <= set(FAULT_TYPES), \
        "故障类型出现未登记取值，历史哑变量名单要重核"
    assert set(frame["status"].unique()) <= {"RESOLVED", "IN_PROGRESS"}, \
        "工单状态出现未登记取值（本列禁入特征，但口径变化要留痕）"
    frame = frame.sort_values("reported_at", kind="stable").reset_index(drop=True)
    notes = {"tickets": int(len(frame)),
             "chargers": int(frame["charger_id"].nunique()),
             "stations": int(frame["station_id"].nunique()),
             "unrestored": int(frame["restored_at"].isna().sum()),
             "severityMix": {str(k): int(v) for k, v in frame["severity"].value_counts().items()},
             "statusMix": {str(k): int(v) for k, v in frame["status"].value_counts().items()},
             "faultMix": {str(k): int(v) for k, v in frame["fault_type"].value_counts().items()},
             "reportedSpan": [str(frame["reported_at"].min()), str(frame["reported_at"].max())]}
    return frame, notes


# --------------------------------------------------------------------------- 稠密面板与标签


def build_panel(bounds: dict[str, pd.Timestamp]) -> tuple[pd.DataFrame, dict]:
    """75 桩 × 178 北京日的稠密面板：身份、静态属性、决策时刻、切分段、删失标记。

    ``maintenance_events`` 表整表不读——它是工单状态流水（note 全部"模拟维修流程"），
    信息是 tickets 四个时刻的重复。桩的 ``commissioned_at`` / 站的 ``opened_at`` 是
    "何时投运"的静态事实，只以"决策时点已投运多少天"的形式进特征。
    """
    chargers = common.load_clean_table("chargers", columns=[
        "charger_id", "station_id", "connector_type", "rated_power_kw",
        "commissioned_at", "manufacturer", "model"])
    stations = common.load_clean_table("stations", columns=[
        "station_id", "city_id", "site_type", "transformer_kw", "opened_at"])
    chargers = chargers.rename(columns={"model": "charger_model",
                                        "station_id": "charger_station_id"})
    chargers["commissioned_at"] = pd.to_datetime(chargers["commissioned_at"])
    stations["opened_at"] = pd.to_datetime(stations["opened_at"])

    first_day = (bounds["startInclusive"] + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
                 ).normalize()
    last_day = (bounds["testEndExclusive"] + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
                ).normalize() - pd.Timedelta(days=1)
    days = pd.date_range(first_day, last_day)
    key = pd.MultiIndex.from_product([chargers["charger_id"].sort_values(), days],
                                     names=["charger_id", "business_date"])
    frame = pd.DataFrame(index=key).reset_index()
    frame = frame.merge(chargers, on="charger_id", how="left", validate="many_to_one")
    # 站的 station_id 经 charger_station_id 接入（第八线同款改名法：两侧同名会静默变 _x/_y，
    # 改名后反而多一道"桩所属站与站表一致"的核对）。
    frame = frame.merge(stations, left_on="charger_station_id", right_on="station_id",
                        how="left", validate="many_to_one")
    assert (frame["charger_station_id"] == frame["station_id"]).all(), \
        "桩所属站与站表不一致（换批次要先查这条）"
    frame = frame.drop(columns=["charger_station_id"])
    assert len(frame) == len(key) and frame["site_type"].notna().all()
    per_station = chargers.groupby("charger_station_id")["charger_id"].size()
    frame["n_chargers_in_station"] = frame["station_id"].map(per_station).astype("float64")
    assert frame["n_chargers_in_station"].notna().all()

    # 决策时点：北京零点，写成 UTC naive。桩/站投运必须早于它，否则"年龄"为负。
    frame["day_start_utc"] = frame["business_date"] - pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
    assert (frame["commissioned_at"] < frame["day_start_utc"]).all(), \
        "存在投运时刻不早于决策时点的桩日（设备年龄为负，静态表时间线矛盾）"
    assert (frame["opened_at"] < frame["day_start_utc"]).all(), "站点投运时刻不早于决策时点"
    frame["charger_age_days"] = ((frame["day_start_utc"] - frame["commissioned_at"])
                                 .dt.total_seconds() / 86_400.0)
    frame["day_index"] = ((frame["business_date"] - first_day).dt.days).astype("float64")
    frame["day_of_week"] = frame["business_date"].dt.dayofweek.astype("float64")
    frame = frame.drop(columns=["commissioned_at", "opened_at"])

    frame["split"] = common.assign_split(frame["day_start_utc"], bounds)
    panel_notes = {"chargers": int(chargers["charger_id"].nunique()),
                   "stations": int(stations["station_id"].nunique()),
                   "days": int(len(days)),
                   "rows": int(len(frame)),
                   "panelDays": [str(first_day.date()), str(last_day.date())],
                   "connectorMix": {str(k): int(v) for k, v in chargers["connector_type"].value_counts().items()},
                   "manufacturerMix": {str(k): int(v) for k, v in chargers["manufacturer"].value_counts().items()}}
    return frame, panel_notes


def attach_label(frame: pd.DataFrame, tickets: pd.DataFrame,
                 bounds: dict[str, pd.Timestamp]) -> tuple[pd.DataFrame, dict]:
    """前向 7 日工单标签 + purge 标记 + 右端删失标记。

    标签窗 ``[day_start, day_start+7d)``（UTC naive）恰好等于北京日 ``[D, D+6]``。
    ``censored``：窗尾超出**工单数据可见终点**（末张工单的北京日 +1）的行——这些行的
    y_ticket7 是"截至数据结束的计数"，可能被系统性低估，既不进训练也不进盲测，只留痕。
    ``purged``：TRAIN/VALIDATION 段尾标签窗伸进下一段时间域的行（防跨段泄漏，见 common）。
    """
    last_reported_beijing = ((tickets["reported_at"] + pd.Timedelta(
        hours=common.BUSINESS_OFFSET_HOURS)).max().normalize())
    observation_end = last_reported_beijing + pd.Timedelta(days=1) - pd.Timedelta(
        hours=common.BUSINESS_OFFSET_HOURS)  # → UTC naive 的"工单数据终点"

    rep = {k: np.sort(pd.to_datetime(p["reported_at"]).to_numpy(dtype="datetime64[ns]"))
           for k, p in tickets.groupby("charger_id", observed=True)}
    t_ns = frame["day_start_utc"].to_numpy(dtype="datetime64[ns]")
    hi_ns = (frame["day_start_utc"] + pd.Timedelta(days=common.LABEL_HORIZON_DAYS)
             ).to_numpy(dtype="datetime64[ns]")
    counts = np.zeros(len(frame), dtype="int64")
    for charger, part in frame.groupby("charger_id", observed=True):
        arr = rep.get(charger)
        if arr is None:
            continue
        idx = part.index.to_numpy()
        lo = np.searchsorted(arr, t_ns[idx], side="left")
        hi = np.searchsorted(arr, hi_ns[idx], side="left")
        counts[idx] = hi - lo
    frame["tickets_next7d"] = counts
    frame["y_ticket7"] = (counts >= 1).astype("int64")
    frame["label_end"] = frame["business_date"] + pd.Timedelta(days=common.LABEL_HORIZON_DAYS)
    assert (frame["label_end"] == frame["day_start_utc"]
            + pd.Timedelta(days=common.LABEL_HORIZON_DAYS)
            + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)).all(), \
        "label_end（北京轴）与 day_start_utc（UTC 轴）的 +7 天窗不再重合——时区换算被谁动了"

    # 右端删失：标签窗尾越过工单数据可见终点。
    frame["censored"] = (frame["day_start_utc"]
                         + pd.Timedelta(days=common.LABEL_HORIZON_DAYS)) > observation_end
    # 段间 purge：各段**自己的**尾部标签窗咬进下一段时间域的行。TEST 是最后一段，无段可咬、
    # 不 purge（它只需要删失）；把 purge 条件套到全部行会把 VAL/TEST 整段错杀。
    def beijing(ts: pd.Timestamp) -> pd.Timestamp:
        return ts + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)

    is_train = (frame["split"] == "TRAIN").to_numpy()
    is_val = (frame["split"] == "VALIDATION").to_numpy()
    train_keep = common.purge_rows(frame["business_date"], beijing(bounds["trainEndExclusive"]))
    val_keep = common.purge_rows(frame["business_date"], beijing(bounds["validationEndExclusive"]))
    purged = (is_train & ~train_keep) | (is_val & ~val_keep)
    frame["purged"] = purged

    label_notes = {
        "horizonDays": common.LABEL_HORIZON_DAYS,
        "observationEndBeijing": str(observation_end
                                     + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)),
        "baseRate": round(float(frame["y_ticket7"].mean()), 5),
        "positives": int(frame["y_ticket7"].sum()),
        "ticketsTotal": int(frame["tickets_next7d"].sum()),
        "multiTicketRows": int((frame["tickets_next7d"] > 1).sum()),
        "censoredRows": int(frame["censored"].sum()),
        "purgedRows": int(frame["purged"].sum()),
        "splits": {name: int((frame["split"] == name).sum())
                   for name in ("TRAIN", "VALIDATION", "TEST")},
        "keptBySplit": {name: int(((frame["split"] == name) & ~frame["purged"]
                                   & ~frame["censored"]).sum())
                        for name in ("TRAIN", "VALIDATION", "TEST")},
    }
    return frame, label_notes


# --------------------------------------------------------------------------- 日历与天气（T−1 日因果聚合）


def build_day_context() -> pd.DataFrame:
    """城×北京日历日的上下文表：日历事实 + 天气日聚合（与第八线同款）。"""
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
    notes = {
        "weatherRule": "天气只取 T−1 日的城×日聚合，今天的天气不进合并；shift 在日×城矩阵上进行，绝不跨城",
        "coverage": {c: round(float(frame[c].notna().mean()), 4)
                     for c in ("weather_prev", "temp_c_max_prev", "rainfall_mm_sum_prev",
                               "scenario_event", "is_weekend")},
        "sameDayWeatherNotMerged": True,
    }
    return frame, notes


# --------------------------------------------------------------------------- 共用小件


def _query(frame: pd.DataFrame, key_column: str) -> pd.DataFrame:
    """给 ``AsOfCounts`` 用的最小查询表：实体键 + 查询时刻（本桩本行的 day_start_utc）。"""
    return pd.DataFrame({"__query_time": pd.to_datetime(frame["day_start_utc"]),
                         key_column: frame[key_column].to_numpy()})


def time_gap(later, earlier, scale: float) -> np.ndarray:
    """两组时间戳之差除以 ``scale``（天 / 小时）；任一侧 NaT 则该行 NaN。

    刻意走 Series 的 timedelta 算术而不是 int64 减法：``strict_last`` 对"从来没有过"的行给 NaT，
    而 NaT 转 int64 是哨兵最小值——这种边界不该用溢出行为来兜（第八线同款陷阱，同款写法）。
    """
    base = pd.Series(pd.to_datetime(np.asarray(later, dtype="datetime64[ns]")))
    past = pd.Series(pd.to_datetime(np.asarray(earlier, dtype="datetime64[ns]")))
    gap = (base - past).dt.total_seconds()
    assert len(gap) == len(base)
    return (gap / scale).to_numpy(dtype="float64")


# --------------------------------------------------------------------------- 工单历史（双轴 as-of）


def attach_ticket_history(frame: pd.DataFrame, tickets: pd.DataFrame,
                          bounds: dict[str, pd.Timestamp]) -> tuple[pd.DataFrame, dict]:
    """这台桩"到此为止"的维修史：报修轴计数 + 修复轴的时长 + 收缩率 + 上一张的故障类型。

    两条轴各走各的引擎，**查询时刻统一是 day_start_utc**：
      · 报修引擎的事件时刻 = ``reported_at``——同刻报来的工单按"还不知道"处理；
      · 修复引擎的事件时刻 = ``restored_at``（只含有修复时刻的 417 张；NaT 那张未修复工单
        不进这个引擎——否则 int64 哨兵会把它伪装成"最早可见"）。
    ``open_ticket_at_start`` = 报修累计 − 修复累计（下限 0）：与第七线"报修−已恢复"同一手法，
    那张 IN_PROGRESS 工单会让它所在桩的正午前行恒 ≥1，这是决策时点**真看得见**的状态。
    收缩率把"每桩每日历日的历史工单率"当先验，先验本身也只取"截至查询时刻之前"的全局经验值。
    """
    reported = pd.to_datetime(tickets["reported_at"])
    helper = tickets.assign(_one=1.0)
    resolved_helper = tickets[tickets["restored_at"].notna()].assign(
        _one=1.0,
        _downtime_h=((pd.to_datetime(tickets["restored_at"]) - reported)
                     .dt.total_seconds() / 3_600.0))
    query_charger = _query(frame, "charger_id")
    query_station = _query(frame, "station_id")

    report_engine = common.AsOfCounts(helper, key="charger_id", stamps="reported_at", value="_one")
    station_engine = common.AsOfCounts(helper, key="station_id", stamps="reported_at", value="_one")
    restore_engine = common.AsOfCounts(resolved_helper, key="charger_id", stamps="restored_at",
                                       value="_one")

    prior_counts, _ = common.global_prior_at(query_charger["__query_time"], reported)
    elapsed = ((frame["day_start_utc"] - bounds["startInclusive"]).dt.total_seconds()
               / 86_400.0).to_numpy(dtype=float)
    n_chargers = float(frame["charger_id"].nunique())
    exposure = np.maximum(elapsed, 1.0)
    global_rate = prior_counts / (n_chargers * exposure)  # 每桩每日历日的截至当前经验率

    counts_prior, _ = report_engine.cumulative(query_charger, "charger_id", "_one")
    frame["tickets_prior"] = counts_prior
    frame["tickets_7d"] = report_engine.window(query_charger, "charger_id", 7.0, "_one")[0]
    frame["tickets_30d"] = report_engine.window(query_charger, "charger_id", 30.0, "_one")[0]
    frame["tickets_90d"] = report_engine.window(query_charger, "charger_id", 90.0, "_one")[0]
    frame["charger_ticket_rate_shrunk"] = common.smoothed(
        exposure, counts_prior, ALPHA_CHARGER, global_rate)

    station_counts, _ = station_engine.cumulative(query_station, "station_id", "_one")
    frame["station_ticket_rate_shrunk"] = common.smoothed(
        frame["n_chargers_in_station"].to_numpy(dtype=float) * exposure,
        station_counts, ALPHA_STATION, global_rate)

    restored_counts, _ = restore_engine.cumulative(query_charger, "charger_id", "_one")
    frame["open_ticket_at_start"] = np.maximum(counts_prior - restored_counts, 0.0)

    last_report = common.strict_last(frame, "day_start_utc", "charger_id",
                                     helper[["charger_id", "reported_at", "fault_type"]],
                                     "reported_at", ["fault_type"])
    frame["last_ticket_time"] = pd.to_datetime(
        last_report["__last_time"].where(last_report["fault_type"].notna()))
    frame["days_since_last_report"] = time_gap(frame["day_start_utc"], frame["last_ticket_time"],
                                               86_400.0)
    seen = last_report["fault_type"].to_numpy(dtype=object)
    for fault in FAULT_TYPES:
        frame[f"last_ticket_fault_{fault.lower()}"] = np.where(
            pd.isna(seen), np.nan, (seen == fault).astype(float))

    last_restore = common.strict_last(frame, "day_start_utc", "charger_id",
                                      resolved_helper[["charger_id", "restored_at", "_downtime_h"]],
                                      "restored_at", ["_downtime_h"])
    frame["last_repair_hours"] = last_restore["_downtime_h"].to_numpy(dtype="float64")
    frame["last_restore_time"] = pd.to_datetime(
        last_restore["__last_time"].where(last_restore["_downtime_h"].notna()))
    frame["days_since_last_restore"] = time_gap(frame["day_start_utc"], frame["last_restore_time"],
                                                86_400.0)

    notes = {
        "axes": {"countsAndFault": "reported_at（严格早于 day_start_utc）",
                 "downtimeAndRate": "restored_at（只有已修复工单；NaT 不进引擎）",
                 "shrinkagePrior": "全局'每桩每日工单率'，同样只取查询时刻之前"},
        "rowsWithNoTicketHistory": int((counts_prior == 0).sum()),
        "rowsWithOpenTicket": int((frame["open_ticket_at_start"] > 0).sum()),
        "chargerRateSpread": {
            "min": round(float(frame["charger_ticket_rate_shrunk"].min()), 6),
            "p50": round(float(frame["charger_ticket_rate_shrunk"].median()), 6),
            "max": round(float(frame["charger_ticket_rate_shrunk"].max()), 6)},
        "downtimeHours": {"p50": float(resolved_helper["_downtime_h"].median()),
                          "p90": float(resolved_helper["_downtime_h"].quantile(0.9)),
                          "max": float(resolved_helper["_downtime_h"].max())},
    }
    return frame, notes


# --------------------------------------------------------------------------- 使用与尝试


def load_sessions_stream() -> tuple[pd.DataFrame, dict]:
    frame = common.load_clean_table("charging_sessions", columns=[
        "session_id", "charger_id", "ended_at", "energy_wh", "status"])
    frame["ended_at"] = pd.to_datetime(frame["ended_at"])
    assert frame["session_id"].is_unique and frame["charger_id"].notna().all()
    assert frame["ended_at"].notna().all(), "存在没有结束时刻的会话（本线按 ended_at 可见）"
    assert frame["energy_wh"].notna().all(), "会话电量存在缺失，负荷窗口会出现假 0"
    notes = {"sessions": int(len(frame)),
             "statusMix": {str(k): int(v) for k, v in frame["status"].value_counts().items()}}
    return frame, notes


def attach_usage(frame: pd.DataFrame, sessions: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """这台桩到此为止用了多少：会话数/电量/异常收尾，全部按 ``ended_at`` 可见。

    WAITING_PAYMENT（结算挂起）是用户与站端的异常信号——桩充完了电却收不了钱，与"未来 7 天
    被报修"之间有没有因果不由模型说了算，给它 as-of 合法的机会，消融组会给出诚实答案。
    """
    helper = sessions.assign(_one=1.0, _kwh=sessions["energy_wh"].to_numpy(dtype=float) / 1000.0,
                             _abnormal=(sessions["status"] == "WAITING_PAYMENT")
                             .to_numpy(dtype=float))
    query = _query(frame, "charger_id")
    count_engine = common.AsOfCounts(helper, key="charger_id", stamps="ended_at", value="_one")
    kwh_engine = common.AsOfCounts(helper, key="charger_id", stamps="ended_at", value="_kwh")
    abnormal_engine = common.AsOfCounts(helper, key="charger_id", stamps="ended_at",
                                        value="_abnormal")
    frame["usage_sessions_7d"] = count_engine.window(query, "charger_id", 7.0, "_one")[0]
    frame["usage_sessions_30d"] = count_engine.window(query, "charger_id", 30.0, "_one")[0]
    _, kwh_30 = kwh_engine.window(query, "charger_id", 30.0, "_kwh")
    frame["usage_energy_kwh_30d"] = kwh_30
    _, ab_30 = abnormal_engine.window(query, "charger_id", 30.0, "_abnormal")
    frame["usage_abnormal_close_30d"] = ab_30
    notes = {"abnormalRows": int(helper["_abnormal"].sum()),
             "usageWindow": "半开窗 [t−30d, t)：会话以 ended_at 宣告完成，进行中的会话不计"}
    return frame, notes


def load_attempts_stream() -> tuple[pd.DataFrame, dict]:
    frame = common.load_clean_table("charging_attempts", columns=[
        "attempt_id", "charger_id", "attempted_at", "failure_reason"])
    frame["attempted_at"] = pd.to_datetime(frame["attempted_at"])
    assert frame["attempt_id"].is_unique and frame["attempted_at"].notna().all()
    frame["_tech"] = frame["failure_reason"].isin(TECH_FAIL_REASONS).to_numpy(dtype=float)
    notes = {"attempts": int(len(frame)), "techFails": int(frame["_tech"].sum()),
             "failureMix": {str(k): int(v)
                            for k, v in frame["failure_reason"].value_counts().items()}}
    return frame, notes


def attach_attempts(frame: pd.DataFrame, attempts: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """技术失败前兆：三个硬故障码的窗口计数 + 收缩失败率（先验=全局截至查询时刻的失败占比）。"""
    helper = attempts.assign(_one=1.0)
    tech = helper[helper["_tech"] > 0]
    query = _query(frame, "charger_id")
    all_engine = common.AsOfCounts(helper, key="charger_id", stamps="attempted_at", value="_one")
    tech_engine = common.AsOfCounts(tech, key="charger_id", stamps="attempted_at", value="_one")
    frame["attempts_7d"] = all_engine.window(query, "charger_id", 7.0, "_one")[0]
    frame["attempts_30d"] = all_engine.window(query, "charger_id", 30.0, "_one")[0]
    tech_7 = tech_engine.window(query, "charger_id", 7.0, "_one")[0]
    tech_counts_30, tech_sums_30 = tech_engine.window(query, "charger_id", 30.0, "_one")
    frame["tech_fail_7d"] = tech_7
    frame["tech_fail_30d"] = tech_sums_30
    prior_counts, prior_sums = common.global_prior_at(
        query["__query_time"], attempts["attempted_at"], attempts["_tech"].to_numpy(dtype=float))
    fail_prior = np.where(prior_counts > 0, prior_sums / np.maximum(prior_counts, 1.0), 0.0)
    all_counts_30 = all_engine.window(query, "charger_id", 30.0, "_one")[0]
    frame["tech_fail_rate_30d"] = common.smoothed(all_counts_30, tech_counts_30,
                                                  ALPHA_ATTEMPT, fail_prior)
    notes = {"techCodes": sorted(TECH_FAIL_REASONS),
             "ratePriorRange": [round(float(fail_prior.min()), 5), round(float(fail_prior.max()), 5)]}
    return frame, notes


# --------------------------------------------------------------------------- 遥测健康（日表整体滞后）


def build_telemetry_daily() -> tuple[pd.DataFrame, dict]:
    """3,888,000 行 5 分钟栅格 → 桩×北京日日表。每格恒 300 秒，槽位数×5=分钟。

    ``zero_power``：state=CHARGING 却功率≤0 的格子——"说在充、其实没充"是桩 internals 出病
    最直接的遥测证据。``meter_kwh`` 用表码日增量（max−min），与电量列同源时优先表码
    （表码是结算依据，仿真里也按单调累计生成）。
    """
    tel = common.load_clean_table("charger_telemetry", columns=[
        "charger_id", "recorded_at", "state", "power_kw", "meter_wh", "interval_seconds"])
    tel["recorded_at"] = pd.to_datetime(tel["recorded_at"])
    assert (pd.to_numeric(tel["interval_seconds"]) == 300).all(), "遥测栅格不再是 300 秒，分钟换算要改"
    tel["business_date"] = (tel["recorded_at"] + pd.Timedelta(
        hours=common.BUSINESS_OFFSET_HOURS)).dt.normalize()
    state = tel["state"].astype("category")
    flags = pd.DataFrame({
        "charger_id": tel["charger_id"].to_numpy(),
        "business_date": tel["business_date"].to_numpy(),
        "maint": (state == "MAINTENANCE").to_numpy(dtype="int64"),
        "offline": (state == "OFFLINE").to_numpy(dtype="int64"),
        "charging": (state == "CHARGING").to_numpy(dtype="int64"),
        "zero_power": ((state == "CHARGING") & (pd.to_numeric(tel["power_kw"]) <= 0))
        .to_numpy(dtype="int64"),
    })
    grouped = flags.groupby(["charger_id", "business_date"], observed=True).sum()
    grouped *= MINUTES_PER_SLOT
    grouped = grouped.rename(columns=lambda c: f"tel_{c}_min")
    meter = tel.assign(meter=pd.to_numeric(tel["meter_wh"], errors="raise")).groupby(
        ["charger_id", "business_date"], observed=True)["meter"].agg(lambda s: s.max() - s.min())
    daily = grouped.reset_index()
    daily["tel_meter_kwh"] = (meter.reindex(
        pd.MultiIndex.from_arrays([daily["charger_id"], daily["business_date"]])
        .set_names(["charger_id", "business_date"])).to_numpy(dtype=float) / 1000.0)
    grid_days = int(daily["business_date"].nunique())
    chargers_n = int(daily["charger_id"].nunique())
    assert len(daily) == grid_days * chargers_n, "遥测日表不是稠密网格（桩×日缺格），滞后窗口会出现假 0"
    notes = {"rows": int(len(tel)), "chargers": chargers_n, "days": grid_days,
             "daySpan": [str(daily["business_date"].min().date()),
                         str(daily["business_date"].max().date())],
             "maintMinutesTotal": float(daily["tel_maint_min"].sum()),
             "offlineMinutesTotal": float(daily["tel_offline_min"].sum()),
             "zeroPowerMinutesTotal": float(daily["tel_zero_power_min"].sum())}
    return daily, notes


TELEMETRY_DAILY_COLUMNS = ("tel_maint_min", "tel_offline_min", "tel_charging_min",
                           "tel_zero_power_min", "tel_meter_kwh")


def attach_telemetry_history(frame: pd.DataFrame, daily: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """把日表整体滞后挂到面板：``*_prev`` 只含 D−1 日，``*_7d`` 只含 [D−7, D−1] 完整七天。

    滞后靠桩×日矩阵上的 ``shift`` 实现（与天气同款，pivot 保证绝不跨桩）。7 日窗要求
    min_periods=7——缺任何一天就是 NaN，不给"部分窗"充"完整窗"的机会；面板头 7 天相应为空，
    HistGB 原生消化 NaN。当行自己的 D 日遥测**根本不参与**：今天的桩在不在维修态，
    在今天的零点之前不可知。
    """
    out = frame[["charger_id", "business_date"]].copy()
    for column in TELEMETRY_DAILY_COLUMNS:
        matrix = daily.pivot(index="business_date", columns="charger_id", values=column).sort_index()
        lagged = matrix.shift(1)
        pieces = {"prev": lagged}
        pieces["7d"] = lagged.rolling(common.LABEL_HORIZON_DAYS).sum()
        keys = pd.MultiIndex.from_arrays([out["business_date"].to_numpy(),
                                          out["charger_id"].to_numpy()],
                                         names=["business_date", "charger_id"])
        for tag, part in pieces.items():
            # pivot 的 index=日、columns=桩，stack 出来的 MultiIndex 天然是 (日, 桩)——**不许
            # 换序**（换序后 reindex 全 miss、整列静默 NaN，第八线同款代码顺序恰好相反）。
            flat = part.stack()
            flat.index = flat.index.set_names(["business_date", "charger_id"])
            out[f"{column}_{tag}"] = flat.reindex(keys).to_numpy(dtype="float64")
    columns = [f"{c}_{tag}" for c in TELEMETRY_DAILY_COLUMNS for tag in ("prev", "7d")]
    notes = {"lagRule": "prev=D−1 日；7d=[D−7,D−1] 完整七天（缺格即 NaN，min_periods=7）；当日 D 不参与",
             "coverage": {c: round(float(out[c].notna().mean()), 4) for c in columns}}
    frame = pd.concat([frame, out[columns].set_index(frame.index)], axis=1)
    notes = {"lagRule": "prev=D−1 日；7d=[D−7,D−1] 完整七天（缺格即 NaN，min_periods=7）；当日 D 不参与",
             "coverage": {c: round(float(out[c].notna().mean()), 4) for c in columns},
             # 诚实负结果：CHARGING 格子里 power_kw 最小 0.012、无一 ≤0——"说在充其实没充"
             # 这一前兆信号在**本批模拟数据里不存在**，两列被判常数剔除是正确行为，不是事故。
             "zeroPowerConstant": bool(daily["tel_zero_power_min"].max() == 0.0)}
    return frame, notes


# --------------------------------------------------------------------------- 泄漏审计


AUDITED_FEATURES = ("tickets_30d", "tickets_prior", "days_since_last_report",
                    "open_ticket_at_start", "last_repair_hours", "days_since_last_restore",
                    "charger_ticket_rate_shrunk", "station_ticket_rate_shrunk",
                    "last_ticket_fault_connector", "usage_sessions_30d", "usage_energy_kwh_30d",
                    "usage_abnormal_close_30d", "tech_fail_7d", "tech_fail_rate_30d",
                    "tel_maint_min_prev", "tel_zero_power_min_7d", "temp_c_max_prev")

#: 本线独有：前向标签三量也用暴力路径独立重算（标签窗与历史可见性是两套算术，错一边就全错）。
AUDITED_LABELS = ("y_ticket7", "tickets_next7d", "label_end")


def verify_no_future_leak(frame: pd.DataFrame, tickets: pd.DataFrame, sessions: pd.DataFrame,
                          attempts: pd.DataFrame, daily: pd.DataFrame, context: pd.DataFrame,
                          bounds: dict[str, pd.Timestamp], sample: int = 250,
                          seed: int = common.SEED) -> dict:
    """随机抽若干桩-日，用**全表布尔掩码暴力重算**，要求逐条 0 偏差。

    审计刻意走另一条代码路径（不排序、不二分、不 merge_asof、不复用 ``AsOfCounts``/
    ``global_prior_at``/pivot-shift）。重点盯本线三条新语义：① 时长走**修复轴**
    （reported<t 不够，必须 restored<t 才知"修了多久"）；② open = 报修累计−修复累计；
    ③ 遥测只能看 D−1 及更早；外加前向标签窗的 [t, t+7d) 半开算术。
    """
    rng = random.Random(seed)
    rows = sorted(rng.sample(range(len(frame)), min(sample, len(frame))))
    all_reported = pd.to_datetime(tickets["reported_at"])
    tickets_by_charger = {k: p for k, p in tickets.groupby("charger_id", observed=True)}
    tickets_by_station = {k: p for k, p in tickets.groupby("station_id", observed=True)}
    sessions_by_charger = {k: p for k, p in sessions.groupby("charger_id", observed=True)}
    attempts_by_charger = {k: p for k, p in attempts.groupby("charger_id", observed=True)}
    tel_by_charger = {k: p for k, p in daily.groupby("charger_id", observed=True)}
    context_by_city = {k: p for k, p in context.groupby("city_id", observed=True)}
    n_chargers = float(frame["charger_id"].nunique())
    empty = tickets.iloc[0:0]

    worst = {name: 0.0 for name in list(AUDITED_FEATURES) + list(AUDITED_LABELS)}
    for position in rows:
        row = frame.iloc[position]
        stamp = pd.to_datetime(row["day_start_utc"])
        horizon = stamp + pd.Timedelta(days=common.LABEL_HORIZON_DAYS)
        day30 = stamp - pd.Timedelta(days=30.0)

        part = tickets_by_charger.get(row["charger_id"], empty)
        reported = pd.to_datetime(part["reported_at"])
        restored = pd.to_datetime(part["restored_at"])
        before = (reported < stamp).to_numpy()
        w30 = before & (reported >= day30).to_numpy()
        elapsed = max((stamp - bounds["startInclusive"]).total_seconds() / 86_400.0, 1.0)
        global_count = int((all_reported < stamp).sum())
        global_rate = global_count / (n_chargers * elapsed)

        station_part = tickets_by_station.get(row["station_id"], empty)
        s_before = (pd.to_datetime(station_part["reported_at"]) < stamp).to_numpy()

        open_truth = float(int(before.sum())
                           - int(((restored < stamp) & restored.notna()).sum()))

        resolved_part = part[part["restored_at"].notna()]
        rest_stamp = pd.to_datetime(resolved_part["restored_at"])
        last_repair_hours, last_restore = np.nan, pd.NaT
        if len(resolved_part) and (rest_stamp < stamp).any():
            pick = rest_stamp[rest_stamp < stamp].max()
            hit = resolved_part[resolved_part["restored_at"] == pick].iloc[-1]
            last_repair_hours = float((hit["restored_at"] - hit["reported_at"])
                                      .total_seconds() / 3_600.0)
            last_restore = pd.Timestamp(pick)

        last_fault = np.nan
        if before.any():
            latest_reported = reported[before].max()
            last_fault = str(part[part["reported_at"] == latest_reported]
                             .iloc[-1]["fault_type"])

        s_part = sessions_by_charger.get(row["charger_id"], sessions.iloc[0:0])
        s_ended = pd.to_datetime(s_part["ended_at"])
        s_w30 = ((s_ended < stamp) & (s_ended >= day30)).to_numpy()
        a_part = attempts_by_charger.get(row["charger_id"], attempts.iloc[0:0])
        a_stamp = pd.to_datetime(a_part["attempted_at"])
        a_w7 = ((a_stamp < stamp) & (a_stamp >= stamp - pd.Timedelta(days=7.0))).to_numpy()
        a_w30 = ((a_stamp < stamp) & (a_stamp >= day30)).to_numpy()
        a_tech = a_part["_tech"].to_numpy(dtype=float)
        g_tech = attempts["_tech"].to_numpy(dtype=float)[(pd.to_datetime(
            attempts["attempted_at"]) < stamp).to_numpy()]
        fail_prior = float(g_tech.mean()) if len(g_tech) else 0.0

        tel_part = tel_by_charger.get(row["charger_id"], daily.iloc[0:0])
        tel_day = pd.to_datetime(tel_part["business_date"])
        # 日表键是**北京零点归一化**的日历日；先还原本行的北京日，再取 D−1 与 [D−7, D−1]。
        beijing_day = (stamp + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)).normalize()
        prev_tel_day = beijing_day - pd.Timedelta(days=1)
        d_prev = tel_part[tel_day == prev_tel_day]
        w7_days = ((tel_day >= prev_tel_day - pd.Timedelta(days=common.LABEL_HORIZON_DAYS - 1))
                   & (tel_day <= prev_tel_day))
        tel_complete = int(w7_days.sum()) == common.LABEL_HORIZON_DAYS

        city_part = context_by_city.get(row["city_id"], context.iloc[0:0])
        prev_day = (stamp + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
                    - pd.Timedelta(days=1)).normalize()
        prev_row = city_part[city_part["business_date"] == prev_day]

        truth = {
            "tickets_30d": float(w30.sum()),
            "tickets_prior": float(before.sum()),
            "days_since_last_report": np.nan if not before.any()
            else (stamp - reported[before].max()).total_seconds() / 86_400.0,
            "open_ticket_at_start": max(open_truth, 0.0),
            "last_repair_hours": last_repair_hours,
            "days_since_last_restore": np.nan if pd.isna(last_restore)
            else (stamp - last_restore).total_seconds() / 86_400.0,
            "charger_ticket_rate_shrunk": (float(before.sum()) + ALPHA_CHARGER * global_rate)
            / (elapsed + ALPHA_CHARGER),
            "station_ticket_rate_shrunk": (float(s_before.sum())
                                           + ALPHA_STATION * global_rate)
            / (float(row["n_chargers_in_station"]) * elapsed + ALPHA_STATION),
            "last_ticket_fault_connector": np.nan if not before.any()
            else float(last_fault == "CONNECTOR"),
            "usage_sessions_30d": float(s_w30.sum()),
            "usage_energy_kwh_30d": float(
                pd.to_numeric(s_part["energy_wh"]).to_numpy(dtype=float)[s_w30].sum()) / 1000.0,
            "usage_abnormal_close_30d": float(
                (s_part["status"].to_numpy() == "WAITING_PAYMENT")[s_w30].sum()),
            "tech_fail_7d": float(a_tech[a_w7].sum()),
            "tech_fail_rate_30d": (float(a_tech[a_w30].sum()) + ALPHA_ATTEMPT * fail_prior)
            / (float(a_w30.sum()) + ALPHA_ATTEMPT),
            "tel_maint_min_prev": np.nan if len(d_prev) == 0
            else float(d_prev["tel_maint_min"].iloc[0]),
            "tel_zero_power_min_7d": np.nan if not tel_complete
            else float(tel_part[w7_days]["tel_zero_power_min"].sum()),
            "temp_c_max_prev": np.nan if len(prev_row) == 0
            else float(prev_row["temp_c_max"].iloc[0]),
            # —— 前向标签：独立算窗口交集，防标签算术与特征算术不同步 ——
            "tickets_next7d": float(((all_reported >= stamp) & (all_reported < horizon)
                                     & (tickets["charger_id"] == row["charger_id"])).sum()),
            "y_ticket7": float(((all_reported >= stamp) & (all_reported < horizon)
                                & (tickets["charger_id"] == row["charger_id"])).any()),
            "label_end": (pd.Timestamp(row["business_date"])
                          + pd.Timedelta(days=common.LABEL_HORIZON_DAYS)).value,
        }
        for name, expected in truth.items():
            if name == "label_end":  # 时间戳列按 ns 原值比，不走 float（NaN 语义也不同）
                worst[name] = max(worst[name],
                                  float(abs(pd.Timestamp(row[name]).value - int(expected))))
                continue
            got = float(row[name])
            if pd.isna(expected) or pd.isna(got):
                # NaN 与数值的差是 NaN，`>1e-9` 判不出来——本行全 NaN 特征曾在静默状态下溜过审计，
                # 所以"一边缺、一边有"本身就是最大偏差，直接记 inf。
                if pd.isna(expected) != pd.isna(got):
                    worst[name] = float("inf")
                continue
            worst[name] = max(worst[name], abs(got - float(expected)))
    audit = {"sampled": len(rows), "checked": list(AUDITED_FEATURES) + list(AUDITED_LABELS),
             "maxAbsDiff": {k: float(v) for k, v in worst.items()},
             "method": "全表布尔掩码独立重算（与向量路径不共用排序/二分/merge_asof/pivot-shift 代码）；"
                       "含前向标签窗算术核对"}
    offenders = {k: v for k, v in audit["maxAbsDiff"].items() if v > 1e-9}
    if offenders:
        raise AssertionError(f"as-of 特征与暴力重算不一致（未来信息泄漏）：{offenders}")
    return audit


# --------------------------------------------------------------------------- 特征名单


#: 候选集归属规则（前缀，顺序即优先级）。"工单历史"是本线的主语；"运维信号"= 遥测 + 尝试
#: + 使用负荷；日历位置列（第几天/星期/周末）与场景事件是**时段上下文**不是桩属性——把它们
#: 混进 staticOnly 会让"只靠画像"那组回答一个没人问的问题（桩属性能不能预测 vs 日子本身）。
TICKET_PREFIXES = ("tickets_", "charger_ticket_rate", "station_ticket_rate",
                   "days_since_last_report", "days_since_last_restore", "open_ticket",
                   "last_repair", "last_ticket_fault")
OPS_PREFIXES = ("tel_", "attempts_", "tech_fail_", "usage_")
CONTEXT_NUMERIC = frozenset({"day_index", "day_of_week", "is_weekend"})
CONTEXT_CATEGORICAL = frozenset({"scenario_event"})


def candidate_sets(numeric: list[str], categorical: list[str]) -> dict[str, list[str]]:
    """四组候选：只桩属性/站型/天气、只工单历史、只运维信号、全部（含日历上下文）。

    ``staticOnly`` 回答"桩型/站型/环境本身能预测多少来修"；``ticketOnly`` 回答"历史工单流
    单独值多少"；``opsOnly`` 回答"没人报修时，负荷与遥测还能不能独立发现病桩"——
    消融差就是本线交给运维的三句话。
    """
    def is_ticket(column: str) -> bool:
        return column.startswith(TICKET_PREFIXES)

    def is_ops(column: str) -> bool:
        return column.startswith(OPS_PREFIXES) and not is_ticket(column)

    ticket_numeric = [c for c in numeric if is_ticket(c)]
    ops_numeric = [c for c in numeric if is_ops(c)]
    static_numeric = [c for c in numeric
                      if not is_ticket(c) and not is_ops(c) and c not in CONTEXT_NUMERIC]
    groups = {
        "staticOnly": sorted(set(static_numeric) | (set(categorical) - CONTEXT_CATEGORICAL)),
        "ticketOnly": sorted(set(ticket_numeric)),
        "opsOnly": sorted(set(ops_numeric)),
        "full": sorted(set(numeric) | set(categorical)),
    }
    universe = set(numeric) | set(categorical)
    for name, columns in groups.items():
        extra = set(columns) - universe
        assert not extra, f"候选集 {name} 引用了不在特征名单里的列：{sorted(extra)}"
    assert groups["staticOnly"] and groups["ticketOnly"] and groups["opsOnly"], "有空候选集"
    assert set(groups["full"]) == universe
    banned = set(common.NON_FEATURE_COLUMNS) | set(FORBIDDEN_FEATURES) | set(IGNORED_COLUMNS)
    assert not (set(groups["ticketOnly"]) & banned)
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
    tickets, ticket_notes = load_tickets()
    frame, panel_notes = build_panel(bounds)
    frame, label_notes = attach_label(frame, tickets, bounds)
    context = build_day_context()
    frame, calendar_notes = attach_calendar_and_weather(frame, context)
    frame, ticket_hist_notes = attach_ticket_history(frame, tickets, bounds)
    sessions, session_notes = load_sessions_stream()
    frame, usage_notes = attach_usage(frame, sessions)
    attempts, attempt_notes = load_attempts_stream()
    frame, attempts_notes = attach_attempts(frame, attempts)
    daily, telemetry_notes = build_telemetry_daily()
    frame, telemetry_hist_notes = attach_telemetry_history(frame, daily)
    frame = frame.sort_values(["day_start_utc", "charger_id"],
                              kind="stable").reset_index(drop=True)
    usable = (~frame["purged"]) & (~frame["censored"])
    label_notes["usableBaseRate"] = {
        name: round(float(frame.loc[(frame["split"] == name) & usable, "y_ticket7"].mean()), 5)
        for name in ("TRAIN", "VALIDATION", "TEST")}
    label_notes["usablePositives"] = {
        name: int(frame.loc[(frame["split"] == name) & usable, "y_ticket7"].sum())
        for name in ("TRAIN", "VALIDATION", "TEST")}
    features = pick_features(frame)
    source_rows = {"maintenance_tickets": int(len(tickets)),
                   "charging_sessions": int(len(sessions)),
                   "charging_attempts": int(len(attempts)),
                   "charger_telemetry": int(telemetry_notes["rows"]),
                   "chargers": panel_notes["chargers"],
                   "stations": panel_notes["stations"]}
    notes = {"tickets": ticket_notes, "panel": panel_notes, "label": label_notes,
             "calendar": calendar_notes, "ticketHistory": ticket_hist_notes,
             "usage": usage_notes, "sessions": session_notes, "attempts": attempt_notes,
             "telemetry": {**telemetry_notes, "history": telemetry_hist_notes}}
    return frame, {"bounds": bounds, "features": features, "notes": notes,
                   "manifest": manifest, "context": context, "tickets": tickets,
                   "sessions": sessions, "attempts": attempts, "telemetryDaily": daily,
                   "sourceRows": source_rows}


def main() -> dict:
    common.require_empty_run_dir(common.OUT_DIR)
    frame, built = build()
    features, notes = built["features"], built["notes"]
    dead_tel = [c for c in features["droppedConstant"] if c.startswith("tel_")
                and "zero_power" not in c]
    assert not dead_tel, (f"遥测历史列 {dead_tel} 被判零方差剔除——等价于整列 NaN/常数，"
                          "滞后关联断了（本线真实事故过一次：stack 换序让 10 列全 NaN 且静默）。"
                          "zero_power 两列除外：实测本批 CHARGING 格子功率全 >0，常数剔除是真负结果")
    audit = verify_no_future_leak(frame, built["tickets"], built["sessions"], built["attempts"],
                                  built["telemetryDaily"], built["context"], built["bounds"])
    common.write_new_pickle(common.FEATURES_PATH, frame)

    summary = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": built["manifest"].get("pipelineRunId"),
        "datasetId": common.DATASET_ID,
        "modelId": common.MODEL_ID,
        "featuresSha256": common.sha256_file(common.FEATURES_PATH),
        "sourceTables": common.SOURCE_TABLES,
        "sourceRows": built["sourceRows"],
        "label": (f"y_ticket7 = 桩×北京日 D 的 [D, D+{common.LABEL_HORIZON_DAYS - 1}] 内"
                  "出现 ≥1 张新维修工单（按 reported_at 的北京日历日计）；"
                  "tickets_next7d = 该窗内工单条数（回归目标）；决策时点 = D 的北京零点"),
        "sampleUnit": "一台桩 × 一个北京日历日（稠密面板）",
        "decisionTime": "day_start_utc = 北京零点（UTC naive）；此后发生的一切不可见",
        "purgeDiscipline": (f"标签窗前向 {common.LABEL_HORIZON_DAYS} 天、相邻日重叠 {common.LABEL_HORIZON_DAYS - 1} 天，"
                            "故 TRAIN/VAL 段尾标签窗伸入下一段时间域的行整日剔除（purged），"
                            "TEST 右端放不下完整窗的行删失（censored），两者都不进模型与盲测"),
        "scope": notes["label"],
        "boundaries": {k: str(v) for k, v in built["bounds"].items()},
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
    print(f"[health] 面板 {len(frame):,} 桩·日（TRAIN {summary['scope']['splits']['TRAIN']} / "
          f"VAL {summary['scope']['splits']['VALIDATION']} / TEST {summary['scope']['splits']['TEST']}）")
    print(f"[health] purge 剔除 {summary['scope']['purgedRows']}，右端删失 {summary['scope']['censoredRows']}")
    print(f"[health] 7 日基础率（全面板）= {summary['scope']['baseRate']}，"
          f"可用行分率 TRAIN={summary['scope']['usableBaseRate']['TRAIN']} "
          f"VAL={summary['scope']['usableBaseRate']['VALIDATION']} "
          f"TEST={summary['scope']['usableBaseRate']['TEST']}")
    print(f"[health] 特征 {len(features['numeric'])} 数值 + {len(features['categorical'])} 类别，"
          f"候选集 {[(k, len(v)) for k, v in features['groups'].items()]}")
    print(f"[health] 泄漏审计 抽样 {audit['sampled']} 行 × {len(audit['checked'])} 个量，"
          f"maxAbsDiff = {max(audit['maxAbsDiff'].values()):.1e}")
    print(f"[health] -> {common.FEATURES_PATH}")
    return summary


if __name__ == "__main__":
    main()
