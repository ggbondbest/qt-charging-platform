"""Opt-in real Spark integration tests: RUN_SPARK_TESTS=1 python -m unittest ..."""

import csv
import gzip
import json
import os
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

from data_analysis.charging_data.schema import SCHEMA_VERSION, TABLES


@unittest.skipUnless(os.environ.get("RUN_SPARK_TESTS") == "1",
                     "Requires explicit RUN_SPARK_TESTS=1, PySpark 3.5.6, and Java 17")
class SparkPipelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("SPARK_LOCAL_IP", "127.0.0.1")
        from pyspark.sql import SparkSession
        cls.spark = (SparkSession.builder.master("local[2]")
                     .appName("charging-batch-tests")
                     .config("spark.ui.enabled", "false")
                     .config("spark.sql.shuffle.partitions", "2")
                     .config("spark.sql.session.timeZone", "UTC").getOrCreate())
        cls.spark.sparkContext.setLogLevel("ERROR")

    @classmethod
    def tearDownClass(cls):
        cls.spark.stop()

    def frame(self, table, records):
        from pyspark.sql import types as T
        from data_analysis.spark_jobs.pipeline import field_type
        schema = T.StructType([T.StructField(name, field_type(name)) for name in TABLES[table]])
        return self.spark.createDataFrame([
            tuple(record.get(name) for name in TABLES[table]) for record in records], schema)

    def fixture_records(self):
        stamp = datetime(2025, 12, 1, 15, 0, tzinfo=timezone.utc)
        session = {
            "session_id": "s1", "attempt_id": "a1", "user_id": "u1", "vehicle_id": "v1",
            "station_id": "st1", "charger_id": "ch1", "started_at": stamp,
            "ended_at": stamp + timedelta(minutes=55), "unplugged_at": stamp + timedelta(minutes=55),
            "energy_wh": 5500, "grid_energy_wh": 5800,
            "electricity_fee_cents": 550, "service_fee_cents": 50,
            "parking_fee_cents": 0, "discount_cents": 0, "total_fee_cents": 600,
            "grid_cost_cents": 220, "status": "COMPLETED", "stop_reason": "TARGET_REACHED",
            "start_soc_pct": 20.0, "end_soc_pct": 30.0,
            "target_mode": "ENERGY", "target_value": 5.5,
        }
        rows = {table: [] for table in TABLES}
        rows["cities"] = [{"city_id": "city1", "city_name": "大连市", "timezone": "Asia/Shanghai"}]
        rows["stations"] = [{"station_id": "st1", "city_id": "city1", "station_name": "样本站"}]
        rows["chargers"] = [{"charger_id": name, "station_id": "st1"} for name in ["ch1", "ch2"]]
        rows["users"] = [{"user_id": "u1", "home_city_id": "city1"}]
        rows["vehicles"] = [{"vehicle_id": "v1", "user_id": "u1"}]
        rows["vehicle_energy_intervals"] = [{
            "interval_id": "ev1", "vehicle_id": "v1",
            "started_at": stamp - timedelta(hours=1), "ended_at": stamp,
            "start_soc_pct": 70.0, "end_soc_pct": 65.0,
            "driving_wh": 5000, "external_charge_wh": 2000,
        }]
        rows["charging_attempts"] = [{"attempt_id": "a1", "session_id": "s1", "user_id": "u1",
                                     "vehicle_id": "v1", "station_id": "st1", "charger_id": "ch1",
                                     "attempted_at": stamp, "outcome": "SUCCESS"}]
        rows["charging_sessions"] = [session]
        for minute in range(0, 60, 5):
            for charger in ["ch1", "ch2"]:
                charging = charger == "ch1" and minute < 55
                rows["charger_telemetry"].append({
                    "charger_id": charger, "station_id": "st1",
                    "recorded_at": stamp + timedelta(minutes=minute), "interval_seconds": 300,
                    "state": "CHARGING" if charging else "AVAILABLE",
                    "session_id": "s1" if charging else None,
                    "power_kw": 6.0 if charging else 0.0,
                    "energy_wh": 500 if charging else 0,
                    "grid_energy_wh": 527 if charging else 0,
                    "grid_cost_cents": 20 if charging else 0,
                    "meter_wh": (minute // 5 + 1) * 500 if charger == "ch1" else 0,
                    "online": 1,
                })
        rows["payments"] = [
            {"payment_id": "p1", "session_id": "s1", "user_id": "u1",
             "occurred_at": datetime(2025, 12, 1, 16, 1, tzinfo=timezone.utc), "transaction_type": "PAYMENT",
             "status": "SUCCESS", "amount_cents": 600},
            {"payment_id": "p2", "session_id": "s1", "user_id": "u1",
             "occurred_at": datetime(2025, 12, 1, 16, 2, tzinfo=timezone.utc), "transaction_type": "PAYMENT",
             "status": "FAILED", "amount_cents": 600},
            {"payment_id": "p3", "session_id": "s1", "user_id": "u1",
             "occurred_at": datetime(2025, 12, 1, 16, 3, tzinfo=timezone.utc), "transaction_type": "REFUND",
             "status": "SUCCESS", "amount_cents": 50},
        ]
        rows["operating_costs"] = [{"station_id": "st1", "business_date": stamp.date(),
                                    "rent_cents": 100, "labor_cents": 200, "network_cents": 10}]
        rows["maintenance_tickets"] = [{"ticket_id": "t1", "charger_id": "ch2", "station_id": "st1",
                                         "restored_at": datetime(2025, 12, 1, 16, 4, tzinfo=timezone.utc),
                                         "labor_cost_cents": 100, "parts_cost_cents": 200,
                                         "status": "RESOLVED"}]
        return rows

    def write_fixture(self, root, rows):
        root.mkdir()
        (root / "manifest.json").write_text(json.dumps({
            "dataset_id": "spark_test", "schema_version": SCHEMA_VERSION,
            "tables": {table: {"rows": len(records)} for table, records in rows.items()}}), encoding="utf-8")
        for table, fields in TABLES.items():
            folder = root / "raw" / table
            folder.mkdir(parents=True)
            with gzip.open(folder / "part-00000.csv.gz", "wt", encoding="utf-8", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields)
                writer.writeheader()
                for row in rows[table]:
                    output = {}
                    for key in fields:
                        value = row.get(key)
                        if isinstance(value, datetime):
                            value = value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
                        output[key] = value
                    writer.writerow(output)

    def test_hourly_energy_and_last_available_are_not_power_sum(self):
        from data_analysis.spark_jobs.pipeline import station_hourly, assert_telemetry_contract
        rows = self.fixture_records()
        tables = {key: self.frame(key, value) for key, value in rows.items()}
        assert_telemetry_contract(tables["charger_telemetry"], tables["stations"], tables["chargers"])
        result = station_hourly(tables["charger_telemetry"], tables["stations"], tables["chargers"]).first()
        self.assertEqual(result.energy_wh, 5500)
        self.assertEqual(result.mean_power_kw, 5.5)
        self.assertEqual(result.sample_count, 24)
        self.assertEqual(result.charging_samples, 11)
        self.assertEqual(result.available_samples, 13)
        self.assertEqual(result.capacity, 2)
        self.assertEqual(result.end_available_count, 2)

    def test_cashflow_uses_shanghai_payment_day_not_order_day(self):
        from data_analysis.spark_jobs.pipeline import station_daily
        tables = {key: self.frame(key, value) for key, value in self.fixture_records().items()}
        result = {str(row.business_date): row for row in station_daily(tables).collect()}
        self.assertEqual(result["2025-12-01"].energy_wh, 5500)
        self.assertEqual(result["2025-12-01"].completed_sessions, 1)
        self.assertEqual(result["2025-12-01"].paid_cents, 0)
        self.assertEqual(result["2025-12-01"].grid_cost_cents, 220)
        self.assertEqual(result["2025-12-01"].operating_cost_cents, 310)
        self.assertEqual(result["2025-12-02"].paid_cents, 600)
        self.assertEqual(result["2025-12-02"].refund_cents, 50)
        self.assertEqual(result["2025-12-02"].maintenance_cost_cents, 300)

    def test_gzip_cleaning_rejects_bad_clone_before_dedup(self):
        from data_analysis.spark_jobs.pipeline import read_table, clean_sessions
        rows = self.fixture_records()
        original = rows["charging_sessions"][0]
        rows["charging_sessions"] += [dict(original), dict(original, status=" completed "),
                                        dict(original, total_fee_cents=-1),
                                        dict(original, station_id="missing"),
                                        dict(original, session_id="")]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "input"
            self.write_fixture(root, rows)
            raw = read_table(self.spark, str(root), "charging_sessions")
            tables = {key: self.frame(key, value) for key, value in self.fixture_records().items()}
            clean, rejected = clean_sessions(raw, tables)
            self.assertEqual(clean.count(), 1)
            self.assertEqual(clean.first().status, "COMPLETED")
            reasons = {row.rejection_reason: row["count"]
                       for row in rejected.groupBy("rejection_reason").count().collect()}
            self.assertEqual(sum(reasons.values()), 5)
            self.assertEqual(reasons["DUPLICATE_SESSION_ID"], 2)
            self.assertIn("UNKNOWN_STATION_ID", reasons)
            self.assertIn("MISSING_SESSION_ID", reasons)
            self.assertIn("INVALID_NONNEGATIVE_TOTAL_FEE_CENTS", reasons)

    def test_duplicate_telemetry_is_not_silently_double_counted(self):
        from data_analysis.spark_jobs.pipeline import assert_telemetry_contract
        rows = self.fixture_records()
        rows["charger_telemetry"].append(dict(rows["charger_telemetry"][0]))
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            assert_telemetry_contract(self.frame("charger_telemetry", rows["charger_telemetry"]),
                                      self.frame("stations", rows["stations"]),
                                      self.frame("chargers", rows["chargers"]))

    def test_end_to_end_parquet_and_overwrite_refusal(self):
        from data_analysis.spark_jobs.pipeline import run_pipeline
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            input_path, output_path = root / "input", root / "output"
            self.write_fixture(input_path, self.fixture_records())
            report = run_pipeline(self.spark, str(input_path), str(output_path))
            self.assertEqual(report["clean_session_rows"], 1)
            self.assertEqual(report["rejected_session_rows"], 0)
            self.assertFalse(report["reference_aggregates_used_as_input"])
            self.assertTrue(report["manifest_row_counts_verified"])
            self.assertTrue((output_path / "_SUCCESS").is_file())
            self.assertFalse((output_path / "_RUNNING").exists())
            actual = self.spark.read.parquet(str(output_path / "statistics/station_hourly")).first()
            self.assertEqual(actual.energy_wh, 5500)
            external = self.spark.read.parquet(str(output_path / "clean/vehicle_energy_intervals"))
            self.assertEqual(external.schema["driving_wh"].dataType.simpleString(), "bigint")
            self.assertEqual(external.first().external_charge_wh, 2000)
            # Off-network charging is retained for SOC analysis, not added to
            # platform electricity sales, receipts or grid purchase costs.
            daily = {str(row.business_date): row for row in self.spark.read.parquet(
                str(output_path / "statistics/station_daily")).collect()}
            self.assertEqual(daily["2025-12-01"].energy_wh, 5500)
            self.assertEqual(daily["2025-12-01"].grid_cost_cents, 220)
            self.assertEqual(daily["2025-12-02"].paid_cents, 600)
            with self.assertRaises(FileExistsError):
                run_pipeline(self.spark, str(input_path), str(output_path))
            with self.assertRaisesRegex(ValueError, "non-nested"):
                run_pipeline(self.spark, str(input_path), str(input_path / "processed"))

    def test_missing_rows_and_shards_fail_manifest_before_success(self):
        from data_analysis.spark_jobs.pipeline import run_pipeline
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for scenario in ["missing_row", "missing_shard"]:
                with self.subTest(scenario=scenario):
                    dataset, output = root / scenario, root / (scenario + "_output")
                    self.write_fixture(dataset, self.fixture_records())
                    original = dataset / "raw/charger_telemetry/part-00000.csv.gz"
                    with gzip.open(original, "rt", encoding="utf-8", newline="") as stream:
                        records = list(csv.DictReader(stream))
                    if scenario == "missing_row":
                        retained = records[:-1]
                    else:
                        retained = records[:12]
                        # Build two real input shards, then remove one as a
                        # missing-upload case. The manifest still expects 24 rows.
                        missing = original.with_name("part-00001.csv.gz")
                        with gzip.open(missing, "wt", encoding="utf-8", newline="") as stream:
                            writer = csv.DictWriter(stream, fieldnames=TABLES["charger_telemetry"])
                            writer.writeheader()
                            writer.writerows(records[12:])
                        missing.unlink()
                    with gzip.open(original, "wt", encoding="utf-8", newline="") as stream:
                        writer = csv.DictWriter(stream, fieldnames=TABLES["charger_telemetry"])
                        writer.writeheader()
                        writer.writerows(retained)
                    with self.assertRaisesRegex(ValueError, "Manifest row count mismatch for charger_telemetry"):
                        run_pipeline(self.spark, str(dataset), str(output))
                    self.assertFalse((output / "_SUCCESS").exists())
                    self.assertFalse((output / "statistics").exists())

    def test_generated_data_matches_independent_python_reference(self):
        """Compare every hourly/daily metric, not only a grand-total checksum."""
        from data_analysis.charging_data.generator import generate_dataset
        from data_analysis.charging_data.io import read_table as reference_rows
        from data_analysis.spark_jobs.pipeline import run_pipeline
        from data_analysis.spark_jobs.verify_aggregates import verify_aggregates
        config = {"dataset_id": "spark_reference_test", "seed": 20260912,
                  "start_date": "2025-12-01", "days": 1, "users_per_city": 15,
                  "interval_minutes": 5, "dirty_rate": 0.05}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dataset, output = root / "dataset", root / "spark_output"
            manifest = generate_dataset(config, dataset)
            report = run_pipeline(self.spark, str(dataset), str(output))
            self.assertEqual(report["clean_session_rows"], manifest["canonical_session_count"])
            for table, time_key in [("station_hourly", "recorded_at"),
                                    ("station_daily", "business_date")]:
                expected = {(row["station_id"], row[time_key]): row
                            for row in reference_rows(dataset, table, prefix="reference_aggregates")}
                actual = self.spark.read.parquet(str(output / "statistics" / table)).collect()
                self.assertEqual(len(actual), len(expected))
                for row in actual:
                    timestamp = row[time_key]
                    # Collected Spark timestamps are naive driver-local values;
                    # convert explicitly instead of requiring time.tzset (absent on Windows).
                    timestamp_text = (timestamp.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
                                      if isinstance(timestamp, datetime)
                                      else str(timestamp))
                    reference = expected[(row.station_id, timestamp_text)]
                    for field, value in row.asDict().items():
                        if field in {"station_id", "city_id", time_key}:
                            continue
                        if isinstance(value, float):
                            self.assertAlmostEqual(value, float(reference[field]), places=6,
                                                   msg=f"{table}/{row.station_id}/{timestamp_text}/{field}")
                        else:
                            self.assertEqual(value, int(reference[field]),
                                             msg=f"{table}/{row.station_id}/{timestamp_text}/{field}")
            verified = verify_aggregates(self.spark, str(dataset), str(output))
            self.assertEqual(verified["status"], "PASSED")
            self.assertEqual(verified["comparison"]["station_hourly"]["rows"], 600)
            # Alter only an isolated temporary control file *after* production
            # calculation: the verifier must detect a one-Wh difference.
            control = next((dataset / "reference_aggregates/station_hourly").glob("part-*.csv.gz"))
            with gzip.open(control, "rt", encoding="utf-8", newline="") as stream:
                reader = csv.DictReader(stream)
                fields, controls = reader.fieldnames, list(reader)
            controls[0]["energy_wh"] = str(int(controls[0]["energy_wh"]) + 1)
            with gzip.open(control, "wt", encoding="utf-8", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields)
                writer.writeheader()
                writer.writerows(controls)
            with self.assertRaisesRegex(ValueError, "Aggregate mismatch for station_hourly"):
                verify_aggregates(self.spark, str(dataset), str(output))


if __name__ == "__main__":
    unittest.main()
