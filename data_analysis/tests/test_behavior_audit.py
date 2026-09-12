"""Independent tiny business fixture with exact expected hourly denominators."""

from contextlib import redirect_stderr, redirect_stdout
import copy
from datetime import datetime, timedelta, timezone
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from data_analysis.charging_data.behavior_audit import audit_behavior, distribution, main, write_report
from data_analysis.charging_data.io import DatasetWriter, write_json
from data_analysis.charging_data.schema import TABLES


def fixture(destination, days=4, dirty=True, conflicting_duplicate=False):
    """No external dataset or generator behavior assumptions are copied here."""
    destination.mkdir()
    writer = DatasetWriter(destination, TABLES)
    start = datetime(2025, 12, 3, 16, tzinfo=timezone.utc)  # Thursday 00:00 local.
    stamp = lambda value: value.isoformat().replace("+00:00", "Z")

    def emit(table, **values):
        row = dict.fromkeys(TABLES[table], "")
        row.update(values)
        writer.write(table, row)
        return row

    emit("cities", city_id="C1", city_name="Fixture city", timezone="Asia/Shanghai")
    emit("stations", station_id="S1", city_id="C1", site_type="OFFICE")
    emit("stations", station_id="S2", city_id="C1", site_type="RESIDENTIAL")
    for cid, sid, connector in [("A", "S1", "AC"), ("B", "S1", "DC"), ("C", "S2", "AC")]:
        emit("chargers", charger_id=cid, station_id=sid, connector_type=connector)
    emit("users", user_id="U", home_city_id="C1")
    emit("vehicles", vehicle_id="V", user_id="U", vehicle_class="COMMUTER")
    sessions = []
    for day in range(days):
        emit("charging_attempts", attempt_id=f"T{day}", station_id="S1",
             attempted_at=stamp(start+timedelta(days=day, hours=8)))
        for hour in range(24):
            emit("weather_hourly", city_id="C1", recorded_at=stamp(start+timedelta(days=day, hours=hour)),
                 temperature_c=day*10, humidity_pct=50, rainfall_mm=1, weather="RAIN")
        if day in (1, 2):
            begin = start+timedelta(days=day, hours=8)
            row = emit("charging_sessions", session_id=f"E{day}", station_id="S1", charger_id="A", vehicle_id="V",
                started_at=stamp(begin), ended_at=stamp(begin+timedelta(hours=1)),
                unplugged_at=stamp(begin+timedelta(hours=2 if day == 1 else 1)),
                total_fee_cents=1200, energy_wh=12000,
                status=" completed " if dirty and day == 2 else "COMPLETED", start_soc_pct=20+day*10)
            sessions.append(row)
        for tick in range(288):
            for cid, sid in [("A", "S1"), ("B", "S1"), ("C", "S2")]:
                state = "AVAILABLE"
                if cid == "A" and day in (1, 2) and tick//12 == 8:
                    state = "CHARGING"
                if cid == "A" and day == 1 and tick//12 == 9:
                    state = "OCCUPIED"
                emit("charger_telemetry", charger_id=cid, station_id=sid,
                    recorded_at=stamp(start+timedelta(days=day, minutes=tick*5)), interval_seconds=300,
                    energy_wh=1000 if state == "CHARGING" else 0, state=state)
    corruptions = {}
    if sessions and dirty:
        variants = [("DUPLICATE", "REMOVE_DUPLICATE", {}),
                    ("MISSING_ID", "QUARANTINE", {"session_id": ""}),
                    ("NEGATIVE_FEE", "QUARANTINE", {"total_fee_cents": -1})]
        for index, (kind, action, changes) in enumerate(variants):
            writer.write("charging_sessions", dict(sessions[0], **changes))
            emit("corruption_log", corruption_id=f"X{index}", table_name="charging_sessions",
                 record_id=sessions[0]["session_id"], corruption_type=kind, expected_action=action)
            corruptions[kind] = 1
        if days > 2:
            emit("corruption_log", corruption_id="X-status", table_name="charging_sessions", record_id="E2",
                 corruption_type="STATUS_FORMAT", expected_action="NORMALIZE")
            corruptions["STATUS_FORMAT"] = 1
    if conflicting_duplicate and sessions:
        writer.write("charging_sessions", dict(sessions[0], energy_wh=12001))
    emit("vehicle_energy_intervals", interval_id="I", vehicle_id="V", driving_wh=3000, external_charge_wh=5000)
    manifest = dict(source="SIMULATED", schema_version="1.1.0", dataset_id="fixture", business_timezone="Asia/Shanghai",
        config={"interval_minutes": 5}, period_start=stamp(start), period_end_exclusive=stamp(start+timedelta(days=days)),
        tables=writer.finish(), canonical_session_count=len(sessions), corruptions=corruptions)
    write_json(destination/"manifest.json", manifest)
    return manifest


class BehaviorAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="behavior-audit-test-")
        cls.root = Path(cls.temporary.name)
        cls.dataset = cls.root/"fixture"
        fixture(cls.dataset)
        cls.report = audit_behavior(cls.dataset)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_exact_hourly_denominators_and_edge_exclusion(self):
        report = self.report
        self.assertEqual(report["behavior_window"]["day_counts"], {"all_days": 2, "weekday": 1, "weekend": 1})
        office = {item["day_type"]: item for item in report["city_site_hourly"] if item["site_type"] == "OFFICE"}
        self.assertEqual(office["all_days"]["total_attempts"], 2)
        self.assertEqual(office["weekday"]["denominator_charger_seconds_per_hour"], 7200)
        self.assertEqual(office["weekday"]["charging_utilization_pct"][8], 50)
        self.assertEqual(office["weekday"]["charging_utilization_pct"][9], 0)
        self.assertEqual(office["weekday"]["charging_plus_occupied_utilization_pct"][9], 50)
        self.assertEqual(office["all_days"]["charging_plus_occupied_utilization_pct"][9], 25)
        self.assertEqual(office["all_days"]["energy_kwh"][8], 24)
        self.assertEqual(office["weekend"]["mean_energy_kwh_per_day"], 12)

    def test_cleaning_and_exact_session_quantiles(self):
        report = self.report
        self.assertEqual(report["cleaning"]["canonical_sessions"], 2)
        self.assertEqual(sum(report["cleaning"]["rejected_copies"].values()), 3)
        self.assertEqual(report["cleaning"]["normalized_status_rows"], 1)
        profile = report["session_profiles"][0]
        self.assertEqual(profile["energy_kwh"]["quantiles"]["p50"], 12)
        self.assertEqual(profile["active_minutes"]["quantiles"]["p50"], 60)
        self.assertEqual(profile["connected_minutes"]["quantiles"]["p50"], 90)
        self.assertEqual(report["start_soc_pct"]["mean"], 35)
        self.assertEqual(report["revisit"]["start_to_start_hours"]["mean"], 24)
        self.assertEqual(report["revisit"]["unplug_to_next_start_hours"]["mean"], 22)

    def test_weather_and_external_energy_separate_scopes(self):
        weather = self.report["weather_city_month"][0]
        self.assertEqual(weather["hours"], 48)
        self.assertEqual(weather["mean_temperature_c"], 15)
        self.assertEqual(weather["total_preceding_hour_precipitation_mm"], 48)
        ledger = self.report["full_window_energy_ledger"]
        self.assertEqual(ledger["total_external_charge_kwh"], 5)
        self.assertFalse(ledger["included_in_platform_revenue"])
        self.assertNotIn(str(self.dataset), json.dumps(self.report))

    def test_short_window_skips_interior_without_fake_zero_rates(self):
        path = self.root/"short"
        fixture(path, days=2)
        report = audit_behavior(path)
        self.assertEqual(report["behavior_window"]["status"], "SKIPPED_INSUFFICIENT_INTERIOR_DAYS")
        self.assertTrue(all(item["charging_utilization_pct"] == [None]*24 for item in report["city_site_hourly"]))
        self.assertEqual(report["start_soc_pct"]["count"], 0)

    def test_conflicting_duplicates_fail_closed(self):
        path = self.root/"conflict"
        fixture(path, conflicting_duplicate=True)
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            audit_behavior(path)

    def test_output_is_deterministic_and_refuses_overwrite_before_read(self):
        path = self.root/"report.json"
        write_report(self.dataset, path)
        self.assertEqual(json.loads(path.read_text()), self.report)
        with patch("data_analysis.charging_data.behavior_audit.audit_behavior", side_effect=AssertionError("must not read")):
            with self.assertRaises(FileExistsError):
                write_report(self.dataset, path)
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            main(["--dataset", str(self.dataset), "--output", str(path)])
        with redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--dataset", str(self.dataset), "--output", str(self.root/"cli.json")]), 0)

    def test_checksum_tampering_and_unsafe_manifest_paths_rejected(self):
        path = self.root/"bad"
        manifest = fixture(path, days=1, dirty=False)
        catalog = manifest["tables"]["cities"]["files"][0]
        part = path/catalog["path"]
        with part.open("ab") as stream:
            stream.write(b"tampered")
        with self.assertRaisesRegex(ValueError, "Checksum"):
            audit_behavior(path)
        bad = copy.deepcopy(manifest)
        bad["tables"]["cities"]["files"][0]["path"] = "../../outside.csv.gz"
        (path/"manifest.json").write_text(json.dumps(bad), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            audit_behavior(path)

    def test_empty_and_interpolated_quantiles(self):
        self.assertEqual(distribution([0, 100])["quantiles"]["p25"], 25)
        self.assertIsNone(distribution([])["quantiles"]["p50"])


if __name__ == "__main__":
    unittest.main()
