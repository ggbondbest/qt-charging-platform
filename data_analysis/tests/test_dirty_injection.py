"""Independent dirty-batch derivation, provenance, preservation and parser tests."""

from collections import Counter
import contextlib
import csv
import gzip
import hashlib
import io
import json
import math
import os
from pathlib import Path
import shutil
import tempfile
import unittest

from data_analysis.charging_data.generator import generate_dataset
from data_analysis.charging_data.inject_dirty import inject_dataset, main
from data_analysis.charging_data.schema import TABLES
from data_analysis.charging_data.validate import validate_dataset


def records(root, table):
    for part in sorted((root / "raw" / table).glob("part-*.csv.gz")):
        with gzip.open(part, "rt", encoding="utf-8", newline="") as stream:
            yield from csv.DictReader(stream)


def snapshot(root):
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


def fixture(destination):
    return generate_dataset({"dataset_id": "dirty_injection_test_source", "seed": 20260912,
        "start_date": "2025-12-01", "days": 1, "users_per_city": 12,
        "interval_minutes": 5, "dirty_rate": 0.05}, destination)


class DirtyInjectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="charging-dirty-tests-")
        cls.root = Path(cls.temporary.name)
        cls.source = cls.root / "source"
        cls.source_manifest = fixture(cls.source)
        cls.original_files = snapshot(cls.source)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def output(self, name):
        return self.root / (self._testMethodName + "-" + name)

    def test_append_to_existing_dirty_batch_preserves_canonical_and_source(self):
        output = self.output("derived")
        manifest = inject_dataset(self.source, output, seed=42, rate=1)
        report = validate_dataset(output)
        self.assertTrue(report["valid"], report)
        self.assertEqual(report["canonical_sessions"], self.source_manifest["canonical_session_count"])
        self.assertEqual(snapshot(self.source), self.original_files)
        prior = Counter(self.source_manifest["corruptions"])
        added = Counter(manifest["dirty_injection"]["added_corruptions"])
        self.assertEqual(set(added), {"DUPLICATE", "NEGATIVE_FEE", "UNKNOWN_STATION", "MISSING_ID", "STATUS_FORMAT"})
        self.assertEqual(Counter(manifest["corruptions"]), prior + added)
        self.assertEqual(report["dirty_rows"], sum(prior.values()) + sum(added.values()))
        self.assertEqual(manifest["tables"]["charging_sessions"]["rows"],
            self.source_manifest["tables"]["charging_sessions"]["rows"] + sum(added.values()) - added["STATUS_FORMAT"])
        for relative, checksum in self.original_files.items():
            if (relative.startswith("raw/") and relative.split("/")[1] not in {"charging_sessions", "corruption_log"}
                    or relative.startswith("reference_aggregates/")):
                self.assertEqual(hashlib.sha256((output / relative).read_bytes()).hexdigest(), checksum, relative)
        self.assertTrue((output / "_INJECTION_SUCCESS").exists())
        self.assertFalse((output / "_INJECTION_RUNNING").exists())

    def test_parent_manifest_and_plan_hashes_are_auditable(self):
        output = self.output("provenance")
        manifest = inject_dataset(self.source, output, 77, 0.2)
        parent_bytes = (output / "provenance" / "parent_manifest.json").read_bytes()
        self.assertEqual(parent_bytes, (self.source / "manifest.json").read_bytes())
        self.assertEqual(hashlib.sha256(parent_bytes).hexdigest(), manifest["parent_manifest_sha256"])
        self.assertNotEqual(manifest["dataset_id"], self.source_manifest["dataset_id"])
        self.assertEqual(manifest["dataset_id"], manifest["config"]["dataset_id"])
        metadata = manifest["dirty_injection"]["plan"]
        plan_path = output / metadata["path"]
        self.assertEqual(hashlib.sha256(plan_path.read_bytes()).hexdigest(), metadata["sha256"])
        self.assertEqual(plan_path.stat().st_size, metadata["bytes"])
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        self.assertEqual(plan["selected_sessions"], len(plan["operations"]))
        logged = {row["corruption_id"]: row for row in records(output, "corruption_log")}
        for operation in plan["operations"]:
            self.assertEqual(logged[operation["corruption_id"]],
                             {key: operation[key] for key in TABLES["corruption_log"]})

    def test_repeated_seed_and_input_reproduce_every_output_byte(self):
        first, second = self.output("first"), self.output("second")
        inject_dataset(self.source, first, 302, 0.2)
        inject_dataset(self.source, second, 302, 0.2)
        self.assertEqual(snapshot(first), snapshot(second))

    def test_different_seed_changes_the_injection_plan(self):
        first, second = self.output("first"), self.output("second")
        a = inject_dataset(self.source, first, 10, 0.3)
        b = inject_dataset(self.source, second, 11, 0.3)
        self.assertNotEqual((first / "injection_plan.json").read_bytes(), (second / "injection_plan.json").read_bytes())
        self.assertEqual(a["canonical_session_count"], b["canonical_session_count"])

    def test_reinjection_never_selects_prior_corrupted_sessions(self):
        first, second = self.output("first"), self.output("second")
        inject_dataset(self.source, first, 13, 0.3)
        a = inject_dataset(first, second, 13, 0.3)
        prior_touched = {row["record_id"] for row in records(first, "corruption_log")}
        plan = json.loads((second / "injection_plan.json").read_text(encoding="utf-8"))
        self.assertTrue(prior_touched.isdisjoint({row["record_id"] for row in plan["operations"]}))
        self.assertEqual(a["dirty_injection"]["eligible_sessions"], a["canonical_session_count"] - len(prior_touched))
        result = validate_dataset(second)
        self.assertTrue(result["valid"], result)

    def test_zero_rate_keeps_existing_corruption_without_new_operations(self):
        output = self.output("zero")
        manifest = inject_dataset(self.source, output, 20, 0)
        self.assertEqual(manifest["corruptions"], self.source_manifest["corruptions"])
        self.assertEqual(manifest["dirty_injection"]["selected_sessions"], 0)
        self.assertEqual(manifest["tables"]["charging_sessions"]["rows"],
                         self.source_manifest["tables"]["charging_sessions"]["rows"])

    def test_clean_source_and_exhausted_eligible_population_are_supported(self):
        clean_source = self.output("clean-source")
        generate_dataset({"dataset_id": "clean_injection_fixture", "seed": 20260912,
            "start_date": "2025-12-01", "days": 1, "users_per_city": 1,
            "interval_minutes": 5, "dirty_rate": 0}, clean_source)
        first, second = self.output("first"), self.output("second")
        a = inject_dataset(clean_source, first, 9, 1)
        self.assertEqual(a["dirty_injection"]["input_corruptions"], {})
        self.assertEqual(a["dirty_injection"]["selected_sessions"], a["canonical_session_count"])
        b = inject_dataset(first, second, 9, 1)
        self.assertEqual(b["dirty_injection"]["eligible_sessions"], 0)
        self.assertEqual(b["dirty_injection"]["selected_sessions"], 0)
        self.assertEqual(a["corruptions"], b["corruptions"])

    def test_interrupted_input_is_not_reused_even_if_its_raw_files_are_valid(self):
        incomplete = self.output("incomplete")
        shutil.copytree(self.source, incomplete)
        (incomplete / "_INJECTION_RUNNING").touch()
        output = self.output("not-created")
        with self.assertRaisesRegex(ValueError, "incomplete"):
            inject_dataset(incomplete, output, 21, 0.1)
        self.assertFalse(output.exists())

    def test_reinjection_checks_parent_plan_and_manifest_sidecars(self):
        first = self.output("first")
        inject_dataset(self.source, first, 31, 0.1)
        for relative in ["injection_plan.json", "provenance/parent_manifest.json"]:
            with self.subTest(sidecar=relative):
                broken = self.output(relative.replace("/", "-"))
                shutil.copytree(first, broken)
                with (broken / relative).open("ab") as stream:
                    stream.write(b" ")
                output = Path(str(broken) + "-result")
                with self.assertRaisesRegex(ValueError, "checksum"):
                    inject_dataset(broken, output, 1, 0.1)
                self.assertFalse(output.exists())

    def test_overwrite_and_path_overlap_are_rejected_before_changes(self):
        existing = self.output("exists")
        existing.mkdir()
        for target in [existing, self.source, self.source / "nested", self.source.parent]:
            with self.subTest(target=target):
                with self.assertRaises((ValueError, FileExistsError)):
                    inject_dataset(self.source, target, 0, 0.1)
        self.assertFalse((self.source / "nested").exists())
        self.assertEqual(snapshot(self.source), self.original_files)

    def test_invalid_seed_rate_and_identity_are_rejected(self):
        invalid = [(True, 0.1), (-1, 0.1), (2**32, 0.1), (1.5, 0.1),
                   (1, True), (1, -0.1), (1, 1.1), (1, math.nan), (1, math.inf)]
        for seed, rate in invalid:
            with self.subTest(seed=seed, rate=rate):
                with self.assertRaises(ValueError):
                    inject_dataset(self.source, self.output("bad"), seed, rate)
        for identifier in ["", "../escape", self.source_manifest["dataset_id"]]:
            with self.subTest(dataset_id=identifier):
                with self.assertRaises(ValueError):
                    inject_dataset(self.source, self.output("identity"), 1, 0.1, identifier)
        self.assertFalse(self.output("bad").exists())
        self.assertFalse(self.output("identity").exists())

    def test_source_checksum_corruption_fails_before_output_creation(self):
        broken = self.output("broken")
        shutil.copytree(self.source, broken)
        part = next((broken / "raw" / "payments").glob("*.csv.gz"))
        with part.open("ab") as stream:
            stream.write(b"tampered")
        output = self.output("not-created")
        with self.assertRaisesRegex(ValueError, "checksum/size"):
            inject_dataset(broken, output, 1, 0.1)
        self.assertFalse(output.exists())

    def test_missing_shard_and_unsafe_manifest_paths_fail_closed(self):
        for case in ["missing", "escape"]:
            with self.subTest(case=case):
                source = self.output(case)
                shutil.copytree(self.source, source)
                manifest_path = source / "manifest.json"
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                metadata = manifest["tables"]["payments"]["files"][0]
                metadata["path"] = "../outside.csv.gz" if case == "escape" else "raw/payments/part-missing.csv.gz"
                manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
                output = self.output(case + "-result")
                with self.assertRaises(ValueError):
                    inject_dataset(source, output, 1, 0.1)
                self.assertFalse(output.exists())

    def test_cli_outputs_summary_and_invalid_arguments_exit_nonzero(self):
        output = self.output("cli")
        with contextlib.redirect_stdout(io.StringIO()) as text:
            result = main(["--input", str(self.source), "--output", str(output), "--seed", "89", "--rate", "0.01"])
        self.assertEqual(result, 0)
        self.assertIn("canonical_session_count", json.loads(text.getvalue()))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as failed:
            main(["--input", str(self.source), "--output", str(output), "--seed", "89", "--rate", "0.01"])
        self.assertNotEqual(failed.exception.code, 0)


