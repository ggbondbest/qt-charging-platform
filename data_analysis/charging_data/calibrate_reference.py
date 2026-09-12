"""Build an aggregate-only calibration profile from a local reference dataset.

Example, from repository root:
python -m data_analysis.charging_data.calibrate_reference --input REFERENCE_DIR \
    --output data_analysis/config/reference_profile.json

Only Python's standard library is required. The output never includes source
paths, user/station/location identifiers, individual records or raw sample pairs.
Input files are read-only and the output is created exclusively, never replaced.
"""

import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import csv
from datetime import datetime
import hashlib
import json
import math
from pathlib import Path
import statistics


PROFILE_VERSION = "1.0.0"
WEEKDAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
REQUIRED_FIELDS = {
    "sessionId", "kwhTotal", "chargeTimeHrs", "startTime", "weekday",
    "userId", "stationId", "locationId", "facilityType",
}
OPTIONAL_FIELDS = {"created", "ended", "endTime"}
METADATA_FIELDS = {"stationId", "locationId", "facilityType"}
QUANTILES = {"p05": .05, "p25": .25, "p50": .5, "p75": .75, "p95": .95, "p99": .99}


def _read_csv(path, required, optional=()):
    """Keep only fields needed for analysis; do not retain names or addresses."""
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        missing = required - set(reader.fieldnames or ())
        if missing:
            raise ValueError("Missing required columns: " + ", ".join(sorted(missing)))
        if len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise ValueError("Duplicate column names in reference CSV")
        selected = required | set(optional)
        rows = []
        for row in reader:
            if None in row or any(value is None for value in row.values()):
                raise ValueError("Inconsistent reference CSV row width")
            rows.append({key: row.get(key, "").strip() for key in selected})
    if not rows:
        raise ValueError("Reference CSV has no data rows")
    return rows


def _source_info(path, role, row_count):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"role": role, "sha256": digest.hexdigest(),
            "bytes": path.stat().st_size, "row_count": row_count}


def _number(text, field):
    try:
        value = float(text)
    except (TypeError, ValueError) as error:
        raise ValueError(f"Invalid numeric reference field: {field}") from error
    if not math.isfinite(value):
        raise ValueError(f"Non-finite reference field: {field}")
    return value


def _quantile(values, probability):
    """Linear interpolation at (n - 1) * p; no fitted distribution is implied."""
    position = (len(values) - 1) * probability
    lower = math.floor(position)
    upper = math.ceil(position)
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def _bin_index(value, edges):
    if value < edges[0] or value > edges[-1]:
        raise ValueError("Histogram edges do not cover the reference observations")
    return min(bisect_right(edges, value) - 1, len(edges) - 2)


def _histogram(values, edges):
    counts = [0] * (len(edges) - 1)
    for value in values:
        counts[_bin_index(value, edges)] += 1
    return {"edges": edges, "counts": counts}


def _summary(values, edges=None):
    if not values:
        return {"count": 0, "min": None, "max": None, "mean": None,
                "quantiles": {key: None for key in QUANTILES}}
    ordered = sorted(values)
    result = {"count": len(values), "min": ordered[0], "max": ordered[-1],
              "mean": round(statistics.mean(ordered), 8),
              "quantiles": {key: round(_quantile(ordered, probability), 8)
                            for key, probability in QUANTILES.items()}}
    if edges is not None:
        result["histogram"] = _histogram(values, edges)
    return result


def _pearson(x_values, y_values):
    if len(x_values) < 2:
        return None
    mean_x, mean_y = statistics.mean(x_values), statistics.mean(y_values)
    dx = [value - mean_x for value in x_values]
    dy = [value - mean_y for value in y_values]
    denominator = math.sqrt(sum(value * value for value in dx) *
                            sum(value * value for value in dy))
    if denominator == 0:
        return None
    return round(sum(x * y for x, y in zip(dx, dy)) / denominator, 8)


def _hour_counts(records):
    counts = [0] * 24
    for record in records:
        counts[record["hour"]] += 1
    return counts


