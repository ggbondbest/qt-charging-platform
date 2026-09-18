"""Run: python -m unittest discover -s data_analysis/tests -v

Small but complete five-city fixtures exercise the same code as the full data.
No network, GPU, Hadoop, Spark or third-party Python library is required.
"""

import copy
from contextlib import redirect_stderr, redirect_stdout
import csv
import gzip
import hashlib
import io
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from data_analysis.charging_data.generator import generate_dataset
from data_analysis.charging_data.schema import TABLES
from data_analysis.charging_data.validate import main as validate_main, validate_dataset


CONFIG = {
    "dataset_id": "charging_test_2d_v1",
    "seed": 20260912,
    "start_date": "2025-12-01",
    "days": 2,
    "users_per_city": 20,
    "interval_minutes": 5,
    "dirty_rate": 0.05,
}


class DatasetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="charging-dataset-tests-")
        cls.root = Path(cls.temp.name)
        cls.dataset = cls.root / "baseline"
        cls.returned_manifest = generate_dataset(copy.deepcopy(CONFIG), cls.dataset)
        cls.manifest = json.loads((cls.dataset / "manifest.json").read_text(encoding="utf-8"))
        cls.validation = validate_dataset(cls.dataset)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_full_business_validation(self):
        self.assertTrue(self.validation["valid"], self.validation)
        self.assertGreater(self.validation["checks"], 10000)
        self.assertGreater(self.validation["canonical_sessions"], 0)

    def test_all_tables_and_coverage(self):
        self.assertEqual(set(self.manifest["tables"]), set(TABLES))
        expected = {"cities": 5, "stations": 25, "chargers": 75,
                    "users": 100, "charger_telemetry": 75 * 2 * 288,
                    "calendar": 5 * 2, "weather_hourly": 5 * 2 * 24,
                    "operating_costs": 25 * 2}
        for table, rows in expected.items():
            with self.subTest(table=table):
                self.assertEqual(self.manifest["tables"][table]["rows"], rows)
        self.assertEqual(self.manifest["source"], "SIMULATED")

    def test_same_seed_is_byte_deterministic(self):
        destination = self.root / "deterministic-copy"
        generate_dataset(copy.deepcopy(CONFIG), destination)
        original_paths = sorted(path.relative_to(self.dataset) for path in self.dataset.rglob("*")
                                if path.is_file())
        copied_paths = sorted(path.relative_to(destination) for path in destination.rglob("*")
                              if path.is_file())
        self.assertEqual(original_paths, copied_paths)
        for relative in original_paths:
            with self.subTest(file=str(relative)):
                self.assertEqual((self.dataset / relative).read_bytes(),
                                 (destination / relative).read_bytes())

    def test_different_seed_changes_observations(self):
        config = dict(CONFIG, seed=CONFIG["seed"] + 1)
        destination = self.root / "different-seed"
        generate_dataset(config, destination)
        baseline = self.manifest["tables"]["charging_sessions"]["files"][0]["path"]
        self.assertNotEqual((self.dataset / baseline).read_bytes(),
                            (destination / baseline).read_bytes())
        report = validate_dataset(destination)
        self.assertTrue(report["valid"], report)

    def test_minimal_config_and_no_corruption_remain_valid(self):
        destination = self.root / "minimal-clean"
        generate_dataset(dict(CONFIG, days=1, users_per_city=1, dirty_rate=0), destination)
        report = validate_dataset(destination)
        self.assertTrue(report["valid"], report)
        self.assertEqual(report["dirty_rows"], 0)

    def test_invalid_config_is_rejected(self):
        bad_values = [{"days": 0}, {"days": -1}, {"users_per_city": 0},
                      {"interval_minutes": 7}, {"dirty_rate": -0.1},
                      {"dirty_rate": 1.1}, {"start_date": "not-a-date"}]
        for index, changes in enumerate(bad_values):
            with self.subTest(changes=changes):
                with self.assertRaises((ValueError, TypeError)):
                    generate_dataset(dict(CONFIG, **changes), self.root / f"invalid-{index}")

    def test_existing_data_is_never_overwritten(self):
        before = (self.dataset / "manifest.json").read_bytes()
        with self.assertRaises((ValueError, FileExistsError)):
            generate_dataset(copy.deepcopy(CONFIG), self.dataset)
        self.assertEqual(before, (self.dataset / "manifest.json").read_bytes())

    def test_injected_dirty_rows_are_accounted_for(self):
        self.assertGreater(self.validation["dirty_rows"], 0)
        self.assertEqual(self.validation["dirty_rows"],
                         self.manifest["tables"]["corruption_log"]["rows"])
        self.assertEqual(self.manifest["tables"]["charging_sessions"]["rows"],
                         self.validation["canonical_sessions"] +
                         sum(self.validation["rejected_session_copies"].values()))

    def test_checksum_tampering_is_detected(self):
        destination = self.root / "bad-checksum"
        shutil.copytree(self.dataset, destination)
        metadata = self.manifest["tables"]["cities"]["files"][0]
        path = destination / metadata["path"]
        with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
            text = stream.read()
        with gzip.open(path, "wt", encoding="utf-8", newline="") as stream:
            stream.write(text.replace("city_name", "CITY_NAME", 1))
        report = validate_dataset(destination)
        self.assertFalse(report["valid"])

    def test_business_tampering_is_detected_even_with_updated_hash(self):
        destination = self.root / "bad-business-data"
        shutil.copytree(self.dataset, destination)
        manifest = json.loads((destination / "manifest.json").read_text(encoding="utf-8"))
        metadata = manifest["tables"]["charging_sessions"]["files"][0]
        path = destination / metadata["path"]
        with gzip.open(path, "rt", encoding="utf-8", newline="") as stream:
            reader = csv.DictReader(stream)
            fields = reader.fieldnames
            rows = list(reader)
        # Keep a plausible positive value and correct the total-fee arithmetic;
        # independent interval-meter reconciliation must still catch this.
        rows[0]["energy_wh"] = str(int(rows[0]["energy_wh"]) + 100)
        with path.open("wb") as raw:
            with gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as compressed:
                with io.TextIOWrapper(compressed, encoding="utf-8", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
                    writer.writeheader()
                    writer.writerows(rows)
        metadata["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        metadata["bytes"] = path.stat().st_size
        (destination / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        report = validate_dataset(destination)
        self.assertFalse(report["valid"])
        self.assertTrue(any("energy_wh mismatch" in error or "conflicting valid duplicate" in error
                            for error in report["errors"]), report)

    def test_missing_dataset_fails_closed(self):
        report = validate_dataset(self.root / "does-not-exist")
        self.assertFalse(report["valid"])

    def test_validation_report_is_created_without_overwrite(self):
        destination = self.root / "validation-report.json"
        with redirect_stdout(io.StringIO()):
            result = validate_main(["--dataset", str(self.dataset), "--report", str(destination)])
        self.assertEqual(result, 0)
        self.assertTrue(json.loads(destination.read_text(encoding="utf-8"))["valid"])
        original = destination.read_bytes()
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            validate_main(["--dataset", str(self.dataset), "--report", str(destination)])
        self.assertNotEqual(raised.exception.code, 0)
        self.assertEqual(destination.read_bytes(), original)

    def test_failed_validation_can_write_failure_report(self):
        destination = self.root / "invalid-validation-report.json"
        with redirect_stdout(io.StringIO()):
            result = validate_main(["--dataset", str(self.root / "missing"), "--report", str(destination)])
        self.assertEqual(result, 1)
        self.assertFalse(json.loads(destination.read_text(encoding="utf-8"))["valid"])

    def test_requested_target_and_early_stop_are_not_rewritten_as_achieved(self):
        part = self.manifest["tables"]["charging_sessions"]["files"][0]["path"]
        with gzip.open(self.dataset / part, "rt", encoding="utf-8", newline="") as stream:
            rows = list(csv.DictReader(stream))
        stopped = [row for row in rows if row["stop_reason"] == "USER_STOPPED"]
        self.assertTrue(stopped, "fixture must exercise early-stop target preservation")
        for row in stopped:
            self.assertLess(int(row["energy_wh"]), int(row["target_value"]))
        reached = next(row for row in rows if row["stop_reason"] == "TARGET_REACHED")
        stopped = stopped[0]
        for name, selected, target in [
            ("false-target-reached", reached, int(reached["target_value"]) + 1),
            ("false-early-stop-target", stopped, int(stopped["energy_wh"])),
        ]:
            with self.subTest(case=name):
                destination = self.root / name
                shutil.copytree(self.dataset, destination)
                changed = [dict(row, target_value=str(target)) if row["session_id"] == selected["session_id"]
                           else row for row in rows]
                path = destination / part
                with path.open("wb") as raw:
                    with gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as compressed:
                        with io.TextIOWrapper(compressed, encoding="utf-8", newline="") as stream:
                            writer = csv.DictWriter(stream, fieldnames=TABLES["charging_sessions"], lineterminator="\n")
                            writer.writeheader()
                            writer.writerows(changed)
                report = validate_dataset(destination)
                self.assertFalse(report["valid"])
                self.assertTrue(any("target" in error for error in report["errors"]), report)

    def test_no_real_contact_identifiers_or_labels_in_model_features(self):
        fields = set().union(*(set(fields) for fields in TABLES.values()))
        self.assertTrue({"phone", "email", "license_plate", "vin", "name"}.isdisjoint(fields))
        self.assertNotIn("anomaly_type", TABLES["battery_samples"])
        self.assertNotIn("anomaly_type", TABLES["charger_telemetry"])


if __name__ == "__main__":
    unittest.main()
