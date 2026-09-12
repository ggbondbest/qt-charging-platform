"""Independent, streaming validation of a generated charging dataset.

The deliberately dirty session copies are reconciled with corruption_log. Every
other relationship and all canonical sessions must obey the business contract.
This module never modifies input data or trusts a generator's validation result.
Only an explicitly requested, exclusively created JSON report is written.
"""

from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import math
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path

from .schema import SCHEMA_VERSION, STATE_VALUES, SUMMARY_TABLES, TABLES

_COVERAGE_FIELDS = {
    "stations": ["site_type"], "chargers": ["connector_type"],
    "users": ["segment", "acquisition_channel", "membership"],
    "charging_attempts": ["outcome", "failure_reason"],
    "reservations": ["status"], "queue_entries": ["status"],
    "payments": ["transaction_type", "status", "channel"],
    "maintenance_tickets": ["fault_type", "status"],
    "reviews": ["rating", "issue_type"], "anomaly_labels": ["anomaly_type"],
    "weather_hourly": ["weather"], "charger_telemetry": ["state"],
}


def _stamp(value):
    if not value or not value.endswith("Z"):
        raise ValueError("timestamp must be ISO 8601 UTC ending in Z")
    return datetime.fromisoformat(value[:-1] + "+00:00")


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class _Audit:
    def __init__(self, root):
        self.root = Path(root)
        self.errors = []
        self._seen_errors = set()
        self.error_count = 0
        self.counts = Counter()
        self.check_count = 0
        self.file_counts = Counter()
        self.coverage = defaultdict(Counter)

    def check(self, condition, message):
        self.check_count += 1
        if not condition:
            self.error_count += 1
            if len(self.errors) < 100 and message not in self._seen_errors:
                self.errors.append(message)
                self._seen_errors.add(message)
        return bool(condition)

    def rows(self, table):
        files = sorted((self.root / "raw" / table).glob("part-*.csv.gz"))
        self.check(bool(files), f"{table}: no CSV gzip part files")
        for path in files:
            try:
                with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
                    reader = csv.DictReader(stream)
                    if not self.check(reader.fieldnames == TABLES[table],
                                      f"{table}: unexpected header in {path.name}"):
                        continue
                    for row in reader:
                        self.counts[table] += 1
                        self.file_counts[path.relative_to(self.root).as_posix()] += 1
                        if self.check(None not in row and None not in row.values(),
                                      f"{table}: malformed CSV row {reader.line_num}"):
                            for field in _COVERAGE_FIELDS.get(table, []):
                                self.coverage[f"{table}.{field}"][row[field] or "(empty)"] += 1
                            yield row
            except (OSError, EOFError, csv.Error, UnicodeError) as exc:
                self.check(False, f"{table}: cannot read {path.name}: {exc}")

    def indexed(self, table, key):
        result = {}
        for row in self.rows(table):
            value = row[key]
            self.check(bool(value), f"{table}: missing {key}")
            self.check(value not in result, f"{table}: duplicate {key} {value}")
            result[value] = row
        return result

    def relation(self, row, key, target, context, optional=False):
        value = row[key]
        return self.check((optional and not value) or value in target,
                          f"{context}: unknown {key} {value}")

    def timestamp(self, value, context, optional=False):
        if optional and not value:
            return None
        try:
            return _stamp(value)
        except (ValueError, TypeError) as exc:
            self.check(False, f"{context}: {exc}: {value!r}")
            return None

    def ordered(self, row, fields, context):
        values = [self.timestamp(row[field], f"{context}.{field}")
                  for field in fields if row[field]]
        if all(value is not None for value in values):
            self.check(values == sorted(values), f"{context}: timestamps are out of order")


