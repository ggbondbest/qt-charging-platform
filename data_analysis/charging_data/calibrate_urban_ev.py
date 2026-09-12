"""Reproduce an aggregate-only Shenzhen public charging utilization profile.

This is an offline, standard-library-only calibration tool. Download duration.csv,
occupancy.csv and inf.csv from the pinned author repository revision, then run:

python -m data_analysis.charging_data.calibrate_urban_ev --input DOWNLOADED_DIR \
    --output data_analysis/config/urban_ev_profile.json

No individual charging records, coordinates or zone/station identifiers are
copied into the output. Utilization is deliberately NOT labelled as arrivals.
"""

import argparse
from collections import defaultdict
import csv
from datetime import datetime, timedelta
import hashlib
import json
import math
from pathlib import Path


REVISION = "44f2aa0c8d89f192bce00bafb0def74a21b39c68"
REPOSITORY = "https://github.com/IntelligentSystemsLab/UrbanEV"
SOURCE_HASHES = {
    "duration.csv": "3afc79ca193267bca327b04d45b6cf24240f8c93ea94bcbb0a52cc119b8080d1",
    "occupancy.csv": "1b9099b8c6c33510a2c48a79c4b524b01f6a2f5df45b9d604361b22ad4bc5211",
    "inf.csv": "03c9830965e9e99b29adfb8cceed0eba98d37631f514273cb3fe61f80d63de7c",
}
START = datetime(2022, 9, 1)
END = datetime(2023, 3, 1)


def _source_info(directory):
    result = []
    for filename, expected in SOURCE_HASHES.items():
        path = directory / filename
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        actual = digest.hexdigest()
        if actual != expected:
            raise ValueError(f"{filename}: SHA256 differs from pinned public source")
        result.append({"name": filename, "sha256": actual,
                       "bytes": path.stat().st_size,
                       "url": f"https://raw.githubusercontent.com/IntelligentSystemsLab/UrbanEV/{REVISION}/data/{filename}"})
    return result


def _capacities(path):
    capacities = defaultdict(int)
    station_ids = set()
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            station = row["station_id"]
            count = int(row["charge_count"])
            if station in station_ids or count <= 0:
                raise ValueError("Duplicate station or invalid charger capacity")
            station_ids.add(station)
            capacities[row["TAZID"]] += count
    if (len(station_ids), len(capacities), sum(capacities.values())) != (1362, 275, 17532):
        raise ValueError("Station metadata does not match pinned UrbanEV cohort")
    return capacities


def _read_hourly(path, capacities):
    totals = []
    maximum = 0.0
    cells_over_100 = 0
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.reader(stream)
        columns = next(reader)
        if columns[0] != "time" or len(columns) != len(set(columns)):
            raise ValueError("Invalid or duplicate UrbanEV header")
        if set(columns[1:]) != set(capacities):
            raise ValueError("Hourly zones do not match capacity metadata")
        limits = [capacities[column] for column in columns[1:]]
        expected_time = START
        for row in reader:
            if len(row) != len(columns):
                raise ValueError("Inconsistent hourly CSV row width")
            when = datetime.strptime(row[0], "%Y-%m-%d %H:%M:%S")
            if when != expected_time:
                raise ValueError("Missing, duplicate or nonchronological hourly row")
            values = [float(value) for value in row[1:]]
            for value, capacity in zip(values, limits):
                if not math.isfinite(value) or not 0 <= value <= capacity + 1e-7:
                    raise ValueError("Invalid duration or availability proxy value")
                maximum = max(maximum, value)
                cells_over_100 += value > 100
            totals.append((when, math.fsum(values)))
            expected_time += timedelta(hours=1)
        if expected_time != END:
            raise ValueError("Incomplete six-month UrbanEV hourly period")
    return totals, {"rows": len(totals), "zones": len(limits),
                    "missing_or_nonfinite_cells": 0, "negative_cells": 0,
                    "cells_above_zone_charger_capacity": 0,
                    "maximum_cell": maximum, "cells_above_100": cells_over_100}


def _profiles(totals, regularization):
    sums = {name: [0.0] * 24 for name in ("all_days", "weekday", "weekend")}
    counts = {name: [0] * 24 for name in sums}
    for when, value in totals:
        kind = "weekend" if when.weekday() >= 5 else "weekday"
        for name in ("all_days", kind):
            sums[name][when.hour] += value
            counts[name][when.hour] += 1
    result = {}
    global_hour_mean = math.fsum(sums["all_days"]) / sum(counts["all_days"])
    for name in sums:
        if min(counts[name]) == 0 or len(set(counts[name])) != 1:
            raise ValueError("Profile requires full calendar days for all 24 hours")
        mean = [value / count for value, count in zip(sums[name], counts[name])]
        day_mean = math.fsum(mean) / 24
        normalized = [value / day_mean for value in mean]
        result[name] = {
            "days": counts[name][0],
            "mean_active_charger_hours_by_hour": [round(value, 6) for value in mean],
            "mean_capacity_utilization_by_hour": [round(value / 17532, 9) for value in mean],
            "normalized_mean_one": [round(value, 9) for value in normalized],
            "regularized_normalized_mean_one": [round((1 - regularization) * value + regularization, 9)
                                                 for value in normalized],
            "mean_daily_active_charger_hours": round(math.fsum(mean), 6),
            "intensity_relative_to_all_days": round(day_mean / global_hour_mean, 9),
            "peak_hour": max(range(24), key=lambda hour: mean[hour]),
            "minimum_hour": min(range(24), key=lambda hour: mean[hour]),
        }
    return result


