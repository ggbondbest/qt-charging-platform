"""Shared offline/online as-of features. All observation intervals end before origin.

The simulator emits state AFTER processing arrivals at each five-minute tick.
Consequently even horizon-zero inference uses the preceding complete tick.
No session outcome, future queue position, or future release time is a feature.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

import numpy as np
import pandas as pd

DATASET = Path(__file__).resolve().parents[2] / "datasets" / "analytics_full_180d_v1"
# parents[2] is chargepilot's parent data_analysis.
STATES = ["AVAILABLE", "CHARGING", "RESERVED", "OCCUPIED", "MAINTENANCE", "OFFLINE"]
HORIZONS = np.array([0, 5, 10, 15, 30, 45, 60], dtype=np.int16)
FEATURE_NAMES = [
    "station", "city", "site_type", "capacity", "rated_kw", "horizon_minutes",
    "origin_hour_sin", "origin_hour_cos", "origin_weekday", "arrival_hour_sin",
    "arrival_hour_cos", "arrival_weekday", "arrival_weekend", "arrival_holiday",
    "arrival_adjusted_workday", *[f"last_{s.lower()}" for s in STATES],
    "last_power_kw", "queue_waiting", "arrivals_15min", "arrivals_60min",
    "available_mean_15min", "available_mean_30min", "available_mean_60min",
    "power_mean_15min", "power_mean_30min", "power_mean_60min",
    "available_lag_15min", "available_lag_60min",
]
SOURCE_TABLES = ["charger_telemetry", "charging_attempts", "charging_sessions",
                 "queue_entries", "reservations", "stations", "chargers", "cities",
                 "calendar", "tariffs"]


def epoch(value):
    """UTC Unix seconds. Parquet's timezone-naive timestamps encode UTC."""
    value = pd.to_datetime(value, utc=True)
    if isinstance(value, pd.Timestamp):
        return int(value.timestamp())
    return value.to_numpy(dtype="datetime64[ns]").astype(np.int64) // 1_000_000_000


def iso(seconds):
    return pd.Timestamp(int(seconds), unit="s", tz="UTC").strftime("%Y-%m-%dT%H:%M:%SZ")


def read_clean(dataset, name, columns=None):
    import pyarrow.parquet as pq
    files = sorted((Path(dataset) / "clean" / name).glob("*.parquet"))
    if not files:
        raise FileNotFoundError(f"Missing CLEAN table: {name}")
    return pd.concat([pq.read_table(p, columns=columns).to_pandas() for p in files], ignore_index=True)


