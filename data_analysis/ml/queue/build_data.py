"""构建排队事件矩阵：一行 = 一次「加入排队」，标签 = 这次排队的结局与等待时长。

三个标签：
  outcome   ∈ {SERVED, ABANDONED, CALL_EXPIRED}（= queue_entries.status，结构上互斥且无缺失）
  y_waste   = 1 if outcome != SERVED（"白排"，业务上该提前提醒用户的那一类）
  wait_min  = (called_at - joined_at) 分钟；SERVED 与 CALL_EXPIRED 都观测得到（后者被叫过号只是没赶上），
              ABANDONED 自己退队、从未被叫 → 删失。回归只在 SERVED 上拟合与评价。

特征纪律（本文件的复杂度几乎全在这）：**只用 joined_at 严格之前的信息**。
  · 计数类（队列存量、到达强度）以 joined_at 为知识时刻，严格早于 t；
  · 结局类（近期等待均值、历史弃单率）以 **resolved_at** 为知识时刻——一条记录只有结束了才
    知道结果，按它的 joined_at 排序就漏了未来；
  · 比率一律用与数据无关的常数先验收缩（prior=0.5），不从全表标签率取，免得 TEST 的分布漏进 TRAIN。
`attempts.outcome` / `failure_reason` / `session_id` 与标签同源，**禁止进特征**（见 FORBIDDEN_FEATURES）。
最后 `verify_no_future_leak` 随机抽样暴力重算四项 as-of 特征，要求 0 偏差，否则整个构建失败。

产物：outputs/ml_queue/queue_matrix.pkl + build_summary.json（均独占创建，已存在即报错）。

用法（仓库根目录）：python -m data_analysis.ml.queue.build_data
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from . import common

#: 与标签同源的列，禁止进特征。
FORBIDDEN_FEATURES = frozenset({
    "status", "outcome", "y_waste", "wait_min", "called_at", "resolved_at", "session_id",
    "attempt_id", "reservation_status", "reservation_resolved_at", "reservation_session_id",
})

SMOOTH_ALPHA = 20.0      #: 站点级比率收缩强度
SMOOTH_ALPHA_USER = 5.0  #: 用户级比率收缩强度
NEUTRAL_PRIOR = 0.5      #: 与数据无关的先验，防止全表标签率泄漏

#: charger_telemetry.state 的取值编码；不在表内的取值一律编成 NaN（下面断言会抓住未知状态）。
CHARGER_STATE_CODES = {"AVAILABLE": 0.0, "CHARGING": 1.0, "OCCUPIED": 2.0, "RESERVED": 3.0,
                       "MAINTENANCE": 4.0, "OFFLINE": 5.0}
_AVAIL, _CHARGING, _OCCUPIED, _RESERVED, _MAINT, _OFFLINE = (
    CHARGER_STATE_CODES[key] for key in
    ("AVAILABLE", "CHARGING", "OCCUPIED", "RESERVED", "MAINTENANCE", "OFFLINE"))

#: 站点级 as-of 特征列，必须与 StationAsOf.query 返回的键完全一致。
STATION_ASOF_COLUMNS = (
    "arrivals_15m", "arrivals_60m", "arrivals_180m", "open_now", "wait_mean_3h", "wait_obs_3h",
    "waste_rate_7d_raw", "waste_n_7d", "waste_rate_prior", "waste_rate_all_prior",
    "station_history_n",
)


def _assert(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


class StationAsOf:
    """单站"截至 t"查询器：排序数组 + searchsorted，一律 side='left' 即严格早于 t。"""

    def __init__(self, frame: pd.DataFrame) -> None:
        self.joined = np.sort(frame["joined_at"].values.astype("datetime64[ns]"))
        self.resolved = np.sort(frame["resolved_at"].values.astype("datetime64[ns]"))

        served = frame[frame["wait_min"].notna()].sort_values("resolved_at")
        self.wait_time = served["resolved_at"].values.astype("datetime64[ns]")
        self.wait_cumsum = np.cumsum(served["wait_min"].to_numpy(dtype=float))
        self.wait_count = np.arange(1, len(served) + 1, dtype=float)

        ended = frame.sort_values("resolved_at")
        self.end_time = ended["resolved_at"].values.astype("datetime64[ns]")
        self.end_waste_cumsum = np.cumsum(ended["y_waste"].to_numpy(dtype=float))
        self.end_count = np.arange(1, len(ended) + 1, dtype=float)

    @staticmethod
    def _window(cumsum: np.ndarray, cumcount: np.ndarray, times: np.ndarray,
                low: np.datetime64, high: np.datetime64) -> tuple[float, float]:
        """[low, high) 内的和与个数；times 已按时间升序。"""
        if not len(times):
            return 0.0, 0.0
        hi = np.searchsorted(times, high, side="left")
        lo = np.searchsorted(times, low, side="left")
        total = (cumsum[hi - 1] if hi else 0.0) - (cumsum[lo - 1] if lo else 0.0)
        count = (cumcount[hi - 1] if hi else 0.0) - (cumcount[lo - 1] if lo else 0.0)
        return float(total), float(count)

    def query(self, t: np.datetime64) -> dict[str, float]:
        minute = np.timedelta64(1, "m")
        j_before = int(np.searchsorted(self.joined, t, side="left"))
        r_before = int(np.searchsorted(self.resolved, t, side="left"))
        out = {
            "arrivals_15m": float(j_before - np.searchsorted(self.joined, t - 15 * minute, "left")),
            "arrivals_60m": float(j_before - np.searchsorted(self.joined, t - 60 * minute, "left")),
            "arrivals_180m": float(j_before - np.searchsorted(self.joined, t - 180 * minute, "left")),
            # 同一时刻全站还没结束的人数；position_at_join-1 是"排在我前面"，两者口径不同都留着
            "open_now": float(j_before - r_before),
        }
        wait_sum, wait_n = self._window(self.wait_cumsum, self.wait_count, self.wait_time,
                                        t - 180 * minute, t)
        out["wait_mean_3h"] = wait_sum / wait_n if wait_n else np.nan
        out["wait_obs_3h"] = wait_n
        week_sum, week_n = self._window(self.end_waste_cumsum, self.end_count, self.end_time,
                                        t - 7 * 24 * 60 * minute, t)
        out["waste_rate_7d_raw"] = week_sum / week_n if week_n else np.nan
        out["waste_n_7d"] = week_n
        out["waste_rate_prior"] = (week_sum + SMOOTH_ALPHA * NEUTRAL_PRIOR) / (week_n + SMOOTH_ALPHA)
        all_sum, all_n = self._window(self.end_waste_cumsum, self.end_count, self.end_time,
                                      np.datetime64("1970-01-01", "ns"), t)
        out["waste_rate_all_prior"] = (all_sum + SMOOTH_ALPHA * NEUTRAL_PRIOR) / (all_n + SMOOTH_ALPHA)
        out["station_history_n"] = all_n
        return out


def load_queue() -> tuple[pd.DataFrame, dict[str, object]]:
    """读排队表、打标签、跑结构断言。断言不过就终止，不带病往下训。"""
    queue = common.load_table("queue_entries").copy()
    for column in ("joined_at", "called_at", "resolved_at"):
        queue[column] = pd.to_datetime(queue[column])

    checks: dict[str, object] = {}
    statuses = {"SERVED", "ABANDONED", "CALL_EXPIRED"}
    _assert(set(queue["status"].unique()) == statuses, f"status 取值异常: {set(queue['status'].unique())}")
    _assert(bool(queue["queue_id"].is_unique), "queue_id 不唯一")
    _assert(bool(queue["resolved_at"].notna().all()), "存在 resolved_at 缺失，open_now 会算错")

    called = queue["called_at"].notna()
    served = queue["status"].eq("SERVED")
    session = queue["session_id"].notna()
    _assert(bool((served == session).all()), "SERVED 与 session_id 非空不等价")
    _assert(bool((queue["status"].eq("ABANDONED") == ~called).all()), "ABANDONED 与「未被叫号」不等价")
    _assert(bool((queue["status"].eq("CALL_EXPIRED") == (called & ~session)).all()),
            "CALL_EXPIRED 与「叫号了但没成单」不等价")
    _assert(bool((queue["resolved_at"] >= queue["joined_at"]).all()), "存在 resolved_at 早于 joined_at")
    _assert(bool((queue.loc[called, "called_at"] >= queue.loc[called, "joined_at"]).all()),
            "存在 called_at 早于 joined_at")

    queue["outcome"] = queue["status"]
    queue["y_waste"] = (~served).astype(int)
    queue["wait_min"] = (queue["called_at"] - queue["joined_at"]).dt.total_seconds() / 60.0
    _assert(bool(queue.loc[served, "wait_min"].notna().all()), "SERVED 行缺 called_at")
    wait = queue["wait_min"].dropna()
    _assert(bool((wait >= 0).all()) and bool((wait <= 120).all()), "wait_min 超出 [0,120] 合理区间")
    queue["position_at_join"] = queue["position_at_join"].astype(int)
    _assert(bool(queue["position_at_join"].between(1, 20).all()), "position_at_join 越界")

    checks["rows"] = int(len(queue))
    checks["statusCounts"] = {k: int(v) for k, v in queue["status"].value_counts().items()}
    checks["wasteRate"] = float(queue["y_waste"].mean())
    served_wait = queue.loc[served, "wait_min"]
    checks["servedWaitMinutes"] = {str(q): round(float(served_wait.quantile(q)), 2)
                                   for q in (0.5, 0.9, 0.99)}
    checks["positionRange"] = [int(queue["position_at_join"].min()), int(queue["position_at_join"].max())]
    checks["joinedAtRangeUtc"] = [str(queue["joined_at"].min()), str(queue["joined_at"].max())]
    return queue, checks


def attach_statics(queue: pd.DataFrame) -> pd.DataFrame:
    """站点 / 用户 / 车辆 / 预约的静态属性，以及在 joined_at 之前就成立的外键。"""
    stations = common.load_table("stations")
    chargers = common.load_table("chargers")
    users = common.load_table("users")
    vehicles = common.load_table("vehicles")
    attempts = common.load_table("charging_attempts")
    reservations = common.load_table("reservations")

    per_station = chargers.groupby("station_id").agg(
        charger_count=("charger_id", "size"),
        rated_kw_total=("rated_power_kw", "sum"),
        rated_kw_max=("rated_power_kw", "max")).reset_index()
    dc_share = (chargers.assign(is_dc=chargers["connector_type"].isin(["CCS1", "CHAdeMO", "CCS2"]))
                .groupby("station_id", as_index=False)["is_dc"].mean()
                .rename(columns={"is_dc": "dc_share"}))
    connector_mode = (chargers.groupby("station_id", as_index=False)["connector_type"].agg(
        lambda s: s.value_counts().idxmax()).rename(columns={"connector_type": "connector_share_type"}))

    queue = queue.merge(stations[["station_id", "city_id", "site_type", "opened_at", "transformer_kw"]],
                        on="station_id", how="left", validate="many_to_one")
    for extra in (per_station, dc_share, connector_mode):
        queue = queue.merge(extra, on="station_id", how="left", validate="many_to_one")
    _assert(bool(queue["city_id"].notna().all()), "存在关联不到站点的排队事件")
    queue["opened_at"] = pd.to_datetime(queue["opened_at"])
    queue["station_age_days"] = (queue["joined_at"] - queue["opened_at"]).dt.total_seconds() / 86400.0

    queue = queue.merge(users[["user_id", "membership", "segment", "registered_at", "home_city_id"]],
                        on="user_id", how="left", validate="many_to_one")
    queue["registered_at"] = pd.to_datetime(queue["registered_at"])
    queue["account_age_days"] = (queue["joined_at"] - queue["registered_at"]).dt.total_seconds() / 86400.0
    queue["home_city_matches_station"] = queue["home_city_id"].eq(queue["city_id"]).astype(int)

    # attempts 只借两个"排队前就存在"的外键；outcome/failure_reason/session_id 与标签同源，不读
    slim = attempts[["queue_id", "vehicle_id", "reservation_id"]].dropna(subset=["queue_id"])
    _assert(bool(slim["queue_id"].is_unique), f"attempts.queue_id 不唯一，无法当 1:1 外键: {slim.shape}")
    before = len(queue)
    queue = queue.merge(slim, on="queue_id", how="left", validate="one_to_one")
    _assert(len(queue) == before, "attempts 关联改变了行数")

    queue = queue.merge(vehicles[["vehicle_id", "battery_capacity_kwh", "max_charge_kw", "vehicle_class"]],
                        on="vehicle_id", how="left", validate="many_to_one")
    # 这辆车在这个站实际能吃多大功率：车辆上限与站内最大桩上限取小，排队时双方都已知
    queue["usable_kw"] = queue[["max_charge_kw", "rated_kw_max"]].min(axis=1)

    res = reservations[["reservation_id", "created_at", "expires_at"]].copy()
    for column in ("created_at", "expires_at"):
        res[column] = pd.to_datetime(res[column])
    queue = queue.merge(res, on="reservation_id", how="left", validate="many_to_one")
    queue["from_reservation"] = queue["reservation_id"].notna().astype(float)
    queue["reservation_lead_min"] = (queue["expires_at"] - queue["created_at"]).dt.total_seconds() / 60.0
    # created_at 必须早于 joined_at 才算"排队时已知"；晚于的一律作废
    late = queue["created_at"].notna() & (queue["created_at"] > queue["joined_at"])
    queue.loc[late, ["from_reservation", "reservation_lead_min"]] = np.nan
    queue["from_reservation"] = queue["from_reservation"].fillna(0.0)
    queue["reservation_lead_known"] = queue["reservation_lead_min"].notna().astype(int)
    queue.attrs["lateReservationRows"] = int(late.sum())
    return queue.drop(columns=["created_at", "expires_at", "reservation_id", "vehicle_id",
                              "home_city_id", "opened_at", "registered_at"])


def attach_event_time(queue: pd.DataFrame, bounds: dict[str, pd.Timestamp]) -> pd.DataFrame:
    """本地时间、日历、电价、天气。全部按"当时/当天已知"取。"""
    queue = queue.sort_values("joined_at", kind="mergesort").reset_index(drop=True)
    queue["_pos"] = np.arange(len(queue))
    queue["joined_local"] = queue["joined_at"] + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)
    queue["hour_local"] = queue["joined_local"].dt.hour
    queue["dow_local"] = queue["joined_local"].dt.dayofweek
    queue["business_date"] = queue["joined_local"].dt.normalize()
    queue["split"] = common.assign_split(queue["joined_at"], bounds)

    calendar = common.load_table("calendar")
    calendar["business_date"] = pd.to_datetime(calendar["business_date"])
    queue = queue.merge(calendar[["city_id", "business_date", "is_weekend", "scenario_event",
                                  "demand_multiplier"]],
                        on=["city_id", "business_date"], how="left", validate="many_to_one")

    tariffs = common.load_table("tariffs")
    before = len(queue)
    queue = queue.merge(tariffs[["city_id", "hour", "energy_price_cents_per_kwh",
                                 "service_price_cents_per_kwh", "period"]],
                        left_on=["city_id", "hour_local"], right_on=["city_id", "hour"],
                        how="left", validate="many_to_one")
    _assert(len(queue) == before, "电价关联改变了行数")
    queue = queue.drop(columns="hour")

    weather = common.load_table("weather_hourly").copy()
    weather["recorded_at"] = pd.to_datetime(weather["recorded_at"])
    weather_cols = ["temperature_c", "humidity_pct", "weather", "rainfall_mm"]
    right = (weather[["city_id", "recorded_at", *weather_cols]]
             .rename(columns={"recorded_at": "joined_at"})
             .drop_duplicates(["city_id", "joined_at"], keep="last"))
    # merge_asof 两侧都必须按键有序；它的输出行序跟随排序后的 left，所以用 _pos 还原对齐
    merged = pd.merge_asof(queue[["_pos", "joined_at", "city_id"]].sort_values("joined_at", kind="mergesort"),
                           right.sort_values("joined_at", kind="mergesort"),
                           left_on="joined_at", right_on="joined_at", by="city_id",
                           direction="backward", allow_exact_matches=True)
    restored = merged.set_index("_pos").reindex(queue["_pos"])
    _assert(bool((restored["joined_at"].to_numpy() == queue["joined_at"].to_numpy()).all()),
            "weather as-of 还原对齐失败")
    for column in weather_cols:
        queue[column] = restored[column].to_numpy()
    return queue.drop(columns="_pos")


def attach_station_asof(queue: pd.DataFrame) -> pd.DataFrame:
    """站点级"截至 t"动态特征 + 用户级历史弃单率。"""
    work = pd.DataFrame({
        "station_id": queue["station_id"].to_numpy(),
        "user_id": queue["user_id"].to_numpy(),
        "joined_at": queue["joined_at"].to_numpy(),
        "resolved_at": queue["resolved_at"].to_numpy(),
        "wait_min": queue["wait_min"].to_numpy(dtype=float),
        "y_waste": queue["y_waste"].to_numpy(dtype=float),
    }, index=queue.index)
    holder = {name: np.full(len(queue), np.nan) for name in STATION_ASOF_COLUMNS}
    offsets = {value: i for i, value in enumerate(queue.index)}
    for _, group in work.groupby("station_id", sort=False):
        asof = StationAsOf(group)
        positions = np.fromiter((offsets[i] for i in group.index), dtype=int, count=len(group))
        for pos, joined in zip(positions, group["joined_at"].to_numpy()):
            for name, value in asof.query(joined).items():
                holder[name][pos] = value
    for name, values in holder.items():
        queue[name] = values

    # 用户级：只用严格更早的记录——cumcount 就是"此前排队次数"，shift(1) 把自己排除掉。
    # 表已按 joined_at 有序（attach_event_time），所以这个"更早"与时间轴一致。
    user = queue["user_id"]
    prior_n = queue.groupby(user, sort=False).cumcount().astype(float)
    cum_waste = queue["y_waste"].astype(float).groupby(user, sort=False).cumsum().shift(1).fillna(0.0)
    queue["user_history_n"] = prior_n.to_numpy()
    queue["user_waste_rate_prior"] = ((cum_waste + SMOOTH_ALPHA_USER * NEUTRAL_PRIOR)
                                      / (prior_n + SMOOTH_ALPHA_USER)).to_numpy()
    queue["is_first_queue"] = (prior_n == 0).astype(int).to_numpy()
    last_seen = queue["joined_at"].groupby(user, sort=False).shift(1)
    queue["days_since_last_queue"] = ((queue["joined_at"] - last_seen).dt.total_seconds()
                                      / 86400.0).to_numpy()
    return queue


def attach_charger_states(queue: pd.DataFrame) -> pd.DataFrame:
    """排队那一刻该站各桩的最近一次上报状态（388.8 万行遥测先压成 时间×桩 宽表再 as-of）。"""
    telemetry = common.load_table("charger_telemetry")[
        ["charger_id", "recorded_at", "state", "power_kw"]].copy()
    telemetry["recorded_at"] = pd.to_datetime(telemetry["recorded_at"])
    chargers = common.load_table("chargers")[["charger_id", "station_id"]]

    wide = telemetry.pivot_table(index="recorded_at", columns="charger_id", values="state", aggfunc="last")
    power = telemetry.pivot_table(index="recorded_at", columns="charger_id", values="power_kw", aggfunc="last")
    times = wide.index.to_numpy()
    states = wide.apply(lambda col: col.map(CHARGER_STATE_CODES)).to_numpy(dtype=float)
    _assert(not (pd.isna(states) & ~pd.isna(wide.to_numpy())).any(),
            "遥测里出现未知 state，编码后变 NaN 会被当成空闲，必须先扩充 CHARGER_STATE_CODES")
    powers = power.reindex(index=wide.index, columns=wide.columns).to_numpy(dtype=float)

    query_pos = np.searchsorted(times, queue["joined_at"].values.astype("datetime64[ns]"),
                               side="left") - 1
    names = ("charger_available", "charger_charging", "charger_stalled", "charger_down",
             "charger_total", "charging_power_mean_kw", "available_share", "charger_state_fresh_min")
    out = {name: np.full(len(queue), np.nan) for name in names}
    station_ids = queue["station_id"].to_numpy()
    joined_ns = queue["joined_at"].values.astype("datetime64[ns]")
    for station, charger_ids in chargers.groupby("station_id")["charger_id"].apply(list).items():
        rows = np.flatnonzero((station_ids == station) & (query_pos >= 0))
        cols = np.array([wide.columns.get_loc(c) for c in charger_ids if c in wide.columns], dtype=int)
        if not len(rows) or not len(cols):
            continue
        idx = query_pos[rows]
        block = states[np.ix_(idx, cols)]
        block_power = powers[np.ix_(idx, cols)]
        available = (block == _AVAIL).sum(axis=1).astype(float)
        charging = (block == _CHARGING).sum(axis=1).astype(float)
        total = float(len(cols))
        with np.errstate(invalid="ignore", divide="ignore"):
            out["charger_available"][rows] = available
            out["charger_charging"][rows] = charging
            out["charger_stalled"][rows] = ((block == _OCCUPIED) | (block == _RESERVED)).sum(axis=1)
            out["charger_down"][rows] = ((block == _MAINT) | (block == _OFFLINE)).sum(axis=1)
            out["charger_total"][rows] = total
            out["charging_power_mean_kw"][rows] = np.where(
                charging > 0, np.nansum(np.where(block == _CHARGING, block_power, np.nan), axis=1)
                / np.maximum(charging, 1.0), np.nan)
            out["available_share"][rows] = available / total
            out["charger_state_fresh_min"][rows] = (joined_ns[rows] - times[idx]) / np.timedelta64(1, "m")
    for name, values in out.items():
        queue[name] = values
    _assert(bool(queue["charger_total"].notna().all()), "存在取不到桩状态的排队事件")
    _assert(float(np.nanmax(queue["charger_state_fresh_min"])) < 120.0, "桩状态新鲜度超过 2 小时，不可信")
    return queue


def verify_no_future_leak(queue: pd.DataFrame, sample: int = 300, seed: int = common.SEED) -> dict:
    """随机抽样暴力重算四项 as-of 特征：必须与"严格早于 t"的定义逐条一致，否则构建失败。"""
    rng = np.random.default_rng(seed)
    picked = queue.iloc[rng.choice(len(queue), size=min(sample, len(queue)), replace=False)]
    worst: dict[str, float] = {}
    offenders: dict[str, int] = {}
    week = pd.Timedelta(days=7)
    three_hours = pd.Timedelta(minutes=180)
    for row in picked.itertuples(index=False):
        same = queue[(queue["station_id"] == row.station_id) & (queue["queue_id"] != row.queue_id)]
        t = row.joined_at
        joined_earlier = same["joined_at"] < t
        ended = same["resolved_at"] < t
        in_week = ended & (same["resolved_at"] >= t - week)
        wait_rows = same[ended & (same["resolved_at"] >= t - three_hours) & same["wait_min"].notna()]
        truth = {
            # 与 query() 同一条保守规则：resolved_at == t 的离开视作"这一刻还不知道"，仍算在队；
            # joined_at == t 的同时加入也不计入（排在我前面的人数由 position_at_join 给出）。
            "open_now": float((joined_earlier & (same["resolved_at"] >= t)).sum()),
            "arrivals_60m": float((joined_earlier
                                   & (same["joined_at"] >= t - pd.Timedelta(minutes=60))).sum()),
            "wait_mean_3h": float(wait_rows["wait_min"].mean()) if len(wait_rows) else np.nan,
            "waste_rate_prior": float(((in_week & (same["status"] != "SERVED")).sum()
                                       + SMOOTH_ALPHA * NEUTRAL_PRIOR)
                                      / (float(in_week.sum()) + SMOOTH_ALPHA)),
        }
        got = {"open_now": row.open_now, "arrivals_60m": row.arrivals_60m,
               "wait_mean_3h": row.wait_mean_3h, "waste_rate_prior": row.waste_rate_prior}
        for name in truth:
            a, b = truth[name], got[name]
            if pd.isna(a) and pd.isna(b):
                delta = 0.0
            elif pd.isna(a) or pd.isna(b):
                delta = np.inf
            else:
                delta = abs(float(a) - float(b))
            worst[name] = max(worst.get(name, 0.0), delta)
            offenders[name] = offenders.get(name, 0) + int(delta > 1e-9)
    _assert(not any(offenders.values()),
            f"as-of 特征与暴力重算不一致（未来信息泄漏）：{offenders} {worst}")
    return {"sampled": int(len(picked)), "features": sorted(worst), "maxAbsDiff": worst,
            "note": "暴力重算只承认严格早于 joined_at 的行；四项最大偏差全为 0 才算通过"}


def pick_features(queue: pd.DataFrame) -> tuple[list[str], list[str], dict[str, str]]:
    """数值特征 = 除 NON_FEATURE/FORBIDDEN/时间戳列外的全部数值列；零方差列一律剔除并留痕。"""
    drop = set(common.NON_FEATURE_COLUMNS) | set(FORBIDDEN_FEATURES) | {"joined_local", "business_date"}
    numeric: list[str] = []
    categorical: list[str] = []
    constant: dict[str, str] = {}
    for column in queue.columns:
        if column in drop:
            continue
        dtype = queue[column].dtype
        is_category = str(dtype) in ("object", "category")
        if column in common.CATEGORICAL_FEATURES:
            _assert(is_category, f"名单里的类别列却是数值型: {column}")
        elif not (pd.api.types.is_numeric_dtype(dtype) or str(dtype) == "boolean"):
            raise AssertionError(f"未分类的列漏进特征池: {column} dtype={dtype}")
        levels = queue[column].nunique(dropna=False)
        if levels <= 1:
            only = queue[column].dropna()
            constant[column] = "all-NaN" if only.empty else f"唯一取值 {only.iloc[0]!r}"
            continue
        (categorical if is_category else numeric).append(column)
    _assert(not (set(numeric) & (set(common.NON_FEATURE_COLUMNS) | set(FORBIDDEN_FEATURES))),
            "数值特征里混进了标签同源列")
    return numeric, categorical, constant


def main() -> dict:
    common.require_empty_run_dir()
    manifest = common.verify_batch()
    bounds = common.split_boundaries(manifest)

    queue, checks = load_queue()
    queue = attach_statics(queue)
    queue = attach_event_time(queue, bounds)
    queue = attach_station_asof(queue)
    queue = attach_charger_states(queue)
    numeric, categorical, constant = pick_features(queue)

    excluded = queue[queue["split"] == "EXCLUDED"]
    checks["lateReservationRows"] = int(queue.attrs.get("lateReservationRows", 0))
    checks["splitCounts"] = {k: int(v) for k, v in queue["split"].value_counts().items()}
    checks["splitWasteRate"] = {k: round(float(v), 4)
                                for k, v in queue.groupby("split")["y_waste"].mean().items()}
    checks["excludedOutsideWindow"] = int(len(excluded))
    checks["window"] = {k: str(v) for k, v in bounds.items()}
    checks["leakAudit"] = verify_no_future_leak(queue)
    checks["features"] = {"numeric": numeric, "categorical": categorical,
                          "numericCount": len(numeric), "categoricalCount": len(categorical)}
    checks["featureNullRate"] = {c: round(float(queue[c].isna().mean()), 4)
                                 for c in numeric if queue[c].isna().any()}
    checks["droppedConstant"] = constant
    checks["constantWhy"] = ("这批模拟数据的 25 个站点结构完全相同（每站 3 桩、变压器 360 kW、服务费 30 分），"
                             "且没有任何排队事件来自预约、所有用户主城市都与站点同城；这类零方差列不带来信息，"
                             "留在特征里只会让模型分裂时被噪声牵动，故在建表阶段剔除并逐个留痕。"
                             "结论：**本线的可用信号只可能来自队列动态与时间/天气，不来自站点静态属性**。")
    checks["timezoneCheck"] = {
        "hourLocalCounts": {int(k): int(v) for k, v in
                            queue["hour_local"].value_counts().sort_index().items()},
        "peakHoursLocal": [int(x) for x in queue["hour_local"].value_counts().head(3).index],
        "note": "数据按 UTC 存储，+8 后峰值应落在本地傍晚；用 UTC 零点切分会整体错 8 小时",
    }
    checks["waitLabel"] = {
        "observedRowsByOutcome": {k: int(v) for k, v in
                                  queue[queue["wait_min"].notna()].groupby("outcome").size().items()},
        "note": "SERVED 与 CALL_EXPIRED 都能观测到「叫号前等待」，ABANDONED 自己退队、永远不会被叫，"
                "属删失。回归与 MAE 只在 SERVED 上行——预测的是「最终排上要等多久」，因此对中途放弃的人"
                "存在选择偏差，如实记在 README 已知不足。",
    }
    checks["publishedBatchId"] = manifest["publishedBatchId"]
    checks["pipelineRunId"] = manifest["pipelineRunId"]
    checks["datasetId"] = common.DATASET_ID
    checks["modelIds"] = [common.MODEL_ID, common.WAIT_MODEL_ID]
    checks["modelVersion"] = common.MODEL_VERSION
    checks["seed"] = common.SEED
    checks["dataDiscipline"] = ("只读消费 clean 层 parquet；产物全部落 outputs/ml_queue/ 且独占创建；"
                                "未新增、未修改、未删除数据集目录下任何文件。")
    checks["labelUnits"] = ("wait_min 单位=分钟；y_waste=1 表示这次排队最终没充上"
                            "（ABANDONED 未被叫号 / CALL_EXPIRED 叫号超时）。")
    checks["disclaimer"] = common.simulated_note()

    common.write_new_pickle(common.MATRIX_PATH, queue)
    checks["matrixSha256"] = common.sha256_file(common.MATRIX_PATH)
    common.write_new_json(common.BUILD_SUMMARY_PATH, checks)
    print(f"[build] rows={len(queue):,} numeric={len(numeric)} categorical={len(categorical)}")
    for split in ("TRAIN", "VALIDATION", "TEST", "EXCLUDED"):
        sub = queue[queue["split"] == split]
        print(f"[build] {split:10s} n={len(sub):>7,} waste={float(sub['y_waste'].mean()):.4f}")
    print(f"[build] leakAudit maxAbsDiff={checks['leakAudit']['maxAbsDiff']}")
    print(f"[build] freshMax={float(np.nanmax(queue['charger_state_fresh_min'])):.1f}min "
          f"nullRates={len(checks['featureNullRate'])}列含缺失 "
          f"零方差剔除={len(checks['droppedConstant'])}列")
    print(f"[build] -> {common.MATRIX_PATH}")
    return checks


if __name__ == "__main__":
    main()