def calibrate_urban_ev(directory, regularization=0.2):
    if not math.isfinite(regularization) or not 0 <= regularization <= 1:
        raise ValueError("regularization must be between 0 and 1")
    directory = Path(directory)
    sources = _source_info(directory)
    capacities = _capacities(directory / "inf.csv")
    totals, duration_quality = _read_hourly(directory / "duration.csv", capacities)
    _, occupancy_quality = _read_hourly(directory / "occupancy.csv", capacities)
    return {
        "profile_version": "1.0.0",
        "profile_id": "urban_ev_shenzhen_public_utilization_202209_202302",
        "source_kind": "PUBLIC_EMPIRICAL_AGGREGATE",
        "provenance": {
            "dataset": "UrbanEV",
            "authors": "Han Li, Haohao Qu and collaborators",
            "repository": REPOSITORY,
            "repository_revision": REVISION,
            "license": "CC0-1.0",
            "license_url": f"{REPOSITORY}/blob/{REVISION}/LICENSE",
            "paper": "https://doi.org/10.1038/s41597-025-04874-4",
            "source_files": sources,
            "raw_source_redistributed": False,
        },
        "scope": {
            "city": "深圳市", "facility_access": "public",
            "start_inclusive_local": START.isoformat(),
            "end_exclusive_local": END.isoformat(),
            "timezone_interpretation": "Asia/Shanghai; source wall-clock timestamps have no explicit offset",
            "days": 181, "hourly_rows": 4344, "zones": 275,
            "stations": 1362, "charging_piles": 17532,
            "day_type_rule": "weekday=Monday-Friday; weekend=Saturday-Sunday; Chinese holiday and make-up-workday rules NOT applied",
        },
        "method": {
            "source_metric": "duration.csv: active charging duration summed across piles within a traffic zone, in pile-hours per one-hour interval",
            "city_aggregation": "Sum all zone durations before calculating per-hour calendar-day means; do not equally average zone utilization rates",
            "normalization": "Divide each 24-hour mean series by its own hourly mean; each profile has arithmetic mean 1",
            "regularization_uniform_weight": regularization,
            "regularization_formula": "(1-alpha)*observed_normalized + alpha*1",
            "regularization_is_simulation_design_choice": True,
            "day_type_intensity": "Keep relative daily intensity separately; independent normalization removes level differences",
            "our_additional_imputation": "none; nonfinite values, missing hours, invalid capacities and source hash mismatches are rejected",
        },
        "utilization_profiles": _profiles(totals, regularization),
        "quality": {
            "source_sha256_verified": True,
            "duration": duration_quality,
            "occupancy_diagnostic_only": occupancy_quality,
            "occupancy_used_for_profile": False,
        },
        "limitations": [
            "Utilization measures charging in progress, not arrival/session-start counts. It cannot identify arrival rates without assumptions about durations and queueing.",
            "Use as a Shenzhen public-station aggregate prior or output-shape validation. Transferring it to another city or a specific station type is a modelling assumption, not city-specific observation.",
            "The source is a selected, already-cleaned zone cohort; low-activity regions were removed. It is not the entire Shenzhen charger fleet or home charging.",
            "The authors report forward/backward filling, IQR replacement using adjacent values, zero-value filtering and spatial/hourly aggregation; original missing/outlier flags are unavailable.",
            "This period includes pandemic-era travel disruption and holidays. These six months do not establish a current full-year causal price response or a universal night-time peak.",
            "Source README labels occupancy as percent, but the pinned file contains values above 100 and bounded by zone pile counts. Its exact scale is not resolved here and it is not used as a rate.",
            "The source volume.csv is rated-power-derived, not electricity-meter ground truth. Neither volume file is used for energy calibration.",
            "Regularization is explicitly a simulator stability choice. It is not a fitted confidence interval, official guidance, or proof of real-world predictive performance.",
        ],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--regularization", type=float, default=0.2)
    args = parser.parse_args(argv)
    try:
        profile = calibrate_urban_ev(args.input, args.regularization)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(profile, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
    except (OSError, ValueError, KeyError, StopIteration) as error:
        parser.exit(2, f"UrbanEV calibration failed: {error}\n")
    print("UrbanEV utilization profile written: 181 days, 275 zones, no individual records")


if __name__ == "__main__":
    main()
