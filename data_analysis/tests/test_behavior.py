"""Statistical regression of the declared synthetic behavior assumptions.

These tests prove reproducible directional relationships, not fidelity to a real
operator or a measured forecasting accuracy. Fixed seeds and broad comparisons
avoid treating one particular random count as a business requirement.
"""

import csv
import gzip
import hashlib
import io
import json
import math
import random
import shutil
import statistics
import tempfile
import unittest
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path

from data_analysis.charging_data.behavior import BehaviorModel
from data_analysis.charging_data.generator import generate_dataset
from data_analysis.charging_data.validate import validate_dataset


BUSINESS_TZ = timezone(timedelta(hours=8))
SEGMENTS = {"COMMUTER", "RIDE_HAILING", "FAMILY", "FLEET"}
SITE_TYPES = {"OFFICE", "SHOPPING", "RESIDENTIAL", "TRANSIT", "CAMPUS"}


def moment(hour, weekend=False):
    return datetime(2025, 12, 6 if weekend else 2, hour, 30, tzinfo=BUSINESS_TZ)


def vehicle(segment="FAMILY"):
    return {"vehicle_id": "V-TEST", "user_id": "U-TEST", "vehicle_class": segment,
            "segment": segment, "battery_capacity_kwh": 60,
            "max_charge_kw": 120, "soc_pct": 25}


def charger(connector="AC", identifier="CH-TEST"):
    return {"charger_id": identifier, "station_id": "ST-TEST",
            "connector_type": connector, "rated_power_kw": 7 if connector == "AC" else 120}


def rows(dataset, table):
    for part in sorted((dataset / "raw" / table).glob("part-*.csv.gz")):
        with gzip.open(part, "rt", encoding="utf-8", newline="") as stream:
            yield from csv.DictReader(stream)


