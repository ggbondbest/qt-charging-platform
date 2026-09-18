"""Stdlib-only regressions over the actual committed analysis/cleaning artifacts.

These tests deliberately do not import Spark, the API, ML libraries or a mock
dataset. A missing, incomplete, shifted or stale delivered bundle must fail CI.
"""

from collections import defaultdict
import csv
from datetime import date, datetime, timedelta, timezone
import gzip
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import unittest


DATASETS = Path(__file__).resolve().parents[1] / "datasets"
SOURCE = DATASETS / "analytics_full_180d_v1"
ADVANCED = DATASETS / "advanced_analytics_v2"
CHALLENGE = DATASETS / "cleaning_challenge_v1"
TABLES = ("station_day", "station_hour", "attempt_flow", "session_segments", "retention", "user_behavior", "service_hour")
SHANGHAI = timezone(timedelta(hours=8))  # Fixed business offset throughout this 2025–2026 dataset.
MAX_ROWS = 250_000
MAX_EXPANDED_BYTES = 300_000_000
BINDINGS = ("datasetId", "publishedBatchId", "pipelineRunId", "sourceManifestSha256")


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def invalid_constant(value):
    raise ValueError("Nonfinite JSON scalar: " + value)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"), parse_constant=invalid_constant)


def safe_file(root, relative):
    if not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative:
        raise ValueError("Invalid portable artifact path")
    path = PurePosixPath(relative)
    if path.is_absolute() or ".." in path.parts or str(path) != relative:
        raise ValueError("Artifact path escapes its bundle")
    result = root / relative
    if result.is_symlink() or not result.resolve().is_relative_to(root.resolve()) or not result.is_file():
        raise ValueError("Artifact is not a regular in-bundle file: " + relative)
    return result


def month_number(value):
    value = date.fromisoformat(value)
    return value.year * 12 + value.month - 1


class CommittedAdvancedBundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = read_json(SOURCE / "serving_manifest.json")
        cls.acceptance = read_json(SOURCE / "acceptance_manifest.json")
        cls.manifest = read_json(ADVANCED / "advanced_manifest.json")
        cls.tables = {}
        for name in TABLES:
            descriptor = cls.manifest["tables"][name]
            path = safe_file(ADVANCED, descriptor["file"])
            with gzip.open(path, "rb") as stream:
                content = stream.read(MAX_EXPANDED_BYTES + 1)
            if len(content) > MAX_EXPANDED_BYTES:
                raise ValueError("Expanded committed aggregate exceeds fixed safety limit")
            cls.tables[name] = json.loads(content, parse_constant=invalid_constant)

    @classmethod
    def tearDownClass(cls):
        cls.tables.clear()

    def test_completed_publication_and_existing_source_binding(self):
        for root in (SOURCE, ADVANCED):
            self.assertTrue((root / "_SUCCESS").is_file(), str(root))
            self.assertFalse((root / "_SUCCESS").is_symlink())
            self.assertFalse((root / "_RUNNING").exists())
        for key in BINDINGS:
            self.assertEqual(self.manifest[key], self.source[key], key)
            self.assertEqual(self.manifest[key], self.acceptance[key], key)
        self.assertEqual(self.manifest["inputSha256"]["serving_manifest.json"], digest(SOURCE / "serving_manifest.json"))
        self.assertEqual(self.manifest["inputSha256"]["acceptance_manifest.json"], digest(SOURCE / "acceptance_manifest.json"))
        self.assertEqual(self.acceptance["servingManifestSha256"], digest(SOURCE / "serving_manifest.json"))
        self.assertEqual(self.manifest["engine"], "PySpark")
        self.assertEqual(self.manifest["schemaVersion"], "1.0.0")
        self.assertEqual(self.manifest["analysisVersion"], "2.0.0")
        self.assertIs(self.manifest["rawFactsCollected"], False)
        self.assertIs(self.manifest["referenceAggregatesUsedAsInput"], False)
        self.assertIs(self.manifest["authenticatedCleanInventoryVerified"], True)
        self.assertEqual(set(self.manifest["invariants"]), {
            "stationDayUnique", "attemptsConserved", "sessionsEnergyBillsConserved", "retentionDenominatorsValid",
            "behaviorSessionsEnergyConserved", "behaviorIntervalsCensoredCorrectly", "serviceHourUnique",
            "serviceSessionsConserved", "serviceDurationRatioValid"})
        self.assertTrue(all(value is True for value in self.manifest["invariants"].values()))

    def test_every_consumed_input_has_the_original_bytes_not_just_same_row_count(self):
        inventory = {entry["path"]: entry for entry in self.acceptance["files"]}
        seen = set()
        for relative, expected in self.manifest["inputSha256"].items():
            path = safe_file(SOURCE, relative)
            self.assertEqual(digest(path), expected, relative)
            if relative.startswith("clean/"):
                self.assertIn(relative, inventory)
                self.assertEqual(expected, inventory[relative]["sha256"])
                self.assertEqual(path.stat().st_size, inventory[relative]["bytes"])
                seen.add(relative.split("/")[1])
        self.assertEqual(seen, set(self.manifest["sourceCounts"]))
        for name, count in self.manifest["sourceCounts"].items():
            self.assertEqual(count, self.acceptance["cleanTables"][name]["rowCount"], name)

    def test_all_seven_gzip_tables_have_exact_hashes_schemas_and_bounded_counts(self):
        self.assertEqual(set(self.manifest["tables"]), set(TABLES))
        self.assertLessEqual(self.manifest["maxExportRows"], MAX_ROWS)
        forbidden = {"user_id", "vehicle_id", "session_id", "attempt_id", "charger_id", "payment_id", "_previous_start"}
        for name, rows in self.tables.items():
            descriptor = self.manifest["tables"][name]
            path = safe_file(ADVANCED, descriptor["file"])
            self.assertEqual(path.stat().st_size, descriptor["bytes"], name)
            self.assertEqual(digest(path), descriptor["sha256"], name)
            self.assertIsInstance(rows, list)
            self.assertEqual(len(rows), descriptor["rows"], name)
            self.assertGreater(len(rows), 0, name)
            self.assertLessEqual(len(rows), MAX_ROWS, name)
            columns = {field["name"] for field in descriptor["schema"]}
            self.assertFalse(columns & forbidden, name)
            for row in rows:
                self.assertEqual(set(row), columns, name)
                self.assertTrue(all(not isinstance(value, float) or math.isfinite(value) for value in row.values()), name)
            plan = safe_file(ADVANCED, self.manifest["sparkPlans"][name])
            self.assertGreater(plan.stat().st_size, 100, name)

    def test_hour_timestamps_are_utc_not_host_local_relabelled_as_z(self):
        rows = self.tables["station_hour"]
        dimensions = {row["station_id"]: row for row in self.manifest["stations"]}
        seen = set()
        days = defaultdict(set)
        hourly_energy = defaultdict(int)
        for row in rows:
            self.assertTrue(row["recorded_at"].endswith("Z"))
            stamp = datetime.fromisoformat(row["recorded_at"].replace("Z", "+00:00"))
            local = stamp.astimezone(SHANGHAI)
            self.assertEqual(local.date().isoformat(), row["business_date"])
            self.assertEqual(local.hour, row["local_hour"])
            self.assertEqual((local.minute, local.second, local.microsecond), (0, 0, 0))
            key = (row["station_id"], row["recorded_at"])
            self.assertNotIn(key, seen)
            seen.add(key)
            day_key = (row["station_id"], row["business_date"])
            days[day_key].add(row["local_hour"])
            self.assertEqual(row["city_id"], dimensions[row["station_id"]]["city_id"])
            self.assertEqual(row["site_type"], dimensions[row["station_id"]]["site_type"])
            if row["has_observation"]:
                self.assertIsNotNone(row["energy_wh"])
                hourly_energy[day_key] += row["energy_wh"]
            else:
                self.assertIsNone(row["energy_wh"])
            if row["is_complete"]:
                self.assertTrue(row["has_observation"])
                self.assertEqual(row["sample_count"], row["expected_sample_count"])
                self.assertEqual(sum(row[state + "_samples"] for state in
                    ("available", "charging", "reserved", "occupied", "maintenance", "offline")), row["sample_count"])
        self.assertTrue(all(hours == set(range(24)) for hours in days.values()))
        daily = self.tables["station_day"]
        self.assertEqual(len(rows), len(daily) * 24)
        for row in daily:
            self.assertEqual(row["energy_wh"], hourly_energy[(row["station_id"], row["business_date"])])

    def test_attempts_sessions_and_joint_segments_are_conserved(self):
        flow = self.tables["attempt_flow"]
        segments = self.tables["session_segments"]
        self.assertEqual(sum(row["attempt_count"] for row in flow), self.manifest["sourceCounts"]["charging_attempts"])
        self.assertEqual(sum(row["session_count"] for row in segments), self.manifest["sourceCounts"]["charging_sessions"])
        self.assertEqual(sum(row["session_count"] for row in self.tables["station_day"]), self.manifest["sourceCounts"]["charging_sessions"])
        self.assertEqual(sum(row["attempt_count"] for row in self.tables["station_day"]), self.manifest["sourceCounts"]["charging_attempts"])
        path_values = {row["access_path"] for row in flow}
        self.assertEqual(path_values, {"DIRECT", "QUEUE", "RESERVATION"})
        self.assertTrue(all(type(row["attempt_count"]) is int and row["attempt_count"] > 0 for row in flow))
        by_day = defaultdict(int)
        for row in flow:
            self.assertIs(type(row["local_hour"]), int)
            self.assertIn(row["local_hour"], range(24))
            date.fromisoformat(row["business_date"])
            by_day[(row["station_id"], row["business_date"])] += row["attempt_count"]
        self.assertEqual(dict(by_day), {(row["station_id"], row["business_date"]): row["attempt_count"]
                                      for row in self.tables["station_day"] if row["attempt_count"]})
        for name, keys in (("station_day", ("station_id", "business_date")),
            ("attempt_flow", ("station_id", "business_date", "local_hour", "access_path", "outcome", "session_status", "failure_reason")),
            ("session_segments", ("station_id", "business_date", "site_type", "user_segment", "battery_capacity_band", "connector_type"))):
            identities = [tuple(row[key] for key in keys) for row in self.tables[name]]
            self.assertEqual(len(identities), len(set(identities)), name)
        bands = {row["battery_capacity_band"] for row in segments}
        self.assertTrue(bands <= {"LT50", "50_TO_69", "GE70", "UNKNOWN"})
        self.assertGreaterEqual(len(bands - {"UNKNOWN"}), 2)
        self.assertTrue(all("vehicle_class" not in row for row in segments))
        self.assertGreater(len({(row["site_type"], row["user_segment"], row["battery_capacity_band"], row["connector_type"])
                               for row in segments}), len(bands))

    def test_behavior_preserves_session_energy_and_censors_first_observations(self):
        """The first observed event is unknown history, never a zero-day interval."""
        rows = self.tables["user_behavior"]
        dimensions = {row["station_id"]: row for row in self.manifest["stations"]}
        gap_bounds = {"LT1D": (0, 86400), "1_TO_3D": (86400, 3 * 86400),
                      "3_TO_7D": (3 * 86400, 7 * 86400), "7_TO_14D": (7 * 86400, 14 * 86400),
                      "GE14D": (14 * 86400, None)}
        energy_bounds = {"LT10": (0, 10000), "10_TO_20": (10000, 20000),
                         "20_TO_40": (20000, 40000), "GE40": (40000, None)}
        seen, by_day, expected = set(), defaultdict(lambda: [0, 0]), defaultdict(lambda: [0, 0])
        for row in rows:
            identity = tuple(row[key] for key in
                ("station_id", "business_date", "user_segment", "gap_bucket", "energy_bucket"))
            self.assertNotIn(identity, seen)
            seen.add(identity)
            self.assertEqual(row["city_id"], dimensions[row["station_id"]]["city_id"])
            n, valid, first = row["session_count"], row["interval_count"], row["first_observed_count"]
            self.assertGreater(n, 0)
            self.assertEqual(valid + first, n)
            self.assertGreaterEqual(row["interval_seconds_sum"], 0)
            if row["gap_bucket"] == "FIRST_OBSERVED":
                self.assertEqual((valid, first, row["interval_seconds_sum"]), (0, n, 0))
            else:
                self.assertEqual((valid, first), (n, 0))
                lower, upper = gap_bounds[row["gap_bucket"]]
                self.assertGreaterEqual(row["interval_seconds_sum"], lower * n)
                if upper is not None:
                    self.assertLess(row["interval_seconds_sum"], upper * n)
            lower, upper = energy_bounds[row["energy_bucket"]]
            self.assertGreaterEqual(row["energy_wh"], lower * n)
            if upper is not None:
                self.assertLess(row["energy_wh"], upper * n)
            summary = by_day[(row["station_id"], row["business_date"])]
            summary[0] += n
            summary[1] += row["energy_wh"]
        for row in self.tables["session_segments"]:
            summary = expected[(row["station_id"], row["business_date"])]
            summary[0] += row["session_count"]
            summary[1] += row["energy_wh"]
        self.assertEqual(dict(by_day), dict(expected))
        self.assertEqual(sum(row["session_count"] for row in rows), self.manifest["sourceCounts"]["charging_sessions"])
        self.assertGreater(sum(row["first_observed_count"] for row in rows), 0)
        self.assertLessEqual(sum(row["first_observed_count"] for row in rows), self.manifest["sourceCounts"]["users"])

    def test_service_context_grid_and_static_interfaces_are_conserved(self):
        """Event-hour context is not an instantaneous failure snapshot or a daily duplicated inventory."""
        rows = self.tables["service_hour"]
        dimensions = {row["station_id"]: row for row in self.manifest["stations"]}
        identities = set()
        sessions_by_day, expected_by_day = defaultdict(int), defaultdict(int)
        fields = ("session_count", "occupied_seconds_sum", "connected_seconds_sum", "queue_wait_seconds_sum", "queue_wait_count")
        for row in rows:
            key = (row["station_id"], row["business_date"], row["local_hour"])
            self.assertNotIn(key, identities)
            identities.add(key)
            self.assertIs(type(row["local_hour"]), int)
            self.assertIn(row["local_hour"], range(24))
            self.assertEqual(row["city_id"], dimensions[row["station_id"]]["city_id"])
            for field in fields:
                self.assertIs(type(row[field]), int, field)
                self.assertGreaterEqual(row[field], 0, field)
            self.assertLessEqual(row["occupied_seconds_sum"], row["connected_seconds_sum"])
            if row["queue_wait_count"] == 0:
                self.assertEqual(row["queue_wait_seconds_sum"], 0)
            sessions_by_day[key[:2]] += row["session_count"]
        hour_grid = {(row["station_id"], row["business_date"], row["local_hour"])
                     for row in self.tables["station_hour"]}
        self.assertTrue(hour_grid <= identities)
        for row in self.tables["session_segments"]:
            expected_by_day[(row["station_id"], row["business_date"])] += row["session_count"]
        self.assertEqual({key: value for key, value in sessions_by_day.items() if value}, dict(expected_by_day))
        self.assertEqual(sum(row["session_count"] for row in rows), self.manifest["sourceCounts"]["charging_sessions"])
        # Full immutable source totals can agree while the two charts intentionally
        # assign queue events to different dates (JOIN hour versus resolution day).
        for field in ("queue_wait_seconds_sum", "queue_wait_count"):
            self.assertEqual(sum(row[field] for row in rows), sum(row[field] for row in self.tables["station_day"]), field)
        for field in ("occupied_seconds_sum", "connected_seconds_sum"):
            self.assertEqual(sum(row[field] for row in rows), sum(row[field] for row in self.tables["session_segments"]), field)
        self.assertEqual(sum(station["capacity"] for station in dimensions.values()), self.manifest["sourceCounts"]["chargers"])
        for station in dimensions.values():
            interfaces = station["interfaces"]
            self.assertTrue(interfaces)
            self.assertEqual(len(interfaces), len({row["connector_type"] for row in interfaces}))
            self.assertEqual(sum(row["charger_count"] for row in interfaces), station["capacity"])
            self.assertAlmostEqual(sum(row["rated_power_kw"] for row in interfaces), station["rated_capacity_kw"])
            for interface in interfaces:
                self.assertGreater(interface["charger_count"], 0)
                self.assertGreater(interface["rated_power_kw"], 0)

    def test_retention_has_exact_cohort_denominators_and_no_partial_future_month(self):
        start = datetime.fromisoformat(self.manifest["periodStart"].replace("Z", "+00:00")).astimezone(SHANGHAI)
        end = datetime.fromisoformat(self.manifest["periodEndExclusive"].replace("Z", "+00:00")).astimezone(SHANGHAI)
        first_month = start.date().replace(day=1)
        if (start.day, start.hour, start.minute, start.second, start.microsecond) != (1, 0, 0, 0, 0):
            first_month = (first_month + timedelta(days=32)).replace(day=1)
        last_month = (end.date().replace(day=1) - timedelta(days=1)).replace(day=1)
        self.assertEqual(self.manifest["completeMonthsFrom"], first_month.isoformat())
        self.assertEqual(self.manifest["completeMonthsThrough"], last_month.isoformat())
        cohorts = defaultdict(list)
        identities = set()
        dimensions = {row["station_id"]: row for row in self.manifest["stations"]}
        cities = {row["city_id"] for row in dimensions.values()}
        for row in self.tables["retention"]:
            key = (row["scope_type"], row["scope_id"], row["cohort_month"], row["month_offset"])
            self.assertNotIn(key, identities)
            identities.add(key)
            self.assertIn(row["scope_type"], ("ALL", "CITY", "STATION"))
            self.assertIn(row["scope_id"], {"ALL"} if row["scope_type"] == "ALL" else cities if row["scope_type"] == "CITY" else dimensions)
            self.assertGreater(row["cohort_size"], 0)
            self.assertGreaterEqual(row["n"], 0)
            self.assertLessEqual(row["n"], row["cohort_size"])
            self.assertGreaterEqual(row["month_offset"], 0)
            self.assertGreaterEqual(row["cohort_month"], first_month.isoformat())
            self.assertLessEqual(month_number(row["cohort_month"]) + row["month_offset"], month_number(last_month.isoformat()))
            cohorts[key[:3]].append(row)
        for (_, _, month), rows in cohorts.items():
            self.assertEqual(len({row["cohort_size"] for row in rows}), 1)
            expected_offsets = set(range(month_number(last_month.isoformat()) - month_number(month) + 1))
            self.assertEqual({row["month_offset"] for row in rows}, expected_offsets)
            origin = next(row for row in rows if row["month_offset"] == 0)
            self.assertEqual(origin["n"], origin["cohort_size"])


class CommittedCleaningChallengeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.report = read_json(CHALLENGE / "report.json")
        cls.plan = read_json(CHALLENGE / "injection_plan.json")
        cls.source = read_json(SOURCE / "serving_manifest.json")

    def test_completed_challenge_uses_same_immutable_source(self):
        self.assertTrue((CHALLENGE / "_SUCCESS").is_file())
        self.assertFalse((CHALLENGE / "_RUNNING").exists())
        for key in BINDINGS:
            self.assertEqual(self.report[key], self.source[key])
        source = self.report["source"]
        self.assertEqual(source["servingManifestSha256"], digest(SOURCE / "serving_manifest.json"))
        self.assertEqual(source["acceptanceManifestSha256"], digest(SOURCE / "acceptance_manifest.json"))
        for entry in source["verifiedInputFiles"]:
            path = safe_file(SOURCE, entry["path"])
            self.assertEqual(path.stat().st_size, entry["bytes"])
            self.assertEqual(digest(path), entry["sha256"])

    def test_all_evidence_hashes_and_raw_record_labels_are_real(self):
        declared = set()
        for entry in self.report["evidenceFiles"]:
            self.assertNotIn(entry["path"], declared)
            declared.add(entry["path"])
            path = safe_file(CHALLENGE, entry["path"])
            self.assertEqual(path.stat().st_size, entry["bytes"])
            self.assertEqual(digest(path), entry["sha256"])
        actual = {path.relative_to(CHALLENGE).as_posix() for path in CHALLENGE.rglob("*")
                  if path.is_file() and not path.name.startswith(".") and path.relative_to(CHALLENGE).as_posix() not in {"report.json", "_SUCCESS"}}
        self.assertEqual(declared, actual)
        with gzip.open(CHALLENGE / "raw/charging_sessions/part-00000.csv.gz", "rt", encoding="utf-8", newline="") as stream:
            raw_rows = list(csv.DictReader(stream))
        fingerprints = {hashlib.sha256(json.dumps({key: value or None for key, value in row.items()},
                        ensure_ascii=False, separators=(",", ":")).encode("utf-8")).hexdigest() for row in raw_rows}
        self.assertEqual(len(raw_rows), self.report["inputRows"])
        for case in self.plan["cases"]:
            self.assertIn(case["rawRecordSha256"], fingerprints)

    def test_case_outcomes_row_conservation_and_metric_restoration(self):
        report = self.report
        self.assertIs(report["passed"], True)
        self.assertEqual(report["engine"]["name"], "PySpark")
        self.assertEqual(report["engine"]["sessionTimezone"], "UTC")
        self.assertEqual(report["scope"], "ISOLATED_LABELED_CHALLENGE_NOT_PRODUCTION_ERROR_RATE")
        self.assertGreaterEqual(report["testedCaseTypes"], 20)
        self.assertEqual(report["testedCaseTypes"], len(report["byCaseType"]))
        self.assertEqual(report["testedCases"], len(report["cases"]))
        self.assertEqual(report["testedCases"], report["passedCases"])
        self.assertEqual(report["testedCases"], sum(row["cases"] for row in report["byCaseType"]))
        self.assertTrue(all(row["cases"] == row["passed"] and row["cases"] > 0 for row in report["byCaseType"]))
        self.assertEqual(report["inputRows"], report["cleanRows"] + report["rejectedRows"])
        self.assertEqual(report["cleanRows"], report["sampleRows"])
        self.assertEqual(sum(report["rejectionReasons"].values()), report["rejectedRows"])
        self.assertEqual(report["invariants"], dict(exactBaselineRestored=True,
            missingOrChangedBaselineRows=0, rowConservation=True, unexpectedCleanRows=0))
        for case in report["cases"]:
            self.assertIs(case["passed"], True)
            self.assertGreater(case["observedInputRows"], 0)
            self.assertEqual(case["observedRejections"], {case["expectedReason"]: 1} if case["expectedReason"] else {})
            self.assertTrue(set(case["expectedNormalizationRules"]) <= set(case["observedNormalizationRules"]))
            self.assertNotIn("user_id", case)
            self.assertNotIn("session_id", case)
        with gzip.open(CHALLENGE / "baseline/charging_sessions.csv.gz", "rt", encoding="utf-8", newline="") as stream:
            baseline = list(csv.DictReader(stream))
        self.assertEqual(len(baseline), report["sampleRows"])
        impact = report["metricImpact"]
        self.assertEqual(sum(int(row["total_fee_cents"]) for row in baseline), impact["expectedBaseline"]["billedCents"])
        self.assertEqual(sum(int(row["energy_wh"]) for row in baseline), impact["expectedBaseline"]["energyWh"])
        self.assertEqual(impact["expectedBaseline"], impact["afterCleaning"])
        self.assertEqual(impact["afterBillDistortionCents"], 0)
        self.assertEqual(impact["beforeBillDistortionCents"], impact["beforeNaiveParsedSum"]["billedCents"] - impact["expectedBaseline"]["billedCents"])


if __name__ == "__main__":
    unittest.main()
