"""Small independent acceptance aggregates; opt-in Spark, no full dataset scan."""

import csv
from datetime import datetime, timedelta, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

from data_analysis.charging_data.schema import SCHEMA_VERSION, TABLES
from data_analysis.spark_jobs.acceptance_analysis import ANALYSES, DIMENSIONS, INPUT_TABLES, METRICS, PREVIEW_LIMIT, group_columns


class AcceptanceDefinitionTests(unittest.TestCase):
    def test_real_dimensions_not_metric_count(self):
        used = {dimension for spec in ANALYSES.values() for dimension in spec["dimensions"]}
        self.assertEqual(used, set(DIMENSIONS))
        self.assertGreaterEqual(len(used), 8)
        self.assertEqual(len(used), 11)
        self.assertEqual(DIMENSIONS["city"], ["city_id", "city_name"])

    def test_at_least_two_distinct_comparisons_and_bounded_preview(self):
        comparisons = {tuple(spec["dimensions"]) for spec in ANALYSES.values() if len(spec["dimensions"]) >= 2}
        self.assertGreaterEqual(len(comparisons), 2)
        self.assertIn(("city", "start_hour"), comparisons)
        self.assertIn(("site_type", "day_type"), comparisons)
        self.assertEqual(PREVIEW_LIMIT, 12)
        for spec in ANALYSES.values():
            self.assertEqual(len(group_columns(spec)), len(set(group_columns(spec))))

    def test_metrics_have_units_and_explicit_denominators(self):
        self.assertIn("NOT cash", METRICS["billed_cents"][1])
        self.assertIn("payment_request_count", METRICS["payment_success_rate"][1])
        self.assertIn("restored_count", METRICS["mean_resolution_seconds"][1])
        self.assertTrue(all(unit and definition for unit, definition in METRICS.values()))


