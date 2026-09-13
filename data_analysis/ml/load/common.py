"""Shared loading and feature construction for the load forecasting task.

Conventions verified against ``analytics_full_180d_v1`` (see prepare_data.py):
- ``rolling_std_kw_24h`` is the population standard deviation (ddof=0).
- ``hour_of_day`` / ``day_of_week`` / ``is_weekend`` follow Asia/Shanghai,
  while ``reference_time`` and table timestamps stay in UTC.
- Online inference rebuilds every model input from the 24 raw station hours
  through :func:`build_feature_row`, and :func:`audit_offline_parity` proves
  that reconstruction matches the exported feature table before training.
"""

from __future__ import annotations

import glob
import hashlib
import json
import pickle
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np
import pandas as pd

DATA_ANALYSIS_ROOT = Path(__file__).resolve().parents[2]
DATASET_DIR = DATA_ANALYSIS_ROOT / "datasets" / "analytics_full_180d_v1"
BUSINESS_TZ_OFFSET = timedelta(hours=8)

LAG_FEATURES = [f"lag_power_kw_h{i:02d}" for i in range(1, 25)]
ROLLING_FEATURES = [
    "rolling_mean_kw_3h",
    "rolling_mean_kw_6h",
    "rolling_mean_kw_24h",
    "rolling_std_kw_24h",
    "rolling_max_kw_24h",
]
CALENDAR_FEATURES = [
    "hour_of_day",
    "day_of_week",
    "is_weekend",
    "is_public_holiday",
    "is_adjusted_workday",
]
STATION_FEATURES = ["capacity", "rated_capacity_kw", "last_available_count"]
CATEGORICAL_FEATURES = ["city_id", "station_id"]
NUMERIC_FEATURES = CALENDAR_FEATURES + STATION_FEATURES + LAG_FEATURES + ROLLING_FEATURES
FEATURE_COLUMNS = CATEGORICAL_FEATURES + NUMERIC_FEATURES
TARGET_COLUMNS = [f"label_power_kw_h{i:02d}" for i in range(1, 25)]
HORIZON_SPLITS = {1: "split_1h", 6: "split_6h", 24: "split_24h"}

# Shipped recipe: per-horizon HistGradientBoostingRegressor. v0.2 was the
# L2-loss race winner "hgb-deep"; v0.4 promotes the round-2 finding that a
# quantile-0.5 (median) objective directly optimizes the MAE metric the
# contract reports, beating hgb-deep on VALIDATION (12.942 vs 13.294 kW mean).
MODEL_ID = "hgb-q50-history24-v1"
MODEL_VERSION = "0.4.0"


def read_manifest() -> dict:
    with open(DATASET_DIR / "serving_manifest.json", encoding="utf-8") as handle:
        return json.load(handle)


def load_table(name: str) -> pd.DataFrame:
    """Read every shard of one exported table, never just the first part file."""
    files = sorted(glob.glob(str(DATASET_DIR / "csv" / name / "*.csv.gz")))
    if not files:
        raise FileNotFoundError(f"No shards found for table {name} under {DATASET_DIR}")
    return pd.concat((pd.read_csv(path) for path in files), ignore_index=True)


def load_hourly_metrics() -> pd.DataFrame:
    hourly = load_table("station_hourly_metrics")
    hourly["recorded_at"] = pd.to_datetime(hourly["recorded_at"])
    return hourly


def load_training_frame() -> pd.DataFrame:
    """Join features and targets 1:1 and attach horizon split columns."""
    features = load_table("ml_features_hourly")
    targets = load_table("ml_targets_hourly")
    frame = features.merge(
        targets[["station_id", "reference_time", *TARGET_COLUMNS, *HORIZON_SPLITS.values()]],
        on=["station_id", "reference_time"],
        validate="one_to_one",
    )
    frame["reference_dt"] = pd.to_datetime(frame["reference_time"])
    return frame


def parse_utc(stamp: str) -> datetime:
    return datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def format_utc(stamp: datetime) -> str:
    return stamp.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def split_end_exclusive_utc(manifest: dict) -> dict[str, str]:
    """Split boundaries from ``mlSplits`` as Z-suffixed UTC instants.

    Per the dataset manifest, split dates are Asia/Shanghai calendar dates with
    start inclusive / end exclusive, so an end date D maps to the instant
    ``D 00:00:00+08:00`` expressed in UTC (e.g. trainEnd ``2026-03-30`` ->
    ``2026-03-29T16:00:00Z``). Verified against the frame: no TRAIN row has a
    business reference date at or after ``trainEnd``.
    """
    splits = manifest["mlSplits"]

    def boundary(date_str: str) -> str:
        local_midnight = datetime.strptime(date_str, "%Y-%m-%d").replace(
            tzinfo=timezone(BUSINESS_TZ_OFFSET)
        )
        return format_utc(local_midnight)

    return {
        "trainEndExclusive": boundary(splits["trainEnd"]),
        "validationEndExclusive": boundary(splits["validationEnd"]),
        "testEndExclusive": boundary(splits["end"]),
    }