def _validate(root):
    audit = _Audit(root)
    manifest_path = audit.root / "manifest.json"
    if not manifest_path.is_file():
        audit.check(False, "manifest.json is missing")
        return _report(audit)
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        audit.check(False, f"cannot read manifest.json: {exc}")
        return _report(audit)

    audit.check(manifest.get("schema_version") == SCHEMA_VERSION,
                "manifest schema_version does not match supported schema")
    config = manifest.get("config", {})
    audit.check(manifest.get("source") == "SIMULATED", "dataset must be explicitly labeled SIMULATED")
    audit.check(manifest.get("dataset_id") == config.get("dataset_id"), "dataset_id/config mismatch")
    cities = audit.indexed("cities", "city_id")
    stations = audit.indexed("stations", "station_id")
    chargers = audit.indexed("chargers", "charger_id")
    users = audit.indexed("users", "user_id")
    vehicles = audit.indexed("vehicles", "vehicle_id")
    campaigns = audit.indexed("campaigns", "campaign_id")
    tariffs = list(audit.rows("tariffs"))
    tariff_index = {(row["city_id"], int(row["hour"])): row for row in tariffs}
    audit.check(len(cities) == 5 and len(stations) == 25 and len(chargers) == 75,
                "expected 5 cities, 25 stations and 75 chargers")
    audit.check(len(tariff_index) == 24 * len(cities), "tariffs must cover every city/hour exactly once")
    for row in stations.values():
        audit.relation(row, "city_id", cities, "stations")
        audit.timestamp(row["opened_at"], "stations.opened_at")
    for row in chargers.values():
        audit.relation(row, "station_id", stations, "chargers")
        audit.check(float(row["rated_power_kw"]) > 0, "charger power must be positive")
        audit.timestamp(row["commissioned_at"], "chargers.commissioned_at")
    for row in users.values():
        audit.relation(row, "home_city_id", cities, "users")
        audit.timestamp(row["registered_at"], "users.registered_at")
    for row in vehicles.values():
        audit.relation(row, "user_id", users, "vehicles")
        audit.check(float(row["battery_capacity_kwh"]) > 0, "vehicle battery capacity must be positive")
    for row in campaigns.values():
        audit.relation(row, "city_id", cities, "campaigns")
        audit.ordered(row, ["starts_at", "ends_at"], "campaigns")

    attempts = audit.indexed("charging_attempts", "attempt_id")
    reservations = audit.indexed("reservations", "reservation_id")
    queues = audit.indexed("queue_entries", "queue_id")
    tickets = audit.indexed("maintenance_tickets", "ticket_id")
    corruption = list(audit.rows("corruption_log"))
    audit.check(len({row["corruption_id"] for row in corruption}) == len(corruption),
                "corruption_log has duplicate corruption_id")
    sessions = {}
    rejected = Counter()
    normalized = 0
    for raw in audit.rows("charging_sessions"):
        row = dict(raw)
        row["status"] = row["status"].strip().upper()
        normalized += row["status"] != raw["status"]
        if not row["session_id"]:
            rejected["MISSING_SESSION_ID"] += 1
            continue
        if row["station_id"] not in stations:
            rejected["UNKNOWN_STATION"] += 1
            continue
        if int(row["total_fee_cents"]) < 0:
            rejected["NEGATIVE_TOTAL_FEE"] += 1
            continue
        if row["session_id"] in sessions:
            rejected["DUPLICATE"] += 1
            audit.check(row == sessions[row["session_id"]],
                        f"conflicting valid duplicate session {row['session_id']}")
            continue
        sessions[row["session_id"]] = row

    # Mutation rows and normalizations must be declared, not silently excused.
    logged_actions = Counter(row["expected_action"].strip().upper() for row in corruption)
    actual_dirty = sum(rejected.values()) + normalized
    audit.check(actual_dirty == len(corruption),
                f"corruption_log mismatch: detected {actual_dirty}, declared {len(corruption)}")
    audit.check(all(row["table_name"] == "charging_sessions" for row in corruption),
                "corruption outside charging_sessions is not supported by this contract")
    declared_corruptions = Counter(row["corruption_type"] for row in corruption)
    observed_corruptions = Counter({"DUPLICATE": rejected["DUPLICATE"],
        "NEGATIVE_FEE": rejected["NEGATIVE_TOTAL_FEE"], "UNKNOWN_STATION": rejected["UNKNOWN_STATION"],
        "MISSING_ID": rejected["MISSING_SESSION_ID"], "STATUS_FORMAT": normalized})
    audit.check(dict(+declared_corruptions) == dict(+observed_corruptions),
                "corruption_log types do not match the observed dirty records")
    audit.check(dict(+declared_corruptions) == manifest.get("corruptions"),
                "manifest corruption breakdown does not match corruption_log")
    for row in corruption:
        audit.check(row["record_id"] in sessions, "corruption_log does not refer to a canonical session")
        expected_action = {"STATUS_FORMAT": "NORMALIZE", "DUPLICATE": "REMOVE_DUPLICATE",
                           "NEGATIVE_FEE": "QUARANTINE", "UNKNOWN_STATION": "QUARANTINE",
                           "MISSING_ID": "QUARANTINE"}.get(row["corruption_type"])
        audit.check(row["expected_action"] == expected_action, "corruption_log cleaning action mismatch")

    sessions_by_charger = defaultdict(list)
    sessions_by_vehicle = defaultdict(list)
    campaign_spend = Counter()
    for session_id, row in sessions.items():
        context = f"session {session_id}"
        for field in ["status", "stop_reason", "target_mode"]:
            audit.coverage[f"canonical_sessions.{field}"][row[field]] += 1
        for key, target in [("user_id", users), ("vehicle_id", vehicles),
                            ("charger_id", chargers), ("attempt_id", attempts)]:
            audit.relation(row, key, target, context)
        audit.relation(row, "campaign_id", campaigns, context, optional=True)
        if row["vehicle_id"] in vehicles:
            audit.check(vehicles[row["vehicle_id"]]["user_id"] == row["user_id"],
                        f"{context}: vehicle belongs to another user")
            expected_soc = float(row["start_soc_pct"]) + int(row["energy_wh"]) / (
                float(vehicles[row["vehicle_id"]]["battery_capacity_kwh"]) * 10)
            audit.check(abs(expected_soc - float(row["end_soc_pct"])) <= 0.00011,
                        f"{context}: SOC increase does not match battery energy")
        if row["charger_id"] in chargers:
            audit.check(chargers[row["charger_id"]]["station_id"] == row["station_id"],
                        f"{context}: charger belongs to another station")
        if row["attempt_id"] in attempts:
            attempt = attempts[row["attempt_id"]]
            for field in ["session_id", "user_id", "vehicle_id", "station_id", "charger_id"]:
                audit.check(attempt[field] == row[field], f"{context}: attempt {field} mismatch")
        audit.ordered(row, ["started_at", "ended_at", "unplugged_at"], context)
        if row["user_id"] in users:
            audit.check(users[row["user_id"]]["registered_at"] <= row["started_at"],
                        f"{context}: user has not registered yet")
        for field in TABLES["charging_sessions"]:
            if field.endswith("_cents") or field.endswith("_wh"):
                audit.check(int(row[field]) >= 0, f"{context}: negative {field}")
        audit.check(int(row["total_fee_cents"]) == int(row["electricity_fee_cents"]) +
                    int(row["service_fee_cents"]) + int(row["parking_fee_cents"]) -
                    int(row["discount_cents"]), f"{context}: fee formula is inconsistent")
        audit.check(0 <= float(row["start_soc_pct"]) <= float(row["end_soc_pct"]) <= 100,
                    f"{context}: invalid SOC range")
        audit.check(row["status"] in {"COMPLETED", "WAITING_PAYMENT"},
                    f"{context}: unknown canonical status")
        audit.check(row["target_mode"] == "ENERGY", f"{context}: unsupported target mode")
        target = int(row["target_value"])
        charged = int(row["energy_wh"])
        audit.check(target > 0 and 0 < charged <= target,
                    f"{context}: charged energy exceeds requested ENERGY target or is not positive")
        audit.check(row["stop_reason"] in {"TARGET_REACHED", "DURATION_LIMIT", "USER_STOPPED"},
                    f"{context}: unknown charging stop reason")
        if row["stop_reason"] == "TARGET_REACHED":
            audit.check(charged == target, f"{context}: TARGET_REACHED energy does not equal requested target")
        else:
            audit.check(charged < target, f"{context}: early/limited stop must be below requested target")
        sessions_by_charger[row["charger_id"]].append(row)
        sessions_by_vehicle[row["vehicle_id"]].append(row)
        if row["campaign_id"]:
            campaign_spend[row["campaign_id"]] += int(row["discount_cents"])
    for charger_id, rows in sessions_by_charger.items():
        rows.sort(key=lambda row: row["started_at"])
        for previous, current in zip(rows, rows[1:]):
            audit.check(previous["unplugged_at"] <= current["started_at"],
                        f"{charger_id}: charging/parking sessions overlap")
    for vehicle_id, rows in sessions_by_vehicle.items():
        rows.sort(key=lambda row: row["started_at"])
        for previous, current in zip(rows, rows[1:]):
            audit.check(previous["unplugged_at"] <= current["started_at"],
                        f"{vehicle_id}: vehicle appears in overlapping charging/parking sessions")
    for campaign_id, spend in campaign_spend.items():
        if campaign_id in campaigns:
            audit.check(spend <= int(campaigns[campaign_id]["budget_cents"]),
                        f"campaign {campaign_id}: budget exceeded")

    for row in attempts.values():
        for key, target in [("user_id", users), ("vehicle_id", vehicles),
                            ("station_id", stations)]:
            audit.relation(row, key, target, "charging_attempts")
        for key, target in [("charger_id", chargers), ("session_id", sessions),
                            ("queue_id", queues), ("reservation_id", reservations)]:
            audit.relation(row, key, target, "charging_attempts", optional=True)
        audit.timestamp(row["attempted_at"], "charging_attempts.attempted_at")
        if row["user_id"] in users:
            audit.check(users[row["user_id"]]["registered_at"] <= row["attempted_at"],
                        "attempt precedes user registration")
    for table, records, time_fields in [
        ("reservations", reservations, ["created_at", "resolved_at"]),
        ("queue_entries", queues, ["joined_at", "called_at", "resolved_at"]),
    ]:
        for row in records.values():
            for key, target in [("user_id", users), ("station_id", stations)]:
                audit.relation(row, key, target, table)
            audit.relation(row, "session_id", sessions, table, optional=True)
            audit.ordered(row, time_fields, table)
            if table == "reservations":
                audit.relation(row, "charger_id", chargers, table)
                audit.ordered(row, ["created_at", "expires_at"], table)
            else:
                audit.check(int(row["position_at_join"]) > 0, "queue position must be positive")

    maintenance_by_charger = defaultdict(list)
    for row in tickets.values():
        audit.relation(row, "charger_id", chargers, "maintenance_tickets")
        audit.relation(row, "station_id", stations, "maintenance_tickets")
        audit.ordered(row, ["reported_at", "accepted_at", "work_started_at", "restored_at"],
                      "maintenance_tickets")
        audit.check(row["status"] in {"RESOLVED", "IN_PROGRESS"}, "unexpected ticket status")
        if row["reported_at"]:
            maintenance_by_charger[row["charger_id"]].append(
                (row["reported_at"], row["restored_at"] or "9999"))
    for charger_id, ranges in maintenance_by_charger.items():
        for row in sessions_by_charger[charger_id]:
            for start, end in ranges:
                audit.check(row["unplugged_at"] <= start or row["started_at"] >= end,
                            f"{charger_id}: session overlaps confirmed maintenance")
    event_last = {}
    for row in audit.rows("maintenance_events"):
        audit.relation(row, "ticket_id", tickets, "maintenance_events")
        audit.timestamp(row["event_at"], "maintenance_events.event_at")
        previous = event_last.get(row["ticket_id"])
        audit.check(previous is None or previous <= row["event_at"], "maintenance event order invalid")
        event_last[row["ticket_id"]] = row["event_at"]

    payments = Counter()
    refunds = Counter()
    payment_times = {}
    refund_times = defaultdict(list)
    payment_ids = set()
    for row in audit.rows("payments"):
        audit.check(row["payment_id"] not in payment_ids, "duplicate payment_id")
        payment_ids.add(row["payment_id"])
        audit.relation(row, "session_id", sessions, "payments")
        audit.relation(row, "user_id", users, "payments")
        audit.timestamp(row["occurred_at"], "payments.occurred_at")
        amount = int(row["amount_cents"])
        audit.check(amount >= 0, "payment amount must be nonnegative")
        audit.check(row["transaction_type"] in {"PAYMENT", "REFUND"}, "unknown payment type")
        audit.check(row["status"] in {"SUCCESS", "FAILED"}, "unknown payment status")
        if row["session_id"] in sessions:
            session = sessions[row["session_id"]]
            audit.check(session["user_id"] == row["user_id"], "payment user/session mismatch")
            audit.check(row["occurred_at"] >= session["ended_at"], "payment precedes charging end")
        if row["status"] == "SUCCESS":
            (payments if row["transaction_type"] == "PAYMENT" else refunds)[row["session_id"]] += amount
            if row["transaction_type"] == "PAYMENT":
                payment_times[row["session_id"]] = min(row["occurred_at"],
                    payment_times.get(row["session_id"], row["occurred_at"]))
            else:
                refund_times[row["session_id"]].append(row["occurred_at"])
    for session_id, row in sessions.items():
        audit.check(payments[session_id] <= int(row["total_fee_cents"]), f"{session_id}: overpayment")
        audit.check(refunds[session_id] <= payments[session_id], f"{session_id}: refund exceeds paid amount")
        expected = int(row["total_fee_cents"]) if row["status"] == "COMPLETED" else 0
        audit.check(payments[session_id] == expected, f"{session_id}: status/payment mismatch")
        for refund_at in refund_times[session_id]:
            audit.check(session_id in payment_times and payment_times[session_id] <= refund_at,
                        f"{session_id}: refund precedes successful payment")

    for row in audit.rows("reviews"):
        for key, target in [("session_id", sessions), ("user_id", users), ("station_id", stations)]:
            audit.relation(row, key, target, "reviews")
        audit.check(1 <= int(row["rating"]) <= 5, "review rating outside 1..5")
        audit.timestamp(row["created_at"], "reviews.created_at")
        if row["session_id"] in sessions:
            audit.check(row["created_at"] >= sessions[row["session_id"]]["ended_at"],
                        "review precedes charging end")

    _validate_telemetry(audit, config, chargers, stations, vehicles, sessions, tariff_index)
    _validate_battery(audit, config, sessions, chargers)
    for row in audit.rows("anomaly_labels"):
        audit.relation(row, "session_id", sessions, "anomaly_labels")
        audit.timestamp(row["recorded_at"], "anomaly_labels.recorded_at")
        if row["session_id"] in sessions:
            session = sessions[row["session_id"]]
            audit.check(session["started_at"] <= row["recorded_at"] < session["ended_at"],
                        "anomaly label is outside its charging session")
            if row["anomaly_type"] == "EARLY_STOP":
                audit.check(int(session["energy_wh"]) < int(session["target_value"]),
                            f"{row['session_id']}: EARLY_STOP must retain an unmet original target")
    for table in ["calendar", "weather_hourly", "operating_costs"]:
        seen = set()
        for row in audit.rows(table):
            key = "station_id" if table == "operating_costs" else "city_id"
            audit.relation(row, key, stations if key == "station_id" else cities, table)
            time_key = "recorded_at" if table == "weather_hourly" else "business_date"
            unique = (row[key], row[time_key])
            audit.check(unique not in seen, f"{table}: duplicate dimension/time row")
            seen.add(unique)
            if time_key == "recorded_at":
                audit.timestamp(row[time_key], table)
                audit.check(0 <= float(row["humidity_pct"]) <= 100, "humidity outside 0..100")
                audit.check(float(row["rainfall_mm"]) >= 0, "negative rainfall")
            else:
                datetime.strptime(row[time_key], "%Y-%m-%d")
    _validate_manifest(audit, manifest)
    return _report(audit, dataset_id=manifest.get("dataset_id"), schema_version=SCHEMA_VERSION,
                   manifest_sha256=_sha256(manifest_path), source=manifest.get("source"),
                   canonical_sessions=len(sessions), dirty_rows=actual_dirty,
                   rejected_session_copies=dict(rejected), normalized_rows=normalized,
                   corruption_actions=dict(logged_actions),
                   business_totals={
                       "delivered_energy_wh": sum(int(row["energy_wh"]) for row in sessions.values()),
                       "grid_energy_wh": sum(int(row.get("grid_energy_wh", 0)) for row in sessions.values()),
                       "gross_session_fee_cents": sum(int(row["total_fee_cents"]) for row in sessions.values()),
                       "successful_payment_cents": sum(payments.values()),
                       "successful_refund_cents": sum(refunds.values()),
                       "grid_cost_cents": sum(int(row["grid_cost_cents"]) for row in sessions.values()),
                   })


