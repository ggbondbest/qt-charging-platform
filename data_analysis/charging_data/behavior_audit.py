"""Read-only, streaming behavior audit of SIMULATED charging data.

python -m data_analysis.charging_data.behavior_audit --dataset PATH --output REPORT.json
This describes generated patterns, not empirical realism or predictive accuracy.
Only session-size collections are retained; telemetry is accumulated row by row.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
from datetime import datetime, timedelta, timezone
import gzip
import hashlib
import json
import math
from pathlib import Path

from .io import write_json
from .schema import STATE_VALUES, TABLES

BUSINESS_TZ = timezone(timedelta(hours=8))
DAY_TYPES = ("all_days", "weekday", "weekend")


def stamp(value):
    if not value.endswith("Z"):
        raise ValueError("Expected UTC timestamp ending in Z")
    return datetime.fromisoformat(value[:-1] + "+00:00")


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0, "mean": None, "min": None, "max": None,
                "quantiles": {key: None for key in ("p05", "p25", "p50", "p75", "p95", "p99")}}
    result = {}
    for key, probability in (("p05", .05), ("p25", .25), ("p50", .5),
                             ("p75", .75), ("p95", .95), ("p99", .99)):
        index = (len(values) - 1) * probability
        low = int(index)
        result[key] = round(values[low] + (values[min(low+1, len(values)-1)] - values[low]) * (index-low), 8)
    return dict(count=len(values), mean=round(sum(values)/len(values), 8),
                min=values[0], max=values[-1], quantiles=result)


class Reader:
    """Fail closed on changed inputs and unlisted/escaping part files."""

    def __init__(self, root):
        self.root = Path(root).resolve()
        path = self.root / "manifest.json"
        self.manifest_hash = digest(path)
        self.manifest = json.loads(path.read_text(encoding="utf-8"))
        if self.manifest.get("source") != "SIMULATED":
            raise ValueError("Behavior audit requires a SIMULATED dataset")
        if self.manifest.get("schema_version") != "1.1.0":
            raise ValueError("Behavior audit requires schema 1.1.0 including unplugged_at and energy ledger")
        self.checked = []

    def rows(self, table):
        catalog = self.manifest["tables"][table]
        paths = []
        for info in catalog["files"]:
            relative = Path(info["path"])
            if (relative.is_absolute() or ".." in relative.parts or
                    relative.parent.as_posix() != f"raw/{table}" or
                    not relative.name.startswith("part-") or not relative.name.endswith(".csv.gz")):
                raise ValueError(f"Unsafe manifest path for {table}")
            path = self.root / relative
            if path.is_symlink() or path.resolve().parent != (self.root / "raw" / table):
                raise ValueError(f"Part path escapes expected directory: {table}")
            paths.append((path, info))
        discovered = set((self.root / "raw" / table).glob("part-*.csv.gz"))
        if not paths or len({path for path, _ in paths}) != len(paths) or discovered != {path for path, _ in paths}:
            raise ValueError(f"Missing, duplicate or unlisted parts: {table}")
        total = 0
        for path, info in sorted(paths):
            if path.stat().st_size != info["bytes"] or digest(path) != info["sha256"]:
                raise ValueError(f"Checksum/size mismatch: {table}/{path.name}")
            count = 0
            with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
                reader = csv.DictReader(stream)
                if reader.fieldnames != TABLES[table]:
                    raise ValueError(f"Unexpected CSV header: {table}")
                for row in reader:
                    if None in row or None in row.values():
                        raise ValueError(f"Malformed CSV row: {table}")
                    count += 1
                    yield row
            if count != info["rows"]:
                raise ValueError(f"Part row-count mismatch: {table}")
            total += count
        if total != catalog["rows"]:
            raise ValueError(f"Table row-count mismatch: {table}")
        self.checked.append(table)

    def indexed(self, table, key):
        result = {}
        for row in self.rows(table):
            if not row[key] or row[key] in result:
                raise ValueError(f"Missing or duplicate key: {table}")
            result[row[key]] = row
        return result


def canonical_sessions(reader, stations):
    """Same documented cleaning cohort as validate.py; never alter raw files."""
    sessions, rejected, normalized = {}, Counter(), 0
    for raw in reader.rows("charging_sessions"):
        row = dict(raw)
        row["status"] = row["status"].strip().upper()
        normalized += row["status"] != raw["status"]
        if not row["session_id"]:
            rejected["MISSING_ID"] += 1
            continue
        if row["station_id"] not in stations:
            rejected["UNKNOWN_STATION"] += 1
            continue
        if int(row["total_fee_cents"]) < 0:
            rejected["NEGATIVE_FEE"] += 1
            continue
        if row["session_id"] in sessions:
            if row != sessions[row["session_id"]]:
                raise ValueError("Conflicting canonical session duplicate")
            rejected["DUPLICATE"] += 1
            continue
        sessions[row["session_id"]] = row
    observed = +Counter(dict(rejected, STATUS_FORMAT=normalized))
    declared = Counter()
    actions = {"MISSING_ID": "QUARANTINE", "UNKNOWN_STATION": "QUARANTINE",
               "NEGATIVE_FEE": "QUARANTINE", "DUPLICATE": "REMOVE_DUPLICATE", "STATUS_FORMAT": "NORMALIZE"}
    for row in reader.rows("corruption_log"):
        if (row["table_name"] != "charging_sessions" or row["record_id"] not in sessions or
                row["expected_action"] != actions.get(row["corruption_type"])):
            raise ValueError("Unexpected corruption-log action or canonical reference")
        declared[row["corruption_type"]] += 1
    if observed != declared or dict(observed) != reader.manifest.get("corruptions", {}):
        raise ValueError("Session cleaning counts do not reconcile with corruption log and manifest")
    if len(sessions) != reader.manifest["canonical_session_count"]:
        raise ValueError("Canonical session count mismatch")
    return sessions, dict(canonical_sessions=len(sessions), rejected_copies=dict(rejected), normalized_status_rows=normalized)


def bucket():
    return {key: [0] * 24 for key in ("attempt_count", "charging_seconds", "occupied_seconds", "observed_seconds", "energy_wh")}


def audit_behavior(dataset):
    reader = Reader(dataset)
    manifest = reader.manifest
    start, end = stamp(manifest["period_start"]), stamp(manifest["period_end_exclusive"])
    if manifest.get("business_timezone") != "Asia/Shanghai":
        raise ValueError("Only Asia/Shanghai business-day cohorts are supported")
    if (start.astimezone(BUSINESS_TZ).time() != datetime.min.time() or
            end.astimezone(BUSINESS_TZ).time() != datetime.min.time() or end <= start):
        raise ValueError("Dataset period must contain complete local business days")
    days = (end - start).days
    left, right = start + timedelta(days=1), end - timedelta(days=1)
    day_counts = Counter()
    for day in range(max(0, days-2)):
        when = (left + timedelta(days=day)).astimezone(BUSINESS_TZ)
        day_counts["all_days"] += 1
        day_counts["weekend" if when.weekday() >= 5 else "weekday"] += 1
    report = {
        "report_version": "1.0.0", "dataset_id": manifest["dataset_id"], "source": "SIMULATED",
        "manifest_sha256": reader.manifest_hash, "business_timezone": "Asia/Shanghai",
        "behavior_window": {"status": "COMPARED" if days >= 3 else "SKIPPED_INSUFFICIENT_INTERIOR_DAYS",
            "start_inclusive_utc": left.isoformat().replace("+00:00", "Z") if days >= 3 else None,
            "end_exclusive_utc": right.isoformat().replace("+00:00", "Z") if days >= 3 else None,
            "first_business_date": left.astimezone(BUSINESS_TZ).date().isoformat() if days >= 3 else None,
            "last_business_date": (right-timedelta(seconds=1)).astimezone(BUSINESS_TZ).date().isoformat() if days >= 3 else None,
            "excluded_edge_days": min(2, days), "day_counts": {key: day_counts[key] for key in DAY_TYPES}},
        "definitions": {
            "hour_index": "All 24-element arrays are local clock hours 0..23; Mon-Fri versus Sat-Sun, no holiday adjustment.",
            "attempt_count": "Initial platform requests in interior window, including failed/cancelled/queued requests; not electrical handshakes.",
            "utilization": "Percent of installed charger-seconds: CHARGING / denominator, or (CHARGING+OCCUPIED) / denominator. RESERVED, MAINTENANCE and OFFLINE remain in denominator.",
            "hour_denominator": "charger_count * selected_day_type_days * 3600 for EACH clock hour; zero-day cohorts return null, not zero utilization.",
            "energy_kwh": "Telemetry output Wh summed over selected intervals /1000, not nameplate-derived energy or session totals assigned to the starting hour.",
            "session_cohort": "Canonical sessions fully contained inside interior window: started_at >= window start and unplugged_at <= window end. Crossing sessions excluded from quantiles, but their interior telemetry still contributes.",
            "active_minutes": "ended_at-started_at; session interval duration, distinct from connection including post-charge occupation.",
            "connected_minutes": "unplugged_at-started_at; includes post-charge OCCUPIED, excludes initial queue/reservation waits.",
            "repeat_gap": "Consecutive canonical platform sessions of the same vehicle, both starts inside interior window; report start-to-start and previous-unplug-to-next-start hours.",
            "weather": "Interior-window hourly rows grouped by local month; averages of the available same-city hourly background, not city-wide station observations.",
            "energy_ledger": "Entire dataset window; interval totals cannot be exactly cut across interior/day boundaries without extra assumptions. External charging is SIMULATED and excluded from platform income and meters.",
            "quantiles": "Linear interpolation at (n-1)*p; null when no observations; memory proportional to sessions, not telemetry.",
        },
        "limitations": ["Generated behavior diagnostics are not evidence of real-market representativeness or forecast accuracy.",
            "First and last business days excluded due to empty initialization and clipped final stays.",
            "Checksums, schemas, cleaning and telemetry coverage checked here; run validate.py for full business/financial/energy consistency.",
            "Reference aggregates are not read; all statistics computed from raw files, not presented as Spark output."],
    }
    cities = reader.indexed("cities", "city_id")
    stations = reader.indexed("stations", "station_id")
    chargers = reader.indexed("chargers", "charger_id")
    vehicles = reader.indexed("vehicles", "vehicle_id")
    group_of = {sid: (row["city_id"], row["site_type"]) for sid, row in stations.items()}
    capacities = Counter()
    for row in chargers.values():
        capacities[group_of[row["station_id"]]] += 1
    groups = {(city, site, kind): bucket() for city, site in capacities for kind in DAY_TYPES}

    def cohorts(when):
        if not left <= when < right:
            return ()
        return ("all_days", "weekend" if when.astimezone(BUSINESS_TZ).weekday() >= 5 else "weekday")

    for row in reader.rows("charging_attempts"):
        when = stamp(row["attempted_at"])
        city, site = group_of[row["station_id"]]
        hour = when.astimezone(BUSINESS_TZ).hour
        for kind in cohorts(when):
            groups[city, site, kind]["attempt_count"][hour] += 1

    last_stamp = {}
    expected_interval = manifest["config"]["interval_minutes"] * 60
    if expected_interval <= 0 or 3600 % expected_interval:
        raise ValueError("Telemetry interval must evenly divide a clock hour")
    telemetry_rows = 0
    for row in reader.rows("charger_telemetry"):
        charger = chargers[row["charger_id"]]
        if charger["station_id"] != row["station_id"] or row["state"] not in STATE_VALUES:
            raise ValueError("Telemetry relation or state mismatch")
        when = stamp(row["recorded_at"])
        seconds, energy = int(row["interval_seconds"]), int(row["energy_wh"])
        if (seconds != expected_interval or seconds <= 0 or energy < 0 or
                not start <= when < end or (when-start).total_seconds() % seconds != 0 or
                (row["charger_id"] in last_stamp and when <= last_stamp[row["charger_id"]])):
            raise ValueError("Telemetry interval is invalid, duplicated or unordered")
        last_stamp[row["charger_id"]] = when
        telemetry_rows += 1
        city, site = group_of[row["station_id"]]
        hour = when.astimezone(BUSINESS_TZ).hour
        for kind in cohorts(when):
            item = groups[city, site, kind]
            item["observed_seconds"][hour] += seconds
            item["energy_wh"][hour] += energy
            if row["state"] == "CHARGING":
                item["charging_seconds"][hour] += seconds
            elif row["state"] == "OCCUPIED":
                item["occupied_seconds"][hour] += seconds
    expected_rows = len(chargers) * int((end-start).total_seconds()/expected_interval)
    if telemetry_rows != expected_rows:
        raise ValueError("Telemetry full-period row coverage mismatch")
    hourly = []
    for (city, site, kind), item in sorted(groups.items()):
        count = day_counts[kind]
        denominator = capacities[city, site] * count * 3600
        if any(value != denominator for value in item["observed_seconds"]):
            raise ValueError("Telemetry hourly charger-second coverage mismatch")
        active = item["charging_seconds"]
        occupied = [a+b for a, b in zip(active, item["occupied_seconds"])]
        energy = [value/1000 for value in item["energy_wh"]]
        attempts = item["attempt_count"]
        pct = lambda values: [round(value*100/denominator, 8) if denominator else None for value in values]
        hourly.append(dict(city_id=city, site_type=site, day_type=kind, days=count,
            charger_count=capacities[city, site], denominator_charger_seconds_per_hour=denominator,
            attempt_count=attempts, mean_attempt_count_per_day_by_hour=[round(v/count, 8) if count else None for v in attempts],
            charging_seconds=active, post_charge_occupied_seconds=item["occupied_seconds"],
            charging_utilization_pct=pct(active), charging_plus_occupied_utilization_pct=pct(occupied),
            energy_kwh=energy, total_attempts=sum(attempts), total_energy_kwh=round(sum(energy), 8),
            mean_attempts_per_day=round(sum(attempts)/count, 8) if count else None,
            mean_energy_kwh_per_day=round(sum(energy)/count, 8) if count else None))
    report["city_site_hourly"] = hourly

    sessions, report["cleaning"] = canonical_sessions(reader, stations)
    profiles = defaultdict(lambda: defaultdict(list))
    for charger in chargers.values():
        city, site = group_of[charger["station_id"]]
        for key in ("energy_kwh", "active_minutes", "connected_minutes", "start_soc_pct"):
            profiles[city, site, charger["connector_type"]][key] = []
    soc, by_vehicle = [], defaultdict(list)
    crossing = 0
    for row in sessions.values():
        begin, finish, unplug = (stamp(row[key]) for key in ("started_at", "ended_at", "unplugged_at"))
        if not begin <= finish <= unplug:
            raise ValueError("Session time order invalid")
        if row["vehicle_id"] not in vehicles or chargers[row["charger_id"]]["station_id"] != row["station_id"]:
            raise ValueError("Session foreign-key mismatch")
        if left <= begin < right:
            by_vehicle[row["vehicle_id"]].append((begin, unplug))
            if unplug > right:
                crossing += 1
                continue
            city, site = group_of[row["station_id"]]
            group = profiles[city, site, chargers[row["charger_id"]]["connector_type"]]
            values = dict(energy_kwh=int(row["energy_wh"])/1000,
                active_minutes=(finish-begin).total_seconds()/60,
                connected_minutes=(unplug-begin).total_seconds()/60,
                start_soc_pct=float(row["start_soc_pct"]))
            if not all(math.isfinite(value) and value >= 0 for value in values.values()) or values["start_soc_pct"] > 100:
                raise ValueError("Invalid session metric")
            for key, value in values.items():
                group[key].append(value)
            soc.append(values["start_soc_pct"])
    report["session_profiles"] = [dict(city_id=key[0], site_type=key[1], connector_type=key[2],
        **{metric: distribution(values) for metric, values in item.items()}) for key, item in sorted(profiles.items())]
    report["interior_start_sessions_crossing_right_boundary"] = crossing
    report["start_soc_pct"] = distribution(soc)
    starts, releases, repeat_vehicles = [], [], 0
    for events in by_vehicle.values():
        events.sort()
        repeat_vehicles += len(events) > 1
        for previous, current in zip(events, events[1:]):
            gap = (current[0]-previous[1]).total_seconds()/3600
            if gap < 0:
                raise ValueError("Vehicle platform stays overlap")
            starts.append((current[0]-previous[0]).total_seconds()/3600)
            releases.append(gap)
    report["revisit"] = dict(vehicles_with_interior_session=len(by_vehicle), vehicles_with_multiple_sessions=repeat_vehicles,
        start_to_start_hours=distribution(starts), unplug_to_next_start_hours=distribution(releases))

    weather = defaultdict(lambda: [0, 0.0, 0.0, 0.0])
    weather_last, weather_count = {}, 0
    for row in reader.rows("weather_hourly"):
        when = stamp(row["recorded_at"])
        city = row["city_id"]
        if (city not in cities or not start <= when < end or
                (when-start).total_seconds() % 3600 or
                (city in weather_last and when <= weather_last[city])):
            raise ValueError("Weather city, hourly interval or ordering mismatch")
        weather_last[city] = when
        weather_count += 1
        if cohorts(when):
            key = row["city_id"], when.astimezone(BUSINESS_TZ).strftime("%Y-%m")
            values = [float(row[field]) for field in ("temperature_c", "humidity_pct", "rainfall_mm")]
            if not all(math.isfinite(v) for v in values):
                raise ValueError("Nonfinite weather values")
            weather[key][0] += 1
            for index, value in enumerate(values, 1):
                weather[key][index] += value
    if weather_count != len(cities)*days*24:
        raise ValueError("Weather full-period hourly coverage mismatch")
    report["weather_city_month"] = [dict(city_id=city, month=month, hours=value[0],
        mean_temperature_c=round(value[1]/value[0], 6), mean_humidity_pct=round(value[2]/value[0], 6),
        total_preceding_hour_precipitation_mm=round(value[3], 6)) for (city, month), value in sorted(weather.items())]
    ledger = defaultdict(Counter)
    for row in reader.rows("vehicle_energy_intervals"):
        vehicle = vehicles[row["vehicle_id"]]
        # The user identifier carries no location assumptions: infer the city
        # from the separately loaded users table below, not from ID spelling.
        drive, external = int(row["driving_wh"]), int(row["external_charge_wh"])
        if drive < 0 or external < 0:
            raise ValueError("Negative energy-ledger value")
        item = ledger[vehicle["user_id"]]
        item.update(intervals=1, driving_wh=drive, external_charge_wh=external,
                    intervals_with_external_charge=int(external > 0))
    users = reader.indexed("users", "user_id")
    city_ledger = defaultdict(Counter)
    for user, item in ledger.items():
        city_ledger[users[user]["home_city_id"]].update(item)
    report["full_window_energy_ledger"] = dict(scope="ENTIRE_DATASET_NOT_INTERIOR_COMPARISON",
        external_charging_is_simulated=True, included_in_platform_revenue=False,
        total_external_charge_kwh=round(sum(item["external_charge_wh"] for item in city_ledger.values())/1000, 8),
        cities=[dict(city_id=city, intervals=item["intervals"],
            intervals_with_external_charge=item["intervals_with_external_charge"],
            driving_kwh=item["driving_wh"]/1000, external_charge_kwh=item["external_charge_wh"]/1000)
            for city, item in sorted(city_ledger.items())])
    report["input_checks"] = dict(checked_tables=sorted(reader.checked), telemetry_rows_streamed=telemetry_rows,
        all_read_parts_verified_against_manifest=True, telemetry_retained_in_memory=False)
    return report


def write_report(dataset, output):
    output = Path(output)
    if output.exists() or output.is_symlink():
        raise FileExistsError("Refusing to overwrite existing output")
    report = audit_behavior(dataset)
    write_json(output, report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        report = write_report(args.dataset, args.output)
    except (OSError, ValueError, KeyError, TypeError, csv.Error, EOFError) as exc:
        parser.error(str(exc))
    print(f"Behavior audit: {report['dataset_id']}; {report['behavior_window']['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
