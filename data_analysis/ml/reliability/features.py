"""特征层：一台桩在 ``T`` 日的所有特征**只能来自 ``T`` 日起点之前**（含工单/遥测/面板历史）。

决策时点写死成一个值：``day_start_utc = T − 8h``（北京 T 日 00:00）。所有 as-of 量以它为查询时刻，
并且一律"严格早于"——同刻（同一天）的任何行互相不可见。于是本线的三类特征各有各的因果口径：

  · **面板历史**（尝试量、失败天数、连续失败、距今上次失败）：来自 ``derived/charger_day_panel_v1``
    里 ``day_start_utc`` 严格更早的日子。今天被用几次**不能**进特征（见 common 模块文档的暴露度陷阱），
    能进的是"昨天为止的日均尝试量"。
  · **工单 / 遥测**：来自 clean 表，按 ``reported_at`` / ``restored_at`` / ``recorded_at`` 严格早于
    ``day_start_utc``。工单只读报修与恢复两个时间戳，绝不读 ``status``（全表终态）。
  · **日历 / 天气**：日历的周末与场景事件是计划量，取 ``T`` 日本身；天气**只取 T−1 日及更早**的
    日聚合（``day_context`` 存的是当天事实，因果判断在这一层做）。另加"前一日气温相对过去 30 日的
    偏离"，因为热冲击是巡检排程里最像有因果味道的量。

禁入名单 ``FORBIDDEN_FEATURES`` 把 ``attempts_on_day`` / ``users_on_day`` / ``started_on_day`` /
``tech_fails_on_day`` / ``fail_flag`` 全部封死；``verify_no_future_leak`` 再用**全表布尔掩码**独立
重算 ``AUDITED_FEATURES`` 里的 17 个量（不复用排序/二分/merge_asof/pivot-rolling 代码），
逐行要求 0 偏差。

用法（仓库根目录）：python -m data_analysis.ml.reliability.features
"""

from __future__ import annotations

import random

import numpy as np
import pandas as pd

from . import common

#: 事后 / 身份 / **同窗**量：留在表里做分组、溯源、评价与诊断，绝不进特征。
#: 注意 ``last_state_code`` / ``last_online`` / ``prev_day_*`` 不在这份名单里——它们是"昨天为止"
#: 的 as-of 量，是本线正牌的可用信号；被禁的是"今天"的形状（当日尝试数、当日失败数、当日用量）。
FORBIDDEN_FEATURES = frozenset({
    "attempt_id", "user_id", "vehicle_id", "session_id", "queue_id", "reservation_id",
    "outcome", "failure_reason", "is_tech_fail", "started_flag", "recorded_at", "created_at",
    "expires_at", "opened_at", "commissioned_at", "reported_at", "restored_at",
    "charger_id", "station_id", "city_id", "business_date", "day_start_utc", "split",
    "attempts_on_day", "started_on_day", "tech_fails_on_day", "fail_flag", "users_on_day",
    "y_fail", "y_fails",
})

#: 明确"读过但不用"的量，逐条留原因——换批次时一定有人问。
#: 这里是 **dict**（写成 ``frozenset({...})`` 会只留下键、把原因悄悄丢掉，第六线那份是纯名单）：
#: 消费方一律 ``set(IGNORED_COLUMNS)``，取键即可，两种写法都能跑，但只有 dict 留得住解释。
IGNORED_COLUMNS = {
    "demand_multiplier": "仿真侧计划系数（第六线实测单变量 AUC 0.4932，等于噪声），本线不放进任何"
                         "候选集，列仍留在表里以便复核这句话",
    "weather_hours": "上下文表的完整性核对列（应为 24），不是业务量",
    "status": "maintenance_tickets 的全表终态，泄答案",
    "severity": "工单等级与成本同源，修完才填的量",
}

#: 收缩强度。日粒度上一次观测就是一天，所以"按天"的 α 比事件线的"按次"α 小一个量级。
ALPHA_CHARGER_DAYS = 15.0
ALPHA_STATION_DAYS = 20.0
ALPHA_CHARGER_ATTEMPTS = 40.0
ALPHA_RECENT_DAYS = 5.0

TELEMETRY_STATES = ("AVAILABLE", "CHARGING", "OCCUPIED", "RESERVED", "MAINTENANCE", "OFFLINE")
STATE_CODES = {name: code for code, name in enumerate(TELEMETRY_STATES)}

#: 天气日聚合里可以做"前一日 / 近 30 日均值"的量。
WEATHER_LAG_COLUMNS = ("temp_c_mean", "temp_c_min", "temp_c_max", "humidity_mean",
                       "rainfall_mm_sum", "rain_hours")
#: 只有这几个量再叠一个 [T−30, T−1] 的均值（右端=昨天）做异常检测用。
WEATHER_BASELINE_COLUMNS = ("temp_c_mean", "temp_c_max", "humidity_mean", "rainfall_mm_sum")


# --------------------------------------------------------------------------- 静态与当日计划量


def load_statics() -> pd.DataFrame:
    """桩的静态属性（型号/厂商/接口/额定功率/站/城）——与"哪天"无关，天然无泄漏。"""
    chargers = common.load_clean_table("chargers")
    stations = common.load_clean_table("stations", columns=["station_id", "city_id", "site_type",
                                                            "opened_at", "transformer_kw"])
    frame = (chargers.merge(stations, on="station_id", how="left", validate="many_to_one")
                    .rename(columns={"model": "charger_model"}))
    assert frame["charger_id"].is_unique and frame["charger_model"].notna().all()
    assert frame["site_type"].notna().all(), "站点缺少 site_type"
    assert (frame["rated_power_kw"] > 0).all() and (frame["transformer_kw"] > 0).all()
    for column in ("commissioned_at", "opened_at"):
        frame[column] = pd.to_datetime(frame[column])
    # station_id / city_id 面板里已经有了：同名再带一遍，pandas 会加 _x/_y 后缀，
    # 于是 merge 之后的 frame 根本没有 "city_id" 这一列（第一次跑就在这儿 KeyError）。
    return frame[["charger_id", "connector_type", "rated_power_kw", "manufacturer",
                 "charger_model", "site_type", "commissioned_at", "opened_at",
                 "transformer_kw"]].copy()