def _validate_telemetry(audit, config, chargers, stations, vehicles, sessions, tariffs):
    totals = defaultdict(lambda: Counter())
    last = {}
    counts = Counter()
    interval = int(config.get("interval_minutes", 5)) * 60
    period_start = None
    period_end = None
    if config.get("start_date") and config.get("days"):
        period_start = datetime.strptime(config["start_date"], "%Y-%m-%d").replace(
            tzinfo=timezone(timedelta(hours=8))).astimezone(timezone.utc)
        period_end = period_start + timedelta(days=int(config["days"]))
    date_cache = {}
    station_time = defaultdict(float)
    current_time = None

    def check_capacity():
        for station_id, power in station_time.items():
            if station_id in stations:
                audit.check(power <= float(stations[station_id]["transformer_kw"]) + 0.01,
                            f"{station_id}: station transformer power exceeded at {current_time}")
        station_time.clear()

    for row in audit.rows("charger_telemetry"):
        charger_id, station_id, recorded = row["charger_id"], row["station_id"], row["recorded_at"]
        state = row["state"].strip().upper()
        audit.relation(row, "charger_id", chargers, "charger_telemetry")
        audit.relation(row, "station_id", stations, "charger_telemetry")
        if recorded not in date_cache:
            date_cache[recorded] = audit.timestamp(recorded, "charger_telemetry.recorded_at")
        moment = date_cache[recorded]
        seconds, energy, meter = int(row["interval_seconds"]), int(row["energy_wh"]), int(row["meter_wh"])
        power = float(row["power_kw"])
        grid_energy, grid_cost = int(row.get("grid_energy_wh", energy)), int(row.get("grid_cost_cents", 0))
        audit.check(seconds == interval, "telemetry interval does not match config")
        audit.check(state in STATE_VALUES, f"telemetry unknown state {state}")
        audit.check(energy >= 0 and grid_energy >= energy and grid_cost >= 0 and meter >= 0,
                    "telemetry has invalid energy/cost/meter")
        audit.check(math.isfinite(power) and power >= 0 and
                    abs(power - energy * 3.6 / max(seconds, 1)) <= 0.00011,
                    "telemetry power does not match interval energy")
        if charger_id in chargers:
            charger = chargers[charger_id]
            audit.check(charger["station_id"] == station_id, "telemetry charger/station mismatch")
            audit.check(power <= float(charger["rated_power_kw"]) + 0.00011,
                        "telemetry exceeds charger rated power")
        previous = last.get(charger_id)
        if previous is None and period_start is not None:
            audit.check(moment == period_start, f"{charger_id}: first telemetry interval is not dataset start")
            audit.check(meter == energy, f"{charger_id}: initial meter is not zero-based interval end energy")
        if previous and moment is not None and previous[0] is not None:
            audit.check(moment - previous[0] == timedelta(seconds=seconds),
                        f"{charger_id}: missing, duplicate or unordered telemetry")
            audit.check(meter - previous[1] == energy, f"{charger_id}: cumulative meter mismatch")
        last[charger_id] = (moment, meter)
        counts[charger_id] += 1
        audit.check(row["online"] in {"0", "1"}, "online must be 0 or 1")
        audit.check((row["online"] == "0") == (state == "OFFLINE"), "online/state mismatch")
        if state == "CHARGING":
            if audit.relation(row, "session_id", sessions, "charging telemetry"):
                session = sessions[row["session_id"]]
                audit.check(session["charger_id"] == charger_id, "telemetry session/charger mismatch")
                audit.check(session["started_at"] <= recorded < session["ended_at"],
                            "charging telemetry outside session interval")
                if session["vehicle_id"] in vehicles:
                    audit.check(power <= float(vehicles[session["vehicle_id"]]["max_charge_kw"]) + 0.00011,
                                "telemetry exceeds vehicle power limit")
                total = totals[row["session_id"]]
                total["energy_wh"] += energy
                total["grid_energy_wh"] += grid_energy
                total["grid_cost_cents"] += grid_cost
                total["samples"] += 1
                if moment is not None and station_id in stations:
                    city = stations[station_id]["city_id"]
                    tariff = tariffs.get((city, (moment.hour + 8) % 24))
                    if tariff:
                        for field, target in [("energy_price_cents_per_kwh", "electricity_fee_cents"),
                                              ("service_price_cents_per_kwh", "service_fee_cents")]:
                            total[target] += (energy * int(tariff[field]) + 500) // 1000
                        expected_cost = (grid_energy * int(tariff["grid_price_cents_per_kwh"]) + 500) // 1000
                        audit.check(grid_cost == expected_cost, "telemetry grid cost/tariff mismatch")
        else:
            audit.check(energy == 0 and grid_energy == 0 and grid_cost == 0,
                        "non-charging interval consumes charging energy")
            if state == "OCCUPIED":
                if audit.relation(row, "session_id", sessions, "occupied telemetry"):
                    session = sessions[row["session_id"]]
                    audit.check(session["ended_at"] <= recorded < session["unplugged_at"],
                                "occupied telemetry outside parking interval")
            else:
                audit.check(not row["session_id"], "inactive telemetry unexpectedly links a session")
        if current_time != recorded:
            check_capacity()
            current_time = recorded
        station_time[station_id] += power
    check_capacity()
    expected_count = int(config.get("days", 0)) * 24 * 60 // max(interval // 60, 1)
    if expected_count:
        for charger_id in chargers:
            audit.check(counts[charger_id] == expected_count,
                        f"{charger_id}: expected {expected_count} telemetry intervals, found {counts[charger_id]}")
            if charger_id in last and period_end is not None:
                audit.check(last[charger_id][0] == period_end - timedelta(seconds=interval),
                            f"{charger_id}: final telemetry interval is not dataset end")
    for session_id, session in sessions.items():
        total = totals[session_id]
        for field in ["energy_wh", "grid_energy_wh", "grid_cost_cents",
                      "electricity_fee_cents", "service_fee_cents"]:
            if field in session:
                audit.check(int(session[field]) == total[field], f"{session_id}: telemetry/session {field} mismatch")
        start, end = _stamp(session["started_at"]), _stamp(session["ended_at"])
        audit.check(total["samples"] * interval == int((end - start).total_seconds()),
                    f"{session_id}: incomplete charging telemetry")