def stamp(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


class BehaviorModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = BehaviorModel()

    def test_all_site_hour_weights_are_finite_and_nonnegative(self):
        for site in sorted(SITE_TYPES):
            all_weights = []
            for weekend in [False, True]:
                weights = [self.model.arrival_weight(site, moment(hour, weekend)) for hour in range(24)]
                self.assertTrue(all(math.isfinite(value) and value >= 0 for value in weights), site)
                all_weights.extend(weights)
            self.assertGreater(sum(all_weights), 0, site)

    def test_workday_office_is_not_a_flat_overnight_profile(self):
        daytime = sum(self.model.arrival_weight("OFFICE", moment(hour)) for hour in range(8, 18)) / 10
        overnight = sum(self.model.arrival_weight("OFFICE", moment(hour)) for hour in range(0, 6)) / 6
        self.assertGreater(daytime, overnight * 2)
        weekend = sum(self.model.arrival_weight("OFFICE", moment(hour, True)) for hour in range(8, 18)) / 10
        self.assertLess(weekend, daytime)

    def test_new_year_holiday_reduces_office_arrivals(self):
        holiday = datetime(2026, 1, 1, 9, 30, tzinfo=BUSINESS_TZ)
        workday = datetime(2026, 1, 6, 9, 30, tzinfo=BUSINESS_TZ)
        self.assertTrue(self.model.calendar_event(holiday).startswith("PUBLIC_HOLIDAY_"))
        self.assertFalse(self.model.is_workday(holiday))
        self.assertTrue(self.model.is_workday(workday))
        self.assertLess(self.model.arrival_weight("OFFICE", holiday),
                        self.model.arrival_weight("OFFICE", workday))

    def test_adjusted_sunday_uses_workday_office_profile(self):
        adjusted = datetime(2026, 1, 4, 9, 30, tzinfo=BUSINESS_TZ)
        normal = datetime(2026, 1, 6, 9, 30, tzinfo=BUSINESS_TZ)
        self.assertEqual(adjusted.weekday(), 6)
        self.assertEqual(self.model.calendar_event(adjusted), "ADJUSTED_WORKDAY")
        self.assertTrue(self.model.is_workday(adjusted))
        self.assertEqual(self.model.arrival_weight("OFFICE", adjusted),
                         self.model.arrival_weight("OFFICE", normal))

    def test_spring_festival_boundary_is_not_literal_weekend_only(self):
        adjusted = datetime(2026, 2, 14, 12, tzinfo=BUSINESS_TZ)
        holiday = datetime(2026, 2, 15, 12, tzinfo=BUSINESS_TZ)
        self.assertEqual(self.model.calendar_event(adjusted), "ADJUSTED_WORKDAY")
        self.assertTrue(self.model.is_workday(adjusted))
        self.assertTrue(self.model.calendar_event(holiday).startswith("PUBLIC_HOLIDAY_"))
        self.assertFalse(self.model.is_workday(holiday))

    def test_session_plan_rejects_full_or_invalid_battery_state(self):
        invalid = [("soc_pct", value) for value in [-1, 100, 101, math.nan, math.inf]]
        invalid += [("battery_capacity_kwh", value) for value in [0, -60, math.nan, math.inf]]
        for key, value in invalid:
            with self.subTest(field=key, value=value):
                current = dict(vehicle(), **{key: value})
                with self.assertRaises(ValueError):
                    self.model.session_plan(current, charger(), "RESIDENTIAL", moment(21), random.Random(1))

    def test_residential_evening_and_shopping_weekend_profiles(self):
        evening = sum(self.model.arrival_weight("RESIDENTIAL", moment(hour)) for hour in range(18, 24)) / 6
        midday = sum(self.model.arrival_weight("RESIDENTIAL", moment(hour)) for hour in range(10, 16)) / 6
        self.assertGreater(evening, midday * 1.5)
        weekday = sum(self.model.arrival_weight("SHOPPING", moment(hour)) for hour in range(11, 21))
        weekend = sum(self.model.arrival_weight("SHOPPING", moment(hour, True)) for hour in range(11, 21))
        self.assertGreater(weekend, weekday)

    def test_segment_selection_depends_on_site_instead_of_only_labels(self):
        probabilities = {}
        for site, hour in [("OFFICE", 9), ("RESIDENTIAL", 21), ("TRANSIT", 14)]:
            weights = self.model.segment_weights(site, hour)
            self.assertEqual(set(weights), SEGMENTS)
            self.assertTrue(all(math.isfinite(value) and value >= 0 for value in weights.values()))
            total = sum(weights.values())
            self.assertGreater(total, 0)
            probabilities[site] = {key: value / total for key, value in weights.items()}
        self.assertGreater(probabilities["OFFICE"]["COMMUTER"], probabilities["TRANSIT"]["COMMUTER"])
        self.assertGreater(probabilities["RESIDENTIAL"]["FAMILY"], probabilities["TRANSIT"]["FAMILY"])
        self.assertGreater(probabilities["TRANSIT"]["RIDE_HAILING"], probabilities["RESIDENTIAL"]["RIDE_HAILING"])

    def test_equivalent_chargers_are_not_selected_by_fixed_list_position(self):
        candidates = [charger("DC", name) for name in ["CH-A", "CH-B", "CH-C"]]
        for ordered in [candidates, list(reversed(candidates))]:
            rng = random.Random(72919)
            counts = Counter(self.model.choose_charger(ordered, vehicle("RIDE_HAILING"), "TRANSIT", 14, rng)
                             ["charger_id"] for _ in range(3000))
            self.assertEqual(set(counts), {"CH-A", "CH-B", "CH-C"})
            self.assertGreater(min(counts.values()), 600)
            self.assertLess(max(counts.values()), 1400)

    def test_session_plans_are_bounded_and_reproducible(self):
        plans = []
        for seed in [913, 913]:
            rng = random.Random(seed)
            series = []
            for site in sorted(SITE_TYPES):
                for connector in ["AC", "DC"]:
                    for _ in range(30):
                        current = vehicle()
                        plan = self.model.session_plan(current, charger(connector), site, moment(21), rng)
                        self.assertTrue({"target_wh", "max_charge_ticks", "connected_ticks", "parking_ticks"} <= set(plan))
                        remaining_wh = current["battery_capacity_kwh"] * (100 - current["soc_pct"]) * 10
                        self.assertGreater(plan["target_wh"], 0)
                        self.assertLessEqual(plan["target_wh"], remaining_wh)
                        self.assertGreater(plan["max_charge_ticks"], 0)
                        self.assertGreaterEqual(plan["connected_ticks"], 0)
                        self.assertGreaterEqual(plan["parking_ticks"], 0)
                        self.assertTrue(all(type(plan[key]) is int for key in
                            ["target_wh", "max_charge_ticks", "connected_ticks", "parking_ticks"]))
                        series.append(plan)
            plans.append(series)
        self.assertEqual(plans[0], plans[1])

    def test_high_soc_dc_goal_is_above_current_soc_not_a_fixed_floor(self):
        current = dict(vehicle(), soc_pct=83, battery_capacity_kwh=60)
        rng = random.Random(80199)
        plans = [self.model.session_plan(current, charger("DC"), "TRANSIT", moment(14), rng)
                 for _ in range(500)]
        minimum_wh = 60_000 * 3 // 100
        self.assertTrue(all(plan["target_wh"] >= minimum_wh for plan in plans))
        self.assertTrue(all(plan["target_wh"] <= 60_000 * 17 // 100 for plan in plans))
        self.assertGreater(len({plan["target_wh"] for plan in plans}), 100)

    def test_residential_ac_departure_window_and_dc_postcharge_parking_differ(self):
        rng = random.Random(12377)
        home = [self.model.session_plan(vehicle("FAMILY"), charger("AC"), "RESIDENTIAL", moment(21), rng)
                ["connected_ticks"] for _ in range(500)]
        transit = [self.model.session_plan(vehicle("RIDE_HAILING"), charger("DC"), "TRANSIT", moment(14), rng)
                   ["parking_ticks"] for _ in range(500)]
        self.assertGreater(statistics.mean(home), statistics.mean(transit) * 2)
        self.assertGreaterEqual(statistics.median(home), 6 * 12)
        self.assertLessEqual(statistics.median(home), 13 * 12)

    def test_driving_accumulates_without_instantaneous_soc_resets(self):
        start = datetime(2025, 12, 2, tzinfo=BUSINESS_TZ)
        times = [start + timedelta(minutes=5 * index) for index in range(2 * 288 + 1)]
        for segment in sorted(SEGMENTS):
            prefix = self.model.driving_prefix(segment, times)
            self.assertEqual(len(prefix), len(times))
            self.assertEqual(prefix[0], 0)
            self.assertTrue(all(math.isfinite(value) for value in prefix))
            self.assertTrue(all(b >= a for a, b in zip(prefix, prefix[1:])))
            self.assertGreater(prefix[-1], 0)
            self.assertIs(type(self.model.min_revisit_ticks(segment)), int)
            self.assertGreater(self.model.min_revisit_ticks(segment), 0)
        commuter = self.model.driving_prefix("COMMUTER", times)
        rush = commuter[10 * 12] - commuter[7 * 12]
        night = commuter[4 * 12] - commuter[1 * 12]
        self.assertGreater(rush, night)


class BehaviorDatasetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="charging-behavior-tests-")
        cls.dataset = Path(cls.temp.name) / "dataset"
        generate_dataset({"dataset_id": "charging_behavior_test_v2", "seed": 20260912,
            "start_date": "2025-12-01", "days": 14, "users_per_city": 120,
            "interval_minutes": 5, "dirty_rate": 0}, cls.dataset)
        cls.stations = {row["station_id"]: row for row in rows(cls.dataset, "stations")}
        cls.chargers = {row["charger_id"]: row for row in rows(cls.dataset, "chargers")}
        cls.vehicles = {row["vehicle_id"]: row for row in rows(cls.dataset, "vehicles")}
        cls.sessions = list(rows(cls.dataset, "charging_sessions"))
        cls.energy_by_vehicle = defaultdict(list)
        for row in rows(cls.dataset, "vehicle_energy_intervals"):
            cls.energy_by_vehicle[row["vehicle_id"]].append(row)
        cls.model = BehaviorModel()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_behavior_still_obeys_independent_business_validation(self):
        result = validate_dataset(self.dataset)
        self.assertTrue(result["valid"], result)

    def test_generated_weather_uses_cached_reanalysis_values(self):
        config_dir = Path(__file__).resolve().parents[1] / "config"
        with gzip.open(config_dir / "weather_reference.csv.gz", "rt", encoding="utf-8", newline="") as stream:
            cached = {(row["city_id"], row["recorded_at"]): row for row in csv.DictReader(stream)}
        count = 0
        for observation in rows(self.dataset, "weather_hourly"):
            original = cached[observation["city_id"], observation["recorded_at"]]
            for field in ["temperature_c", "humidity_pct", "rainfall_mm"]:
                self.assertEqual(float(observation[field]), float(original[field]), field)
            # Check representative WMO categories without calling production mapping code.
            code = int(original["weather"])
            if code in {0, 1}:
                self.assertEqual(observation["weather"], "CLEAR")
            elif code in {2, 3}:
                self.assertEqual(observation["weather"], "CLOUD")
            elif code in {45, 48}:
                self.assertEqual(observation["weather"], "FOG")
            elif code in {71, 73, 75, 77, 85, 86}:
                self.assertEqual(observation["weather"], "SNOW")
            else:
                self.assertEqual(observation["weather"], "RAIN")
            count += 1
        self.assertEqual(count, 5 * 14 * 24)
        manifest = json.loads((self.dataset / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["weather_source_rows"], {"ERA5_REANALYSIS": count})

    def test_dates_outside_weather_cache_are_explicitly_labeled_fallback(self):
        destination = Path(self.temp.name) / "fallback-weather"
        manifest = generate_dataset({"dataset_id": "charging_fallback_test_v2", "seed": 20260912,
            "start_date": "2026-06-01", "days": 1, "users_per_city": 1,
            "interval_minutes": 5, "dirty_rate": 0}, destination)
        self.assertEqual(manifest["weather_source_rows"], {"SEASONAL_SIMULATION_FALLBACK": 120})
        self.assertEqual(len(list(rows(destination, "weather_hourly"))), 120)
        report = validate_dataset(destination)
        self.assertTrue(report["valid"], report)

    def test_vehicle_energy_conservation_detects_tampering_with_updated_hash(self):
        destination = Path(self.temp.name) / "tampered-energy-ledger"
        shutil.copytree(self.dataset, destination)
        manifest_path = destination / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        metadata = manifest["tables"]["vehicle_energy_intervals"]["files"][0]
        path = destination / metadata["path"]
        with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
            reader = csv.DictReader(stream)
            fieldnames = reader.fieldnames
            records = list(reader)
        self.assertTrue(records)
        records[0]["external_charge_wh"] = str(int(records[0]["external_charge_wh"]) + 1000)
        with path.open("wb") as raw:
            with gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as compressed:
                with io.TextIOWrapper(compressed, encoding="utf-8", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=fieldnames, lineterminator="\n")
                    writer.writeheader()
                    writer.writerows(records)
        metadata["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        metadata["bytes"] = path.stat().st_size
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        result = validate_dataset(destination)
        self.assertFalse(result["valid"])
        self.assertTrue(any("does not conserve SOC" in error for error in result["errors"]), result)

    def test_vehicle_revisits_respect_cooldown_and_persistent_soc(self):
        by_vehicle = defaultdict(list)
        for session in self.sessions:
            by_vehicle[session["vehicle_id"]].append(session)
        compared = 0
        for identifier, sessions in by_vehicle.items():
            sessions.sort(key=lambda row: row["started_at"])
            segment = self.vehicles[identifier]["vehicle_class"]
            minimum_gap = self.model.min_revisit_ticks(segment) * 300
            for previous, current in zip(sessions, sessions[1:]):
                previous_exit, current_start = stamp(previous["unplugged_at"]), stamp(current["started_at"])
                gap = (current_start - previous_exit).total_seconds()
                self.assertGreaterEqual(gap, minimum_gap, identifier)
                intervals = [row for row in self.energy_by_vehicle[identifier]
                             if stamp(row["started_at"]) >= previous_exit and stamp(row["ended_at"]) <= current_start]
                driving = sum(int(row["driving_wh"]) for row in intervals)
                external = sum(int(row["external_charge_wh"]) for row in intervals)
                capacity_wh = float(self.vehicles[identifier]["battery_capacity_kwh"]) * 1000
                observed_change = (float(current["start_soc_pct"]) - float(previous["end_soc_pct"])) * capacity_wh / 100
                self.assertAlmostEqual(observed_change, external - driving,
                    delta=capacity_wh * 0.0002 + len(intervals),
                    msg="Between-session SOC must be explained by explicit driving/external-energy records")
                if not external:
                    self.assertLessEqual(float(current["start_soc_pct"]), float(previous["end_soc_pct"]) + 0.011)
                if gap <= 3600:
                    self.assertEqual(external, 0, "A one-hour revisit must not invent an unobserved external charge")
                    self.assertLess(float(previous["end_soc_pct"]) - float(current["start_soc_pct"]), 30,
                                    "Implausible short-gap SOC reset reappeared")
                compared += 1
        self.assertGreater(compared, 100, "fixture must exercise repeated vehicle behavior")
        self.assertGreater(sum(len(intervals) for intervals in self.energy_by_vehicle.values()), 0)

    def test_office_arrivals_are_daytime_weighted(self):
        counts = Counter()
        for row in rows(self.dataset, "charging_attempts"):
            when = stamp(row["attempted_at"]).astimezone(BUSINESS_TZ)
            # Exclude the cold-start day and deliberately closed final day.
            if not 2 <= when.day <= 13 or when.weekday() >= 5:
                continue
            if self.stations[row["station_id"]]["site_type"] != "OFFICE":
                continue
            if 9 <= when.hour < 17:
                counts["day"] += 1
            elif when.hour < 6:
                counts["night"] += 1
        self.assertGreater(counts["day"], 0)
        self.assertGreater(counts["day"] / 8, counts["night"] / 6 * 1.25)

    def test_residential_overnight_occupancy_is_not_just_a_chart_label(self):
        occupied = Counter()
        total = Counter()
        for row in rows(self.dataset, "charger_telemetry"):
            if self.stations[row["station_id"]]["site_type"] != "RESIDENTIAL":
                continue
            when = stamp(row["recorded_at"]).astimezone(BUSINESS_TZ)
            if not 2 <= when.day <= 13:
                continue
            bucket = "night" if when.hour >= 20 or when.hour < 7 else "day" if 9 <= when.hour < 17 else None
            if bucket:
                total[bucket] += 1
                occupied[bucket] += row["state"] in {"CHARGING", "OCCUPIED"}
        self.assertGreater(total["night"], 0)
        self.assertGreater(total["day"], 0)
        self.assertGreater(occupied["night"] / total["night"], occupied["day"] / total["day"] * 1.1)

    def test_connection_duration_differs_between_home_ac_and_transit_dc(self):
        durations = defaultdict(list)
        for row in self.sessions:
            station = self.stations[row["station_id"]]
            connector = self.chargers[row["charger_id"]]["connector_type"]
            when = stamp(row["started_at"]).astimezone(BUSINESS_TZ)
            if not 2 <= when.day <= 13:
                continue
            if station["site_type"] == "RESIDENTIAL" and connector == "AC" and when.hour >= 18:
                durations["home"].append((stamp(row["unplugged_at"]) - stamp(row["started_at"])).total_seconds())
            elif station["site_type"] == "TRANSIT" and connector == "DC":
                durations["transit"].append((stamp(row["unplugged_at"]) - stamp(row["started_at"])).total_seconds())
        self.assertGreater(len(durations["home"]), 20)
        self.assertGreater(len(durations["transit"]), 20)
        self.assertGreater(statistics.median(durations["home"]), statistics.median(durations["transit"]) * 2)


if __name__ == "__main__":
    unittest.main()
