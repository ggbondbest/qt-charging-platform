"""No-Spark handoff checks: corrupt exports must never replace a publication."""

import copy
import csv
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from data_analysis.contracts.serving import SERVING_TABLES, TABLE_KEYS
from data_analysis.publishing import publish as publishing_module
from data_analysis.publishing.publish import inspect_export, publish_dataset


def column(name, kind="string", nullable=False, **extra):
    return dict(name=name, type=kind, nullable=nullable, **extra)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ExportFixture:
    """Complete, independently constructed typed contract; no Spark imports."""

    def __init__(self, root):
        self.root = root
        key_types = {"business_date": "date", "recorded_at": "timestamp", "reference_time": "timestamp"}
        self.columns = {name: [column(key, key_types.get(key, "string")) for key in keys]
                        for name, keys in TABLE_KEYS.items()}
        for name in SERVING_TABLES - {"cities", "city_daily"}:
            self.columns[name].append(column("city_id"))
        self.columns["cities"] += [column("latitude", "number"), column("longitude", "number")]
        self.columns["station_snapshot"] += [column("capacity", "integer"),
            column("observed_pile_count", "integer"), column("unknown_count", "integer")]
        daily_columns = [column("energy_wh", "integer"), column("paid_cents", "integer"),
            column("refund_cents", "integer"), column("net_paid_cents", "integer"),
            column("complete_charging_samples", "integer"), column("complete_sample_count", "integer"),
            column("charging_utilization", "number", True, unit="ratio_0_1"), column("is_complete", "boolean")]
        self.columns["station_metrics_daily"] += copy.deepcopy(daily_columns)
        self.columns["city_daily"] += [column("station_count", "integer")] + copy.deepcopy(daily_columns)
        self.columns["ml_features_hourly"] += [column("lag_power_kw_h01", "number"), column("capacity", "integer"),
            column("last_available_count", "integer"), column("history_complete", "boolean")]
        self.columns["ml_targets_hourly"] += [column("label_power_kw_h01", "number", True), column("split_1h")]
        instant = "2025-12-01T01:00:00Z"
        metrics = dict(energy_wh=1000, paid_cents=100, refund_cents=150, net_paid_cents=-50,
            complete_charging_samples=6, complete_sample_count=12, charging_utilization=0.5, is_complete=True)
        self.rows = {name: [] for name in TABLE_KEYS}
        self.rows.update({
            "cities": [dict(city_id="C1", latitude=38.9, longitude=121.6)],
            "station_snapshot": [dict(station_id="S1", city_id="C1", capacity=3, observed_pile_count=3, unknown_count=0)],
            "station_hourly_metrics": [dict(station_id="S1", city_id="C1", recorded_at=instant)],
            "station_metrics_daily": [dict(station_id="S1", city_id="C1", business_date="2025-12-01", **metrics)],
            "city_daily": [dict(city_id="C1", business_date="2025-12-01", station_count=1, **metrics)],
            "ml_features_hourly": [dict(station_id="S1", city_id="C1", reference_time=instant,
                lag_power_kw_h01=10.5, capacity=3, last_available_count=2, history_complete=True)],
            "ml_targets_hourly": [dict(station_id="S1", reference_time=instant, label_power_kw_h01=123.456, split_1h="train")],
        })
        self.quality = dict(dataset_id="test-data", pipeline_run_id="run-test", source_manifest_sha256="a" * 64,
            raw_file_checksums_verified=True, manifest_row_counts_verified=True, reference_aggregates_used_as_input=False)
        self.manifest = dict(schemaVersion="1.0.0", featureVersion="history24-v1", datasetId="test-data", source="SIMULATED",
            publishedBatchId="batch-test", pipelineRunId="run-test", sourceManifestSha256="a" * 64,
            generatedAt="2026-01-01T00:00:00Z", startDate="2025-12-01", endDate="2025-12-03", businessTimezone="Asia/Shanghai",
            periodStart="2025-11-30T16:00:00Z", periodEndExclusive="2025-12-02T16:00:00Z",
            qualityReportPath="quality_report.json", definitions={"simulation": "fixture"}, tables={})

    def write(self):
        self.root.mkdir(parents=True, exist_ok=True)
        for name, columns in self.columns.items():
            path = self.root / "csv" / name / "part-00000.csv.gz"
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("wb") as binary:
                with gzip.GzipFile(fileobj=binary, mode="wb", filename="", mtime=0) as compressed:
                    with io.TextIOWrapper(compressed, encoding="utf-8", newline="") as text:
                        writer = csv.DictWriter(text, fieldnames=[item["name"] for item in columns])
                        writer.writeheader()
                        for row in self.rows[name]:
                            writer.writerow({key: str(value).lower() if isinstance(value, bool) else value
                                             for key, value in row.items()})
            self.manifest["tables"][name] = dict(columns=columns, primaryKey=TABLE_KEYS[name], rows=len(self.rows[name]),
                parquetPath="parquet/" + name,
                files=[dict(path=path.relative_to(self.root).as_posix(), bytes=path.stat().st_size, sha256=sha(path))])
        (self.root / "quality_report.json").write_text(json.dumps(self.quality), encoding="utf-8")
        self.manifest["qualityReportSha256"] = sha(self.root / "quality_report.json")
        self.save_metadata()
        (self.root / "_SUCCESS").write_text("", encoding="utf-8")

    def save_metadata(self):
        (self.root / "serving_manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")
        schemas = dict(schemaVersion="1.0.0", tables={name: {key: meta[key] for key in ["columns", "primaryKey"]}
                       for name, meta in self.manifest["tables"].items()})
        (self.root / "table_schemas.json").write_text(json.dumps(schemas), encoding="utf-8")


class PublishingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fixture = ExportFixture(self.root / "export")
        self.fixture.write()
        self.output = self.root / "published" / "analytics.sqlite3"

    def publish(self):
        return publish_dataset(self.fixture.root, self.output)

    def assert_rejected(self):
        with self.assertRaises((ValueError, KeyError, TypeError, OSError, csv.Error, sqlite3.Error)):
            self.publish()
        self.assertFalse(self.output.exists())
        if self.output.parent.exists():
            self.assertEqual(list(self.output.parent.iterdir()), [])

    def test_success_streams_nine_tables_preserves_metadata_and_excludes_labels(self):
        before = {path: sha(path) for path in self.fixture.root.rglob("*") if path.is_file()}
        report = self.publish()
        self.assertEqual(report["status"], "PUBLISHED")
        self.assertEqual(set(report["importedTables"]), SERVING_TABLES)
        self.assertFalse(report["targetLabelsImported"])
        self.assertEqual(report["validatedTargetRows"], 1)
        self.assertEqual(report["databaseSha256"], sha(self.output))
        self.assertEqual(report["servingManifestSha256"], sha(self.fixture.root / "serving_manifest.json"))
        with sqlite3.connect(self.output.as_uri() + "?mode=ro", uri=True) as database:
            tables = {row[0] for row in database.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            self.assertEqual(tables, SERVING_TABLES | {"__metadata"})
            self.assertEqual(database.execute("SELECT net_paid_cents FROM city_daily").fetchone()[0], -50)
            metadata = {key: json.loads(value) for key, value in database.execute("SELECT key,value FROM __metadata")}
            self.assertEqual(metadata, dict(self.fixture.manifest, qualityReport=self.fixture.quality))
            with self.assertRaises(sqlite3.OperationalError):
                database.execute("DELETE FROM cities")
        self.assertEqual(before, {path: sha(path) for path in self.fixture.root.rglob("*") if path.is_file()})
        if os.name != "nt":
            self.assertEqual(self.output.stat().st_mode & 0o222, 0)

    def test_all_empty_schema_tables_publish(self):
        self.fixture.rows = {name: [] for name in TABLE_KEYS}
        self.fixture.write()
        self.assertEqual(sum(self.publish()["importedTables"].values()), 0)

    def test_streaming_crosses_batch_boundaries_and_checks_duplicate_keys(self):
        self.fixture.rows["user_activity_daily"] = [dict(station_id="S1", city_id="C1",
            business_date="2025-12-01", user_id="U" + str(index)) for index in range(1005)]
        self.fixture.write()
        self.assertEqual(self.publish()["importedTables"]["user_activity_daily"], 1005)
        self.output = self.output.with_name("duplicate.sqlite3")
        self.fixture.rows["user_activity_daily"].append(self.fixture.rows["user_activity_daily"][0])
        self.fixture.write()
        with self.assertRaises(ValueError):
            self.publish()
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.output.parent.glob(".analytics-*")))

    def test_changed_export_during_publication_is_rejected(self):
        real_relationships = publishing_module._relationships
        def mutate_after_rows(connection, manifest):
            real_relationships(connection, manifest)
            path = self.fixture.root / "serving_manifest.json"
            path.write_text(path.read_text(encoding="utf-8") + " ", encoding="utf-8")
        with patch("data_analysis.publishing.publish._relationships", side_effect=mutate_after_rows):
            self.assert_rejected()

    def test_metadata_sidecar_and_quality_provenance_are_required(self):
        for corruption in ["quality_hash", "quality_identity", "schema", "source", "version", "date_range", "missing_table"]:
            with self.subTest(corruption=corruption):
                self.fixture = ExportFixture(self.root / corruption)
                self.fixture.write()
                if corruption == "quality_hash":
                    self.fixture.manifest["qualityReportSha256"] = "0" * 64
                elif corruption == "quality_identity":
                    self.fixture.quality["pipeline_run_id"] = "another-run"
                    self.fixture.write()
                elif corruption == "schema":
                    (self.fixture.root / "table_schemas.json").write_text("{}")
                    self.assert_rejected()
                    continue
                elif corruption == "source":
                    self.fixture.manifest["source"] = "REAL"
                elif corruption == "version":
                    self.fixture.manifest["schemaVersion"] = "9.0"
                elif corruption == "date_range":
                    self.fixture.manifest["periodEndExclusive"] = "2026-02-01T00:00:00Z"
                elif corruption == "missing_table":
                    del self.fixture.manifest["tables"]["ml_targets_hourly"]
                self.fixture.save_metadata()
                self.assert_rejected()

    def test_checksums_sizes_headers_and_row_counts_are_verified(self):
        for corruption in ["sha", "bytes", "header", "rows", "extra", "missing"]:
            with self.subTest(corruption=corruption):
                self.fixture = ExportFixture(self.root / corruption)
                self.fixture.write()
                meta = self.fixture.manifest["tables"]["cities"]
                path = self.fixture.root / meta["files"][0]["path"]
                if corruption in {"sha", "bytes"}:
                    meta["files"][0]["sha256" if corruption == "sha" else "bytes"] = "0" * 64 if corruption == "sha" else 1
                elif corruption == "header":
                    payload = gzip.decompress(path.read_bytes()).replace(b"city_id,", b"wrong_id,", 1)
                    path.write_bytes(gzip.compress(payload, mtime=0))
                    meta["files"][0].update(bytes=path.stat().st_size, sha256=sha(path))
                elif corruption == "rows":
                    meta["rows"] = 2
                elif corruption == "extra":
                    (path.parent / "part-unlisted.csv.gz").write_bytes(path.read_bytes())
                else:
                    path.unlink()
                self.fixture.save_metadata()
                self.assert_rejected()

    def test_duplicate_primary_keys_in_serving_and_nonimported_targets_fail(self):
        for table in ["cities", "ml_targets_hourly"]:
            with self.subTest(table=table):
                self.fixture = ExportFixture(self.root / table)
                self.fixture.rows[table] *= 2
                self.fixture.write()
                self.assert_rejected()

    def test_types_and_numeric_ranges_fail_closed(self):
        invalid = [("cities", "latitude", "NaN"), ("cities", "latitude", "inf"),
            ("cities", "latitude", 91), ("station_snapshot", "capacity", "1.5"),
            ("station_snapshot", "capacity", -1), ("station_snapshot", "station_id", None),
            ("ml_features_hourly", "history_complete", "TRUE"),
            ("ml_features_hourly", "reference_time", "2025-12-01 01:00:00"),
            ("station_metrics_daily", "business_date", "2025-13-01"),
            ("station_metrics_daily", "charging_utilization", 1.2),
            ("station_metrics_daily", "charging_utilization", 0.4),
            ("station_metrics_daily", "net_paid_cents", 100),
            ("ml_features_hourly", "last_available_count", 4),
            ("ml_targets_hourly", "label_power_kw_h01", "Infinity")]
        for index, (table, field, value) in enumerate(invalid):
            with self.subTest(table=table, field=field, value=value):
                self.fixture = ExportFixture(self.root / ("invalid-" + str(index)))
                self.fixture.rows[table][0][field] = value
                self.fixture.write()
                self.assert_rejected()

    def test_city_and_station_foreign_keys_and_rollups_are_checked(self):
        invalid = [("station_snapshot", "city_id", "missing"),
            ("station_hourly_metrics", "station_id", "missing"),
            ("station_metrics_daily", "city_id", "missing"),
            ("city_daily", "energy_wh", 1001), ("city_daily", "station_count", 2),
            ("city_daily", "business_date", "2025-12-02"),
            ("ml_targets_hourly", "reference_time", "2025-12-01T02:00:00Z")]
        for index, (table, field, value) in enumerate(invalid):
            with self.subTest(table=table, field=field):
                self.fixture = ExportFixture(self.root / ("relationship-" + str(index)))
                self.fixture.rows[table][0][field] = value
                self.fixture.write()
                self.assert_rejected()

    def test_undeclared_future_label_cannot_be_smuggled_into_serving_schema(self):
        self.fixture.columns["ml_features_hourly"].append(column("label_power_kw_h01", "number"))
        self.fixture.rows["ml_features_hourly"][0]["label_power_kw_h01"] = 123.456
        self.fixture.write()
        self.assert_rejected()

    def test_completion_markers_are_enforced(self):
        (self.fixture.root / "_RUNNING").write_text("")
        self.assert_rejected()
        (self.fixture.root / "_RUNNING").unlink()
        (self.fixture.root / "_SUCCESS").unlink()
        self.assert_rejected()

    def test_path_traversal_and_symlinks_are_rejected(self):
        for path in ["../escape.csv.gz", "/tmp/escape.csv.gz", "C:/escape.csv.gz", "csv\\cities\\part.csv.gz", "csv//cities/part.csv.gz"]:
            with self.subTest(path=path):
                self.fixture.manifest["tables"]["cities"]["files"][0]["path"] = path
                self.fixture.save_metadata()
                self.assert_rejected()
        self.fixture.write()
        shard = self.fixture.root / self.fixture.manifest["tables"]["cities"]["files"][0]["path"]
        outside = self.root / "outside.csv.gz"
        outside.write_bytes(shard.read_bytes())
        shard.unlink()
        try:
            shard.symlink_to(outside)
        except OSError:
            self.skipTest("Platform does not permit unprivileged symlinks")
        self.assert_rejected()

    def test_existing_database_or_report_is_preserved(self):
        self.output.parent.mkdir()
        self.output.write_bytes(b"existing user database")
        with self.assertRaises(FileExistsError):
            self.publish()
        self.assertEqual(self.output.read_bytes(), b"existing user database")
        self.output.unlink()
        report_path = self.output.with_name(self.output.name + ".publication_report.json")
        report_path.write_bytes(b"existing user report")
        with self.assertRaises(FileExistsError):
            self.publish()
        self.assertEqual(report_path.read_bytes(), b"existing user report")
        self.assertFalse(self.output.exists())

    def test_output_inside_source_is_rejected(self):
        with self.assertRaises(ValueError):
            publish_dataset(self.fixture.root, self.fixture.root / "new.sqlite3")
        self.assertFalse((self.fixture.root / "new.sqlite3").exists())

    def test_atomic_publish_racer_does_not_overwrite_and_cleans_owned_report(self):
        real_link = os.link
        def racing_link(source, destination):
            if Path(destination) == self.output.resolve():
                self.output.write_bytes(b"concurrent user database")
            return real_link(source, destination)
        with patch("data_analysis.publishing.publish.os.link", side_effect=racing_link):
            with self.assertRaises(FileExistsError):
                self.publish()
        self.assertEqual(self.output.read_bytes(), b"concurrent user database")
        self.assertEqual(list(self.output.parent.iterdir()), [self.output])

    def test_inspection_is_read_only_and_cli_publishes_without_spark(self):
        inspected = inspect_export(self.fixture.root)
        self.assertEqual(inspected["manifest"], self.fixture.manifest)
        self.assertFalse(self.output.parent.exists())
        result = subprocess.run([sys.executable, "-m", "data_analysis.publishing.publish",
            "--input", str(self.fixture.root), "--output", str(self.output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "PUBLISHED")
        second = subprocess.run([sys.executable, "-m", "data_analysis.publishing.publish",
            "--input", str(self.fixture.root), "--output", str(self.output)], capture_output=True, text=True)
        self.assertNotEqual(second.returncode, 0)
        self.assertIn("already exists", second.stderr)


if __name__ == "__main__":
    unittest.main()