def _validate_battery(audit, config, sessions, chargers):
    last = {}
    counts = Counter()
    for row in audit.rows("battery_samples"):
        session_id = row["session_id"]
        audit.relation(row, "session_id", sessions, "battery_samples")
        audit.relation(row, "charger_id", chargers, "battery_samples")
        audit.timestamp(row["recorded_at"], "battery_samples.recorded_at")
        soc = float(row["soc_pct"])
        audit.check(0 <= soc <= 100, "battery SOC outside 0..100")
        if session_id in sessions:
            session = sessions[session_id]
            audit.check(session["charger_id"] == row["charger_id"], "battery charger/session mismatch")
            audit.check(session["started_at"] <= row["recorded_at"] < session["ended_at"],
                        "battery sample outside charging interval")
            audit.check(float(session["start_soc_pct"]) - 0.02 <= soc <= float(session["end_soc_pct"]) + 0.02,
                        "battery SOC outside session range")
        previous = last.get(session_id)
        if previous:
            audit.check(previous[0] < row["recorded_at"] and previous[1] <= soc + 0.02,
                        "battery samples duplicated, unordered or decreasing SOC")
        last[session_id] = (row["recorded_at"], soc)
        counts[session_id] += 1
        audit.check(float(row["max_cell_voltage_v"]) >= float(row["min_cell_voltage_v"]),
                    "battery max cell voltage below min")
        audit.check(float(row["max_temperature_c"]) >= float(row["min_temperature_c"]),
                    "battery max temperature below min")
    interval = int(config.get("interval_minutes", 5)) * 60
    for session_id, row in sessions.items():
        expected = int((_stamp(row["ended_at"]) - _stamp(row["started_at"])).total_seconds()) // interval
        audit.check(counts[session_id] == expected, f"{session_id}: missing or duplicate charging battery samples")