@unittest.skipUnless(os.environ.get("RUN_SPARK_TESTS") == "1", "Requires real Java/PySpark runtime")
class SparkDirtyParsingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from pyspark.sql import SparkSession
        os.environ.setdefault("SPARK_LOCAL_IP", "127.0.0.1")
        cls.spark = (SparkSession.builder.master("local[2]").appName("charging-dirty-parser-tests")
                     .config("spark.ui.enabled", "false").config("spark.sql.shuffle.partitions", "2")
                     .config("spark.sql.session.timeZone", "UTC").getOrCreate())
        cls.spark.sparkContext.setLogLevel("ERROR")
        cls.temporary = tempfile.TemporaryDirectory(prefix="charging-dirty-spark-")
        cls.root = Path(cls.temporary.name)
        cls.source = cls.root / "source"
        fixture(cls.source)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()
        cls.spark.stop()

    def write_rows(self, name, table, rows):
        root = self.root / name
        folder = root / "raw" / table
        folder.mkdir(parents=True)
        with gzip.open(folder / "part-test.csv.gz", "wt", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=TABLES[table])
            writer.writeheader()
            writer.writerows(rows)
        return root

    def test_nan_and_infinity_are_parse_errors_not_valid_measurements(self):
        from data_analysis.spark_jobs.pipeline import read_table
        original = next(records(self.source, "weather_hourly"))
        rows = [original, dict(original, temperature_c="NaN"), dict(original, humidity_pct="Infinity"),
                dict(original, rainfall_mm="-Infinity")]
        root = self.write_rows("nonfinite", "weather_hourly", rows)
        parsed = read_table(self.spark, str(root), "weather_hourly")
        self.assertEqual(parsed.filter("_parse_error").count(), 3)

    def test_fractional_integer_text_is_not_silently_truncated(self):
        from data_analysis.spark_jobs.pipeline import read_table
        original = next(records(self.source, "charger_telemetry"))
        root = self.write_rows("fractional", "charger_telemetry", [original, dict(original, energy_wh="1.5")])
        self.assertEqual(read_table(self.spark, str(root), "charger_telemetry").filter("_parse_error").count(), 1)

    def test_all_injected_operations_leave_original_canonical_spark_sessions(self):
        from data_analysis.spark_jobs.pipeline import clean_sessions, read_table
        output = self.root / "injected"
        manifest = inject_dataset(self.source, output, 91, 1)
        raw = read_table(self.spark, str(output), "charging_sessions")
        tables = {name: read_table(self.spark, str(output), name).select(*TABLES[name])
                  for name in ["stations", "chargers", "users", "vehicles", "charging_attempts"]}
        clean, rejected = clean_sessions(raw, tables)
        self.assertEqual(clean.count(), manifest["canonical_session_count"])
        expected = sum(manifest["corruptions"].values()) - manifest["corruptions"].get("STATUS_FORMAT", 0)
        self.assertEqual(rejected.count(), expected)


if __name__ == "__main__":
    unittest.main()