def calibrate_reference(input_path):
    """Return deterministic aggregate statistics, never a source-row export.

    input_path may be the source directory or its session CSV. Metadata is
    optional and, if present, read from nvv2t_md_end.csv alongside that CSV.
    Hourly counts retain all records. Energy/connection statistics and their
    joint histogram use only finite, strictly positive energy AND duration.
    """
    source = Path(input_path)
    session_path = source / "nvv2t.csv" if source.is_dir() else source
    metadata_path = session_path.parent / "nvv2t_md_end.csv"
    rows = _read_csv(session_path, REQUIRED_FIELDS, OPTIONAL_FIELDS)
    source_files = [_source_info(session_path, "charging_sessions_reference", len(rows))]
    records = []
    quality = Counter()
    seen_sessions = set()
    for row in rows:
        if any(not row[key] for key in ("sessionId", "userId", "stationId", "locationId", "facilityType")):
            raise ValueError("Reference identifiers/category must not be empty")
        hour_value = _number(row["startTime"], "startTime")
        if not hour_value.is_integer() or not 0 <= hour_value < 24:
            raise ValueError("startTime must be an integer in [0, 23]")
        if row["weekday"] not in WEEKDAYS:
            raise ValueError("weekday must be Mon/Tue/Wed/Thu/Fri/Sat/Sun")
        energy = _number(row["kwhTotal"], "kwhTotal")
        duration = _number(row["chargeTimeHrs"], "chargeTimeHrs")
        quality["nonpositive_energy_rows"] += int(energy <= 0)
        quality["nonpositive_connection_duration_rows"] += int(duration <= 0)
        quality["duplicate_session_identifiers"] += int(row["sessionId"] in seen_sessions)
        seen_sessions.add(row["sessionId"])
        if energy <= 0 or duration <= 0:
            quality["excluded_from_energy_connection_metrics"] += 1
        if row["created"]:
            quality["created_timestamp_rows"] += 1
            try:
                created = datetime.fromisoformat(row["created"])
            except ValueError:
                quality["unparseable_created_timestamps"] += 1
            else:
                quality["created_year_before_2000"] += int(created.year < 2000)
                quality["created_timezone_unspecified"] += int(created.tzinfo is None)
                quality["created_hour_mismatch"] += int(created.hour != int(hour_value))
                quality["created_weekday_mismatch"] += int(WEEKDAYS[created.weekday()] != row["weekday"])
                if row["ended"]:
                    try:
                        ended = datetime.fromisoformat(row["ended"])
                        seconds = (ended - created).total_seconds()
                    except (ValueError, TypeError):
                        quality["unparseable_or_incompatible_ended_timestamps"] += 1
                    else:
                        quality["end_before_start"] += int(seconds < 0)
                        quality["connection_duration_mismatch_over_one_second"] += int(abs(seconds - duration * 3600) > 1)
        records.append({"hour": int(hour_value), "weekday": row["weekday"],
                        "energy": energy, "connection_hours": duration,
                        "user": row["userId"], "station": row["stationId"],
                        "location": row["locationId"], "facility": row["facilityType"]})

    if quality["duplicate_session_identifiers"]:
        raise ValueError("Duplicate session identifiers: deduplicate explicitly before calibration")
    metadata_quality = {"present": metadata_path.is_file()}
    if metadata_path.is_file():
        metadata = _read_csv(metadata_path, METADATA_FIELDS)
        source_files.append(_source_info(metadata_path, "station_metadata_reference", len(metadata)))
        by_station = {row["stationId"]: row for row in metadata}
        if len(by_station) != len(metadata):
            raise ValueError("Duplicate station identifiers in metadata")
        metadata_quality.update(
            metadata_row_count=len(metadata),
            missing_station_references=sum(record["station"] not in by_station for record in records),
            location_pair_mismatches=sum(record["location"] != by_station[record["station"]]["locationId"]
                                         for record in records if record["station"] in by_station),
            facility_type_mismatches=sum(record["facility"] != by_station[record["station"]]["facilityType"]
                                         for record in records if record["station"] in by_station),
        )

    positive = [record for record in records if record["energy"] > 0 and record["connection_hours"] > 0]
    if not positive:
        raise ValueError("No positive-energy, positive-duration sessions for calibration")
    energy = [record["energy"] for record in positive]
    connection = [record["connection_hours"] for record in positive]
    energy_edges = [0, 2, 4, 6, 8, 12, 20, max(40, math.ceil(max(energy)))]
    connection_edges = [0, .5, 1, 2, 4, 8, 24, max(72, math.ceil(max(connection)))]
    joint = [[0] * (len(connection_edges) - 1) for _ in range(len(energy_edges) - 1)]
    for record in positive:
        joint[_bin_index(record["energy"], energy_edges)][_bin_index(record["connection_hours"], connection_edges)] += 1

    weekday_records = [record for record in records if record["weekday"] in WEEKDAYS[:5]]
    weekend_records = [record for record in records if record["weekday"] in WEEKDAYS[5:]]
    weekday_counts = Counter(record["weekday"] for record in records)
    facility_records, station_records, user_records = defaultdict(list), defaultdict(list), defaultdict(list)
    for record in records:
        facility_records[record["facility"]].append(record)
        station_records[record["station"]].append(record)
        user_records[record["user"]].append(record)

    facility_groups = []
    for index, (_, group) in enumerate(sorted(facility_records.items()), start=1):
        valid = [record for record in group if record["energy"] > 0 and record["connection_hours"] > 0]
        facility_groups.append({
            "group": f"reference_facility_group_{index}", "row_count": len(group),
            "hour_counts": _hour_counts(group),
            "weekday_counts": [sum(record["weekday"] == day for record in group) for day in WEEKDAYS],
            "energy_kwh": _summary([record["energy"] for record in valid]),
            "connection_hours": _summary([record["connection_hours"] for record in valid]),
        })

    min_station_sessions = 20
    station_peaks, station_peak_shares = [0] * 24, []
    for group in station_records.values():
        if len(group) < min_station_sessions:
            continue
        counts = _hour_counts(group)
        peak = max(counts)
        station_peaks[counts.index(peak)] += 1  # Earliest hour wins a tie.
        station_peak_shares.append(peak / len(group))

    user_counts = [len(group) for group in user_records.values()]
    repeat_groups = [group for group in user_records.values() if len(group) >= 2]
    top_station_shares = [max(Counter(record["station"] for record in group).values()) / len(group)
                          for group in repeat_groups]
    user_edges = [0, 1, 2, 5, 10, 20, 50, 100, max(200, max(user_counts))]
    quality_fields = (
        "nonpositive_energy_rows", "nonpositive_connection_duration_rows",
        "excluded_from_energy_connection_metrics", "duplicate_session_identifiers",
        "created_timestamp_rows", "unparseable_created_timestamps", "created_year_before_2000",
        "created_timezone_unspecified", "created_hour_mismatch", "created_weekday_mismatch",
        "unparseable_or_incompatible_ended_timestamps", "end_before_start",
        "connection_duration_mismatch_over_one_second",
    )
    return {
        "profile_version": PROFILE_VERSION,
        "provenance": {"source": "USER_PROVIDED_REFERENCE_AGGREGATES", "source_files": source_files,
                       "source_row_count": len(records), "energy_connection_sample_count": len(positive),
                       "privacy": "Aggregate counts, quantiles and histograms only; no identifiers, paths or raw sample pairs.",
                       "original_license_and_timezone_verified": False},
        "quality": {**{key: quality[key] for key in quality_fields}, "metadata": metadata_quality},
        "arrival": {
            "population": "All reference records, including zero-energy/nonpositive-duration records; these are recorded sessions, not all real-world attempted visits.",
            "hour_index": list(range(24)), "weekday_index": list(WEEKDAYS),
            "global_hour_counts": _hour_counts(records),
            "weekday_hour_counts": _hour_counts(weekday_records),
            "weekend_hour_counts": _hour_counts(weekend_records),
            "weekday_counts": [weekday_counts[day] for day in WEEKDAYS],
            "weekday_sample_count": len(weekday_records), "weekend_sample_count": len(weekend_records),
            "day_07_to_18_count": sum(7 <= record["hour"] < 18 for record in records),
            "night_18_to_07_count": sum(record["hour"] >= 18 or record["hour"] < 7 for record in records),
        },
        "facility_groups": facility_groups,
        "station_heterogeneity": {
            "distinct_station_count": len(station_records), "minimum_sessions_for_peak": min_station_sessions,
            "eligible_station_count": sum(station_peaks), "peak_hour_station_counts": station_peaks,
            "peak_tie_rule": "earliest_hour", "peak_hour_share": _summary(station_peak_shares),
            "facility_category_meanings_verified": False,
        },
        "session_metrics": {
            "population": "Only rows with energy_kwh > 0 AND connection_hours > 0; no outlier trimming.",
            "sample_count": len(positive),
            "quantile_method": "linear interpolation at (n-1)*p",
            "histogram_interval_rule": "[left,right), except final bin includes its right edge",
            "energy_kwh": _summary(energy, energy_edges),
            "connection_hours": _summary(connection, connection_edges),
            "connection_averaged_kw": _summary([value / hours for value, hours in zip(energy, connection)]),
            "energy_connection_pearson": _pearson(energy, connection),
            "energy_connection_joint": {"energy_edges": energy_edges,
                                        "connection_hour_edges": connection_edges, "counts": joint},
        },
        "users": {
            "distinct_user_count": len(user_records), "repeat_user_count": len(repeat_groups),
            "window_repeat_user_fraction": round(len(repeat_groups) / len(user_records), 8),
            "sessions_per_user": _summary(user_counts, user_edges),
            "top_station_share_among_repeat_users": _summary(top_station_shares),
            "interpretation": "Within-observation-window repeat use, not retention or a precise physical schedule constraint.",
        },
        "limitations": [
            "This is a user-supplied reference sample, not a representative survey of Chinese cities or proof that all charging demand peaks at night.",
            "Source timezone/year meaning is unverified. Original years are never corrected. No absolute daily visit-frequency calibration is produced.",
            "Hourly/weekday counts describe recorded sessions. Missing visits, failed attempts, operating exposure days and sampling selection are unknown.",
            "Energy and connection duration are jointly observed. Connection duration can include non-charging occupancy; energy/duration is not rated charger power or charging-only power.",
            "The positive-energy/positive-duration filter applies jointly to energy, duration, average-power, correlation and joint-histogram metrics; arrival counts retain all source records.",
            "Facility groups are anonymous categories. Their real site-type meanings and stationId-versus-connector semantics are not verified.",
            "Small weekend/category samples, a long-duration tail and selection bias remain. Fitted accuracy or real-world causality cannot be inferred from these aggregates.",
            "Synthetic residential/nighttime, high-power DC and other operational patterns require separately disclosed scenario assumptions, not relabeling this reference as evidence.",
            "Do not feed future observed energy/duration outcomes into forecasting features. These aggregates calibrate a simulator, not per-user or per-station replicas.",
        ],
    }


def write_profile(input_path, output_path):
    """Write a newly created JSON profile; refuse every existing destination."""
    destination = Path(output_path)
    if destination.exists() or destination.is_symlink():
        raise FileExistsError("Refusing to overwrite an existing reference profile")
    profile = calibrate_reference(input_path)
    payload = json.dumps(profile, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False) + "\n"
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(payload)
    return profile


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Reference directory or session CSV")
    parser.add_argument("--output", type=Path, required=True, help="New aggregate JSON file; never overwritten")
    args = parser.parse_args(argv)
    try:
        profile = write_profile(args.input, args.output)
    except (OSError, ValueError, csv.Error) as error:
        parser.error(str(error))
    print(f"Reference profile created: {profile['provenance']['source_row_count']} records, "
          f"{profile['session_metrics']['sample_count']} positive-energy/positive-duration sessions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
