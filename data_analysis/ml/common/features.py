"""Feature derivation shared by offline training and online inference.

The exported ``ml_features_hourly`` columns are produced by ``spark_jobs/ml_features.py``
from the hour *ending* at ``reference_time``.  :func:`derive_features` reproduces that rule
for a list of 24 station-hour rows, so a predictor that only sees raw hourly history builds
the same numbers the model was trained on (asserted by ``ml/tests/test_ml_contract.py``).

Extra columns beyond the export stay inside the same 24-hour window: free-charger counts
observed at ``t-1h ... t-24h`` plus their rolling mean.  Those are past observations, which
the contracts allow, and they are what makes availability forecasting work at all - the
export only carries ``last_available_count``.  Anything further back (a 168-hour weekly
input) cannot be a model feature because ``PredictionContext`` hands the predictor 24 rows.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo

import numpy as np
import pandas as pd

HISTORY_HOURS = 24
BUSINESS_ZONE = ZoneInfo("Asia/Shanghai")
LAG_HOURS = tuple(range(1, HISTORY_HOURS + 1))

POWER_LAGS = tuple(f"lag_power_kw_h{k:02d}" for k in LAG_HOURS)
AVAILABLE_LAGS = tuple(f"lag_available_h{k:02d}" for k in LAG_HOURS)
EXPORT_ROLLINGS = (
    "rolling_mean_kw_3h",
    "rolling_mean_kw_6h",
    "rolling_mean_kw_24h",
    "rolling_std_kw_24h",
    "rolling_max_kw_24h",
)
DERIVED_COLUMNS = AVAILABLE_LAGS + ("rolling_mean_available_24h",)
CALENDAR_COLUMNS = ("hour_of_day", "day_of_week", "is_weekend", "is_public_holiday", "is_adjusted_workday")
STATIC_COLUMNS = ("capacity", "rated_capacity_kw")


class HistoryError(ValueError):
    """The supplied history cannot produce a contract-complete feature row."""

    code = "HISTORY_TOO_SHORT"


def as_utc(value: datetime | str | pd.Timestamp) -> datetime:
    """Strict UTC parse that also accepts pandas timestamps."""
    if isinstance(value, pd.Timestamp):
        if value.tzinfo is None:
            raise HistoryError("timestamps must be timezone-aware")
        return value.to_pydatetime().astimezone(timezone.utc)
    if isinstance(value, str):
        text = value.strip()
        if not text.endswith("Z"):
            raise HistoryError(f"timestamp {value!r} is not UTC Zulu")
        value = datetime.fromisoformat(text.replace("Z", "+00:00"))
    if value.tzinfo is None:
        raise HistoryError("timestamps must be timezone-aware")
    return value.astimezone(timezone.utc)


def valid_hour_mask(hourly: pd.DataFrame) -> pd.Series:
    """Vectorised mirror of ``_valid_hour`` in ``spark_jobs/ml_features.py``."""
    power = pd.to_numeric(hourly["mean_power_kw"], errors="coerce")
    available = pd.to_numeric(hourly["end_available_count"], errors="coerce")
    capacity = pd.to_numeric(hourly["capacity"], errors="coerce")
    return (
        hourly["is_complete"].astype("boolean").fillna(False).astype(bool)
        & power.notna() & np.isfinite(power) & (power >= 0)
        & capacity.notna() & (capacity > 0)
        & available.notna() & (available >= 0) & (available <= capacity)
    )


def _valid_row(row: dict) -> bool:
    return bool(valid_hour_mask(pd.DataFrame([row])).iloc[0])


def derive_features(
    history: list[dict],
    *,
    station: dict,
    reference_time: datetime | str,
    calendar: dict[tuple[str, str], tuple[bool, bool]],
    cities: list[str],
) -> dict:
    """Build one model input row from 24 complete station hours strictly before ``reference_time``.

    ``history`` rows are snake_case ``station_hourly_metrics`` records; ``recorded_at`` labels
    the START of the interval, so the newest row must cover ``[reference_time - 1h, reference_time)``.
    """
    start = as_utc(reference_time)
    if (start.minute, start.second, start.microsecond) != (0, 0, 0):
        raise HistoryError("reference_time must be an exact hour boundary")
    if len(history) != HISTORY_HOURS:
        raise HistoryError(f"expected {HISTORY_HOURS} history rows, received {len(history)}")

    rows = sorted(history, key=lambda row: as_utc(row["recorded_at"]))
    window_start = start - timedelta(hours=HISTORY_HOURS)
    for offset, row in enumerate(rows):
        stamped = as_utc(row["recorded_at"])
        expected = window_start + timedelta(hours=offset)
        if stamped != expected:
            raise HistoryError(
                f"history is not contiguous: row {offset} starts {stamped.isoformat()}, expected {expected.isoformat()}"
            )
        if not _valid_row(row):
            raise HistoryError(f"history hour {stamped.isoformat()} is not complete and valid")

    powers = np.array([float(row["mean_power_kw"]) for row in rows], dtype=float)
    available = np.array([float(row["end_available_count"]) for row in rows], dtype=float)

    features: dict = {}
    for lag in LAG_HOURS:
        # lag 1 is the hour ending at reference_time, which is the newest row.
        features[f"lag_power_kw_h{lag:02d}"] = float(powers[-lag])
        features[f"lag_available_h{lag:02d}"] = float(available[-lag])
    features["rolling_mean_kw_3h"] = float(powers[-3:].mean())
    features["rolling_mean_kw_6h"] = float(powers[-6:].mean())
    features["rolling_mean_kw_24h"] = float(powers.mean())
    features["rolling_std_kw_24h"] = float(powers.std(ddof=0))  # population std, as Spark's stddev_pop
    features["rolling_max_kw_24h"] = float(powers.max())
    features["rolling_mean_available_24h"] = float(available.mean())
    features["last_available_count"] = float(available[-1])

    local = start.astimezone(BUSINESS_ZONE)
    features["hour_of_day"] = local.hour
    features["day_of_week"] = local.weekday()  # Monday=0, matching pmod(dayofweek+5,7)
    features["is_weekend"] = local.weekday() >= 5
    features["business_date"] = local.date().isoformat()
    city_id = str(station.get("city_id") or "")
    holiday, workday = calendar.get((city_id, features["business_date"]), (False, False))
    features["is_public_holiday"] = bool(holiday)
    features["is_adjusted_workday"] = bool(workday)

    features["capacity"] = int(station["capacity"])
    rated = station.get("rated_capacity_kw")
    features["rated_capacity_kw"] = None if rated is None else float(rated)
    for city in cities:
        features[f"city_{city.lower()}"] = int(city_id == city)
    return features


def add_window_features(frame: pd.DataFrame, hourly: pd.DataFrame) -> pd.DataFrame:
    """Attach the free-charger lag block and its rolling mean to a merged features/targets frame."""
    usable = hourly.loc[valid_hour_mask(hourly), ["station_id", "recorded_at", "end_available_count"]]
    usable = usable.assign(end_available_count=usable["end_available_count"].astype(float))
    result = frame
    for lag in LAG_HOURS:
        column = f"lag_available_h{lag:02d}"
        keyed = usable.assign(reference_time=usable["recorded_at"] + pd.Timedelta(hours=lag))
        result = result.merge(keyed[["station_id", "reference_time", "end_available_count"]].rename(
            columns={"end_available_count": column}), on=["station_id", "reference_time"], how="left")
    result["rolling_mean_available_24h"] = result[list(AVAILABLE_LAGS)].mean(axis=1, skipna=False)
    missing = int(result[list(AVAILABLE_LAGS)].isna().any(axis=1).sum())
    if missing:
        raise ValueError(
            f"{missing} prediction points lost history hours when rebuilding free-charger lags; "
            "the hourly table and the feature table do not agree on this batch"
        )
    return result


def calendar_lookup(frame: pd.DataFrame) -> dict[tuple[str, str], tuple[bool, bool]]:
    """Freeze the calendar flags the exported features already carry."""
    subset = frame[["city_id", "business_date", "is_public_holiday", "is_adjusted_workday"]].dropna(
        subset=["city_id", "business_date"]).drop_duplicates()
    return {
        (str(city), _date_text(date)): (bool(holiday), bool(workday))
        for city, date, holiday, workday in subset.itertuples(index=False)
    }


def city_list(frame: pd.DataFrame) -> list[str]:
    return sorted(str(city) for city in frame["city_id"].dropna().unique())


def one_hot_columns(cities: list[str]) -> list[str]:
    return [f"city_{city.lower()}" for city in cities]


def _date_text(value) -> str:
    if isinstance(value, (pd.Timestamp, datetime)):
        return value.date().isoformat()
    return str(value)[:10]