def source_provenance(source):
    binding = {key: source[key] for key in ["datasetId", "pipelineRunId", "publishedBatchId", "sourceManifestSha256"]}
    binding["cleanFilesManifestSha256"] = hashlib.sha256(
        json.dumps(source["sourceFilesSha256"], sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return binding


def prepare(dataset=DATASET):
    import pyarrow.parquet as pq

    dataset = Path(dataset)
    manifest = json.loads((dataset / "serving_manifest.json").read_text())
    stations = read_clean(dataset, "stations").sort_values("station_id").reset_index(drop=True)
    chargers = read_clean(dataset, "chargers")
    cities = read_clean(dataset, "cities").sort_values("city_id").reset_index(drop=True)
    tariffs = read_clean(dataset, "tariffs")
    station_lookup = {v: i for i, v in enumerate(stations.station_id)}
    city_lookup = {v: i for i, v in enumerate(cities.city_id)}
    site_lookup = {v: i for i, v in enumerate(sorted(stations.site_type.unique()))}
    start = epoch(manifest["periodStart"])
    end = epoch(manifest["periodEndExclusive"])
    n_ticks = (end - start) // 300
    shape = (len(stations), n_ticks)
    counts = np.zeros((*shape, 6), dtype=np.uint8)
    power = np.zeros(shape, dtype=np.float32)
    state_lookup = {v: i for i, v in enumerate(STATES)}
    seen = np.zeros((len(chargers), n_ticks), dtype=np.uint8)
    charger_lookup = {v: i for i, v in enumerate(chargers.charger_id)}
    telemetry_rows = 0
    for path in sorted((dataset / "clean" / "charger_telemetry").glob("*.parquet")):
        for batch in pq.ParquetFile(path).iter_batches(batch_size=200_000, columns=[
            "charger_id", "station_id", "recorded_at", "interval_seconds", "state", "power_kw"
        ]):
            f = batch.to_pandas()
            times = epoch(f.recorded_at)
            s = f.station_id.map(station_lookup).to_numpy(dtype=int)
            c = f.charger_id.map(charger_lookup).to_numpy(dtype=int)
            j = (times - start) // 300
            k = f.state.map(state_lookup).to_numpy(dtype=int)
            if not ((times - start) % 300 == 0).all() or not (f.interval_seconds == 300).all():
                raise ValueError("Expected complete aligned 300-second telemetry intervals")
            if j.min() < 0 or j.max() >= n_ticks:
                raise ValueError("Telemetry outside manifest")
            np.add.at(counts, (s, j, k), 1)
            np.add.at(power, (s, j), f.power_kw.to_numpy(dtype=np.float32))
            np.add.at(seen, (c, j), 1)
            telemetry_rows += len(f)
    if not np.all(seen == 1):
        raise ValueError("Duplicate or missing charger telemetry intervals")
    capacity = chargers.groupby("station_id").size().reindex(stations.station_id).to_numpy(dtype=np.int16)
    rated = chargers.groupby("station_id").rated_power_kw.sum().reindex(stations.station_id).to_numpy(dtype=np.float32)
    if not np.all(counts.sum(axis=2) == capacity[:, None]):
        raise ValueError("Station state counts do not match installed capacity")
    del seen
    attempts = read_clean(dataset, "charging_attempts")
    sessions = read_clean(dataset, "charging_sessions")
    queues = read_clean(dataset, "queue_entries")
    arrivals = np.zeros(shape, dtype=np.int16)
    attempt_s = attempts.station_id.map(station_lookup).to_numpy(dtype=int)
    attempt_j = (epoch(attempts.attempted_at) - start) // 300
    np.add.at(arrivals, (attempt_s, attempt_j), 1)
    queue_delta = np.zeros((len(stations), n_ticks + 1), dtype=np.int16)
    joined = (epoch(queues.joined_at) - start) // 300
    leave_at = pd.concat([queues.called_at, queues.resolved_at], axis=1).min(axis=1)
    if leave_at.isna().any():
        leave_at = leave_at.fillna(pd.Timestamp(end, unit="s"))
    left = np.clip((epoch(leave_at) - start) // 300, 0, n_ticks)
    queue_s = queues.station_id.map(station_lookup).to_numpy(dtype=int)
    np.add.at(queue_delta, (queue_s, joined), 1)
    np.add.at(queue_delta, (queue_s, left), -1)
    waiting = queue_delta[:, :-1].cumsum(axis=1).astype(np.int16)
    if waiting.min() < 0:
        raise ValueError("Negative reconstructed queue")
    calendar = read_clean(dataset, "calendar")
    holiday = {f"{r.city_id}:{str(r.business_date)[:10]}": [
        int(str(r.scenario_event).startswith("PUBLIC_HOLIDAY_")),
        int(r.scenario_event == "ADJUSTED_WORKDAY")
    ] for r in calendar.itertuples()}
    price_lookup = {f"{r.city_id}:{int(r.hour)}":
                    float(r.energy_price_cents_per_kwh + r.service_price_cents_per_kwh) / 100
                    for r in tariffs.itertuples()}
    catalog = [{"stationId": r.station_id, "cityId": r.city_id,
                "stationName": r.station_name, "siteType": r.site_type,
                "latitude": float(r.latitude), "longitude": float(r.longitude),
                "capacity": int(capacity[i]), "powerKw": float(rated[i] / capacity[i]),
                "ratedCapacityKw": float(rated[i]), "powerAssumption": "VIRTUAL_EQUAL_SHARE",
                "pricePerKwh": price_lookup[f"{r.city_id}:8"]}
               for i, r in enumerate(stations.itertuples())]
    city_catalog = [{"cityId": r.city_id, "cityName": r.city_name,
                     "latitude": float(r.latitude), "longitude": float(r.longitude)}
                    for r in cities.itertuples()]
    config = {"startEpoch": start, "endEpoch": end, "catalog": catalog,
              "cities": city_catalog, "calendar": holiday, "prices": price_lookup,
              "cityCodes": [city_lookup[v] for v in stations.city_id],
              "siteCodes": [site_lookup[v] for v in stations.site_type],
              "splits": manifest["mlSplits"]}
    hashes = {}
    for name in SOURCE_TABLES:
        for p in sorted((dataset / "clean" / name).glob("*.parquet")):
            hashes[str(p.relative_to(dataset))] = hashlib.sha256(p.read_bytes()).hexdigest()
    hashes["serving_manifest.json"] = hashlib.sha256((dataset / "serving_manifest.json").read_bytes()).hexdigest()
    audit = {"telemetryRows": telemetry_rows, "stationTicks": int(np.prod(shape)),
             "sessions": len(sessions), "attempts": len(attempts), "queueEntries": len(queues),
             "sourceFilesSha256": hashes,
             "datasetId": manifest["datasetId"], "pipelineRunId": manifest["pipelineRunId"],
             "publishedBatchId": manifest["publishedBatchId"],
             "sourceManifestSha256": manifest["sourceManifestSha256"],
             "availabilityCounts": np.bincount(counts[:, :, 0].ravel()).tolist()}
    config["provenance"] = source_provenance(audit)
    replay = Replay(config, counts, power, waiting, arrivals)
    # Outcomes are held separately, used exclusively for labels / completed histories.
    labelled = attempts.merge(sessions[["attempt_id", "started_at"]], on="attempt_id", how="left", validate="one_to_one")
    labelled = labelled.merge(queues[["queue_id", "resolved_at"]], on="queue_id", how="left", validate="many_to_one")
    labelled = labelled[labelled.reservation_id.isna() | (labelled.reservation_id == "")].copy()
    labelled["arrivalEpoch"] = epoch(labelled.attempted_at)
    labelled["stationCode"] = labelled.station_id.map(station_lookup)
    labelled["success"] = labelled.started_at.notna().astype(int)
    finish = labelled.started_at.fillna(labelled.resolved_at).fillna(labelled.attempted_at)
    labelled["finalEpoch"] = epoch(finish)
    # Missing (unsuccessful) waits remain NaN, never zero.
    labelled["waitMinutes"] = (pd.to_datetime(labelled.started_at, utc=True) - pd.to_datetime(labelled.attempted_at, utc=True)).dt.total_seconds() / 60
    if not (labelled.loc[labelled.success == 1, "waitMinutes"] >= 0).all():
        raise ValueError("Invalid wait labels")
    return replay, labelled.reset_index(drop=True), audit


class Replay:
    def __init__(self, config, counts, power, waiting, arrivals):
        self.config = config
        self.counts, self.power, self.waiting, self.arrivals = counts, power, waiting, arrivals
        self.start = config["startEpoch"]
        self.catalog = config["catalog"]
        self.lookup = {v["stationId"]: i for i, v in enumerate(self.catalog)}
        self.capacity = np.array([s["capacity"] for s in self.catalog])
        self.rated = np.array([s.get("ratedCapacityKw", s["powerKw"]) for s in self.catalog])
        # Prefix sums only accelerate strictly past windows; future values cannot enter a slice.
        self.cumulative = {"available": np.pad(counts[:, :, 0].astype(np.float64).cumsum(axis=1), ((0, 0), (1, 0))),
                           "power": np.pad(power.astype(np.float64).cumsum(axis=1), ((0, 0), (1, 0))),
                           "arrivals": np.pad(arrivals.astype(np.int64).cumsum(axis=1), ((0, 0), (1, 0)))}

    def features(self, station, reference, horizon):
        s, ref, h = np.broadcast_arrays(np.asarray(station, dtype=int), np.asarray(reference, dtype=np.int64), np.asarray(horizon, dtype=float))
        s, ref, h = s.ravel(), ref.ravel(), h.ravel()
        if not np.isfinite(h).all() or (h < 0).any() or (h > 60).any():
            raise ValueError("ETA must be finite and between 0 and 60 minutes")
        if (s < 0).any() or (s >= len(self.catalog)).any():
            raise ValueError("Unknown station")
        j = (ref - self.start) // 300 - 1
        if (j < 11).any() or (j >= self.counts.shape[1]).any():
            raise ValueError("Insufficient strictly historical complete five-minute samples")
        # Target slot is a floor to 5 minutes, matching the simulator's constant state interval.
        target = ref + np.rint(h * 60).astype(np.int64)
        local_origin = pd.to_datetime(ref, unit="s", utc=True).tz_convert("Asia/Shanghai")
        local_target = pd.to_datetime(target, unit="s", utc=True).tz_convert("Asia/Shanghai")
        oh = local_origin.hour.to_numpy() + local_origin.minute.to_numpy() / 60
        ah = local_target.hour.to_numpy() + local_target.minute.to_numpy() / 60
        day = local_target.strftime("%Y-%m-%d")
        holidays = np.array([self.config["calendar"].get(f"{self.catalog[si]['cityId']}:{d}", [0, 0]) for si, d in zip(s, day)])
        matrix = [s, np.asarray(self.config["cityCodes"])[s], np.asarray(self.config["siteCodes"])[s],
                  self.capacity[s], self.rated[s], h, np.sin(oh * np.pi / 12), np.cos(oh * np.pi / 12),
                  local_origin.dayofweek, np.sin(ah * np.pi / 12), np.cos(ah * np.pi / 12),
                  local_target.dayofweek, (local_target.dayofweek >= 5).astype(int), holidays[:, 0], holidays[:, 1]]
        matrix.extend(self.counts[s, j, k] for k in range(6))
        matrix.extend([self.power[s, j], self.waiting[s, j]])
        def history_sum(name, ticks):
            c = self.cumulative[name]
            return c[s, j + 1] - c[s, j + 1 - ticks]
        matrix.extend([history_sum("arrivals", 3), history_sum("arrivals", 12)])
        for name in ["available", "power"]:
            matrix.extend(history_sum(name, n) / n for n in [3, 6, 12])
        matrix.extend([self.counts[s, j - 2, 0], self.counts[s, j - 11, 0]])
        result = np.column_stack(matrix).astype(np.float32)
        if result.shape[1] != len(FEATURE_NAMES) or not np.isfinite(result).all():
            raise ValueError("Invalid feature matrix")
        return result

    def save(self, output):
        output = Path(output)
        (output / "replay.json").write_text(json.dumps(self.config, ensure_ascii=False, indent=2))
        np.savez_compressed(output / "replay.npz", counts=self.counts, power=self.power,
                            waiting=self.waiting, arrivals=self.arrivals)

    @classmethod
    def load(cls, output):
        output = Path(output)
        cfg = json.loads((output / "replay.json").read_text())
        with np.load(output / "replay.npz", allow_pickle=False) as d:
            return cls(cfg, d["counts"], d["power"], d["waiting"], d["arrivals"])


def split_boundaries(config):
    split = config["splits"]
    def boundary(v):
        return epoch(pd.Timestamp(v, tz="Asia/Shanghai"))
    return {"TRAIN": (boundary(split["start"]), boundary(split["trainEnd"])),
            "VALIDATION": (boundary(split["trainEnd"]), boundary(split["validationEnd"])),
            "TEST": (boundary(split["validationEnd"]), boundary(split["end"]))}


def purged_mask(reference, arrival, finalized, start, end):
    """Origin and label completion must share a split. End is exclusive."""
    return (reference >= start) & (reference < end) & (arrival >= start) & (arrival < end) & (finalized < end)
