"""No-Hadoop safety regressions; mocks test guards, never certify real HDFS."""

import copy
from contextlib import ExitStack
import unittest
from unittest.mock import Mock, call, patch

from data_analysis.contracts import CONTRACT_VERSION, FEATURE_VERSION
from data_analysis.contracts.serving import SERVING_TABLES, TABLE_KEYS
from data_analysis.scripts.verify_hdfs import validate_export_metadata, verify_hdfs_batch


RAW = "hdfs://nn:9000/charging/raw"
PROCESSED = "hdfs://nn:9000/charging/processed"
EXPORT = "hdfs://nn:9000/charging/export"


def metadata_fixture():
    quality = {"dataset_id": "fixture", "pipeline_run_id": "spark-fixture", "source_manifest_sha256": "a" * 64,
        "input": RAW, "output": PROCESSED, "raw_file_checksums_verified": True,
        "manifest_row_counts_verified": True, "reference_aggregates_used_as_input": False, "station_hourly_rows": 1}
    tables = {}
    for name, keys in TABLE_KEYS.items():
        columns = [{"name": key, "type": "string", "nullable": False} for key in keys]
        if name in SERVING_TABLES and "city_id" not in keys:
            columns.append({"name": "city_id", "type": "string", "nullable": False})
        tables[name] = {"columns": columns, "primaryKey": keys.copy(), "rows": 0,
            "files": [{"path": "csv/" + name + "/part-00000.csv.gz", "bytes": 25, "sha256": "c" * 64}]}
    serving = {"schemaVersion": CONTRACT_VERSION, "featureVersion": FEATURE_VERSION, "source": "SIMULATED",
        "businessTimezone": "Asia/Shanghai", "datasetId": quality["dataset_id"],
        "pipelineRunId": quality["pipeline_run_id"], "sourceManifestSha256": "a" * 64,
        "publishedBatchId": "publication-fixture", "qualityReportPath": "quality_report.json",
        "qualityReportSha256": "b" * 64, "tables": tables}
    schemas = {"schemaVersion": CONTRACT_VERSION, "tables": {name: {
        key: copy.deepcopy(metadata[key]) for key in ("columns", "primaryKey")}
        for name, metadata in tables.items()}}
    return serving, schemas, quality


def export_quality_fixture(processed):
    return {**{key: copy.deepcopy(value) for key, value in processed.items() if key not in {"input", "output"}},
            "rejection_samples": [{"session_id": None, "rejection_reason": "INVALID_SESSION"}]}


