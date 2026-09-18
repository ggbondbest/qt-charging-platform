"""Frame assembly shared by the forecasting tasks.

Everything is read through :class:`ml.common.data_io.Export`, so a training run always uses
the full shard list of one published batch.  The feature block is frozen into the model
metadata, and the same builder is reused by the online path, which makes silent offline /
online drift a test failure instead of a lucky demo.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np
import pandas as pd

from data_analysis.ml.common import features as feat
from data_analysis.ml.common.data_io import (
    DEFAULT_EXPORT,
    FEATURES_TABLE,
    HOURLY_TABLE,
    SNAPSHOT_TABLE,
    TARGETS_TABLE,
    Export,
    open_export,
)

JOIN_KEYS = ["station_id", "reference_time"]
SPLIT_LABELS = ("TRAIN", "VALIDATION", "TEST")


@dataclass
class Frame:
    export: Export
    data: pd.DataFrame
    feature_columns: list[str]
    cities: list[str]
    site_types: list[str]
    calendar: dict[tuple[str, str], tuple[bool, bool]]
    stations: dict[str, dict] = field(default_factory=dict)
    hourly: pd.DataFrame | None = None

    def mask(self, horizon_hours: int, split: str) -> pd.Series:
        column = self.export.split_column(horizon_hours)
        return self.data[column] == split

    def labels(self, prefix: str, steps: range | tuple[int, ...]) -> list[str]:
        return [f"{prefix}{step:02d}" for step in steps]


def build_frame(export_dir=DEFAULT_EXPORT, *, use_cache: bool = True) -> Frame:
    export = open_export(export_dir)
    features = export.table(FEATURES_TABLE, use_cache=use_cache)
    targets = export.table(TARGETS_TABLE, use_cache=use_cache)
    hourly = export.table(HOURLY_TABLE, use_cache=use_cache)

    merged = features.merge(targets, on=JOIN_KEYS, how="inner", validate="one_to_one")
    if len(merged) != len(features) or len(merged) != len(targets):
        raise ValueError(
            f"features ({len(features)}) and targets ({len(targets)}) do not join one-to-one "
            f"on {JOIN_KEYS}; got {len(merged)} rows"
        )
    for column in ("feature_version_x", "feature_version_y"):
        if (merged[column].isna().any() or merged[column].nunique() != 1 or
                merged[column].iloc[0] != export.feature_version):
            raise ValueError("feature table and targets table disagree on feature_version")
    merged = merged.drop(columns=["feature_version_x", "feature_version_y"])

    merged = feat.add_window_features(merged, hourly)

    cities = feat.city_list(merged)
    for city in cities:
        merged[f"city_{city.lower()}"] = (merged["city_id"] == city).astype("int64")

    site_types: list[str] = []
    if SNAPSHOT_TABLE in export.manifest["tables"]:
        snapshot = export.table(SNAPSHOT_TABLE, use_cache=use_cache)
        if "site_type" in snapshot.columns:
            profile = snapshot[["station_id", "site_type"]].drop_duplicates("station_id")
            merged = merged.merge(profile, on="station_id", how="left")
            site_types = sorted(str(value) for value in merged["site_type"].dropna().unique())
            for site in site_types:
                merged[f"site_{_slug(site)}"] = (merged["site_type"] == site).astype("int64")
            if merged["site_type"].isna().any():
                raise ValueError("some stations have no site_type in station_snapshot")
            merged = merged.drop(columns=["site_type"])

    # Known-calendar cyclical encodings: safe (derived from the prediction boundary only) and
    # much easier for trees than raw 0-23 integers.
    merged["hour_sin"] = np.sin(2 * math.pi * merged["hour_of_day"] / 24.0)
    merged["hour_cos"] = np.cos(2 * math.pi * merged["hour_of_day"] / 24.0)
    merged["dow_sin"] = np.sin(2 * math.pi * merged["day_of_week"] / 7.0)
    merged["dow_cos"] = np.cos(2 * math.pi * merged["day_of_week"] / 7.0)
    for column in ("is_weekend", "is_public_holiday", "is_adjusted_workday"):
        merged[column] = merged[column].astype(bool).astype("int64")

    feature_columns = [
        *feat.POWER_LAGS,
        *feat.AVAILABLE_LAGS,
        *feat.EXPORT_ROLLINGS,
        "rolling_mean_available_24h",
        "hour_of_day",
        "day_of_week",
        "hour_sin",
        "hour_cos",
        "dow_sin",
        "dow_cos",
        "is_weekend",
        "is_public_holiday",
        "is_adjusted_workday",
        "capacity",
        "rated_capacity_kw",
        *[f"city_{city.lower()}" for city in cities],
        *[f"site_{_slug(site)}" for site in site_types],
    ]
    missing = [column for column in feature_columns if column not in merged.columns]
    if missing:
        raise ValueError(f"feature columns missing from the assembled frame: {missing}")
    for column in feature_columns:
        if column.startswith("label_") or column.startswith("split_"):
            raise ValueError(f"{column} must never be a model input")

    profile_columns = ["station_id", "city_id", "capacity", "rated_capacity_kw"] + [
        f"site_{_slug(site)}" for site in site_types
    ]
    stations: dict[str, dict] = {}
    for record in merged[profile_columns].drop_duplicates("station_id").to_dict("records"):
        rated = record["rated_capacity_kw"]
        profile = {
            "city_id": str(record["city_id"]),
            "capacity": int(record["capacity"]),
            "rated_capacity_kw": None if pd.isna(rated) else float(rated),
        }
        for site in site_types:
            column = f"site_{_slug(site)}"
            profile[column] = int(record[column])
        stations[str(record["station_id"])] = profile
    return Frame(
        export=export,
        data=merged,
        feature_columns=feature_columns,
        cities=cities,
        site_types=site_types,
        calendar=feat.calendar_lookup(features),
        stations=stations,
        hourly=hourly,
    )


def _station_profiles(merged: pd.DataFrame, site_types: list[str]):
    subset = merged[["station_id", "city_id", "capacity", "rated_capacity_kw"] + [f"site_{_slug(s)}" for s in site_types]]
    return subset.drop_duplicates("station_id").itertuples(index=False)


def _slug(value: str) -> str:
    return "".join(character if character.isalnum() else "_" for character in str(value).lower()).strip("_")


def split_summary(frame: Frame) -> dict:
    summary = {}
    for horizon in (1, 6, 24):
        column = frame.export.split_column(horizon)
        counts = frame.data[column].value_counts().to_dict()
        summary[f"{horizon}h"] = {key: int(counts.get(key, 0)) for key in (*SPLIT_LABELS, "EXCLUDED")}
    return summary


def online_feature_row(profile_like, reference_time, station_id: str, history: list[dict]) -> dict:
    """Derive one model input row from raw hourly history, in the training column order.

    ``profile_like`` only needs the four attributes :func:`ml.common.features.derive_features`
    consumes, so a loaded model bundle can be used directly instead of a full training frame.
    """
    station = profile_like.stations[station_id]
    if any(row.get("station_id") != station_id for row in history):
        raise feat.HistoryError("history belongs to a different station")
    if any(row.get("capacity") != station["capacity"] for row in history):
        raise feat.HistoryError("history capacity differs from the station profile")
    derived = feat.derive_features(
        history,
        station=station,
        reference_time=reference_time,
        calendar=profile_like.calendar,
        cities=profile_like.cities,
    )
    derived["hour_sin"] = math.sin(2 * math.pi * derived["hour_of_day"] / 24.0)
    derived["hour_cos"] = math.cos(2 * math.pi * derived["hour_of_day"] / 24.0)
    derived["dow_sin"] = math.sin(2 * math.pi * derived["day_of_week"] / 7.0)
    derived["dow_cos"] = math.cos(2 * math.pi * derived["day_of_week"] / 7.0)
    for column in ("is_weekend", "is_public_holiday", "is_adjusted_workday"):
        derived[column] = int(bool(derived[column]))
    # Station attributes the model saw as constants per station (capacity, site_type one-hot).
    for column in profile_like.feature_columns:
        if column not in derived and column in station:
            derived[column] = station[column]
    row = {}
    for column in profile_like.feature_columns:
        if column not in derived:
            raise ValueError(f"online feature build produced no value for {column}")
        value = derived[column]
        row[column] = None if value is None else value
    return row


def validate_bundle_frame(bundle: dict, metadata: dict, frame: Frame) -> None:
    """Never relabel a model trained on another export as belonging to this batch."""
    expected = {"datasetId": frame.export.dataset_id,
                "trainingPublishedBatchId": frame.export.published_batch_id,
                "sourceManifestSha256": frame.export.source_manifest_sha256,
                "featureVersion": frame.export.feature_version,
                "featureColumns": frame.feature_columns}
    if any(metadata.get(key) != value for key, value in expected.items()):
        raise ValueError("model metadata differs from the verified export batch/features")
    fields = {"datasetId": "datasetId", "publishedBatchId": "trainingPublishedBatchId",
              "featureVersion": "featureVersion", "featureColumns": "featureColumns"}
    if any(bundle.get(key) != metadata.get(value) for key, value in fields.items()):
        raise ValueError("model bundle differs from its metadata batch/features")
