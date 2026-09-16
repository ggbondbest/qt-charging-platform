"""Production-cleaner fault injection: fast plan tests plus opt-in Spark audit."""

from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

from data_analysis.charging_data.schema import TABLES
from data_analysis.spark_jobs.cleaning_challenge import (
    CASES, MAX_SAMPLE_ROWS, _csv, digest, evaluate_frames, make_cases,
    validate_paths, verify_source,
)


def fixture_records(count=40):
    stamp = datetime(2026, 1, 1, tzinfo=timezone.utc)
    result = []
    for index in range(count):
        record = {name: None for name in TABLES["charging_sessions"]}
        for name in record:
            if name.endswith("_cents") or name.endswith("_wh"):
                record[name] = 0
        record.update(session_id=f"session-{index:04d}", attempt_id=f"attempt-{index:04d}",
            user_id="user-A", vehicle_id="vehicle-A", station_id="station-A", charger_id="charger-A",
            started_at=stamp + timedelta(hours=index), ended_at=stamp + timedelta(hours=index+1),
            unplugged_at=stamp + timedelta(hours=index+1, minutes=5),
            energy_wh=10000, grid_energy_wh=11000, electricity_fee_cents=800,
            service_fee_cents=200, total_fee_cents=1000, grid_cost_cents=500,
            status="COMPLETED", stop_reason="TARGET_REACHED", start_soc_pct=20.0,
            end_soc_pct=40.0, target_mode="ENERGY", target_value=10.0)
        result.append(record)
    return result