def _validate_manifest(audit, manifest):
    """Verify both compressed bytes and row counts, not only manifest claims."""
    tables = manifest.get("tables", {})
    audit.check(set(tables) == set(TABLES), "manifest tables do not match schema tables")
    declared_files = set()
    for table, entry in tables.items():
        if table not in TABLES:
            continue
        if isinstance(entry, int):
            expected_rows, files = entry, []
        else:
            expected_rows = entry.get("rows", entry.get("row_count"))
            files = entry.get("files", [])
        audit.check(expected_rows == audit.counts[table],
                    f"manifest {table}: rows {expected_rows} != observed {audit.counts[table]}")
        if isinstance(files, dict):
            files = [dict(metadata, path=path) if isinstance(metadata, dict)
                     else {"path": path, "sha256": metadata} for path, metadata in files.items()]
        for file_info in files:
            if not isinstance(file_info, dict):
                audit.check(False, f"manifest {table}: file lacks checksum metadata")
                continue
            relative = file_info.get("path", file_info.get("file", ""))
            audit.check(relative not in declared_files, f"manifest lists file twice: {relative}")
            declared_files.add(relative)
            path = (audit.root / relative).resolve()
            if not audit.check(path.is_relative_to(audit.root.resolve()), "manifest file escapes dataset directory"):
                continue
            if not audit.check(path.is_file(), f"manifest file missing: {relative}"):
                continue
            audit.check(file_info.get("sha256") == _sha256(path), f"checksum mismatch: {relative}")
            if "rows" in file_info:
                audit.check(file_info["rows"] == audit.file_counts[relative], f"file row count mismatch: {relative}")
            if "bytes" in file_info:
                audit.check(file_info["bytes"] == path.stat().st_size, f"file byte count mismatch: {relative}")
    actual_files = {path.relative_to(audit.root).as_posix()
                    for path in (audit.root / "raw").rglob("*.csv.gz")}
    audit.check(declared_files == actual_files, "manifest/raw compressed file inventory mismatch")
    for table, entry in manifest.get("reference_aggregates", {}).items():
        audit.check(table in SUMMARY_TABLES, f"unknown reference aggregate {table}")
        total = 0
        for metadata in entry.get("files", []):
            relative = metadata["path"]
            path = (audit.root / relative).resolve()
            if not audit.check(path.is_relative_to(audit.root.resolve()), "reference file escapes dataset directory"):
                continue
            if not audit.check(path.is_file(), f"reference aggregate missing: {relative}"):
                continue
            audit.check(_sha256(path) == metadata["sha256"], f"reference checksum mismatch: {relative}")
            audit.check(path.stat().st_size == metadata["bytes"], f"reference byte count mismatch: {relative}")
            count = 0
            with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
                reader = csv.DictReader(stream)
                audit.check(reader.fieldnames == SUMMARY_TABLES.get(table), f"reference {table}: unexpected header")
                for _ in reader:
                    count += 1
            audit.check(count == metadata["rows"], f"reference file row count mismatch: {relative}")
            total += count
        audit.check(total == entry["rows"], f"reference aggregate row count mismatch: {table}")