class HdfsExportContractTests(unittest.TestCase):
    def setUp(self):
        self.serving, self.schemas, self.quality = metadata_fixture()

    def validate(self):
        return validate_export_metadata(self.serving, self.schemas, export_quality_fixture(self.quality), self.quality)

    def test_complete_ten_table_zero_row_export_is_valid_and_labels_are_checked(self):
        shards = self.validate()
        self.assertEqual(len(shards), 10)
        self.assertIn("csv/ml_targets_hourly/part-00000.csv.gz", shards)
        self.assertTrue(all(meta["rows"] == 0 for meta in self.serving["tables"].values()))

    def test_empty_missing_and_extra_tables_are_rejected(self):
        for change in ("empty", "missing", "extra"):
            serving = copy.deepcopy(self.serving)
            if change == "empty":
                serving["tables"] = {}
            elif change == "missing":
                del serving["tables"]["ml_targets_hourly"]
            else:
                serving["tables"]["unexpected"] = serving["tables"]["cities"]
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "ten tables"):
                validate_export_metadata(serving, self.schemas, export_quality_fixture(self.quality), self.quality)

    def test_every_table_requires_a_nonempty_file_list(self):
        for files in ([], None, {}):
            self.serving["tables"]["cities"]["files"] = files
            with self.subTest(files=files), self.assertRaisesRegex(ValueError, "CSV schema shard"):
                self.validate()

    def test_schema_and_feature_versions_must_match_current_contract(self):
        for field in ("schemaVersion", "featureVersion"):
            serving = copy.deepcopy(self.serving)
            serving[field] = "future-version"
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "schema/feature version"):
                validate_export_metadata(serving, self.schemas, export_quality_fixture(self.quality), self.quality)

    def test_schema_sidecar_must_exactly_match_columns_and_primary_keys(self):
        self.schemas["tables"]["cities"]["columns"][0]["type"] = "integer"
        with self.assertRaisesRegex(ValueError, "table_schemas"):
            self.validate()

    def test_table_contract_primary_key_and_serving_label_leak_are_rejected(self):
        self.serving["tables"]["cities"]["primaryKey"] = ["unexpected_id"]
        with self.assertRaisesRegex(ValueError, "primary key"):
            self.validate()
        self.serving, self.schemas, self.quality = metadata_fixture()
        self.serving["tables"]["cities"]["columns"].append({"name": "label_future", "type": "number", "nullable": True})
        with self.assertRaisesRegex(ValueError, "Future labels"):
            self.validate()

    def test_shard_paths_are_relative_table_owned_and_not_duplicate(self):
        for path in ("../outside.csv.gz", "csv/cities/nested/part-000.csv.gz", "csv/station_snapshot/part-000.csv.gz",
                     "hdfs://other/part-000.csv.gz", "csv/cities/part-000.csv", "csv\\cities\\part-000.csv.gz"):
            self.serving["tables"]["cities"]["files"][0]["path"] = path
            with self.subTest(path=path), self.assertRaises(ValueError):
                self.validate()
        self.serving, self.schemas, self.quality = metadata_fixture()
        self.serving["tables"]["cities"]["files"] *= 2
        with self.assertRaisesRegex(ValueError, "more than once"):
            self.validate()

    def test_shard_size_and_digest_are_validated_before_filesystem_calls(self):
        for field, invalid in (("bytes", 0), ("bytes", -1), ("bytes", True), ("sha256", "not-a-hash")):
            serving = copy.deepcopy(self.serving)
            serving["tables"]["cities"]["files"][0][field] = invalid
            with self.subTest(field=field, value=invalid), self.assertRaisesRegex(ValueError, "size/hash"):
                validate_export_metadata(serving, self.schemas, export_quality_fixture(self.quality), self.quality)

    def test_quality_must_be_exact_completed_batch_not_just_matching_ids(self):
        for field, value in (("pipeline_run_id", "other"), ("station_hourly_rows", 2),
                             ("raw_file_checksums_verified", False), ("manifest_row_counts_verified", 1)):
            exported = export_quality_fixture(self.quality)
            exported[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "quality report"):
                validate_export_metadata(self.serving, self.schemas, exported, self.quality)
        self.serving["datasetId"] = "another-dataset"
        with self.assertRaisesRegex(ValueError, "another batch"):
            self.validate()

    def test_real_export_path_redaction_and_added_rejection_preview_are_accepted(self):
        exported = export_quality_fixture(self.quality)
        self.assertIn("input", self.quality)
        self.assertIn("output", self.quality)
        self.assertNotIn("input", exported)
        self.assertNotIn("output", exported)
        self.assertNotIn("rejection_samples", self.quality)
        self.assertIn("rejection_samples", exported)
        self.assertEqual(len(validate_export_metadata(self.serving, self.schemas, exported, self.quality)), 10)
        exported["rejection_samples"] = [{"session_id": "S1", "rejection_reason": "DUPLICATE_SESSION_ID"}]
        self.assertEqual(len(validate_export_metadata(self.serving, self.schemas, exported, self.quality)), 10)

    def test_extra_fields_or_unredacted_paths_cannot_be_added_to_export_quality(self):
        for field in ("unexpected", "input", "output"):
            exported = export_quality_fixture(self.quality)
            exported[field] = "should-not-be-here"
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "quality report differs"):
                validate_export_metadata(self.serving, self.schemas, exported, self.quality)

    def test_rejection_samples_are_bounded_and_typed_with_exact_safe_fields(self):
        for samples in (None, {}, [None], [{}], [{"session_id": "S1", "rejection_reason": 1}],
                [{"session_id": 1, "rejection_reason": "INVALID_SESSION"}],
                [{"session_id": None, "rejection_reason": "INVALID_SESSION", "raw_json": "private"}],
                [{"session_id": "S1", "rejection_reason": ""}],
                [{"session_id": None, "rejection_reason": "INVALID_SESSION"}] * 13):
            exported = export_quality_fixture(self.quality)
            exported["rejection_samples"] = samples
            with self.subTest(samples=samples), self.assertRaisesRegex(ValueError, "rejection samples"):
                validate_export_metadata(self.serving, self.schemas, exported, self.quality)

    def test_quality_path_cannot_redirect_outside_export(self):
        self.serving["qualityReportPath"] = "../quality_report.json"
        with self.assertRaisesRegex(ValueError, "path/hash"):
            self.validate()