class ChallengePlanTests(unittest.TestCase):
    def test_twenty_distinct_reasoned_cases_are_deterministic(self):
        self.assertEqual(len(CASES), 20)
        self.assertEqual(len({case[0] for case in CASES}), 20)
        self.assertLessEqual(MAX_SAMPLE_ROWS, 5000)
        args = (fixture_records(), ["station-A", "station-B"], ["user-A", "user-B"])
        first = make_cases(*args)
        self.assertEqual(first, make_cases(*args))
        baseline, dirty, labels = first
        self.assertEqual(len(baseline), 40)
        self.assertEqual(len(dirty), 72)  # 4 format-only cases each round; other cases add a copy
        self.assertEqual(len(labels), 40)
        self.assertTrue(all(len(row["sourceSessionSha256"]) == 64 for row in labels))
        self.assertFalse(any("user_id" in row or "session_id" in row for row in labels))

    def test_original_records_are_not_mutated_and_invalid_facts_are_not_invented(self):
        records = fixture_records()
        originals = [dict(row) for row in records]
        baseline, dirty, labels = make_cases(records, ["station-A", "station-B"], ["user-A", "user-B"])
        self.assertEqual(records, originals)
        invalid_types = [label for label in labels if label["expectedAction"] == "QUARANTINE"]
        self.assertTrue(all(label["expectedReason"] for label in invalid_types))
        self.assertIn("2O000", {row["energy_wh"] for row in dirty})
        self.assertIn("9223372036854775808", {row["energy_wh"] for row in dirty})
        self.assertEqual(sum(int(row["total_fee_cents"]) for row in baseline), 40000)

    def test_small_or_single_station_source_refused(self):
        with self.assertRaises(ValueError):
            make_cases(fixture_records(3), ["a", "b"], ["a", "b"])
        with self.assertRaises(ValueError):
            make_cases(fixture_records(), ["a"], ["a", "b"])

    def test_paths_are_new_only_and_independent(self):
        with tempfile.TemporaryDirectory(prefix="cleaning-challenge-paths-") as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            validate_paths(source, root / "new-output")
            for destination in (source, source / "child", root):
                with self.assertRaises((ValueError, FileExistsError)):
                    validate_paths(source, destination)
            existing = root / "existing"
            existing.mkdir()
            with self.assertRaises(FileExistsError):
                validate_paths(source, existing)

    def test_source_manifest_and_clean_checksums_are_verified(self):
        from data_analysis.spark_jobs.cleaning_challenge import INPUT_TABLES
        with tempfile.TemporaryDirectory(prefix="cleaning-challenge-inventory-") as temporary:
            root = Path(temporary)
            (root / "_SUCCESS").touch()
            serving = root / "serving_manifest.json"
            bindings = dict(datasetId="test", publishedBatchId="batch", pipelineRunId="spark",
                            sourceManifestSha256="a" * 64)
            serving.write_text(json.dumps(bindings), encoding="utf-8")
            files = [serving]
            for name in INPUT_TABLES:
                file = root / "clean" / name / "part-00000.parquet"
                file.parent.mkdir(parents=True)
                file.write_bytes(b"verified-test-bytes")
                files.append(file)
            manifest = dict(**bindings, servingManifestSha256=digest(serving), files=[dict(path=file.relative_to(root).as_posix(),
                bytes=file.stat().st_size, sha256=digest(file)) for file in files])
            (root / "acceptance_manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            self.assertEqual(verify_source(root)[0]["datasetId"], "test")
            files[-1].write_bytes(b"changed")
            with self.assertRaises(ValueError):
                verify_source(root)


@unittest.skipUnless(os.environ.get("RUN_SPARK_TESTS") == "1", "Requires RUN_SPARK_TESTS=1, Java17 and PySpark")
class ChallengeSparkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from pyspark.sql import SparkSession, types as T
        from data_analysis.spark_jobs.pipeline import field_type, read_table, clean_sessions
        os.environ.setdefault("SPARK_LOCAL_IP", "127.0.0.1")
        cls.spark = (SparkSession.builder.master("local[1]").appName("cleaning-challenge-tests")
            .config("spark.driver.memory", "2g").config("spark.ui.enabled", "false")
            .config("spark.sql.session.timeZone", "UTC").config("spark.sql.shuffle.partitions", "1").getOrCreate())
        cls.addClassCleanup(cls.spark.stop)
        cls.spark.sparkContext.setLogLevel("ERROR")
        cls.temporary = tempfile.TemporaryDirectory(prefix="cleaning-challenge-spark-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        records = fixture_records()
        baseline, dirty, cls.labels = make_cases(records, ["station-A", "station-B"], ["user-A", "user-B"])
        _csv(cls.root / "raw" / "charging_sessions" / "part-00000.csv.gz", dirty)
        cls.original = cls.spark.createDataFrame(
            [tuple(row[field] for field in TABLES["charging_sessions"]) for row in records],
            T.StructType([T.StructField(field, field_type(field)) for field in TABLES["charging_sessions"]]))
        dimensions = {
            "stations": cls.spark.createDataFrame([("station-A",), ("station-B",)], ["station_id"]),
            "chargers": cls.spark.createDataFrame([("charger-A", "station-A")], ["charger_id", "station_id"]),
            "users": cls.spark.createDataFrame([("user-A",), ("user-B",)], ["user_id"]),
            "vehicles": cls.spark.createDataFrame([("vehicle-A", "user-A")], ["vehicle_id", "user_id"]),
            "charging_attempts": cls.original.select("attempt_id", "session_id", "user_id", "vehicle_id", "station_id", "charger_id"),
        }
        cls.raw = read_table(cls.spark, str(cls.root), "charging_sessions").cache()
        cls.addClassCleanup(cls.raw.unpersist)
        clean, rejected = clean_sessions(cls.raw, dimensions)
        cls.clean, cls.rejected = clean.cache(), rejected.cache()
        cls.addClassCleanup(cls.clean.unpersist)
        cls.addClassCleanup(cls.rejected.unpersist)
        cls.report = evaluate_frames(cls.raw, cls.original, cls.clean, cls.rejected, cls.labels)

    def test_all_labeled_operations_match_actual_production_cleaner(self):
        failed = [row for row in self.report["cases"] if not row["passed"]]
        self.assertEqual(failed, [])
        self.assertTrue(self.report["passed"])
        self.assertEqual((self.report["testedCases"], self.report["passedCases"]), (40, 40))
        self.assertEqual(self.report["testedCaseTypes"], 20)

    def test_quarantine_and_dedup_conserve_rows_and_exact_original_values(self):
        self.assertEqual((self.report["inputRows"], self.report["cleanRows"], self.report["rejectedRows"]), (72, 40, 32))
        self.assertEqual(self.report["invariants"], dict(rowConservation=True,
            missingOrChangedBaselineRows=0, unexpectedCleanRows=0, exactBaselineRestored=True))
        self.assertEqual(self.report["rejectionReasons"]["DUPLICATE_SESSION_ID"], 2)
        impact = self.report["metricImpact"]
        self.assertEqual(impact["afterBillDistortionCents"], 0)
        self.assertGreater(impact["beforeBillDistortionCents"], 0)
        self.assertEqual(impact["beforeNaiveParsedSum"]["nullEnergyRows"], 4)

    def test_false_oracle_cannot_produce_passing_evidence(self):
        # Use a deliberately incorrect label: actual rejection evidence wins.
        labels = [dict(row) for row in self.labels]
        labels[0]["expectedReason"] = "MADE_UP_REASON"
        report = evaluate_frames(self.raw, self.original, self.clean, self.rejected, labels)
        self.assertFalse(report["passed"])
        self.assertFalse(report["cases"][0]["passed"])


if __name__ == "__main__":
    unittest.main()
