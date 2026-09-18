"""Opt-in real generator -> Spark -> export -> bundle -> SQLite integration.

RUN_SPARK_TESTS=1 python -m unittest data_analysis.tests.test_data_export -v
The fixture is five days / five users per city (108,000 telemetry rows). It is
generated and processed once per class. No production dataset is modified.
"""

import gzip
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

from data_analysis.contracts.serving import SERVING_TABLES, TABLE_KEYS


@unittest.skipUnless(os.environ.get("RUN_SPARK_TESTS") == "1",
                     "Requires explicit RUN_SPARK_TESTS=1, PySpark 3.5.6, and Java 17")
class DataExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from pyspark.sql import SparkSession
        from data_analysis.charging_data.generator import generate_dataset
        from data_analysis.spark_jobs.pipeline import run_pipeline
        from data_analysis.spark_jobs.export import export_data

        cls.temporary = tempfile.TemporaryDirectory(prefix="charging-export-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        cls.raw, cls.processed, cls.export = [cls.root / name for name in ["raw", "processed", "export"]]
        cls.config = dict(dataset_id="export_integration_5d", seed=20311, start_date="2025-12-01",
                          days=5, users_per_city=5, interval_minutes=5, dirty_rate=0.05)
        cls.generated = generate_dataset(cls.config, cls.raw)
        os.environ.setdefault("SPARK_LOCAL_IP", "127.0.0.1")
        os.environ.setdefault("PYSPARK_PYTHON", sys.executable)
        cls.spark = (SparkSession.builder.master("local[1]").appName("charging-export-integration")
            .config("spark.ui.enabled", "false").config("spark.driver.memory", "1g")
            .config("spark.sql.shuffle.partitions", "2").config("spark.sql.session.timeZone", "UTC")
            .config("spark.default.parallelism", "1").getOrCreate())
        cls.addClassCleanup(cls.spark.stop)
        cls.spark.sparkContext.setLogLevel("ERROR")
        cls.quality = run_pipeline(cls.spark, str(cls.raw), str(cls.processed))
        cls.manifest = export_data(cls.spark, str(cls.raw), str(cls.processed), str(cls.export))

    def export_to(self, name):
        from data_analysis.spark_jobs.export import export_data
        output = self.root / name
        return output, lambda: export_data(self.spark, str(self.raw), str(self.processed), str(output))

    def test_end_to_end_exports_real_clean_aggregates_features_and_safe_query_database(self):
        from data_analysis.publishing.bundle import bundle_export
        from data_analysis.publishing.publish import publish_dataset, inspect_export
        inspected = inspect_export(self.export)
        self.assertEqual(set(inspected["manifest"]["tables"]), set(TABLE_KEYS))
        self.assertEqual(self.manifest["datasetId"], self.config["dataset_id"])
        self.assertEqual(self.manifest["pipelineRunId"], self.quality["pipeline_run_id"])
        self.assertEqual(self.manifest["sourceManifestSha256"], hashlib.sha256((self.raw / "manifest.json").read_bytes()).hexdigest())
        self.assertTrue(self.quality["raw_file_checksums_verified"])
        self.assertFalse(self.quality["reference_aggregates_used_as_input"])
        self.assertEqual(self.quality["clean_session_rows"], self.generated["canonical_session_count"])
        self.assertEqual(self.quality["input_rows"]["charger_telemetry"], 75 * 5 * 288)
        self.assertGreater(self.quality["rejected_session_rows"], 0)
        expected = {"cities": 5, "station_snapshot": 25, "station_metrics_daily": 125,
                    "city_daily": 25, "station_hourly_metrics": 25 * 5 * 24,
                    "ml_features_hourly": 25 * (5 * 24 - 23), "ml_targets_hourly": 25 * (5 * 24 - 23)}
        for name, rows in expected.items():
            self.assertEqual(self.manifest["tables"][name]["rows"], rows, name)
        for marker in [self.processed / "_SUCCESS", self.export / "_SUCCESS"]:
            self.assertTrue(marker.is_file())
            self.assertFalse(marker.with_name("_RUNNING").exists())
        portable = self.root / "portable"
        copied = bundle_export(self.export, portable)
        self.assertEqual(copied["publishedBatchId"], self.manifest["publishedBatchId"])
        self.assertFalse((portable / "parquet").exists())
        database = self.root / "query" / "analytics.sqlite3"
        published = publish_dataset(portable, database)
        self.assertEqual(set(published["importedTables"]), SERVING_TABLES)
        self.assertEqual(published["validatedTargetRows"], expected["ml_targets_hourly"])
        self.assertFalse(published["targetLabelsImported"])
        with sqlite3.connect(database.as_uri() + "?mode=ro", uri=True) as connection:
            main_tables = {row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            self.assertEqual(main_tables, SERVING_TABLES | {"__metadata"})
            feature_columns = {row[1] for row in connection.execute("PRAGMA table_info(ml_features_hourly)")}
            self.assertFalse(any(name.startswith(("label_", "split_")) for name in feature_columns))
            city_energy = connection.execute("SELECT SUM(energy_wh) FROM city_daily").fetchone()[0]
            station_energy = connection.execute("SELECT SUM(energy_wh) FROM station_metrics_daily").fetchone()[0]
            self.assertEqual(city_energy, station_energy)
            self.assertGreater(city_energy, 0)
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM station_snapshot").fetchone()[0], 25)
            metadata = {key: json.loads(value) for key, value in connection.execute("SELECT key,value FROM __metadata")}
            self.assertEqual(metadata["qualityReport"]["clean_session_rows"], self.generated["canonical_session_count"])

    def test_wrong_raw_manifest_cannot_use_another_processed_batch(self):
        path = self.raw / "manifest.json"
        original = path.read_bytes()
        changed = json.loads(original)
        changed["dataset_id"] = "another_source_batch"
        output, run = self.export_to("wrong-source")
        try:
            path.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "provenance"):
                run()
            self.assertFalse(output.exists())
        finally:
            path.write_bytes(original)

    def test_raw_shard_checksum_mismatch_is_rejected_before_export(self):
        relative = self.generated["tables"]["cities"]["files"][0]["path"]
        path = self.raw / relative
        original = path.read_bytes()
        output, run = self.export_to("wrong-sha")
        try:
            # It remains valid gzip/CSV, but no longer belongs to the hashed batch.
            path.write_bytes(gzip.compress(gzip.decompress(original) + b"\n", mtime=0))
            with self.assertRaisesRegex(ValueError, "checksum/size"):
                run()
            self.assertFalse(output.exists())
        finally:
            path.write_bytes(original)

    def test_existing_and_nested_export_outputs_are_never_overwritten(self):
        from data_analysis.spark_jobs.export import export_data
        before = (self.export / "serving_manifest.json").read_bytes()
        with self.assertRaises(FileExistsError):
            export_data(self.spark, str(self.raw), str(self.processed), str(self.export))
        self.assertEqual(before, (self.export / "serving_manifest.json").read_bytes())
        nested = self.processed / "nested-export"
        with self.assertRaisesRegex(ValueError, "independent"):
            export_data(self.spark, str(self.raw), str(self.processed), str(nested))
        self.assertFalse(nested.exists())

    def test_processed_completion_requires_regular_success_and_no_running(self):
        output, run = self.export_to("incomplete-processed")
        success = self.processed / "_SUCCESS"
        running = self.processed / "_RUNNING"
        running.write_text("")
        try:
            with self.assertRaisesRegex(ValueError, "incomplete"):
                run()
        finally:
            running.unlink()
        success.unlink()
        try:
            with self.assertRaisesRegex(ValueError, "incomplete"):
                run()
            success.mkdir()
            with self.assertRaisesRegex(ValueError, "incomplete"):
                run()
        finally:
            if success.is_dir():
                success.rmdir()
            success.touch()
        self.assertFalse(output.exists())

    def test_unfinished_raw_injection_is_not_accepted_by_pipeline_or_export(self):
        from data_analysis.spark_jobs.pipeline import run_pipeline
        output, run = self.export_to("unfinished-raw-export")
        marker = self.raw / "_INJECTION_RUNNING"
        marker.write_text("")
        try:
            with self.assertRaisesRegex(ValueError, "(?i)incomplete|running"):
                run()
            processed = self.root / "unfinished-raw-processed"
            with self.assertRaisesRegex(ValueError, "(?i)incomplete|running"):
                run_pipeline(self.spark, str(self.raw), str(processed))
            self.assertFalse(processed.exists())
        finally:
            marker.unlink()
        self.assertFalse(output.exists())

    def test_injected_manifest_without_success_is_not_accepted(self):
        from data_analysis.spark_jobs.pipeline import run_pipeline
        path = self.raw / "manifest.json"
        original = path.read_bytes()
        changed = json.loads(original)
        changed["dirty_injection"] = {"seed": 11}
        output, run = self.export_to("missing-injection-success")
        try:
            path.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "completion marker"):
                run()
            with self.assertRaisesRegex(ValueError, "completion marker"):
                run_pipeline(self.spark, str(self.raw), str(self.root / "missing-injection-processed"))
            self.assertFalse(output.exists())
        finally:
            path.write_bytes(original)

    def test_quality_boolean_strings_do_not_certify_provenance(self):
        from data_analysis.spark_jobs import fs
        real_read_json = fs.read_json
        def invalid_report(spark, path):
            result = real_read_json(spark, path)
            if "/reports/quality_report/" in path:
                result["raw_file_checksums_verified"] = "false"
            return result
        output, run = self.export_to("boolean-string-quality")
        with patch("data_analysis.spark_jobs.fs.read_json", side_effect=invalid_report):
            with self.assertRaisesRegex(ValueError, "provenance"):
                run()
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