def canonical_payload_digest(bundle: dict) -> str:
    """SHA-256 of the canonical pickle-protocol-5 serialization of the bundle
    with the ``metadata`` entry removed, taken in stabilized state.

    A file cannot carry the hash of its own bytes, so ``artifactSha256`` in
    ``model_metadata.schema.json`` records this payload digest instead. Fitted
    sklearn objects do not re-serialize byte-identically straight out of a
    fresh ``fit`` and a joblib stream is not byte-stable across load/dump
    cycles, but a plain-pickle stream is byte-stable after one round-trip, so
    the digest is defined as ``sha256(pickle.dumps(pickle.loads(pickle.dumps(
    payload, 5), 5)))`` — idempotent from any already-loaded state. The
    delivered bundle is therefore written as plain protocol-5 pickle (which
    ``joblib.load`` reads transparently); verify with
    ``canonical_payload_digest(joblib.load(bundle_path))``. The plain
    whole-file hash is additionally recorded in ``train_metrics.json`` under
    ``artifactFileSha256`` for transport checks.
    """
    payload = {key: value for key, value in bundle.items() if key != "metadata"}
    stabilized = pickle.loads(pickle.dumps(payload, protocol=5))
    return hashlib.sha256(pickle.dumps(stabilized, protocol=5)).hexdigest()


def audit_raw_alignment(
    frame: pd.DataFrame, hourly: pd.DataFrame, sample: int | None = None
) -> dict:
    """Prove the exported lag block really sits in the strict past.

    :func:`audit_offline_parity` recomputes rolling stats *from* the exported
    lags, so it cannot see a whole-block time-shift of the lag columns onto the
    label hour. This closes that gap independently of the export: every usable
    row is joined back to raw ``station_hourly_metrics`` at
    ``reference_dt - k hours`` for each of the 24 lags, plus
    ``last_available_count`` at ``reference_dt - 1 hour``, and the max absolute
    difference is reported. Any missing history hour for a row with a
    non-null lag also counts as a mismatch.
    """
    if sample is not None:
        frame = frame.sample(n=min(sample, len(frame)), random_state=7)
    lookup = hourly.set_index(["station_id", "recorded_at"])
    result: dict[str, float] = {}
    missing_total = 0
    for lag_index in range(1, 25):
        wanted = pd.MultiIndex.from_arrays(
            [frame["station_id"], frame["reference_dt"] - pd.Timedelta(hours=lag_index)]
        )
        raw = lookup["mean_power_kw"].reindex(wanted).to_numpy(dtype=float)
        missing = int(np.isnan(raw).sum()) if raw.size else 0
        missing_total += missing
        table = frame[f"lag_power_kw_h{lag_index:02d}"].to_numpy(dtype=float)
        finite = np.isfinite(raw)
        result[f"lag_power_kw_h{lag_index:02d}"] = (
            float(np.abs(table[finite] - raw[finite]).max()) if finite.any() else float("inf")
        )
    wanted = pd.MultiIndex.from_arrays(
        [frame["station_id"], frame["reference_dt"] - pd.Timedelta(hours=1)]
    )
    raw_avail = lookup["end_available_count"].reindex(wanted).to_numpy(dtype=float)
    missing_total += int(np.isnan(raw_avail).sum()) if raw_avail.size else 0
    finite = np.isfinite(raw_avail)
    result["last_available_count"] = (
        float(
            np.abs(
                frame["last_available_count"].to_numpy(dtype=float)[finite] - raw_avail[finite]
            ).max()
        )
        if finite.any()
        else float("inf")
    )
    return {
        "maxAbsDiffByColumn": result,
        "worstAbsDiff": max(result.values()),
        "missingRawHistoryHours": missing_total,
    }


def build_calendar_lookup(frame: pd.DataFrame) -> dict[tuple[str, str], tuple[int, int]]:
    """(city_id, business_date) -> (is_public_holiday, is_adjusted_workday)."""
    lookup: dict[tuple[str, str], tuple[int, int]] = {}
    for city, date, holiday, workday in frame[
        ["city_id", "business_date", "is_public_holiday", "is_adjusted_workday"]
    ].itertuples(index=False, name=None):
        key = (str(city), str(date)[:10])
        value = (int(bool(holiday)), int(bool(workday)))
        existing = lookup.get(key)
        if existing is not None and existing != value:
            raise ValueError(f"Conflicting calendar flags for {key}")
        lookup[key] = value
    return lookup