class HdfsReadOnlyVerificationTests(unittest.TestCase):
    """Exercise real orchestration branches with a fake read-only filesystem."""

    def setUp(self):
        self.serving, self.schemas, self.quality = metadata_fixture()
        self.spark = Mock()
        self.spark.version = "3.5.6"
        self.spark.sparkContext.master = "local[2]"
        self.spark.read.parquet.return_value.count.return_value = 1
        self.spark._jvm.org.apache.hadoop.util.VersionInfo.getVersion.return_value = "3.3.4"
        self.filesystem = Mock()
        self.filesystem.getUri.return_value.getScheme.return_value = "hdfs"
        self.filesystem.isDirectory.return_value = True
        self.filesystem.getClass.return_value.getName.return_value = "org.apache.hadoop.hdfs.DistributedFileSystem"
        summary = self.filesystem.getContentSummary.return_value
        summary.getFileCount.return_value = 40
        summary.getDirectoryCount.return_value = 15
        summary.getLength.return_value = 10000
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        def patched(name, **kwargs):
            return self.stack.enter_context(patch(name, **kwargs))
        patched("data_analysis.spark_jobs.pipeline._filesystem", return_value=(self.filesystem, Mock()))
        patched("data_analysis.spark_jobs.pipeline._qualified", side_effect=lambda spark, path: path)
        prefix = "data_analysis.spark_jobs.fs."
        patched(prefix + "independent_paths")
        patched(prefix + "require_raw_complete")
        patched(prefix + "verify_raw_files", return_value=True)
        self.require_success = patched(prefix + "require_success")
        self.is_file = patched(prefix + "is_file", return_value=True)
        self.file_size = patched(prefix + "file_size", return_value=25)
        self.checksum = patched(prefix + "checksum", side_effect=lambda spark, path:
            "a" * 64 if path == RAW + "/manifest.json" else "b" * 64 if path == EXPORT + "/quality_report.json" else "c" * 64)
        self.documents = {RAW + "/manifest.json": {"dataset_id": "fixture"},
            PROCESSED + "/reports/quality_report/part-00000.json": self.quality,
            EXPORT + "/serving_manifest.json": self.serving, EXPORT + "/table_schemas.json": self.schemas,
            EXPORT + "/quality_report.json": export_quality_fixture(self.quality)}
        patched(prefix + "read_json", side_effect=lambda spark, path: self.documents[path])
        self.inventory = [EXPORT + "/" + shard["path"] for metadata in self.serving["tables"].values() for shard in metadata["files"]]
        self.glob = patched(prefix + "glob", side_effect=lambda spark, path:
            [PROCESSED + "/reports/quality_report/part-00000.json"] if "/reports/quality_report/" in path else self.inventory)

    def verify(self):
        return verify_hdfs_batch(self.spark, RAW, PROCESSED, EXPORT)

    def test_success_checks_real_hdfs_scheme_completion_inventory_and_all_hashes(self):
        report = self.verify()
        self.assertEqual(report["status"], "VERIFIED_HDFS")
        self.assertTrue(report["exportVerified"])
        self.require_success.assert_has_calls([call(self.spark, PROCESSED), call(self.spark, EXPORT)])
        self.assertEqual(self.checksum.call_count, 12)
        self.assertIn("not a multi-node", report["scope"])
        self.filesystem.create.assert_not_called()
        self.filesystem.delete.assert_not_called()
        self.filesystem.mkdirs.assert_not_called()

    def test_local_filesystem_never_certifies_hdfs(self):
        self.filesystem.getUri.return_value.getScheme.return_value = "file"
        with self.assertRaisesRegex(ValueError, "not HDFS"):
            self.verify()

    def test_missing_extra_or_duplicate_inventory_cannot_pass(self):
        self.inventory.pop()
        with self.assertRaisesRegex(ValueError, "inventory"):
            self.verify()
        self.inventory.append(EXPORT + "/csv/unexpected/part-00000.csv.gz")
        with self.assertRaisesRegex(ValueError, "inventory"):
            self.verify()

    def test_missing_file_size_or_checksum_mismatch_fails(self):
        self.is_file.return_value = False
        with self.assertRaisesRegex(ValueError, "checksum/size"):
            self.verify()
        self.is_file.return_value = True
        self.file_size.return_value = 24
        with self.assertRaisesRegex(ValueError, "checksum/size"):
            self.verify()
        self.file_size.return_value = 25
        self.checksum.side_effect = lambda spark, path: "a" * 64 if path == RAW + "/manifest.json" else "d" * 64
        with self.assertRaisesRegex(ValueError, "checksum/size"):
            self.verify()

    def test_quality_report_hash_and_batch_mismatch_fail(self):
        self.serving["qualityReportSha256"] = "d" * 64
        with self.assertRaisesRegex(ValueError, "quality report checksum"):
            self.verify()
        self.serving["qualityReportSha256"] = "b" * 64
        self.documents[EXPORT + "/quality_report.json"]["station_hourly_rows"] = 99
        with self.assertRaisesRegex(ValueError, "quality report differs"):
            self.verify()

    def test_incomplete_export_or_empty_table_manifest_never_pass(self):
        self.require_success.side_effect = lambda spark, path: (_ for _ in ()).throw(ValueError("incomplete")) if path == EXPORT else None
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self.verify()
        self.require_success.side_effect = None
        self.serving["tables"] = {}
        with self.assertRaisesRegex(ValueError, "ten tables"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
