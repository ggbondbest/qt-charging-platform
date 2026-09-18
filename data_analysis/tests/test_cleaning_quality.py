"""Conservative cleaning and six-dimension audit regression, using tiny fixtures."""

import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

from data_analysis.spark_jobs.normalization import RULES, RULE_VERSION
from data_analysis.spark_jobs.quality import AUDIT_VERSION, PRIMARY_KEYS, RULE_SHA256
from data_analysis.charging_data.schema import TABLES


class QualityRuleCatalogTests(unittest.TestCase):
    def test_versioned_rules_and_all_primary_keys_are_explicit(self):
        self.assertEqual(RULE_VERSION, "cleaning-rules-1.0.0")
        self.assertEqual(AUDIT_VERSION, "cleaning-audit-1.0.0")
        self.assertEqual(len({rule["id"] for rule in RULES}), len(RULES))
        encoded = json.dumps(RULES, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
        self.assertEqual(hashlib.sha256(encoded).hexdigest(), RULE_SHA256)
        self.assertEqual(set(PRIMARY_KEYS), set(TABLES))
        for table, columns in PRIMARY_KEYS.items():
            self.assertTrue(set(columns) <= set(TABLES[table]))


@unittest.skipUnless(os.environ.get("RUN_SPARK_TESTS") == "1", "Requires opt-in Java17/PySpark3.5.6 runtime")
class CleaningQualitySparkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from pyspark.sql import SparkSession
        from data_analysis.tests.test_spark_pipeline import SparkPipelineTests
        cls.spark = (SparkSession.builder.master("local[2]").appName("cleaning-quality-tests")
                     .config("spark.ui.enabled", "false").config("spark.sql.shuffle.partitions", "2")
                     .config("spark.sql.session.timeZone", "UTC").config("spark.sql.ansi.enabled", "false").getOrCreate())
        cls.spark.sparkContext.setLogLevel("ERROR")
        cls.helper = SparkPipelineTests()
        cls.helper.spark = cls.spark

    @classmethod
    def tearDownClass(cls):
        cls.spark.stop()

    def rows_with_noise(self):
        rows = self.helper.fixture_records()
        session = rows["charging_sessions"][0]
        rows["charging_sessions"] += [
            dict(session, session_id="\ufeffs\u200b1\u200b", status="\u3000comPleted\t", total_fee_cents="６００",
                 grid_cost_cents="２２０", target_value="５．５", started_at="２０２５-１２-０１T１５:００:００Z"),
            dict(session, total_fee_cents="6.00"), dict(session, total_fee_cents="600元"),
            dict(session, total_fee_cents="－１"), dict(session, status="COMP\ufffdLETED"),
            dict(session, session_id="\u3000\t\u200b"),
        ]
        return rows

    def test_unicode_standardization_preserves_raw_and_never_guesses_money(self):
        from data_analysis.spark_jobs.pipeline import read_table, clean_sessions
        from data_analysis.spark_jobs.quality import profile_table
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "raw"
            self.helper.write_fixture(source, self.rows_with_noise())
            raw = read_table(self.spark, str(source), "charging_sessions").cache()
            try:
                tables = {name: self.helper.frame(name, records) for name, records in self.helper.fixture_records().items()}
                clean, rejected = clean_sessions(raw, tables)
                self.assertEqual(clean.count(), 1)
                self.assertEqual(clean.first().total_fee_cents, 600)
                reasons = {row.rejection_reason: row["count"] for row in rejected.groupBy("rejection_reason").count().collect()}
                self.assertEqual(reasons, {"DUPLICATE_SESSION_ID": 1, "INVALID_TYPE_OR_CSV": 2,
                                         "INVALID_NONNEGATIVE_TOTAL_FEE_CENTS": 1, "INVALID_TEXT_ENCODING": 1, "MISSING_SESSION_ID": 1})
                before = profile_table(raw, "charging_sessions", before=True)
                after = profile_table(clean, "charging_sessions", before=False)
                self.assertEqual(before["field_profiles"]["total_fee_cents"]["type_or_encoding_invalid_rows"], 4)
                self.assertEqual(after["field_profiles"]["total_fee_cents"]["type_or_encoding_invalid_rows"], 0)
                self.assertEqual(before["field_profiles"]["total_fee_cents"]["observed_range"], {"min": -1, "max": 600})
                self.assertEqual(after["field_profiles"]["total_fee_cents"]["observed_range"], {"min": 600, "max": 600})
                self.assertEqual(before["field_profiles"]["session_id"]["blank_text_rows"], 1)
                self.assertEqual(before["normalization_action_rows"]["N002"], 1)
                self.assertEqual(before["normalization_action_rows"]["N003"], 2)
                example = raw.filter("array_contains(_normalization_actions, 'N002')").first()
                self.assertEqual(example.session_id, "s1")
                self.assertEqual(json.loads(example._raw_json)["total_fee_cents"], "６００")
                self.assertEqual(hashlib.sha256(example._raw_json.encode("utf-8")).hexdigest(), example._raw_record_sha256)
                self.assertIn("charging_sessions/part-", example._source_file)
            finally:
                raw.unpersist()

    def test_no_imputation_no_nonfinite_and_no_ambiguous_timestamps(self):
        from data_analysis.spark_jobs.pipeline import read_table
        rows = self.helper.fixture_records()
        original = rows["charging_sessions"][0]
        rows["charging_sessions"] = [dict(original, total_fee_cents="", target_value="NaN"),
            dict(original, target_value="Infinity"), dict(original, started_at="2025-12-01 15:00:00"),
            dict(original, started_at="2025-12-01T23:00:00+08:00"),
            dict(original, target_value="1000000.0")]
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "raw"
            self.helper.write_fixture(source, rows)
            actual = read_table(self.spark, str(source), "charging_sessions").collect()
            self.assertEqual(sum(row._parse_error for row in actual), 3)
            missing = [row for row in actual if row.total_fee_cents is None]
            self.assertEqual(len(missing), 1)
            self.assertIn("total_fee_cents", missing[0]._raw_missing_fields)
            self.assertTrue(any(row.target_value == 1000000.0 and not row._parse_error for row in actual))
            offset = [row for row in actual if "+08:00" in row._raw_json][0]
            self.assertFalse(offset._parse_error)

    def test_non_session_encoding_is_quarantined_and_batch_fails_closed(self):
        from data_analysis.spark_jobs.pipeline import run_pipeline
        rows = self.helper.fixture_records()
        rows["cities"][0]["city_name"] = "大\ufffd市"
        with tempfile.TemporaryDirectory() as temporary:
            source, output = Path(temporary) / "raw", Path(temporary) / "output"
            self.helper.write_fixture(source, rows)
            shard = source / "raw/cities/part-00000.csv.gz"
            original_hash = hashlib.sha256(shard.read_bytes()).hexdigest()
            with self.assertRaisesRegex(ValueError, "Unexpected invalid types"):
                run_pipeline(self.spark, str(source), str(output))
            self.assertFalse((output / "_SUCCESS").exists())
            self.assertTrue((output / "_RUNNING").exists())
            self.assertTrue((output / "reports/cleaning_rules.json").exists())
            failure = json.loads((output / "reports/cleaning_failure.json").read_text())
            self.assertEqual(failure["status"], "FAILED")
            rejected = self.spark.read.parquet(str(output / "rejected/cities")).first()
            self.assertEqual(rejected.rejection_reason, "INVALID_TEXT_ENCODING")
            self.assertEqual(hashlib.sha256(shard.read_bytes()).hexdigest(), original_hash)

    def test_complete_audit_conservation_lineage_and_historical_timeliness(self):
        from data_analysis.spark_jobs.pipeline import run_pipeline
        with tempfile.TemporaryDirectory() as temporary:
            source, output = Path(temporary) / "raw", Path(temporary) / "output"
            self.helper.write_fixture(source, self.rows_with_noise())
            manifest_path = source / "manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest.update(period_start="2025-12-01T00:00:00Z", period_end_exclusive="2025-12-03T00:00:00Z")
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            original_hash = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
            report = run_pipeline(self.spark, str(source), str(output))
            self.assertEqual(report["clean_session_rows"], 1)
            self.assertEqual(report["rejected_session_rows"], 6)
            self.assertEqual(report["normalized_enum_rows"]["charging_sessions"], 1)
            self.assertTrue(report["manifest_row_counts_verified"])
            self.assertFalse(report["reference_aggregates_used_as_input"])
            summary = report["cleaning_audit_summary"]
            self.assertTrue(summary["row_conservation_passed"])
            for path_field, hash_field in [("audit_path", "audit_sha256"), ("rules_path", "rules_file_sha256")]:
                self.assertEqual(hashlib.sha256((output / summary[path_field]).read_bytes()).hexdigest(), summary[hash_field])
            audit = json.loads((output / summary["audit_path"]).read_text())
            self.assertEqual(set(audit["tables"]), set(TABLES))
            self.assertEqual(audit["lineage"]["source_manifest_sha256"], original_hash)
            self.assertFalse(audit["lineage"]["source_layer_modified"])
            counts = audit["tables"]["charging_sessions"]["row_conservation"]
            self.assertEqual(counts, {"input": 7, "retained": 1, "quarantined": 5, "duplicate_copies": 1, "passed": True})
            dimensions = audit["six_dimensions"]
            self.assertEqual(set(dimensions), {"accuracy", "completeness", "validity", "uniqueness", "consistency", "timeliness"})
            self.assertEqual(dimensions["accuracy"]["status"], "NOT_MEASURED")
            self.assertIsNone(dimensions["accuracy"]["rate"])
            self.assertEqual(dimensions["timeliness"]["after"]["rate"], 1.0)
            self.assertEqual(dimensions["validity"]["after"]["rate"], 1.0)
            self.assertTrue(audit["correction_samples"])
            empty_changes = [change for sample in audit["correction_samples"] for change in sample["changes"]
                             if change["field"] == "session_id" and change["after"] is None]
            self.assertEqual(len(empty_changes), 1)
            self.assertEqual(empty_changes[0]["before"], "\u3000\t\u200b")
            self.assertEqual(len(audit["issue_samples"]), 6)
            encoding_issue = next(item for item in audit["issue_samples"] if item["rejection_reason"] == "INVALID_TEXT_ENCODING")
            self.assertEqual(encoding_issue["text_error_fields"], ["status"])
            type_issues = [item for item in audit["issue_samples"] if item["rejection_reason"] == "INVALID_TYPE_OR_CSV"]
            self.assertTrue(all(item["type_error_fields"] == ["total_fee_cents"] for item in type_issues))
            self.assertTrue((output / "_SUCCESS").exists())
            self.assertEqual(hashlib.sha256(manifest_path.read_bytes()).hexdigest(), original_hash)


if __name__ == "__main__":
    unittest.main()