def _lagged(ordered: pd.DataFrame, column: str, keys: pd.MultiIndex, rolling: bool) -> dict[str, np.ndarray]:
    """某天气量按 (城, 日) 稠密网格取"前一日"与"[T−30, T−1] 均值"。

    先 pivot 成 日×城 的矩阵再 shift/rolling：这样 shift 只沿日期轴走，**绝不会跨城**（直接在
    拼接后的序列上 rolling 会让下一城的前 30 天吃到上一城的尾巴）。
    """
    matrix = ordered.pivot(index="business_date", columns="city_id", values=column).sort_index()
    previous = matrix.shift(1)
    out = {f"{column}_prev": _flatten(previous, keys)}
    if rolling:
        out[f"{column}_mean_30d"] = _flatten(
            previous.rolling(30, min_periods=10).mean(), keys)
    return out


def _flatten(matrix: pd.DataFrame, keys: pd.MultiIndex) -> np.ndarray:
    """日×城 矩阵 → 按 ``keys``（城, 日）取值的浮点数组。stack 丢掉 NaN，reindex 再补回来。"""
    return _flatten_any(matrix, keys).to_numpy(dtype="float64")


def _flatten_labels(matrix: pd.DataFrame, keys: pd.MultiIndex) -> np.ndarray:
    """同 ``_flatten``，但取的是字符串列（主导天气），缺失留 NaN 而不是猜成类别。"""
    return _flatten_any(matrix, keys).to_numpy(dtype=object)


def _flatten_any(matrix: pd.DataFrame, keys: pd.MultiIndex) -> pd.Series:
    flat = matrix.stack()
    flat.index = flat.index.swaplevel().set_names(["city_id", "business_date"])
    return flat.reindex(keys)