def rolling_from_powers(powers: np.ndarray) -> dict[str, float]:
    """powers must be ordered oldest -> newest, covering lag h24 .. h01."""
    return {
        "rolling_mean_kw_3h": float(powers[-3:].mean()),
        "rolling_mean_kw_6h": float(powers[-6:].mean()),
        "rolling_mean_kw_24h": float(powers.mean()),
        "rolling_std_kw_24h": float(powers.std(ddof=0)),
        "rolling_max_kw_24h": float(powers.max()),
    }


def build_feature_row(
    history: list[dict],
    reference_time: str,
    calendar_lookup: dict[tuple[str, str], tuple[int, int]],
    rated_capacity_kw: float | None = None,
) -> dict:
    """Rebuild one model input row from the 24 raw station hours before *reference_time*.

    Each history entry is a station-hour dict with ``recorded_at`` (UTC hour
    start), ``mean_power_kw``, ``capacity``, ``rated_capacity_kw`` and
    ``end_available_count`` — the hourly fields the API can read from
    ``station_hourly_metrics`` plus the station kW rating from
    ``station_snapshot``.
    """
    ref = parse_utc(reference_time)
    ordered = sorted(history, key=lambda row: row["recorded_at"])
    if len(ordered) != 24:
        raise ValueError(f"HISTORY_TOO_SHORT: expected 24 complete hours, got {len(ordered)}")
    expected = [format_utc(ref - timedelta(hours=offset)) for offset in range(24, 0, -1)]
    stamps = [row["recorded_at"] for row in ordered]
    if stamps != expected:
        raise ValueError("history hours are not the 24 consecutive intervals before reference_time")
    powers = np.array([float(row["mean_power_kw"]) for row in ordered], dtype=float)
    if not np.isfinite(powers).all():
        raise ValueError("history contains non-finite mean_power_kw values")

    last = ordered[-1]
    business = ref + BUSINESS_TZ_OFFSET
    city_id = str(last["city_id"])
    holiday, adjusted = calendar_lookup.get((city_id, business.strftime("%Y-%m-%d")), (0, 0))

    row: dict = {
        "city_id": city_id,
        "station_id": str(last["station_id"]),
        "hour_of_day": business.hour,
        "day_of_week": business.weekday(),
        "is_weekend": int(business.weekday() >= 5),
        "is_public_holiday": holiday,
        "is_adjusted_workday": adjusted,
        "capacity": int(last["capacity"]),
        "rated_capacity_kw": float(
            rated_capacity_kw
            if rated_capacity_kw is not None
            else last["rated_capacity_kw"]
        ),
        "last_available_count": int(last["end_available_count"]),
        **rolling_from_powers(powers),
    }
    for index in range(1, 25):
        row[f"lag_power_kw_h{index:02d}"] = float(powers[-index])
    return row


def features_matrix(frame: pd.DataFrame) -> pd.DataFrame:
    """Model input matrix from the exported feature table (offline path)."""
    matrix = frame[FEATURE_COLUMNS].copy()
    for column in CATEGORICAL_FEATURES:
        matrix[column] = matrix[column].astype("category")
    return matrix


def audit_offline_parity(frame: pd.DataFrame, sample: int | None = None) -> dict[str, float]:
    """Recompute rebuilt-vs-table differences for numeric model inputs (vectorized)."""
    if sample is not None:
        frame = frame.sample(n=min(sample, len(frame)), random_state=7)
    lag_matrix = frame[[f"lag_power_kw_h{i:02d}" for i in range(24, 0, -1)]].to_numpy()
    rebuilt = {
        "rolling_mean_kw_3h": lag_matrix[:, -3:].mean(axis=1),
        "rolling_mean_kw_6h": lag_matrix[:, -6:].mean(axis=1),
        "rolling_mean_kw_24h": lag_matrix.mean(axis=1),
        "rolling_std_kw_24h": lag_matrix.std(axis=1, ddof=0),
        "rolling_max_kw_24h": lag_matrix.max(axis=1),
    }
    business = frame["reference_dt"] + BUSINESS_TZ_OFFSET
    rebuilt.update(
        {
            "hour_of_day": business.dt.hour.to_numpy(),
            "day_of_week": business.dt.dayofweek.to_numpy(),
            "is_weekend": (business.dt.dayofweek >= 5).astype(int).to_numpy(),
        }
    )
    return {
        column: float(np.abs(frame[column].to_numpy(dtype=float) - values).max())
        for column, values in rebuilt.items()
    }
