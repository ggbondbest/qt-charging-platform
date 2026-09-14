"""建矩阵：一行一次"已经选定桩"的启动尝试，标签 = 这次插枪启动是否技术性失败。

样本域（``build_summary.scope`` 里逐项留痕）：
    非排队关联 ∧ charger_id 非空 ∧ outcome ∈ {STARTED, FAILED}
被排除的四块都有明确理由，不是"看着不顺就删"：
  · 带 ``queue_id`` 的 36,594 行——那是第五线的样本与标签，本线整块不碰；
  · ``NO_AVAILABLE_CHARGER`` 的 24,380 行（ABANDONED）——压根没选到桩，没有插枪这一步；
  · ``RESERVATION_CANCELLED`` / ``RESERVATION_EXPIRED`` 3,215 行——预约侧取消/过期，机制不同；
  · ``FAILED`` 但 ``charger_id`` 为空的 2,164 行——App 侧还没选桩就超时/鉴权失败，
    桩侧特征全不可得。它们的三类原因分布与在桩上的失败几乎同比例（762/724/678），
    硬塞进来只会让"缺 charger_id"这个掩码变成一个假特征。

标签：``y_tech = failure_reason ∈ {CONNECTOR_HANDSHAKE, APP_TIMEOUT, AUTH_FAILED}``。

泄漏纪律（本线最容易踩的两个坑，都写在断言与审计里）：
  1. **遥测同刻陷阱**：模拟器在 ``attempted_at`` 那一刻把桩状态翻成 CHARGING。若按
     ``recorded_at <= attempted_at`` 关联"最后一条遥测"，正类 3,676/3,676 全是 AVAILABLE——
     看着像神模型，其实是答案抄进特征。本线一律**严格早于**（同刻视作还不知道）；修完再测，
     该特征 AUC 掉到 0.5018，也就是说遥测没有独立信息，这一点如实写进结果。
  2. **比率特征的收缩目标**必须是"截至 t 之前"的全量经验率（只看过去），不是全表标签率——
     后者会把 TEST 的分布漏进 TRAIN 的特征。取"与数据无关的常数 0.5"在这条线也不合适
     （基础率 3.6%，0.5 会把小样本桩的率推到失真），所以用因果的滚动全量率当目标；
     全量率本身没有历史时（时间序最前那些行）取 0.0，不引入任何外部常数。

所有"取上一刻的值"都走同一个 ``strict_last``，所有"数窗口内的事件"都走同一个 ``AsOfCounts``，
实现只有一条路；``verify_no_future_leak`` 再用全表布尔掩码把其中 9 个量逐行暴力重算一遍——
不复用同一套排序/二分/merge_asof 代码，自证不出错这件事要靠两条独立路径对齐。

产物：``outputs/ml_attempt/attempt_matrix.pkl`` + ``build_summary.json``（结构断言 + 泄漏审计），
全部 O_EXCL 独占创建。用法（仓库根目录）：python -m data_analysis.ml.attempt.build_data
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from . import common

#: 事后字段、身份外键与时间戳本身：留在表里做分组/溯源/评价，绝不进特征。
FORBIDDEN_FEATURES = frozenset({
    "attempt_id", "user_id", "vehicle_id", "station_id", "charger_id", "city_id", "home_city_id",
    "outcome", "failure_reason", "session_id", "queue_id", "reservation_id", "tech_reason_code",
    "attempted_at", "attempted_local", "business_date", "recorded_at", "created_at",
    "expires_at", "opened_at", "registered_at", "commissioned_at",
})

#: 明确宣布"读过但不用"的列：零方差、事后终态、或与已用列共线。留着名单是为了下次换批次时
#: 有人会问"这列为什么不算特征"。
IGNORED_COLUMNS = frozenset({
    "severity", "status", "ticket_id", "interval_seconds", "energy_wh", "grid_energy_wh",
    "grid_cost_cents", "meter_wh", "accepted_at", "work_started_at", "station_name", "latitude",
    "longitude", "rent_daily_cents", "battery_capacity_kwh", "acquisition_channel", "hour",
})

#: 收缩强度：样本越少，越往"截至 t 之前的全量经验率"收。
ALPHA_CHARGER = 20.0
ALPHA_MODEL = 30.0
ALPHA_STATION = 30.0
ALPHA_USER = 5.0
ALPHA_RECENT = 5.0

TELEMETRY_STATES = ("AVAILABLE", "CHARGING", "OCCUPIED", "RESERVED", "MAINTENANCE", "OFFLINE")
STATE_CODES = {name: code for code, name in enumerate(TELEMETRY_STATES)}
#: 与本次尝试同刻发生的观测，一律算"还不知道"（见模块文档第 1 条）。
ONE_NS = pd.Timedelta(nanoseconds=1)


# --------------------------------------------------------------------------- 两个 as-of 原语


class AsOfCounts:
    """按实体分组的"严格早于 t"累计次数/累计值，以及半开窗 ``[t-days, t)`` 内的同样两量。

    实现：把每个实体的事件时间戳压成"唯一时刻 + 该时刻事件数/值和"，做前缀和；查询时
    ``pos = searchsorted(uniq, t, side="left")`` 就是严格早于 t 的唯一时刻个数——同刻并列按
    "还不知道"处理，与第五线 ``StationAsOf`` 同一条规则（那条线在含同刻上踩过同一个坑）。
    """

    def __init__(self, events: pd.DataFrame, key: str, stamps: str, value: str | None = None) -> None:
        self.tables: dict[object, tuple[np.ndarray, np.ndarray, np.ndarray]] = {}
        columns = [key, stamps] + ([value] if value else [])
        for entity, part in events[columns].groupby(key, observed=True, sort=False):
            times = pd.to_datetime(part[stamps]).to_numpy(dtype="datetime64[ns]").astype("int64")
            values = (np.ones(len(part)) if value is None
                      else pd.to_numeric(part[value], errors="raise").to_numpy(dtype=float))
            order = np.argsort(times, kind="stable")
            times, values = times[order], values[order]
            uniq, inverse = np.unique(times, return_inverse=True)
            self.tables[entity] = (
                uniq,
                np.cumsum(np.bincount(inverse, minlength=len(uniq)).astype(float)),
                np.cumsum(np.bincount(inverse, weights=values, minlength=len(uniq))),
            )

    def _prefix(self, entity, times_ns: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """严格早于 ``times_ns`` 的 (事件数, 值和)。"""
        table = self.tables.get(entity)
        if table is None:
            return np.zeros(len(times_ns)), np.zeros(len(times_ns))
        uniq, cum_counts, cum_sums = table
        pos = np.searchsorted(uniq, times_ns, side="left")
        index = np.maximum(pos - 1, 0)
        return (np.where(pos > 0, cum_counts[index], 0.0),
                np.where(pos > 0, cum_sums[index], 0.0))

    def _groups(self, query: pd.DataFrame, key: str) -> tuple[dict, np.ndarray]:
        times = pd.to_datetime(query["__query_time"]).to_numpy(dtype="datetime64[ns]").astype("int64")
        return query.groupby(key, observed=True, sort=False).indices, times

    def cumulative(self, query: pd.DataFrame, key: str, value: str | None = None):
        """严格早于 ``query["__query_time"]`` 的 (事件数, 值和)。纯计数时二者相同。"""
        indices, times = self._groups(query, key)
        counts = np.zeros(len(query))
        sums = np.zeros(len(query))
        for entity, index in indices.items():
            counts[index], sums[index] = self._prefix(entity, times[index])
        return counts, sums

    def window(self, query: pd.DataFrame, key: str, days: float, value: str | None = None):
        """半开窗 ``[t-days, t)`` 内的 (事件数, 值和)：两次前缀和相减，两端都严格早于 t。"""
        indices, times = self._groups(query, key)
        lo_target = times - np.int64(round(days * 86_400e9))
        counts = np.zeros(len(query))
        sums = np.zeros(len(query))
        for entity, index in indices.items():
            hi_counts, hi_sums = self._prefix(entity, times[index])
            lo_counts, lo_sums = self._prefix(entity, lo_target[index])
            counts[index] = hi_counts - lo_counts
            sums[index] = hi_sums - lo_sums
        return counts, sums


def strict_last(left: pd.DataFrame, time: str, key: str, events: pd.DataFrame, event_time: str,
                columns: list[str]) -> pd.DataFrame:
    """每行取"同实体、时刻严格早于本行"的最后一条事件的取值（外加 ``__last_time``）。

    ``merge_asof`` 在这个 pandas 版本没有 ``allow_equal`` 参数，所以严格早于靠把右表时间整体
    +1ns 实现：同刻那条落到本行时刻之后、取不到。模块文档第 1 条泄漏纪律就落在这两行里。
    """
    helper = pd.DataFrame({"__row": np.arange(len(left)), key: left[key].to_numpy(),
                           time: pd.to_datetime(left[time])})
    right = events.sort_values(event_time, kind="stable")
    joined = pd.DataFrame({"__join": pd.to_datetime(right[event_time]) + ONE_NS,
                           key: right[key].to_numpy(),
                           "__last_time": pd.to_datetime(right[event_time]).to_numpy()})
    for column in columns:
        joined[column] = right[column].to_numpy()
    assert joined[key].notna().all(), "as-of 关联键出现空值，'上一次'无定义"
    matched = pd.merge_asof(helper.sort_values(time, kind="stable"), joined,
                            left_on=time, right_on="__join", by=key, direction="backward")
    matched = matched.sort_values("__row")
    assert len(matched) == len(left) and (matched["__row"].to_numpy() == np.arange(len(left))).all()
    return matched[columns + ["__last_time"]].set_index(left.index)


def smoothed(counts: np.ndarray, sums: np.ndarray, alpha: float, priors: np.ndarray) -> np.ndarray:
    """``(和 + α·先验) / (数 + α)``。先验逐行取"截至 t 之前的全量经验率"，只看过去。"""
    return (sums + alpha * priors) / (counts + alpha)


def global_causal_rate(y: np.ndarray, times: pd.Series) -> np.ndarray:
    """每行的"截至 t 之前的全量经验失败率"；没有历史则为 0.0。同刻的行互相不可见。"""
    stamps = pd.to_datetime(times).to_numpy(dtype="datetime64[ns]").astype("int64")
    order = np.argsort(stamps, kind="stable")
    uniq, inverse = np.unique(stamps[order], return_inverse=True)
    counts = np.bincount(inverse, minlength=len(uniq)).astype(float)
    sums = np.bincount(inverse, weights=y[order].astype(float), minlength=len(uniq))
    prior_counts = np.concatenate(([0.0], np.cumsum(counts)[:-1]))
    prior_sums = np.concatenate(([0.0], np.cumsum(sums)[:-1]))
    rate = np.where(prior_counts > 0, prior_sums / np.maximum(prior_counts, 1.0), 0.0)
    # rate[b] 已经是"块 0..b-1"的和（exclusive 前缀和），所以直接取 pos，不再 -1：
    # pos = 严格早于本行时刻的唯一时刻个数，本行所在块自己不进去。
    pos = np.searchsorted(uniq, stamps, side="left")
    return rate[pos]


# --------------------------------------------------------------------------- 表装载与结构断言


def load_attempts() -> pd.DataFrame:
    """读 charging_attempts 并跑结构断言：结局与原因/外键的对应关系必须一一闭合。"""
    table = common.load_table("charging_attempts")
    assert table["attempt_id"].is_unique, "attempt_id 不唯一，一行一次尝试的前提不成立"
    assert table["attempted_at"].notna().all()
    known = {"STARTED", "ABANDONED", "FAILED", "RESERVATION_CANCELLED", "RESERVATION_EXPIRED"}
    assert set(table["outcome"].unique()) <= known, \
        f"出现了未登记的结果值 {set(table['outcome'].unique()) - known}"
    started = table["outcome"] == "STARTED"
    failed = table["outcome"] == "FAILED"
    abandoned = table["outcome"] == "ABANDONED"
    reservation = table["outcome"].isin(["RESERVATION_CANCELLED", "RESERVATION_EXPIRED"])
    reason = table["failure_reason"]
    assert started.eq(reason.isna()).all(), "STARTED 必须无失败原因，反之亦然"
    assert started.eq(table["session_id"].notna()).all(), "STARTED 必须有会话，反之亦然"
    assert failed.eq(reason.isin(common.TECHNICAL_FAILURE_REASONS)).all(), "FAILED 应当恰好对应三类技术原因"
    assert abandoned.eq(reason.isin(common.QUEUE_SIDE_FAILURE_REASONS)).all(), \
        "ABANDONED 应当恰好对应三类排队侧原因（第五线的标签）"
    assert reservation.eq(reason.isin(["CANCELLED", "EXPIRED"])).all()
    # 与第五线的切割是数据事实，不是假设：带排队的尝试里技术失败为 0
    linked = table["queue_id"].notna()
    assert int(table.loc[linked, "failure_reason"].isin(common.TECHNICAL_FAILURE_REASONS).sum()) == 0, \
        "带排队的尝试出现技术失败，第五线切割前提变了"
    table = table.copy()
    table["attempted_at"] = pd.to_datetime(table["attempted_at"])
    table["y_tech"] = reason.isin(common.TECHNICAL_FAILURE_REASONS).astype("int8")
    reason_codes = {name: code for code, name in enumerate(common.TECHNICAL_FAILURE_REASONS)}
    table["tech_reason_code"] = table["failure_reason"].map(reason_codes).astype("float64")
    return table


def build_sample(table: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """筛出"已选桩的插枪启动"样本，并把被排除的部分逐项留痕。"""
    failed_no_charger = (table["outcome"] == "FAILED") & table["charger_id"].isna()
    excluded = {
        "queueLinked_line5Sample": int(table["queue_id"].notna().sum()),
        "noAvailableCharger": int((table["failure_reason"] == "NO_AVAILABLE_CHARGER").sum()),
        "reservationCancelledOrExpired": int(table["outcome"].isin(
            ["RESERVATION_CANCELLED", "RESERVATION_EXPIRED"]).sum()),
        "failedWithoutCharger": int(failed_no_charger.sum()),
        "failedWithoutChargerReasons": {str(k): int(v) for k, v in
                                        table.loc[failed_no_charger, "failure_reason"].value_counts().items()},
        "whyFailedWithoutChargerIsOut": "还没选到桩，桩侧特征全不可得；三类原因占比与在桩上的失败几乎同比例",
    }
    keep = (table["queue_id"].isna() & table["charger_id"].notna()
            & table["outcome"].isin(["STARTED", "FAILED"]))
    sample = (table[keep].copy()
              .sort_values(["attempted_at", "attempt_id"], kind="stable").reset_index(drop=True))
    assert sample["charger_id"].notna().all()
    assert int(sample["y_tech"].sum()) == int((sample["outcome"] == "FAILED").sum())
    scope = {"totalAttempts": int(len(table)), "excluded": excluded,
             "sampleRows": int(len(sample)), "positives": int(sample["y_tech"].sum()),
             "baseRate": round(float(sample["y_tech"].mean()), 5),
             "chargers": int(sample["charger_id"].nunique()),
             "stations": int(sample["station_id"].nunique()),
             "users": int(sample["user_id"].nunique()),
             "attemptedAtRange": [str(sample["attempted_at"].min()), str(sample["attempted_at"].max())],
             "reasonMix": {str(k): int(v) for k, v in
                           sample.loc[sample["y_tech"] == 1, "failure_reason"].value_counts().items()}}
    return sample, scope


# --------------------------------------------------------------------------- 静态与时令特征


def attach_statics(sample: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """桩 / 站 / 车 / 人 / 预约：都是"尝试发生前就固定或已计划"的量，天然无泄漏。"""
    chargers = common.load_table("chargers")
    stations = common.load_table("stations", columns=["station_id", "city_id", "site_type", "opened_at"])
    vehicles = common.load_table("vehicles", columns=["vehicle_id", "user_id", "vehicle_class",
                                                     "max_charge_kw"])
    users = common.load_table("users")
    reservations = common.load_table("reservations",
                                     columns=["reservation_id", "created_at", "expires_at"])
    notes: dict = {}

    pair = sample[["user_id", "vehicle_id"]].drop_duplicates()
    notes["userVehicleOneToOne"] = bool(pair["user_id"].nunique() == len(pair)
                                        and pair["vehicle_id"].nunique() == len(pair))
    notes["droppedVehicleBattery"] = ("battery_capacity_kwh 在车型内是常数，与 max_charge_kw 共线，"
                                      "只留后者；user_id 与 vehicle_id 一一对应，车辆侧历史与用户侧"
                                      "历史是同一份信息，不做两遍")

    frame = (sample
             .merge(chargers[["charger_id", "connector_type", "rated_power_kw", "manufacturer",
                              "model", "commissioned_at"]], on="charger_id", how="left",
                    validate="many_to_one")
             .rename(columns={"model": "charger_model"})
             .merge(stations, on="station_id", how="left", validate="many_to_one")
             # vehicles 表自带 user_id，与样本的 user_id 同名会互相加后缀；车辆侧只取属性列
             .merge(vehicles[["vehicle_id", "vehicle_class", "max_charge_kw"]],
                    on="vehicle_id", how="left", validate="many_to_one")
             .merge(users[["user_id", "segment", "membership", "registered_at", "home_city_id"]],
                    on="user_id", how="left", validate="many_to_one"))
    coverage = {c: round(float(frame[c].notna().mean()), 4)
                for c in ("connector_type", "charger_model", "city_id", "vehicle_class", "segment",
                          "rated_power_kw", "max_charge_kw", "commissioned_at")}
    assert coverage["connector_type"] > 0.99 and coverage["city_id"] > 0.99, coverage
    assert (frame["max_charge_kw"] > 0).all(), "存在额定功率为 0 的车辆，功率比无定义"
    attempted = pd.to_datetime(frame["attempted_at"])
    for name in ("commissioned_at", "opened_at", "registered_at"):
        frame[name] = pd.to_datetime(frame[name])
    frame["charger_age_days"] = (attempted - frame["commissioned_at"]).dt.total_seconds() / 86400
    frame["station_age_days"] = (attempted - frame["opened_at"]).dt.total_seconds() / 86400
    frame["user_age_days"] = (attempted - frame["registered_at"]).dt.total_seconds() / 86400
    frame["power_gap_kw"] = frame["rated_power_kw"] - frame["max_charge_kw"]
    frame["power_ratio"] = frame["rated_power_kw"] / frame["max_charge_kw"]
    frame["has_reservation"] = frame["reservation_id"].notna().astype("int8")
    frame["home_city_matches_station"] = (frame["home_city_id"] == frame["city_id"]).astype("int8")
    notes["staticCoverage"] = coverage

    frame = frame.merge(reservations, on="reservation_id", how="left", validate="many_to_one")
    lead = (attempted - pd.to_datetime(frame["created_at"])).dt.total_seconds() / 60.0
    to_expiry = (pd.to_datetime(frame["expires_at"]) - attempted).dt.total_seconds() / 60.0
    present = frame["reservation_id"].notna()
    assert (lead[present] >= -1e-9).all(), "存在 created_at 晚于 attempted_at 的预约，时点口径不成立"
    assert (to_expiry[present] >= -1e-9).all(), "存在已过期仍被尝试的预约，与预约语义不符"
    frame["reservation_lead_min"] = lead
    frame["minutes_to_reservation_expiry"] = to_expiry
    notes["reservationCoverage"] = round(float(present.mean()), 4)
    return frame, notes


def attach_event_time(frame: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """本地时刻、日历、公示电价、天气。

    这些与"结果"无关、且运营时点本来就能拿到（日历是计划、电价是公示、天气是当时观测），
    所以按**含同刻或更早**关联；"严格早于"只针对标签衍生特征与桩态特征——这是本线刻意的分级，
    不是手松：天气/电价里没有这台桩会不会失败的答案。
    """
    frame = frame.copy()
    n = len(frame)
    frame["attempted_local"] = pd.to_datetime(frame["attempted_at"]) + pd.Timedelta(
        hours=common.BUSINESS_OFFSET_HOURS)
    frame["hour_local"] = frame["attempted_local"].dt.hour.astype("int16")
    frame["dow_local"] = frame["attempted_local"].dt.dayofweek.astype("int16")
    frame["business_date"] = frame["attempted_local"].dt.normalize()
    notes: dict = {}

    calendar = common.load_table("calendar")
    calendar["business_date"] = pd.to_datetime(calendar["business_date"])
    frame = frame.merge(calendar[["city_id", "business_date", "is_weekend", "scenario_event",
                                  "demand_multiplier"]], on=["city_id", "business_date"],
                        how="left", validate="many_to_one")
    assert len(frame) == n and frame["demand_multiplier"].notna().all()

    tariffs = common.load_table("tariffs")
    frame = frame.merge(tariffs[["city_id", "hour", "energy_price_cents_per_kwh",
                                 "service_price_cents_per_kwh", "grid_price_cents_per_kwh",
                                 "period"]],
                        left_on=["city_id", "hour_local"], right_on=["city_id", "hour"],
                        how="left", validate="many_to_one")
    assert len(frame) == n and frame["period"].notna().all()
    frame = frame.drop(columns=["hour"])

    weather = common.load_table("weather_hourly", columns=["city_id", "recorded_at", "temperature_c",
                                                           "humidity_pct", "weather", "rainfall_mm"])
    weather["recorded_at"] = pd.to_datetime(weather["recorded_at"])
    matched = strict_last(frame, "attempted_at", "city_id", weather, "recorded_at",
                          ["temperature_c", "humidity_pct", "weather", "rainfall_mm"])
    for column in ("temperature_c", "humidity_pct", "weather", "rainfall_mm"):
        frame[column] = matched[column].to_numpy()
    notes["weatherLookback"] = {
        "rule": "天气取严格早于尝试时刻的最近一条观测（小时级），不假设有预报可用",
        "ageMinP50": round(float((pd.to_datetime(frame["attempted_at"])
                                  - pd.to_datetime(matched["__last_time"])).dt.total_seconds()
                                 .median() / 60.0), 3),
        "coverage": round(float(frame["weather"].notna().mean()), 4)}
    return frame, notes


# --------------------------------------------------------------------------- as-of 特征


def query_frame(frame: pd.DataFrame, key: str) -> pd.DataFrame:
    """给 ``AsOfCounts`` 用的最小查询表：实体键 + 查询时刻，避免整表反复物化。"""
    return pd.DataFrame({"__query_time": pd.to_datetime(frame["attempted_at"]),
                         key: frame[key].to_numpy()})


def attach_attempt_history(frame: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """桩 / 型号 / 厂商 / 站 / 人 的"严格早于本次"失败率、近期集中度与重试上下文。"""
    y = frame["y_tech"].to_numpy(dtype=float)
    priors = global_causal_rate(y, frame["attempted_at"])
    notes = {"shrinkageTarget": {
        "rule": "先验 = 截至该行时刻之前的全量经验失败率（只看过去，无历史取 0.0）",
        "min": round(float(priors.min()), 5), "max": round(float(priors.max()), 5),
        "lastRow": round(float(priors[-1]), 5)}}
    helper = frame.assign(_one=y)
    specs = (("charger", "charger_id", ALPHA_CHARGER), ("model", "charger_model", ALPHA_MODEL),
             ("manufacturer", "manufacturer", ALPHA_MODEL), ("station", "station_id", ALPHA_STATION),
             ("user", "user_id", ALPHA_USER))
    for name, key, alpha in specs:
        query = query_frame(frame, key)
        engine = AsOfCounts(helper, key=key, stamps="attempted_at", value="_one")
        counts, sums = engine.cumulative(query, key, "_one")
        frame[f"{name}_n_prior"] = counts
        frame[f"{name}_fail_rate_prior"] = smoothed(counts, sums, alpha, priors)
        week_counts, week_fails = engine.window(query, key, 7.0, "_one")
        month_counts, month_fails = engine.window(query, key, 30.0, "_one")
        frame[f"{name}_fails_30d"] = month_fails
        frame[f"{name}_n_30d"] = month_counts
        short = smoothed(week_counts, week_fails, ALPHA_RECENT, priors)
        frame[f"{name}_fail_rate_7d"] = short
        frame[f"{name}_rate_delta_7d_minus_prior"] = short - frame[f"{name}_fail_rate_prior"]

    # "最近不对劲"三件套：紧邻上一次成没成、距上次失败多久、距上次被尝试多久
    assert not frame.duplicated(subset=["charger_id", "attempted_at"]).any(), \
        "同一台桩在同一刻出现两次尝试，「上一次」的定义不唯一"
    self_events = frame[["charger_id", "user_id", "attempted_at", "y_tech"]]
    charger_last = strict_last(frame, "attempted_at", "charger_id", self_events, "attempted_at", ["y_tech"])
    frame["charger_prev_attempt_failed"] = charger_last["y_tech"].to_numpy(dtype=float)
    user_last = strict_last(frame, "attempted_at", "user_id", self_events, "attempted_at", ["y_tech"])
    frame["user_prev_attempt_failed"] = user_last["y_tech"].to_numpy(dtype=float)

    # 距今量只在"有过一次"时才有意义：__last_time 为 NaT 时 astype(int64) 得到负数，落到 NaN
    attempted_ns = pd.to_datetime(frame["attempted_at"]).to_numpy(dtype="datetime64[ns]").astype("int64")

    def gap(name: str, last_time: pd.Series, scale: float) -> None:
        last_ns = pd.to_datetime(last_time.to_numpy()).astype("int64")
        frame[name] = np.where(last_ns > 0, (attempted_ns - last_ns) / scale, np.nan)

    gap("charger_min_since_last_attempt",
        strict_last(frame, "attempted_at", "charger_id", self_events, "attempted_at",
                    [])["__last_time"], 60e9)
    gap("user_min_since_last_attempt", user_last["__last_time"], 60e9)
    failures = frame.loc[frame["y_tech"] == 1, ["charger_id", "attempted_at"]]
    gap("charger_days_since_last_fail",
        strict_last(frame, "attempted_at", "charger_id", failures, "attempted_at",
                    [])["__last_time"], 86_400e9)
    return frame, notes


def attach_maintenance(frame: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """维保工单的 as-of 状态：这台桩/这个站报修过几次、当时是否在修、修了多久。

    只用 ``reported_at`` / ``restored_at`` / ``fault_type``，绝不读 ``status``
    （全表终态——"现在已解决"不等于"尝试当时已解决"，那是泄答案），也不用
    ``accepted_at`` / ``work_started_at``（调度侧时间戳，真实系统里的可得性还要与运营确认），
    也不读工单成本（钱是修完才知道的量，对"这次会不会失败"没有前置信息）。
    """
    tickets = common.load_table("maintenance_tickets",
                                columns=["charger_id", "station_id", "reported_at", "restored_at",
                                         "fault_type"])
    for column in ("reported_at", "restored_at"):
        tickets[column] = pd.to_datetime(tickets[column])
    helper = tickets.assign(_one=1.0)
    completed = tickets[tickets["restored_at"].notna()].assign(
        _one=1.0,
        _hours=(tickets.loc[tickets["restored_at"].notna(), "restored_at"]
                - tickets.loc[tickets["restored_at"].notna(), "reported_at"]).dt.total_seconds() / 3600.0)
    query = query_frame(frame, "charger_id")
    station_query = query_frame(frame, "station_id")
    notes = {"tickets": int(len(tickets)),
             "chargersWithTickets": int(tickets["charger_id"].nunique()),
             "ticketsPerCharger": round(float(len(tickets) / tickets["charger_id"].nunique()), 2),
             "faultTypes": {str(k): int(v) for k, v in tickets["fault_type"].value_counts().items()},
             "unclosedTickets": int(tickets["restored_at"].isna().sum())}

    reported = AsOfCounts(helper, key="charger_id", stamps="reported_at", value="_one")
    counts, _ = reported.cumulative(query, "charger_id", "_one")
    frame["charger_tickets_total_prior"] = counts
    frame["charger_tickets_30d"] = reported.window(query, "charger_id", 30.0, "_one")[0]
    frame["charger_tickets_90d"] = reported.window(query, "charger_id", 90.0, "_one")[0]
    sensitive = helper[helper["fault_type"].isin(["COMMUNICATION", "CONNECTOR"])]
    frame["charger_connector_tickets_30d"] = AsOfCounts(
        sensitive, key="charger_id", stamps="reported_at", value="_one").window(
        query, "charger_id", 30.0, "_one")[0]

    # 当时是否仍在修 = 报修数(严格早于 t) − 已恢复数(恢复时刻严格早于 t)；同刻按"还没恢复"算
    restored_counts, _ = AsOfCounts(completed, key="charger_id", stamps="restored_at",
                                   value="_one").cumulative(query, "charger_id", "_one")
    frame["charger_ticket_open_now"] = np.maximum(counts - restored_counts, 0.0)
    downtime = AsOfCounts(completed, key="charger_id", stamps="restored_at", value="_hours")
    frame["charger_downtime_hours_90d"] = downtime.window(query, "charger_id", 90.0, "_hours")[1]
    frame["charger_downtime_hours_prior"] = downtime.cumulative(query, "charger_id", "_hours")[1]
    last_ticket_ns = strict_last(frame, "attempted_at", "charger_id",
                                tickets[["charger_id", "reported_at"]], "reported_at",
                                [])["__last_time"].astype("int64").to_numpy()
    attempted_ns = pd.to_datetime(frame["attempted_at"]).to_numpy(dtype="datetime64[ns]").astype("int64")
    frame["charger_days_since_last_ticket"] = np.where(last_ticket_ns > 0,
                                                       (attempted_ns - last_ticket_ns) / 86_400e9, np.nan)

    station_engine = AsOfCounts(helper, key="station_id", stamps="reported_at", value="_one")
    frame["station_tickets_30d"] = station_engine.window(station_query, "station_id", 30.0, "_one")[0]
    frame["station_tickets_total_prior"] = station_engine.cumulative(station_query, "station_id", "_one")[0]
    return frame, notes


def attach_telemetry(frame: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """遥测 as-of：**严格早于** attempted_at 的最后一条记录，以及近窗口的异常态行数。

    这条线上最危险的坑就在这儿：模拟器在 attempted_at 当刻把桩翻成 CHARGING，含同刻关联会
    造出"上一条状态=AVAILABLE ⇒ 必失败"的 100% 假特征（正类 3,676/3,676 命中）。做法见
    ``strict_last``（右表时间整体 +1ns）。修完之后遥测基本没有独立信息，这一点如实报告。
    """
    telemetry = common.load_table("charger_telemetry",
                                  columns=["charger_id", "recorded_at", "state", "online", "power_kw"])
    telemetry["recorded_at"] = pd.to_datetime(telemetry["recorded_at"])
    telemetry["state_code"] = telemetry["state"].map(STATE_CODES)
    assert telemetry["state_code"].notna().all(), "遥测出现了未登记的状态值"
    query = query_frame(frame, "charger_id")
    notes = {"rows": int(len(telemetry)),
             "stateMix": {str(k): int(v) for k, v in telemetry["state"].value_counts().items()}}

    last = strict_last(frame, "attempted_at", "charger_id", telemetry, "recorded_at",
                       ["state_code", "online", "power_kw"])
    frame["tel_last_state_code"] = last["state_code"].to_numpy(dtype=float)
    frame["tel_last_online"] = last["online"].to_numpy(dtype=float)
    frame["tel_last_power_kw"] = last["power_kw"].to_numpy(dtype=float)
    frame["tel_age_min"] = (pd.to_datetime(frame["attempted_at"])
                            - pd.to_datetime(last["__last_time"])).dt.total_seconds() / 60.0
    notes["lastStateBeforeAttempt"] = {TELEMETRY_STATES[int(code)]: int(rows) for code, rows in
                                       frame["tel_last_state_code"].value_counts().items()}
    notes["lastStateFailureRate"] = {TELEMETRY_STATES[int(code)]: round(float(rate), 5) for (code, rate)
                                     in frame.groupby("tel_last_state_code")["y_tech"].mean().items()}
    notes["ageMin"] = {"p50": round(float(frame["tel_age_min"].median()), 3),
                       "max": round(float(frame["tel_age_min"].max()), 3),
                       "missing": int(frame["tel_age_min"].isna().sum())}

    for state, name, days in (("OFFLINE", "tel_offline_rows_24h", 1.0),
                              ("MAINTENANCE", "tel_maint_rows_24h", 1.0),
                              ("CHARGING", "tel_charging_rows_6h", 0.25)):
        subset = telemetry.loc[telemetry["state"] == state].assign(_one=1.0)
        frame[name] = AsOfCounts(subset, key="charger_id", stamps="recorded_at",
                                value="_one").window(query, "charger_id", days, "_one")[0]
    frame["tel_abnormal_rows_24h"] = frame["tel_offline_rows_24h"] + frame["tel_maint_rows_24h"]
    return frame, notes


# --------------------------------------------------------------------------- 泄漏审计


AUDITED_FEATURES = ("charger_n_prior", "charger_fail_rate_prior", "charger_fails_30d",
                    "charger_prev_attempt_failed", "charger_days_since_last_fail",
                    "charger_tickets_total_prior", "charger_ticket_open_now",
                    "tel_last_state_code", "tel_offline_rows_24h")


def verify_no_future_leak(frame: pd.DataFrame, sample: int = 250, seed: int = common.SEED) -> dict:
    """随机抽若干行，用**全表布尔掩码暴力重算** ``AUDITED_FEATURES``，要求与向量版逐条 0 偏差。

    审计刻意走另一条代码路径（不排序、不二分、不 merge_asof、不复用 ``AsOfCounts``）：
    同一段逻辑自证不出错没有意义。这九项覆盖三类最可能出错的量——尝试历史（计数/窗口/上一次
    取值/距今）、工单（报修数/在修数）、遥测（最后状态/窗口异常行数）。
    """
    rng = np.random.default_rng(seed)
    rows = np.sort(rng.choice(len(frame), size=min(sample, len(frame)), replace=False))
    times = pd.to_datetime(frame["attempted_at"])
    y = frame["y_tech"].to_numpy(dtype=float)
    stamps_ns = times.to_numpy(dtype="datetime64[ns]").astype("int64")
    chargers = frame["charger_id"].to_numpy()
    history = pd.DataFrame({"charger_id": chargers, "attempted_at": times, "y_tech": y})

    tickets = common.load_table("maintenance_tickets", columns=["charger_id", "reported_at", "restored_at"])
    for column in ("reported_at", "restored_at"):
        tickets[column] = pd.to_datetime(tickets[column])
    telemetry = common.load_table("charger_telemetry", columns=["charger_id", "recorded_at", "state"])
    telemetry["recorded_at"] = pd.to_datetime(telemetry["recorded_at"])
    history_by = {key: part for key, part in history.groupby("charger_id", observed=True)}
    tickets_by = {key: part for key, part in tickets.groupby("charger_id", observed=True)}
    telemetry_by = {key: part for key, part in telemetry.groupby("charger_id", observed=True)}

    worst = dict.fromkeys(AUDITED_FEATURES, 0.0)
    for index in rows:
        position = int(index)
        stamp = times.iloc[position]
        charger = chargers[position]
        past = history_by[charger]
        earlier = past[past["attempted_at"] < stamp]
        n_prior = float(len(earlier))
        sums = float(earlier["y_tech"].sum())
        recent = earlier[earlier["attempted_at"] >= stamp - pd.Timedelta(days=30)]
        failed_rows = earlier[earlier["y_tech"] == 1]
        global_past = y[stamps_ns < stamp.value]
        prior = float(global_past.mean()) if len(global_past) else 0.0
        truth = {
            "charger_n_prior": n_prior,
            "charger_fail_rate_prior": (sums + ALPHA_CHARGER * prior) / (n_prior + ALPHA_CHARGER),
            "charger_fails_30d": float(recent["y_tech"].sum()),
            "charger_prev_attempt_failed": (float(earlier.sort_values("attempted_at")["y_tech"].iloc[-1])
                                           if len(earlier) else np.nan),
            "charger_days_since_last_fail": (float((stamp - failed_rows["attempted_at"].max())
                                                  .total_seconds() / 86400)
                                             if len(failed_rows) else np.nan),
        }
        here = tickets_by.get(charger)
        if here is None:
            truth["charger_tickets_total_prior"] = 0.0
            truth["charger_ticket_open_now"] = 0.0
        else:
            reported_before = float((here["reported_at"] < stamp).sum())
            done = int(((here["restored_at"].notna()) & (here["restored_at"] < stamp)).sum())
            truth["charger_tickets_total_prior"] = reported_before
            truth["charger_ticket_open_now"] = max(reported_before - done, 0.0)
        here_tel = telemetry_by.get(charger)
        before = None if here_tel is None else here_tel[here_tel["recorded_at"] < stamp]
        if before is None or not len(before):
            truth["tel_last_state_code"] = np.nan
            truth["tel_offline_rows_24h"] = 0.0
        else:
            newest = before["recorded_at"].max()
            truth["tel_last_state_code"] = float(
                STATE_CODES[before.loc[before["recorded_at"] == newest, "state"].iloc[-1]])
            window = before[before["recorded_at"] >= stamp - pd.Timedelta(hours=24)]
            truth["tel_offline_rows_24h"] = float((window["state"] == "OFFLINE").sum())
        for name, expected in truth.items():
            got = float(frame.iloc[position][name])
            if pd.isna(expected) and pd.isna(got):
                continue
            worst[name] = max(worst[name], abs(got - float(expected)))
    audit = {"sampled": int(len(rows)), "checked": list(AUDITED_FEATURES),
             "maxAbsDiff": {k: float(v) for k, v in worst.items()},
             "method": "全表布尔掩码独立重算（与向量路径不共用排序/二分/merge_asof 代码）"}
    offenders = {k: v for k, v in audit["maxAbsDiff"].items() if v > 1e-9}
    if offenders:
        raise AssertionError(f"as-of 特征与暴力重算不一致（未来信息泄漏）：{offenders}")
    return audit


# --------------------------------------------------------------------------- 特征名单


def pick_features(frame: pd.DataFrame) -> dict[str, list[str]]:
    """选特征：数值 / 类别两份名单。零方差列剔除并留痕，禁入列一旦出现在名单里即报错。"""
    banned = set(common.NON_FEATURE_COLUMNS) | FORBIDDEN_FEATURES | IGNORED_COLUMNS
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
    return {"numeric": numeric, "categorical": categorical, "droppedConstant": constants}


# --------------------------------------------------------------------------- 主流程


def build() -> tuple[pd.DataFrame, dict]:
    """全流程但不落盘：``rolling.py`` 靠它在内存里重建同一张表。"""
    manifest = common.verify_batch()
    bounds = common.split_boundaries(manifest)
    sample, scope = build_sample(load_attempts())
    frame, static_notes = attach_statics(sample)
    frame, event_notes = attach_event_time(frame)
    frame, history_notes = attach_attempt_history(frame)
    frame, maintenance_notes = attach_maintenance(frame)
    frame, telemetry_notes = attach_telemetry(frame)
    frame = frame.sort_values(["attempted_at", "attempt_id"], kind="stable").reset_index(drop=True)
    frame["split"] = common.assign_split(frame["attempted_at"], bounds)
    features = pick_features(frame)
    notes = {"statics": static_notes, "eventTime": event_notes, "history": history_notes,
             "maintenance": maintenance_notes, "telemetry": telemetry_notes}
    return frame, {"scope": scope, "bounds": bounds, "features": features, "notes": notes,
                   "manifest": manifest}


def main() -> dict:
    common.require_empty_run_dir()
    frame, built = build()
    scope, features, notes = built["scope"], built["features"], built["notes"]
    audit = verify_no_future_leak(frame)

    common.OUT_DIR.mkdir(parents=True, exist_ok=True)
    common.write_new_pickle(common.MATRIX_PATH, frame)
    matrix_sha = common.sha256_file(common.MATRIX_PATH)
    summary = {
        "publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "pipelineRunId": built["manifest"].get("pipelineRunId"),
        "matrixSha256": matrix_sha,
        "datasetId": common.DATASET_ID,
        "modelId": common.MODEL_ID,
        "label": "y_tech = failure_reason ∈ CONNECTOR_HANDSHAKE / APP_TIMEOUT / AUTH_FAILED",
        "scope": scope,
        "splits": {name: int((frame["split"] == name).sum())
                   for name in ("TRAIN", "VALIDATION", "TEST", "EXCLUDED")},
        "splitBaseRate": {name: round(float(frame.loc[frame["split"] == name, "y_tech"].mean()), 5)
                          for name in ("TRAIN", "VALIDATION", "TEST")},
        "boundaries": {k: str(v) for k, v in built["bounds"].items()},
        "features": features,
        "featureCount": {"numeric": len(features["numeric"]), "categorical": len(features["categorical"])},
        "constantWhy": {name: "样本域内取值唯一（非缺失），无信息量" for name in features["droppedConstant"]},
        "leakAudit": audit,
        "notes": notes,
        "matrixRows": int(len(frame)),
        "disclaimer": common.simulated_note(),
    }
    common.write_new_json(common.BUILD_SUMMARY_PATH, summary)
    print(f"[attempt] 样本 {len(frame):,} 行 / 正类 {scope['positives']:,} "
          f"({scope['baseRate']:.4f})，桩 {scope['chargers']} 台、站 {scope['stations']} 个")
    print(f"[attempt] 切分 TRAIN={summary['splits']['TRAIN']:,} VAL={summary['splits']['VALIDATION']:,} "
          f"TEST={summary['splits']['TEST']:,} EXCLUDED={summary['splits']['EXCLUDED']:,}")
    print(f"[attempt] 基础率 TRAIN={summary['splitBaseRate']['TRAIN']} "
          f"VAL={summary['splitBaseRate']['VALIDATION']} TEST={summary['splitBaseRate']['TEST']}")
    print(f"[attempt] 特征 {len(features['numeric'])} 数值 + {len(features['categorical'])} 类别，"
          f"零方差剔除 {len(features['droppedConstant'])} 个：{features['droppedConstant']}")
    print(f"[attempt] 泄漏审计 抽样 {audit['sampled']} 行，"
          f"maxAbsDiff = {max(audit['maxAbsDiff'].values()):.1e}")
    print(f"[attempt] -> {common.MATRIX_PATH}")
    return summary


if __name__ == "__main__":
    main()