def attach_calendar_and_weather(frame: pd.DataFrame, context: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """当日计划量（周末/场景）+ 前一日与近 30 日的天气因果聚合 + 桩/站龄。

    稠密网格（每城 180 天一天不缺）是 ``_lagged`` 成立的前提，由 build_panel 的断言保证；
    今天的天气列（``temp_c_max`` 等）在合并时**当场丢掉**，只留 ``_prev``/``_mean_30d``。
    """
    ordered = context.sort_values(["city_id", "business_date"], kind="stable").reset_index(drop=True)
    keys = pd.MultiIndex.from_arrays([ordered["city_id"].to_numpy(), ordered["business_date"].to_numpy()],
                                    names=["city_id", "business_date"])
    day = ordered[["city_id", "business_date", "is_weekend", "scenario_event", "demand_multiplier"]].copy()
    for column in WEATHER_LAG_COLUMNS:
        for name, values in _lagged(ordered, column, keys,
                                   rolling=column in WEATHER_BASELINE_COLUMNS).items():
            day[name] = values
    weather_matrix = (ordered.assign(_code=ordered["weather"].astype("category"))
                      .pivot(index="business_date", columns="city_id", values="_code").sort_index())
    day["weather_prev"] = _flatten_labels(weather_matrix.shift(1).astype("object"), keys)
    day["temp_shock_prev_c"] = day["temp_c_max_prev"] - day["temp_c_max_mean_30d"]
    day["heatwave_prev"] = np.where(day["temp_c_max_prev"].isna(), np.nan,
                                   (day["temp_c_max_prev"] >= 35.0).astype(float))
    day["rain_prev"] = np.where(day["rainfall_mm_sum_prev"].isna(), np.nan,
                               (day["rainfall_mm_sum_prev"] > 0).astype(float))

    rows_before = len(frame)
    frame = frame.merge(day, on=["city_id", "business_date"], how="left", validate="many_to_one")
    assert len(frame) == rows_before, "城-日上下文关联改变了行数（网格不稠密或有重复）"
    frame["is_weekend"] = frame["is_weekend"].astype("int64")
    starts = pd.to_datetime(frame["day_start_utc"])
    frame["charger_age_days"] = (starts - frame["commissioned_at"]).dt.total_seconds() / 86400
    frame["station_age_days"] = (starts - frame["opened_at"]).dt.total_seconds() / 86400
    notes = {
        "weatherRule": "天气只取 T−1 日的日聚合与 [T−30, T−1] 的均值（rolling 右端=昨天），今天的天气不可得",
        "keptLagColumns": list(WEATHER_LAG_COLUMNS),
        "sameDayWeatherNotMerged": True,
        "coverage": {column: round(float(frame[column].notna().mean()), 4)
                     for column in ("weather_prev", "temp_c_max_prev", "temp_c_max_mean_30d",
                                    "temp_shock_prev_c", "heatwave_prev", "rain_prev",
                                    "scenario_event", "is_weekend", "charger_age_days")},
        "tempShockRangeC": [round(float(frame["temp_shock_prev_c"].min()), 3),
                           round(float(frame["temp_shock_prev_c"].max()), 3)],
        "heatwaveShare": round(float(frame["heatwave_prev"].mean()), 4),
        "transformerNote": "transformer_kw 是站点容量，日级不变，作为桩的站点侧静态属性保留",
    }
    return frame, notes


# --------------------------------------------------------------------------- 面板历史（严格早于当日）


def query_frame(frame: pd.DataFrame, key: str) -> pd.DataFrame:
    """给 ``AsOfCounts`` 用的最小查询表：实体键 + 查询时刻（桩日起点）。"""
    return pd.DataFrame({"__query_time": pd.to_datetime(frame["day_start_utc"]),
                        key: frame[key].to_numpy()})


def time_gap(later, earlier, scale: float) -> np.ndarray:
    """两组时间戳之差除以 ``scale``（天 / 小时）；任一侧 NaT 则该行取 NaN。

    刻意走 Series 的 timedelta 算术而不是 int64 减法：``strict_last`` 对"从来没有过"的行给 NaT，
    而 NaT 转 int64 是 ``-9223372036854775808``，与当日起点相减会溢出，再靠"看起来是负数"被
    ``where`` 蒙混过去——这种边界不该用溢出行为来兜。
    """
    base = pd.Series(pd.to_datetime(np.asarray(later, dtype="datetime64[ns]")))
    past = pd.Series(pd.to_datetime(np.asarray(earlier, dtype="datetime64[ns]")))
    gap = (base - past).dt.total_seconds()
    assert len(gap) == len(base)
    return (gap / scale).to_numpy(dtype="float64")


def attach_panel_history(frame: pd.DataFrame, panel: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """这台桩"昨天为止"的使用量、失败天数、连续性与距今量。

    三个口径关键点：
      · 日均尝试量的分母是**日历天数**（30），不是"有尝试的天数"——否则"天天被用"和"偶尔被用"
        算出同一个数，而这两者的暴露度差一个量级。
      · 日加权失败率（``fail_day_rate_*``，分母=天数）与尝试加权失败率
        （``fail_rate_per_attempt_*``，分母=尝试数）都要：前者就是本线标签的形状，后者把暴露度
        除掉一点，两者的差能读出"失败是零散还是扎堆"。
      · 收缩先验一律"截至当日起点之前"的全量经验率（只看过去，无历史取 0.0），不用全表标签率。
    """
    times = pd.to_datetime(panel["day_start_utc"])
    attempts = panel["attempts_on_day"].to_numpy(dtype=float)
    fails = panel["tech_fails_on_day"].to_numpy(dtype=float)
    day_priors = common.global_causal_rate(panel["fail_flag"].to_numpy(dtype=float), times)
    attempt_priors = common.global_causal_ratio(fails, attempts, times)
    notes = {"shrinkageTarget": {
        "rule": "先验 = 截至该日起点之前的全量经验率（日率按行、尝试率按尝试数加权），无历史取 0.0",
        "dayRate": [round(float(day_priors.min()), 5), round(float(day_priors.max()), 5)],
        "attemptRate": [round(float(attempt_priors.min()), 5), round(float(attempt_priors.max()), 5)]}}

    helper = panel.assign(_fails=fails, _attempts=attempts,
                         _users=panel["users_on_day"].to_numpy(dtype=float))
    query = query_frame(frame, "charger_id")
    attempt_engine = common.AsOfCounts(helper, key="charger_id", stamps="day_start_utc", value="_attempts")
    fail_engine = common.AsOfCounts(helper, key="charger_id", stamps="day_start_utc", value="_fails")
    users_engine = common.AsOfCounts(helper, key="charger_id", stamps="day_start_utc", value="_users")
    days_engine = common.AsOfCounts(helper, key="charger_id", stamps="day_start_utc")
    failday_engine = common.AsOfCounts(helper[helper["fail_flag"] == 1], key="charger_id",
                                       stamps="day_start_utc")

    for days in (7.0, 14.0, 30.0, 90.0):
        tag = f"{int(days)}d"
        window_attempts = attempt_engine.window(query, "charger_id", days, "_attempts")[1]
        window_fails = fail_engine.window(query, "charger_id", days, "_fails")[1]
        frame[f"attempts_sum_{tag}"] = window_attempts
        frame[f"attempts_per_day_{tag}"] = window_attempts / days
        frame[f"fails_sum_{tag}"] = window_fails
        frame[f"fail_rate_per_attempt_{tag}"] = common.smoothed(
            window_attempts, window_fails, ALPHA_CHARGER_ATTEMPTS, attempt_priors)
        frame[f"users_sum_{tag}"] = users_engine.window(query, "charger_id", days, "_users")[1]
        window_fail_days = failday_engine.window(query, "charger_id", days)[0]
        frame[f"fail_days_{tag}"] = window_fail_days
        frame[f"fail_day_rate_{tag}"] = common.smoothed(
            np.full(len(frame), days), window_fail_days, ALPHA_RECENT_DAYS, day_priors)

    # 全量（截至当日起点之前）的桩级先验：本线最强的身份信号，也是主要基线
    prior_days, _ = days_engine.cumulative(query, "charger_id")
    prior_fail_days, _ = failday_engine.cumulative(query, "charger_id")
    _, prior_attempts = attempt_engine.cumulative(query, "charger_id", "_attempts")
    _, prior_fails = fail_engine.cumulative(query, "charger_id", "_fails")
    frame["charger_days_prior"] = prior_days
    frame["charger_fail_days_prior"] = prior_fail_days
    frame["charger_attempts_prior"] = prior_attempts
    frame["charger_fail_rate_prior"] = common.smoothed(
        prior_days, prior_fail_days, ALPHA_CHARGER_DAYS, day_priors)
    frame["charger_fail_rate_attempt_prior"] = common.smoothed(
        prior_attempts, prior_fails, ALPHA_CHARGER_ATTEMPTS, attempt_priors)
    frame["charger_attempts_per_day_prior"] = np.where(
        prior_days > 0, prior_attempts / np.maximum(prior_days, 1.0), np.nan)
    frame["fail_day_rate_30d_minus_prior"] = frame["fail_day_rate_30d"] - frame["charger_fail_rate_prior"]
    frame["fail_day_rate_7d_minus_30d"] = frame["fail_day_rate_7d"] - frame["fail_day_rate_30d"]

    # 距今量与"昨天怎么样"：日粒度上 strict_last 的同刻不可见 = 今天不可见，正是本线要的
    assert not panel.duplicated(subset=["charger_id", "business_date"]).any(), \
        "面板出现重复的 (桩, 日)，「昨天」的定义不唯一"
    source = panel.sort_values(["charger_id", "day_start_utc"], kind="stable")
    last_day = common.strict_last(
        frame, "day_start_utc", "charger_id",
        source[["charger_id", "day_start_utc", "attempts_on_day", "fail_flag"]],
        "day_start_utc", ["attempts_on_day", "fail_flag"])
    frame["prev_day_fail"] = last_day["fail_flag"].to_numpy(dtype="float64")
    frame["prev_day_attempts"] = last_day["attempts_on_day"].to_numpy(dtype="float64")
    starts = pd.to_datetime(frame["day_start_utc"]).to_numpy()
    frame["days_since_last_observed"] = time_gap(starts, last_day["__last_time"].to_numpy(), 86_400.0)
    fail_source = source[source["fail_flag"] == 1][["charger_id", "day_start_utc"]]
    last_fail = common.strict_last(frame, "day_start_utc", "charger_id", fail_source,
                                  "day_start_utc", [])
    frame["days_since_last_fail"] = time_gap(starts, last_fail["__last_time"].to_numpy(), 86_400.0)
    frame["fail_streak_before"] = fail_streak_before(panel, frame)
    return frame, notes


def fail_streak_before(panel: pd.DataFrame, frame: pd.DataFrame, cap: int = 14) -> np.ndarray:
    """当日起点之前**连续**多少天出现失败（不含今天）。

    稠密面板上是一次简单的逐桩扫描：``carry`` 保存"截至昨天为止"的连跑长度，先写进当天再更新，
    所以 T 日拿到的是 [T−1, T−2, …] 的连跑，绝不含 T 自己。

    实测这个方向是**正**的、且随连跑长度单调上升（全样本 P(今日失败日)：昨天没坏 0.1822、
    昨天坏了 0.3446；连跑 0/1/2/3–4/≥5 天分别 0.1823 / 0.3104 / 0.3812 / 0.4406 / 0.4943），
    所以它是本线最强的单个特征之一。
    """
    ordered = panel.sort_values(["charger_id", "business_date"], kind="stable")
    carry: dict[object, int] = {}
    values = np.empty(len(ordered), dtype="float64")
    for position, (charger, day, flag) in enumerate(zip(ordered["charger_id"].to_numpy(),
                                                        ordered["business_date"].to_numpy(),
                                                        ordered["fail_flag"].to_numpy(), strict=True)):
        values[position] = carry.get(charger, 0)
        if flag:
            carry[charger] = min(carry.get(charger, 0) + 1, cap)
        else:
            carry.pop(charger, None)
    index = pd.MultiIndex.from_arrays([ordered["charger_id"].to_numpy(),
                                       pd.to_datetime(ordered["business_date"]).to_numpy()],
                                      names=["charger_id", "business_date"])
    series = pd.Series(values, index=index)
    keys = pd.MultiIndex.from_arrays([frame["charger_id"].to_numpy(),
                                     pd.to_datetime(frame["business_date"]).to_numpy()],
                                     names=["charger_id", "business_date"])
    aligned = series.reindex(keys)
    assert aligned.notna().all(), "面板里有取不到连续失败天数的桩日（网格不稠密？）"
    return aligned.to_numpy(dtype="float64")


# --------------------------------------------------------------------------- 工单 / 遥测（clean 表）


def attach_tickets(frame: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """维保工单的 as-of（严格早于当日起点）状态：报修过几次、当时是否在修、停过多久。

    只用 ``reported_at`` / ``restored_at`` / ``fault_type``；绝不读 ``status``（全表终态）、
    ``accepted_at`` / ``work_started_at``（调度侧时间戳，真实系统夜间可否读到未定）与成本列。
    ``charger_ticket_open_at_start`` = 报修数 − 已恢复数，同刻报修又恢复的按"还没恢复"算（保守）。
    """
    tickets = common.load_clean_table("maintenance_tickets",
                                     columns=["charger_id", "station_id", "reported_at", "restored_at",
                                             "fault_type"])
    for column in ("reported_at", "restored_at"):
        tickets[column] = pd.to_datetime(tickets[column])
    closed_pairs = tickets[tickets["restored_at"].notna()]
    assert (closed_pairs["restored_at"] >= closed_pairs["reported_at"]).all(), "存在恢复早于报修的工单"
    helper = tickets.assign(_one=1.0)
    closed = tickets[tickets["restored_at"].notna()].copy()
    closed["_hours"] = (closed["restored_at"] - closed["reported_at"]).dt.total_seconds() / 3600.0
    closed["_one"] = 1.0
    query = query_frame(frame, "charger_id")
    station_query = query_frame(frame, "station_id")
    notes = {"tickets": int(len(tickets)),
             "chargersWithTickets": int(tickets["charger_id"].nunique()),
             "faultTypes": {str(k): int(v) for k, v in tickets["fault_type"].value_counts().items()},
             "unclosed": int(tickets["restored_at"].isna().sum()),
             "closedHours": {"p50": round(float(closed["_hours"].median()), 3),
                            "p95": round(float(closed["_hours"].quantile(0.95)), 3)}}

    reported = common.AsOfCounts(helper, key="charger_id", stamps="reported_at", value="_one")
    counts, _ = reported.cumulative(query, "charger_id", "_one")
    frame["charger_tickets_total_prior"] = counts
    frame["charger_tickets_30d"] = reported.window(query, "charger_id", 30.0, "_one")[0]
    frame["charger_tickets_90d"] = reported.window(query, "charger_id", 90.0, "_one")[0]
    sensitive = helper[helper["fault_type"].isin(["COMMUNICATION", "CONNECTOR"])]
    frame["charger_connector_tickets_30d"] = common.AsOfCounts(
        sensitive, key="charger_id", stamps="reported_at", value="_one").window(
        query, "charger_id", 30.0, "_one")[0]
    restored_counts, _ = common.AsOfCounts(closed, key="charger_id", stamps="restored_at",
                                          value="_one").cumulative(query, "charger_id", "_one")
    frame["charger_ticket_open_at_start"] = np.maximum(counts - restored_counts, 0.0)
    downtime = common.AsOfCounts(closed, key="charger_id", stamps="restored_at", value="_hours")
    frame["charger_downtime_hours_90d"] = downtime.window(query, "charger_id", 90.0, "_hours")[1]
    frame["charger_downtime_hours_prior"] = downtime.cumulative(query, "charger_id", "_hours")[1]
    last_ticket = common.strict_last(frame, "day_start_utc", "charger_id",
                                    tickets[["charger_id", "reported_at"]], "reported_at", [])
    frame["charger_days_since_last_ticket"] = time_gap(
        pd.to_datetime(frame["day_start_utc"]).to_numpy(),
        last_ticket["__last_time"].to_numpy(), 86_400.0)
    station_engine = common.AsOfCounts(helper, key="station_id", stamps="reported_at", value="_one")
    frame["station_tickets_30d"] = station_engine.window(station_query, "station_id", 30.0, "_one")[0]
    frame["station_tickets_total_prior"] = station_engine.cumulative(station_query, "station_id",
                                                                    "_one")[0]
    return frame, notes


def attach_telemetry(frame: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """遥测 as-of（严格早于当日起点）：昨天报了什么、断了多久没报、近 30 日缺报天数。

    两条实现刻意分开：
      · **日粒度**聚合（每桩每天多少行、其中 OFFLINE/MAINTENANCE/CHARGING 各多少行）走
        ``AsOfCounts``，窗口右端是当日起点、严格不含今天。"近 30 日缺报天数" = 30 − 有上报的
        日历天数——事件线里没有"整天没数据"这种形状的量，本线第一次能用上。
      · **最后一次状态/在线位**走 ``strict_last``（右表 +1ns），所以"今天 00:00 之后的第一条遥测"
        绝不可见。
    """
    telemetry = common.load_clean_table("charger_telemetry",
                                       columns=["charger_id", "recorded_at", "state", "online"])
    telemetry = telemetry.copy()
    telemetry["recorded_at"] = pd.to_datetime(telemetry["recorded_at"])
    telemetry["business_date"] = (telemetry["recorded_at"] + pd.Timedelta(
        hours=common.BUSINESS_OFFSET_HOURS)).dt.normalize()
    state_code = telemetry["state"].map(STATE_CODES)
    assert state_code.notna().all(), "遥测出现了未登记的状态值"
    telemetry["state_code"] = state_code.astype("int8")
    telemetry["off_flag"] = (~telemetry["online"].astype(bool)).astype("int64")
    daily = telemetry.assign(_one=1.0, _off_flag=telemetry["off_flag"],
                            _offline=(telemetry["state"] == "OFFLINE").astype("int64"),
                            _maint=(telemetry["state"] == "MAINTENANCE").astype("int64"),
                            _charging=(telemetry["state"] == "CHARGING").astype("int64"))
    daily = (daily.groupby(["charger_id", "business_date"], observed=True)
             .agg(rows=("_one", "sum"), offline_rows=("_offline", "sum"),
                  maint_rows=("_maint", "sum"), charging_rows=("_charging", "sum"),
                  off_rows=("_off_flag", "sum"))
             .reset_index())
    for column in ("rows", "offline_rows", "maint_rows", "charging_rows", "off_rows"):
        daily[column] = daily[column].astype("int64")
    daily["day_start_utc"] = daily["business_date"] - pd.Timedelta(
        hours=common.BUSINESS_OFFSET_HOURS)
    query = query_frame(frame, "charger_id")
    notes = {"telemetryRows": int(len(telemetry)),
             "chargerDayRows": int(len(daily)),
             "stateMix": {str(k): int(v) for k, v in telemetry["state"].value_counts().items()}}

    days_engine = common.AsOfCounts(daily, key="charger_id", stamps="day_start_utc")
    for days in (7.0, 30.0):
        present = days_engine.window(query, "charger_id", days)[0]
        frame[f"tel_days_missing_{int(days)}d"] = days - present
    for column, label in (("rows", "rows"), ("offline_rows", "offline"), ("maint_rows", "maint"),
                         ("charging_rows", "charging"), ("off_rows", "off")):
        helper = daily.assign(_value=daily[column].astype("float64"))
        engine = common.AsOfCounts(helper, key="charger_id", stamps="day_start_utc", value="_value")
        frame[f"tel_{label}_prev_day"] = engine.window(query, "charger_id", 1.0, "_value")[1]
        frame[f"tel_{label}_7d"] = engine.window(query, "charger_id", 7.0, "_value")[1]
    frame["tel_abnormal_prev_day"] = frame["tel_offline_prev_day"] + frame["tel_maint_prev_day"]

    last = common.strict_last(frame, "day_start_utc", "charger_id",
                             telemetry[["charger_id", "recorded_at", "state_code", "online"]],
                             "recorded_at", ["state_code", "online"])
    frame["last_state_code"] = last["state_code"].to_numpy(dtype="float64")
    frame["last_online"] = last["online"].astype("float64").to_numpy()
    frame["tel_gap_hours_at_start"] = time_gap(
        pd.to_datetime(frame["day_start_utc"]).to_numpy(),
        last["__last_time"].to_numpy(), 3_600.0)
    notes["lastStateBeforeDay"] = {TELEMETRY_STATES[int(code)]: int(rows) for code, rows in
                                  frame["last_state_code"].value_counts().dropna().items()}
    notes["gapHours"] = {"p50": round(float(frame["tel_gap_hours_at_start"].median()), 3),
                        "p95": round(float(frame["tel_gap_hours_at_start"].quantile(0.95)), 3),
                        "max": round(float(frame["tel_gap_hours_at_start"].max()), 3),
                        "missing": int(frame["tel_gap_hours_at_start"].isna().sum())}
    notes["missingDays30d"] = {"p50": float(frame["tel_days_missing_30d"].median()),
                              "max": float(frame["tel_days_missing_30d"].max())}
    return frame, notes


# --------------------------------------------------------------------------- 样本与标签


def attach_label_and_split(frame: pd.DataFrame, bounds: dict[str, pd.Timestamp]) -> tuple[pd.DataFrame, dict]:
    """训练样本 = 当日 ≥1 次尝试的桩日；标签 = 当日是否出现技术失败（+ 次数回归目标）。

    为什么必须丢掉 0 尝试的日子：那种日子按定义不可能出现失败（没尝试就没失败），留着等于让模型
    靠"今天没人用"白拿一截 AUC——本线要回答的是"**明天会被用的桩里，哪几台最可能出问题**"。
    """
    zero = frame["attempts_on_day"] == 0
    notes = {"panelRows": int(len(frame)), "droppedZeroAttemptRows": int(zero.sum()),
             "droppedPositives": int(frame.loc[zero, "fail_flag"].sum())}
    assert notes["droppedPositives"] == 0, "0 次尝试的日子出现了失败，样本口径出错"
    frame = frame[~zero].copy()
    frame["y_fail"] = frame["fail_flag"].astype("int64")
    frame["y_fails"] = frame["tech_fails_on_day"].astype("int64")
    frame["split"] = common.assign_split(frame["day_start_utc"], bounds)
    notes.update({"rows": int(len(frame)), "positives": int(frame["y_fail"].sum()),
                  "baseRate": round(float(frame["y_fail"].mean()), 5),
                  "chargers": int(frame["charger_id"].nunique())})
    return frame, notes


# --------------------------------------------------------------------------- 泄漏审计


AUDITED_FEATURES = ("attempts_sum_30d", "fails_sum_30d", "fail_days_30d", "fail_day_rate_30d",
                    "charger_fail_rate_prior", "fail_rate_per_attempt_7d", "prev_day_fail",
                    "days_since_last_fail", "days_since_last_observed", "fail_streak_before",
                    "charger_tickets_30d", "charger_ticket_open_at_start", "tel_days_missing_30d",
                    "tel_offline_prev_day", "last_state_code", "temp_c_max_prev",
                    "temp_c_max_mean_30d")


def verify_no_future_leak(frame: pd.DataFrame, panel: pd.DataFrame, context: pd.DataFrame,
                         sample: int = 250, seed: int = common.SEED) -> dict:
    """随机抽若干桩日，用**全表布尔掩码暴力重算** ``AUDITED_FEATURES``，要求逐条 0 偏差。

    审计刻意走另一条代码路径（不排序、不二分、不 merge_asof、不复用 ``AsOfCounts``/pivot-rolling）。
    覆盖四类量：面板历史（窗口和/窗口天数/全量先验/昨天/距今/连跑）、工单（近窗数/在修数）、
    遥测（缺报天数/昨天异常行数/最后状态），以及天气日移位（前一日值与 [T−30,T−1] 均值）。
    """
    rng = random.Random(seed)
    rows = sorted(rng.sample(range(len(frame)), min(sample, len(frame))))
    day_starts = pd.to_datetime(frame["day_start_utc"])
    panel_starts = pd.to_datetime(panel["day_start_utc"])
    global_flags = panel["fail_flag"].to_numpy(dtype=float)
    global_attempts = panel["attempts_on_day"].to_numpy(dtype=float)
    global_fails = panel["tech_fails_on_day"].to_numpy(dtype=float)
    panel_by_charger = {key: part.assign(_start=panel_starts)
                        for key, part in panel.groupby("charger_id", observed=True)}
    context_by_city = {key: part for key, part in context.groupby("city_id", observed=True)}
    tickets = common.load_clean_table("maintenance_tickets",
                                     columns=["charger_id", "reported_at", "restored_at"])
    for column in ("reported_at", "restored_at"):
        tickets[column] = pd.to_datetime(tickets[column])
    tickets_by = {key: part for key, part in tickets.groupby("charger_id", observed=True)}
    telemetry = common.load_clean_table("charger_telemetry", columns=["charger_id", "recorded_at",
                                                                     "state", "online"])
    telemetry = telemetry.copy()
    telemetry["recorded_at"] = pd.to_datetime(telemetry["recorded_at"])
    telemetry["beijing_date"] = (telemetry["recorded_at"] + pd.Timedelta(
        hours=common.BUSINESS_OFFSET_HOURS)).dt.normalize()
    tel_by = {key: part for key, part in telemetry.groupby("charger_id", observed=True)}

    worst = {name: 0.0 for name in AUDITED_FEATURES}
    for position in rows:
        stamp = day_starts.iloc[position]
        charger = frame.iloc[position]["charger_id"]
        city = frame.iloc[position]["city_id"]
        past = panel_by_charger[charger]
        earlier = past[past["_start"] < stamp]
        window = earlier[earlier["_start"] >= stamp - pd.Timedelta(days=30)]
        week = earlier[earlier["_start"] >= stamp - pd.Timedelta(days=7)]
        before_mask = (panel_starts < stamp).to_numpy()
        day_prior = float(global_flags[before_mask].mean()) if before_mask.any() else 0.0
        attempts_sum = float(global_attempts[before_mask].sum())
        attempt_prior = (float(global_fails[before_mask].sum()) / attempts_sum
                         if attempts_sum > 0 else 0.0)
        window_attempts = float(window["attempts_on_day"].sum())
        window_fails = float(window["tech_fails_on_day"].sum())
        window_fail_days = float((window["fail_flag"] == 1).sum())
        week_attempts = float(week["attempts_on_day"].sum())
        week_fails = float(week["tech_fails_on_day"].sum())
        prev_row = past[past["_start"] == stamp - pd.Timedelta(days=1)]
        failed_days = earlier[earlier["fail_flag"] == 1]
        prior_fail_days = float(len(failed_days))
        here_tel = tel_by.get(charger)
        here_tickets = tickets_by.get(charger)
        here_context = context_by_city.get(city)

        truth = {
            "attempts_sum_30d": window_attempts,
            "fails_sum_30d": window_fails,
            "fail_days_30d": window_fail_days,
            "fail_day_rate_30d": (window_fail_days + ALPHA_RECENT_DAYS * day_prior)
            / (30 + ALPHA_RECENT_DAYS),
            "charger_fail_rate_prior": (prior_fail_days + ALPHA_CHARGER_DAYS * day_prior)
            / (len(earlier) + ALPHA_CHARGER_DAYS),
            "fail_rate_per_attempt_7d": (week_fails + ALPHA_CHARGER_ATTEMPTS * attempt_prior)
            / (week_attempts + ALPHA_CHARGER_ATTEMPTS),
            "prev_day_fail": float(prev_row["fail_flag"].iloc[0]) if len(prev_row) else np.nan,
            "days_since_last_observed": (float((stamp - earlier["_start"].max()).total_seconds() / 86400)
                                        if len(earlier) else np.nan),
            "days_since_last_fail": (float((stamp - failed_days["_start"].max()).total_seconds() / 86400)
                                     if len(failed_days) else np.nan),
            "fail_streak_before": _brute_streak(past, stamp),
            "tel_days_missing_30d": 30.0 - _brute_present_days(here_tel, stamp, 30),
            "tel_offline_prev_day": 0.0 if here_tel is None else float(
                ((here_tel["beijing_date"] == prev_beijing_day(stamp))
                 & (here_tel["state"] == "OFFLINE")).sum()),
            "charger_tickets_30d": 0.0 if here_tickets is None else float(
                ((here_tickets["reported_at"] < stamp)
                 & (here_tickets["reported_at"] >= stamp - pd.Timedelta(days=30))).sum()),
            "charger_ticket_open_at_start": 0.0 if here_tickets is None else max(
                float((here_tickets["reported_at"] < stamp).sum())
                - float(((here_tickets["restored_at"].notna())
                         & (here_tickets["restored_at"] < stamp)).sum()), 0.0),
            "temp_c_max_prev": np.nan if here_context is None else _brute_context(
                here_context, prev_beijing_day(stamp), "temp_c_max"),
            "temp_c_max_mean_30d": np.nan if here_context is None else _brute_context(
                here_context, stamp - pd.Timedelta(hours=8) - pd.Timedelta(days=30), "temp_c_max",
                mean=True),
        }
        if here_tel is None or not (here_tel["recorded_at"] < stamp).any():
            truth["last_state_code"] = np.nan
        else:
            before = here_tel[here_tel["recorded_at"] < stamp]
            newest = before["recorded_at"].max()
            truth["last_state_code"] = float(STATE_CODES[
                before.loc[before["recorded_at"] == newest, "state"].iloc[-1]])
        for name, expected in truth.items():
            got = float(frame.iloc[position][name])
            if pd.isna(expected) and pd.isna(got):
                continue
            worst[name] = max(worst[name], abs(got - float(expected)))
    audit = {"sampled": len(rows), "checked": list(AUDITED_FEATURES),
             "maxAbsDiff": {k: float(v) for k, v in worst.items()},
             "method": "全表布尔掩码独立重算（与向量路径不共用排序/二分/merge_asof/pivot-rolling 代码）"}
    offenders = {k: v for k, v in audit["maxAbsDiff"].items() if v > 1e-9}
    if offenders:
        raise AssertionError(f"as-of 特征与暴力重算不一致（未来信息泄漏）：{offenders}")
    return audit


def prev_beijing_day(stamp: pd.Timestamp) -> pd.Timestamp:
    """桩日起点（UTC naive）→ 前一北京日历日的零点（``day_context``/遥测聚合用的日子键）。"""
    return (stamp + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS) - pd.Timedelta(days=1)).normalize()


def _brute_context(part: pd.DataFrame, date: pd.Timestamp, column: str, mean: bool = False) -> float:
    """城-日上下量的"前一日值"与"[lo, lo+30 天) 均值"的暴力版。

    ``mean=True`` 时复刻向量路径 ``rolling(30, min_periods=10)`` 的口径：非缺失观测不足 10 天
    就是 NaN，不是"有几天眼就平均几天"——向量与暴力必须在同一条规则上对齐，否则这条审计只能
    证明两边"大致一样"。
    """
    if mean:
        selected = part[(part["business_date"] >= date) & (part["business_date"] < date + pd.Timedelta(days=30))]
        values = selected[column].dropna()
        return float(values.mean()) if len(values) >= 10 else np.nan
    selected = part[part["business_date"] == date]
    return float(selected[column].iloc[0]) if len(selected) else np.nan


def _brute_present_days(part, stamp: pd.Timestamp, days: float) -> float:
    """某桩在 ``[stamp-days, stamp)`` 内有遥测上报的**日历天数**（独立暴力版，不看 AsOfCounts）。"""
    if part is None:
        return 0.0
    starts = part["beijing_date"] - pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
    window = part[(starts >= stamp - pd.Timedelta(days=days)) & (starts < stamp)]
    return float(window["beijing_date"].nunique())


def _brute_streak(past: pd.DataFrame, stamp: pd.Timestamp, cap: int = 14) -> float:
    """从 T−1 往前数连续失败天数（不含 T 自己）。"""
    lookup = past.set_index("_start")["fail_flag"]
    cursor = stamp - pd.Timedelta(days=1)
    streak = 0
    while cursor in lookup.index and float(lookup.loc[cursor]) == 1 and streak < cap:
        streak += 1
        cursor -= pd.Timedelta(days=1)
    return float(streak)


# --------------------------------------------------------------------------- 特征名单


#: 候选集划分的归属规则（前缀匹配，顺序即优先级）。验证段上逐轮按 AUC 选一组——与第五/六线同法。
HISTORY_PREFIXES = ("attempts_sum_", "attempts_per_day_", "fails_sum_", "fail_rate_per_attempt_",
                   "users_sum_", "fail_days_", "fail_day_rate_", "charger_days_prior",
                   "charger_fail_days_prior", "charger_attempts_prior",
                   "charger_attempts_per_day_prior", "charger_fail_rate",
                   "prev_day_", "days_since_", "fail_streak_before")
OPS_PREFIXES = ("charger_tickets_", "charger_connector_tickets_", "charger_ticket_open_at_start",
               "charger_downtime_hours_", "charger_days_since_last_ticket", "station_tickets_",
               "tel_", "last_state_code", "last_online")


def candidate_sets(numeric: list[str], categorical: list[str]) -> dict[str, list[str]]:
    """四组候选特征集：只身份/计划、只面板历史、只运营信号（工单+遥测）、全部。

    刻意把"只面板历史"单列一组：它含**因果的日均尝试量**（暴露度预期），是"只身份"拿不到的信息；
    "只身份"则是第六线实测到的最强单柱。两组谁赢，就是"明天这台桩会被用几次"与"这是台什么桩"
    谁更有预测力——本线真正要回答的问题就在这上面。
    """
    def is_history(column: str) -> bool:
        return column.startswith(HISTORY_PREFIXES)

    def is_ops(column: str) -> bool:
        return column.startswith(OPS_PREFIXES)

    history_numeric = [c for c in numeric if is_history(c)]
    ops_numeric = [c for c in numeric if is_ops(c) and not is_history(c)]
    static_numeric = [c for c in numeric if not is_history(c) and not is_ops(c)]
    ops_categorical = [c for c in categorical if c == "last_state_code"]
    static_categorical = [c for c in categorical if c not in ops_categorical]
    groups = {
        "staticOnly": sorted(set(static_numeric) | set(static_categorical)),
        "historyOnly": sorted(set(history_numeric)),
        "opsOnly": sorted(set(ops_numeric) | set(ops_categorical)),
        "full": sorted(set(numeric) | set(categorical)),
    }
    universe = set(numeric) | set(categorical)
    for name, columns in groups.items():
        extra = set(columns) - universe
        assert not extra, f"候选集 {name} 引用了不在特征名单里的列：{sorted(extra)}"
    assert groups["staticOnly"] and groups["historyOnly"] and groups["opsOnly"], "有空候选集"
    assert set(groups["full"]) == universe
    assert not (set(groups["staticOnly"]) & set(common.NON_FEATURE_COLUMNS))
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
    derived = common.verify_derived()
    bounds = common.split_boundaries(manifest)
    panel = common.load_panel()
    context = common.load_context()
    assert int(panel["attempts_on_day"].sum()) == int(derived["attemptScope"]["inScopeRows"]), \
        "面板总尝试数与派生 manifest 记录不一致"
    assert int(panel["tech_fails_on_day"].sum()) == int(derived["attemptScope"]["techFailures"]), \
        "面板总技术失败数与派生 manifest 记录不一致"
    frame = panel.merge(load_statics(), on="charger_id", how="left", validate="many_to_one")
    assert len(frame) == len(panel) and frame["charger_model"].notna().all()
    frame, calendar_notes = attach_calendar_and_weather(frame, context)
    frame, history_notes = attach_panel_history(frame, panel)
    frame, ticket_notes = attach_tickets(frame)
    frame, telemetry_notes = attach_telemetry(frame)
    frame = frame.sort_values(["day_start_utc", "charger_id"], kind="stable").reset_index(drop=True)
    frame, label_notes = attach_label_and_split(frame, bounds)
    features = pick_features(frame)
    notes = {"calendar": calendar_notes, "history": history_notes, "tickets": ticket_notes,
             "telemetry": telemetry_notes, "label": label_notes}
    return frame, {"bounds": bounds, "features": features, "notes": notes,
                   "manifest": manifest, "derived": derived}


def main() -> dict:
    common.require_empty_run_dir(common.OUT_DIR)
    frame, built = build()
    features, notes = built["features"], built["notes"]
    audit = verify_no_future_leak(frame, common.load_panel(), common.load_context())
    common.write_new_pickle(common.FEATURES_PATH, frame)

    summary = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": built["manifest"].get("pipelineRunId"),
        "derivedDatasetId": common.DERIVED_DATASET_ID,
        "derivedFiles": built["derived"]["files"],
        "featuresSha256": common.sha256_file(common.FEATURES_PATH),
        "datasetId": common.DATASET_ID,
        "modelId": common.MODEL_ID,
        "label": "y_fail = 该桩当日 tech_fails_on_day ≥ 1；y_fails = 当日技术失败次数（回归目标）",
        "sampleUnit": "一台桩 × 一个北京日历日，且当日 ≥1 次插枪启动尝试",
        "decisionTime": "当日起点（day_start_utc）之前一刻；今天发生的一切不可见",
        "scope": notes["label"],
        "splits": {name: int((frame["split"] == name).sum())
                   for name in ("TRAIN", "VALIDATION", "TEST", "EXCLUDED")},
        "splitBaseRate": {name: round(float(frame.loc[frame["split"] == name, "y_fail"].mean()), 5)
                          for name in ("TRAIN", "VALIDATION", "TEST")},
        "splitPositives": {name: int(frame.loc[frame["split"] == name, "y_fail"].sum())
                           for name in ("TRAIN", "VALIDATION", "TEST")},
        "splitDays": {name: int(frame.loc[frame["split"] == name, "business_date"].nunique())
                      for name in ("TRAIN", "VALIDATION", "TEST")},
        "boundaries": {k: str(v) for k, v in built["bounds"].items()},
        "features": features,
        "featureCount": {"numeric": len(features["numeric"]),
                         "categorical": len(features["categorical"])},
        "constantWhy": {name: "样本域内取值唯一（非缺失），无信息量" for name in features["droppedConstant"]},
        "leakAudit": audit,
        "notes": notes,
        "featureRows": int(len(frame)),
        "disclaimer": common.data_note(),
    }
    common.write_new_json(common.BUILD_SUMMARY_PATH, summary)
    print(f"[reliability] 特征表 {len(frame):,} 行（面板 {notes['label']['panelRows']:,} 行，"
          f"丢 0 尝试日子 {notes['label']['droppedZeroAttemptRows']:,} 行）")
    print(f"[reliability] 正类 {summary['scope']['positives']:,} / 基础率 {summary['scope']['baseRate']} "
          f"/ 桩 {summary['scope']['chargers']} 台")
    print(f"[reliability] 切分 TRAIN={summary['splits']['TRAIN']:,}"
          f"({summary['splitDays']['TRAIN']}天) VAL={summary['splits']['VALIDATION']:,}"
          f"({summary['splitDays']['VALIDATION']}天) TEST={summary['splits']['TEST']:,}"
          f"({summary['splitDays']['TEST']}天) EXCLUDED={summary['splits']['EXCLUDED']:,}")
    print(f"[reliability] 基础率 TRAIN={summary['splitBaseRate']['TRAIN']} "
          f"VAL={summary['splitBaseRate']['VALIDATION']} TEST={summary['splitBaseRate']['TEST']}")
    print(f"[reliability] 特征 {len(features['numeric'])} 数值 + {len(features['categorical'])} 类别，"
          f"零方差剔除 {len(features['droppedConstant'])} 个：{features['droppedConstant']}")
    print(f"[reliability] 候选集 {[(k, len(v)) for k, v in features['groups'].items()]}")
    print(f"[reliability] 泄漏审计 抽样 {audit['sampled']} 行 × {len(audit['checked'])} 个量，"
          f"maxAbsDiff = {max(audit['maxAbsDiff'].values()):.1e}")
    print(f"[reliability] -> {common.FEATURES_PATH}")
    return summary


if __name__ == "__main__":
    main()