@unittest.skipUnless(os.environ.get("RUN_SPARK_TESTS") == "1", "Requires explicit RUN_SPARK_TESTS=1, Java17 and PySpark")
class AcceptanceSparkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from pyspark.sql import SparkSession, types as T
        from data_analysis.spark_jobs.pipeline import field_type, _qualified
        from data_analysis.spark_jobs.acceptance_analysis import build_analysis_frames
        cls.temporary = tempfile.TemporaryDirectory(prefix="acceptance-analysis-tests-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        os.environ.setdefault("SPARK_LOCAL_IP", "127.0.0.1")
        cls.spark = (SparkSession.builder.master("local[1]").appName("acceptance-dimension-tests")
            .config("spark.ui.enabled", "false").config("spark.driver.memory", "1g")
            .config("spark.sql.shuffle.partitions", "1").config("spark.sql.session.timeZone", "UTC").getOrCreate())
        cls.addClassCleanup(cls.spark.stop)
        cls.spark.sparkContext.setLogLevel("ERROR")
        cls.records = cls.fixture()
        cls.tables = {name: cls.spark.createDataFrame([tuple(row.get(key) for key in TABLES[name]) for row in cls.records[name]],
            T.StructType([T.StructField(key, field_type(key)) for key in TABLES[name]])) for name in INPUT_TABLES}
        cls.frames = build_analysis_frames(cls.tables)
        cls.raw, cls.processed = cls.root/"raw", cls.root/"processed"
        cls.raw.mkdir()
        metadata = {}
        for name, fields in TABLES.items():
            shard = cls.raw/"raw"/name/"part-00000.csv.gz"
            shard.parent.mkdir(parents=True)
            with gzip.open(shard,"wt",encoding="utf-8",newline="") as stream:
                writer = csv.DictWriter(stream,fieldnames=fields)
                writer.writeheader()
                for row in cls.records.get(name, []):
                    writer.writerow({key: value.isoformat().replace("+00:00","Z") if isinstance(value,datetime) else value
                                     for key,value in row.items()})
            count = len(cls.records.get(name, []))
            metadata[name] = dict(rows=count,files=[dict(path=shard.relative_to(cls.raw).as_posix(),rows=count,
                bytes=shard.stat().st_size,sha256=hashlib.sha256(shard.read_bytes()).hexdigest())])
        manifest = dict(dataset_id="acceptance_fixture",source="SIMULATED",schema_version=SCHEMA_VERSION,tables=metadata)
        (cls.raw/"manifest.json").write_text(json.dumps(manifest),encoding="utf-8")
        for name, frame in cls.tables.items():
            frame.write.mode("errorifexists").parquet(str(cls.processed/"clean"/name))
        cls.quality_path = cls.processed/"reports/quality_report/part-00000.json"
        cls.quality_path.parent.mkdir(parents=True)
        cls.quality_path.write_text(json.dumps(dict(dataset_id="acceptance_fixture", pipeline_run_id="fixture-spark",
            source_manifest_sha256=hashlib.sha256((cls.raw/"manifest.json").read_bytes()).hexdigest(),
            input=_qualified(cls.spark,str(cls.raw)),output=_qualified(cls.spark,str(cls.processed)),
            raw_file_checksums_verified=True,manifest_row_counts_verified=True,
            reference_aggregates_used_as_input=False,clean_session_rows=3)),encoding="utf-8")
        (cls.processed/"_SUCCESS").touch()

    @staticmethod
    def fixture():
        # UTC Sunday 15:30 is Shanghai Sunday 23:30; its end/payment is Monday.
        stamp = datetime(2025,11,30,15,30,tzinfo=timezone.utc)
        rows = {name: [] for name in INPUT_TABLES}
        rows["cities"] = [dict(city_id="c1",city_name="City A"),dict(city_id="c2",city_name="City B")]
        rows["stations"] = [dict(station_id="a",city_id="c1",station_name="A",site_type="OFFICE"),
                            dict(station_id="b",city_id="c2",station_name="B",site_type="TRANSIT")]
        rows["users"] = [dict(user_id="u1",segment="COMMUTER"),dict(user_id="u2",segment="FLEET")]
        rows["vehicles"] = [dict(vehicle_id="v1",user_id="u1",vehicle_class="SEDAN"),dict(vehicle_id="v2",user_id="u2",vehicle_class="SUV")]
        rows["chargers"] = [dict(charger_id="ch1",station_id="a",connector_type="DC"),dict(charger_id="ch2",station_id="b",connector_type="AC")]
        for sid, station, user, vehicle, charger, hours, energy, active, connected in [
            ("s1","a","u1","v1","ch1",0,1000,3600,7200),
            ("s2","a","u1","v1","ch1",2,2000,1800,1800),
            ("s3","b","u2","v2","ch2",3,3000,3600,5400)]:
            start = stamp+timedelta(hours=hours)
            rows["charging_sessions"].append(dict(session_id=sid,station_id=station,user_id=user,vehicle_id=vehicle,charger_id=charger,
                started_at=start,ended_at=start+timedelta(seconds=active),unplugged_at=start+timedelta(seconds=connected),
                energy_wh=energy,grid_energy_wh=energy+50,total_fee_cents=energy//10))
        for pid,sid,kind,status,channel,amount in [("p1","s1","PAYMENT","SUCCESS","WECHAT",100),
            ("p2","s1","REFUND","SUCCESS","WECHAT",20),("p3","s2","PAYMENT","FAILED","WECHAT",200),
            ("p4","s3","PAYMENT","SUCCESS","ALIPAY",300),("p5","s3","REFUND","SUCCESS","CASH",5)]:
            rows["payments"].append(dict(payment_id=pid,session_id=sid,occurred_at=stamp+timedelta(hours=4),
                transaction_type=kind,status=status,channel=channel,amount_cents=amount))
        rows["maintenance_tickets"] = [
            dict(ticket_id="t1",fault_type="NETWORK",reported_at=stamp,restored_at=stamp+timedelta(hours=2),labor_cost_cents=30,parts_cost_cents=20),
            dict(ticket_id="t2",fault_type="NETWORK",reported_at=stamp,labor_cost_cents=999,parts_cost_cents=999),
            dict(ticket_id="t3",fault_type="OVERHEAT",reported_at=stamp,restored_at=stamp-timedelta(hours=1),labor_cost_cents=999,parts_cost_cents=999)]
        return rows

    def rows(self,name):
        return [row.asDict() for row in self.frames[name].collect()]

    def test_city_hour_and_site_day_type_are_real_cross_groupings(self):
        hour = {(row["city_id"],row["start_hour"]):row["energy_wh"] for row in self.rows("comparison_city_hour")}
        self.assertEqual(hour,{("c1",23):1000,("c1",1):2000,("c2",2):3000})
        days = {(row["site_type"],row["day_type"]):row["session_count"] for row in self.rows("comparison_site_day_type")}
        self.assertEqual(days,{("OFFICE","SAT_SUN"):1,("OFFICE","MON_FRI"):1,("TRANSIT","MON_FRI"):1})

    def test_session_aggregation_has_distinct_users_and_weighted_means(self):
        city = {row["city_id"]:row for row in self.rows("sessions_by_city")}["c1"]
        self.assertEqual((city["session_count"],city["distinct_users"],city["energy_wh"]),(2,1,3000))
        self.assertEqual(city["mean_energy_wh"],1500)
        self.assertEqual(city["mean_active_seconds"],2700)
        self.assertEqual(city["mean_connected_seconds"],4500)
        for name in ("sessions_by_user_segment","sessions_by_vehicle_class","sessions_by_connector_type"):
            self.assertEqual(sum(row["energy_wh"] for row in self.rows(name)),6000)

    def test_payment_dates_do_not_relabel_session_bills_as_revenue(self):
        sessions = {str(row["business_date"]):row for row in self.rows("sessions_by_business_date")}
        self.assertEqual(sessions["2025-11-30"]["billed_cents"],100)
        payment = self.rows("payments_by_business_date")
        self.assertEqual(len(payment),1)
        self.assertEqual(str(payment[0]["business_date"]),"2025-12-01")
        self.assertEqual((payment[0]["paid_cents"],payment[0]["refund_cents"],payment[0]["net_paid_cents"]),(400,25,375))
        channels = {row["payment_channel"]:row for row in self.rows("payments_by_channel")}
        self.assertEqual(channels["WECHAT"]["payment_success_rate"],0.5)
        self.assertIsNone(channels["CASH"]["payment_success_rate"])
        self.assertEqual(sum(row["net_paid_cents"] for row in self.rows("comparison_city_payment_channel")),375)

    def test_repair_denominator_excludes_pending_and_invalid_timing(self):
        faults = {row["fault_type"]:row for row in self.rows("repairs_by_fault_type")}
        self.assertEqual((faults["NETWORK"]["reported_count"],faults["NETWORK"]["restored_count"],faults["NETWORK"]["unresolved_count"]),(2,1,1))
        self.assertEqual(faults["NETWORK"]["mean_resolution_seconds"],7200)
        self.assertEqual(faults["NETWORK"]["repair_cost_cents"],50)
        self.assertEqual(faults["OVERHEAT"]["invalid_timing_count"],1)
        self.assertIsNone(faults["OVERHEAT"]["mean_resolution_seconds"])

    def test_export_is_new_only_and_contains_schema_units_and_bounded_previews(self):
        from data_analysis.spark_jobs.acceptance_analysis import export_acceptance_analysis
        output = self.root/"acceptance"
        manifest = export_acceptance_analysis(self.spark,str(self.raw),str(self.processed),str(output))
        self.assertEqual((manifest["semanticDimensionCount"],manifest["comparisonCount"]),(11,3))
        self.assertEqual(set(manifest["analyses"]),set(ANALYSES))
        self.assertTrue((output/"_SUCCESS").is_file())
        self.assertFalse((output/"_RUNNING").exists())
        previews = json.loads((output/"preview.json").read_text())
        self.assertTrue(all(len(rows)<=PREVIEW_LIMIT for rows in previews.values()))
        cash = next(row for row in previews["payments_by_channel"] if row["payment_channel"] == "CASH")
        self.assertIn("payment_success_rate",cash)
        self.assertIsNone(cash["payment_success_rate"])
        self.assertIn("billed_cents",{field["name"] for field in manifest["analyses"]["sessions_by_city"]["schema"]})
        for result in manifest["analyses"].values():
            self.assertTrue(result["files"])
            for shard in result["files"]:
                self.assertEqual(hashlib.sha256((output/shard["path"]).read_bytes()).hexdigest(),shard["sha256"])
        with self.assertRaises(FileExistsError):
            export_acceptance_analysis(self.spark,str(self.raw),str(self.processed),str(output))

    def test_incomplete_and_wrong_batch_are_refused_before_creating_output(self):
        from data_analysis.spark_jobs.acceptance_analysis import export_acceptance_analysis
        output = self.root/"refused"
        marker = self.processed/"_SUCCESS"
        marker.unlink()
        try:
            with self.assertRaises(ValueError):
                export_acceptance_analysis(self.spark,str(self.raw),str(self.processed),str(output))
        finally:
            marker.touch()
        original = self.quality_path.read_text()
        changed = json.loads(original)
        changed["dataset_id"] = "wrong-batch"
        self.quality_path.write_text(json.dumps(changed))
        try:
            with self.assertRaises(ValueError):
                export_acceptance_analysis(self.spark,str(self.raw),str(self.processed),str(output))
        finally:
            self.quality_path.write_text(original)
        self.assertFalse(output.exists())

    def test_report_paths_and_verification_flags_are_strict(self):
        from data_analysis.spark_jobs.acceptance_analysis import export_acceptance_analysis
        output = self.root/"refused-provenance"
        original = self.quality_path.read_text(encoding="utf-8")
        try:
            for changes in ({"output":"file:/different-processed-batch"}, {"raw_file_checksums_verified":1},
                            {"manifest_row_counts_verified":"true"}, {"raw_file_checksums_verified":False}):
                self.quality_path.write_text(json.dumps(dict(json.loads(original),**changes)),encoding="utf-8")
                with self.subTest(changes=changes), self.assertRaises(ValueError):
                    export_acceptance_analysis(self.spark,str(self.raw),str(self.processed),str(output))
                self.assertFalse(output.exists())
        finally:
            self.quality_path.write_text(original,encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