def _report(audit, **details):
    return {"valid": audit.error_count == 0, "error_count": audit.error_count,
            "errors": audit.errors, "checks": audit.check_count,
            "tables": dict(audit.counts),
            "coverage": {key: dict(value) for key, value in audit.coverage.items()}, **details}


def validate_dataset(dataset):
    """Return a JSON-serializable report; malformed inputs fail closed."""
    try:
        return _validate(Path(dataset))
    except (OSError, ValueError, TypeError, KeyError, OverflowError) as exc:
        return {"valid": False, "error_count": 1,
                "errors": [f"Validation could not complete: {type(exc).__name__}: {exc}"],
                "checks": 0, "tables": {}}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--report", type=Path,
                        help="Write a JSON report to a new file; existing files are never overwritten")
    args = parser.parse_args(argv)
    if args.report is not None:
        if args.report.exists() or args.report.is_symlink():
            parser.error(f"Refusing to overwrite an existing report: {args.report}")
        if not args.report.parent.is_dir():
            parser.error(f"Report parent directory does not exist: {args.report.parent}")
    report = validate_dataset(args.dataset)
    text = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.report is not None:
        try:
            # Exclusive creation also closes the race after the read-only check.
            with args.report.open("x", encoding="utf-8", newline="\n") as stream:
                stream.write(text)
        except OSError as exc:
            parser.error(f"Could not create report without overwriting: {exc}")
    print(text, end="")
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
