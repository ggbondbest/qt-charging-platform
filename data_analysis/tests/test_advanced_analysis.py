"""Real Spark regression for cross-grain joins, censoring and conserved flows."""

from datetime import date, datetime, timedelta, timezone
import copy
import os
import unittest

from data_analysis.spark_jobs.advanced_analysis import (
    DEFINITIONS, INPUT_TABLES, MAX_EXPORT_ROWS, TABLE_NAMES, accepted_clean_inventory,
    complete_months_through, first_complete_month, sanitize_spark_plan, verify_clean_shard,
)


class AdvancedDefinitionTests(unittest.TestCase):
    def test_spark_plan_redacts_complete_and_truncated_local_source_only(self):
        root = "/Users/developer/Projects/charging platform/datasets/clean_batch"
        plan = ("HashAggregate(keys=[station_id], functions=[sum(energy_wh)])\n"
                "Location: InMemoryFileIndex(1 paths)[file:" + root + "/clean/sessions], PushedFilters: []\n"
                "Location: InMemoryFileIndex(1 paths)[file:/Users/developer/Projects/charging..., PartitionFilters: []\n"
                "Location: InMemoryFileIndex(1 paths)[file:///Users/developer/Projects/charging%20plat...], ReadSchema: struct<energy_wh:bigint>\n"
                "Location: InMemoryFileIndex(1 paths)[file:/public/other...], PushedFilters: []\n"
                "Statistics(sizeInBytes=2048, rowCount=121539)")
        result = sanitize_spark_plan(plan, root)
        self.assertNotIn("/Users/developer", result)
        self.assertIn("file:${CLEAN_ROOT}/clean/sessions", result)
        self.assertEqual(result.count("file:${CLEAN_ROOT}..."), 2)
        self.assertIn("file:/public/other...", result)
        self.assertIn("HashAggregate(keys=[station_id], functions=[sum(energy_wh)])", result)
        self.assertIn("Statistics(sizeInBytes=2048, rowCount=121539)", result)
        self.assertEqual(sanitize_spark_plan(result, root), result)
        # Absolute foreign-host paths are pure text, even when CI runs Windows.
        from unittest.mock import patch
        with patch("data_analysis.spark_jobs.advanced_analysis.Path.resolve", side_effect=AssertionError("host resolution")):
            self.assertEqual(sanitize_spark_plan(plan, root), result)
        neighbour = "Location: [file:" + root + "_other/clean/sessions]"
        self.assertEqual(sanitize_spark_plan(neighbour, root), neighbour)

    def test_spark_plan_redacts_relative_and_uri_source_roots(self):
        from pathlib import Path
        relative = "data_analysis/datasets/clean_batch"
        resolved = Path(relative).resolve()
        plan = "Location: InMemoryFileIndex(1 paths)[file:" + str(resolved) + "/clean/stations]"
        self.assertEqual(sanitize_spark_plan(plan, relative),
                         "Location: InMemoryFileIndex(1 paths)[file:${CLEAN_ROOT}/clean/stations]")
        self.assertEqual(sanitize_spark_plan("Location: [hdfs://node:8020/data/clean_batch/clean/stations]",
                                            "hdfs://node:8020/data/clean_batch"),
                         "Location: [${CLEAN_ROOT}/clean/stations]")
        self.assertNotIn(str(resolved), sanitize_spark_plan(plan, resolved.as_uri()))
        from unittest.mock import patch
        windows_roots = [r"D:\a\charging platform\datasets\clean_batch",
                         "D:/a/charging platform/datasets/clean_batch",
                         "file:///D:/a/charging%20platform/datasets/clean_batch"]
        windows_plan = (
            r"Location: [file:D:\a\charging platform\datasets\clean_batch/clean/stations]" + "\n"
            "Location: [file:/D:/a/charging%20platform/datasets/clean_batch/clean/sessions]\n"
            "Location: [file:///D:/a/charging%20plat...], PartitionFilters: []\n"
            r"Location: [file:d:\a\charging plat...], PushedFilters: []" + "\n"
            "Location: [file:/E:/a/charging...], ReadSchema: struct<energy_wh:bigint>\n"
            "Location: [file:/D:/unrelated...], PushedFilters: []\n"
            "Location: [file:/D:/a/charging%20platform/datasets/clean_batch_backup/clean/sessions]")
        for source in windows_roots:
            with self.subTest(source=source), patch("data_analysis.spark_jobs.advanced_analysis.Path.resolve",
                                                   side_effect=AssertionError("host resolution")):
                actual = sanitize_spark_plan(windows_plan, source)
                self.assertNotIn(windows_roots[0], actual)
                self.assertEqual(actual.count("file:${CLEAN_ROOT}..."), 2)
                self.assertIn("file:${CLEAN_ROOT}/clean/stations", actual)
                self.assertIn("${CLEAN_ROOT}/clean/sessions", actual)
                self.assertIn("file:/E:/a/charging...", actual)
                self.assertIn("file:/D:/unrelated...", actual)
                self.assertIn("clean_batch_backup/clean/sessions", actual)
        unc_root = "file://fileserver/share/charging%20platform/datasets/clean_batch"
        unc_plan = "Location: [file://fileserver/share/charging%20plat...], PushedFilters: []"
        self.assertEqual(sanitize_spark_plan(unc_plan, unc_root),
                         "Location: [file:${CLEAN_ROOT}...], PushedFilters: []")

    def test_complete_month_boundary_is_shanghai_and_exclusive(self):
        self.assertEqual(complete_months_through("2026-05-29T16:00:00Z"), date(2026, 4, 1))
        self.assertEqual(complete_months_through("2026-05-31T16:00:00Z"), date(2026, 5, 1))
        self.assertEqual(complete_months_through("2024-02-29T16:00:00Z"), date(2024, 2, 1))
        with self.assertRaisesRegex(ValueError, "timezone"):
            complete_months_through("2026-05-30T00:00:00")

    def test_partial_first_month_is_not_a_complete_cohort(self):
        self.assertEqual(first_complete_month("2025-11-30T16:00:00Z"), date(2025, 12, 1))
        self.assertEqual(first_complete_month("2025-12-01T00:00:00Z"), date(2026, 1, 1))

    def test_data_contract_distinguishes_cohorts_cashflow_and_causality(self):
        self.assertEqual(len(TABLE_NAMES), 7)
        self.assertLessEqual(MAX_EXPORT_ROWS, 250000)
        self.assertIn("not accounting profit", DEFINITIONS["station_day"])
        self.assertIn("not true acquisition", DEFINITIONS["retention"])
        self.assertIn("Do not sum rainfall", DEFINITIONS["weather"])
        self.assertIn("no causal", DEFINITIONS["causality"])
        self.assertIn("FULL source history", DEFINITIONS["user_behavior"])
        self.assertIn("not distinct users", DEFINITIONS["user_behavior"])
        self.assertIn("JOIN hour", DEFINITIONS["service_hour"])

    def test_clean_inventory_is_bound_to_existing_acceptance_not_current_hashes(self):
        serving = dict(datasetId="data", publishedBatchId="batch", pipelineRunId="spark", sourceManifestSha256="a" * 64)
        acceptance = dict(serving, bundleVersion="acceptance-bundle-1.0.0", servingManifestSha256="b" * 64,
            cleanTables={name: {"path": "clean/" + name, "rowCount": 1} for name in INPUT_TABLES},
            files=[{"path": "clean/" + name + "/part-00000.parquet", "bytes": 30, "sha256": "c" * 64}
                   for name in INPUT_TABLES])
        inventory = accepted_clean_inventory(acceptance, serving, "b" * 64)
        self.assertEqual(set(inventory), set(INPUT_TABLES))
        shard = next(iter(inventory["charging_sessions"].values()))
        verify_clean_shard(shard, "c" * 64, 30)
        with self.assertRaisesRegex(ValueError, "differs from authenticated"):
            verify_clean_shard(shard, "d" * 64, 30)  # Same bytes/row count does not authorize replacement.
        variants = []
        wrong_batch = copy.deepcopy(acceptance)
        wrong_batch["publishedBatchId"] = "other"
        variants.append(wrong_batch)
        missing = copy.deepcopy(acceptance)
        missing["files"] = missing["files"][1:]
        variants.append(missing)
        duplicate = copy.deepcopy(acceptance)
        duplicate["files"].append(duplicate["files"][0])
        variants.append(duplicate)
        unsafe = copy.deepcopy(acceptance)
        unsafe["files"][0]["path"] = "../part-evil.parquet"
        variants.append(unsafe)
        for item in variants:
            with self.subTest(item=item):
                with self.assertRaises(ValueError):
                    accepted_clean_inventory(item, serving, "b" * 64)
        with self.assertRaisesRegex(ValueError, "does not bind"):
            accepted_clean_inventory(acceptance, serving, "e" * 64)


@unittest.skipUnless(os.environ.get("RUN_SPARK_TESTS") == "1", "Requires opt-in Java17/PySpark runtime")
class AdvancedSparkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from pyspark.sql import SparkSession
        from data_analysis.tests.test_dashboard_aggregates import DashboardAggregatesTests
        from data_analysis.spark_jobs.advanced_analysis import build_advanced_frames
        os.environ.setdefault("SPARK_LOCAL_IP", "127.0.0.1")
        cls.spark = (SparkSession.builder.master("local[2]").appName("advanced-analysis-tests")
            .config("spark.ui.enabled", "false").config("spark.sql.shuffle.partitions", "2")
            .config("spark.sql.session.timeZone", "UTC").getOrCreate())
        cls.spark.sparkContext.setLogLevel("ERROR")
        cls.rows = DashboardAggregatesTests.fixture_records()
        for index, session in enumerate(cls.rows["charging_sessions"], 1):
            session.update(attempt_id="attempt" + str(index), vehicle_id="v1", charger_id=session["station_id"] + "1",
                unplugged_at=session["ended_at"] + timedelta(minutes=15), energy_wh=index * 1000,
                total_fee_cents=index * 100)
            cls.rows["charging_attempts"].append(dict(attempt_id=session["attempt_id"], user_id="u1", vehicle_id="v1",
                station_id=session["station_id"], charger_id=session["charger_id"], attempted_at=session["started_at"],
                outcome="STARTED", session_id=session["session_id"],
                queue_id="q1" if index == 2 else None, reservation_id="r1" if index == 3 else None))
        cls.rows["charging_attempts"].append(dict(attempt_id="failed", user_id="u1", vehicle_id="v1", station_id="a",
            attempted_at=cls.rows["charging_sessions"][0]["started_at"], outcome="FAILED", failure_reason="AUTH_FAILED"))
        cls.rows["users"] = [dict(user_id="u1", segment="COMMUTER")]
        cls.rows["vehicles"] = [dict(vehicle_id="v1", user_id="u1", vehicle_class="COMMUTER", battery_capacity_kwh=60.0)]
        for charger in cls.rows["chargers"]:
            charger["connector_type"] = "DC"
        for day in cls.rows["calendar"]:
            day["scenario_event"] = "NONE"
        cls.rows["weather_hourly"] = [dict(city_id="c1", recorded_at=datetime(2025, 12, 1, tzinfo=timezone.utc),
            temperature_c=12.5, rainfall_mm=1.2, weather="RAIN")]
        cls.tables = {name: cls.frame(name, rows) for name, rows in cls.rows.items()}
        cls.frames = build_advanced_frames(cls.tables, "2025-11-30T16:00:00Z", "2025-12-31T16:00:00Z")
        for frame in cls.frames.values():
            frame.cache()

    @classmethod
    def frame(cls, table, rows):
        from pyspark.sql import types as T
        from data_analysis.charging_data.schema import TABLES
        from data_analysis.spark_jobs.pipeline import field_type
        return cls.spark.createDataFrame([tuple(row.get(field) for field in TABLES[table]) for row in rows],
            T.StructType([T.StructField(field, field_type(field)) for field in TABLES[table]]))

    @classmethod
    def tearDownClass(cls):
        for frame in cls.frames.values():
            frame.unpersist()
        cls.spark.stop()

    def test_request_flow_is_disjoint_and_preserves_unpaid_and_failure(self):
        rows = self.frames["attempt_flow"].collect()
        self.assertEqual(sum(row.attempt_count for row in rows), 4)
        self.assertEqual({row.access_path for row in rows}, {"DIRECT", "QUEUE", "RESERVATION"})
        failed = next(row for row in rows if row.outcome == "FAILED")
        self.assertEqual(failed.session_status, "NO_SESSION")
        self.assertEqual(failed.failure_reason, "AUTH_FAILED")
        unpaid = next(row for row in rows if row.session_status == "WAITING_PAYMENT")
        self.assertEqual(unpaid.access_path, "QUEUE")
        self.assertEqual(unpaid.local_hour, 8)
        midnight = next(row for row in rows if row.access_path == "RESERVATION")
        self.assertEqual(midnight.business_date, date(2025, 12, 2))
        self.assertEqual(midnight.local_hour, 1)

    def test_behavior_intervals_use_full_history_across_station_and_filter_boundary(self):
        from data_analysis.spark_jobs.advanced_analysis import build_user_behavior
        stamp = datetime(2025, 11, 30, 16, tzinfo=timezone.utc)
        # Every subsequent start moves to b; filtering b or the later dates must
        # not erase the earlier start at a and turn the next start into FIRST.
        gaps = [0, 86400 - 1, 86400, 3 * 86400 - 1, 3 * 86400,
                7 * 86400 - 1, 7 * 86400, 14 * 86400 - 1, 14 * 86400]
        energy = [0, 9999, 10000, 19999, 20000, 39999, 40000, 45000, 12000]
        sessions, total = [], 0
        for index, gap in enumerate(gaps):
            total += gap
            sessions.append(dict(self.rows["charging_sessions"][0], session_id="gap" + str(index),
                station_id="a" if index == 0 else "b", started_at=stamp + timedelta(seconds=total), energy_wh=energy[index]))
        tables = dict(self.tables, charging_sessions=self.frame("charging_sessions", sessions))
        frame = build_user_behavior(tables)
        actual = frame.collect()
        grouped = {}
        energy_grouped = {}
        for row in actual:
            grouped[row.gap_bucket] = grouped.get(row.gap_bucket, 0) + row.session_count
            energy_grouped[row.energy_bucket] = energy_grouped.get(row.energy_bucket, 0) + row.session_count
        self.assertEqual(grouped, {"FIRST_OBSERVED": 1, "LT1D": 1, "1_TO_3D": 2,
                                  "3_TO_7D": 2, "7_TO_14D": 2, "GE14D": 1})
        self.assertEqual(energy_grouped, {"LT10": 2, "10_TO_20": 3, "20_TO_40": 2, "GE40": 2})
        self.assertEqual(sum(row.interval_seconds_sum for row in actual), sum(gaps))
        self.assertEqual(sum(row.interval_count for row in actual), 8)
        self.assertEqual(sum(row.first_observed_count for row in actual), 1)
        selected = frame.filter("station_id = 'b' AND business_date >= '2025-12-02'").collect()
        self.assertTrue(selected)
        self.assertEqual(sum(row.first_observed_count for row in selected), 0)
        self.assertNotIn("user_id", frame.columns)
        self.assertNotIn("session_id", frame.columns)

    def test_behavior_same_time_start_is_deterministic_and_not_missing(self):
        from data_analysis.spark_jobs.advanced_analysis import build_user_behavior
        rows = [dict(self.rows["charging_sessions"][0], session_id="tie" + str(index), station_id=station)
                for index, station in [(1, "a"), (2, "b")]]
        actual = build_user_behavior(dict(self.tables, charging_sessions=self.frame("charging_sessions", rows))).collect()
        first = next(row for row in actual if row.station_id == "a")
        second = next(row for row in actual if row.station_id == "b")
        self.assertEqual((first.gap_bucket, first.first_observed_count, first.interval_count), ("FIRST_OBSERVED", 1, 0))
        self.assertEqual((second.gap_bucket, second.interval_count, second.interval_seconds_sum), ("LT1D", 1, 0))

    def test_service_hour_preserves_join_cohort_and_does_not_multiply_context(self):
        rows = self.frames["service_hour"].collect()
        self.assertEqual(len(rows), 3 * 48)
        self.assertEqual(len({(row.station_id, row.business_date, row.local_hour) for row in rows}), len(rows))
        self.assertEqual(sum(row.session_count for row in rows), 3)
        self.assertEqual(sum(row.queue_wait_count for row in rows), 3)
        self.assertEqual(sum(row.queue_wait_seconds_sum for row in rows), (8 + 10 + 3) * 60)
        self.assertEqual(sum(row.occupied_seconds_sum for row in rows), 3 * 15 * 60)
        self.assertEqual(sum(row.connected_seconds_sum for row in rows), 3 * 70 * 60)
        before_midnight = next(row for row in rows if row.station_id == "a" and
                               row.business_date == date(2025, 12, 1) and row.local_hour == 23)
        self.assertEqual(before_midnight.queue_wait_count, 2)
        self.assertEqual(before_midnight.queue_wait_seconds_sum, 18 * 60)
        self.assertEqual(before_midnight.session_count, 0)
        midnight = next(row for row in rows if row.station_id == "a" and
                        row.business_date == date(2025, 12, 2) and row.local_hour == 0)
        self.assertEqual(midnight.queue_wait_seconds_sum, 3 * 60)

    def test_service_hour_excludes_invalid_duration_pairs_and_unresolved_waits(self):
        from data_analysis.spark_jobs.advanced_analysis import build_service_hour
        stamp = self.rows["charging_sessions"][0]["started_at"]
        sessions = [dict(self.rows["charging_sessions"][0], session_id="bad", ended_at=stamp - timedelta(minutes=1)),
                    dict(self.rows["charging_sessions"][0], session_id="missing", unplugged_at=None)]
        queues = [dict(queue_id="backwards", station_id="a", joined_at=stamp,
                       called_at=stamp - timedelta(minutes=1), resolved_at=stamp + timedelta(minutes=2), status="SERVED"),
                  dict(queue_id="waiting", station_id="a", joined_at=stamp, status="WAITING")]
        tables = dict(self.tables, charging_sessions=self.frame("charging_sessions", sessions),
                      queue_entries=self.frame("queue_entries", queues))
        row = build_service_hour(tables, self.frames["station_hour"]).filter("station_id = 'a' AND local_hour = 8").orderBy("business_date").first()
        self.assertEqual(row.session_count, 2)
        self.assertEqual((row.connected_seconds_sum, row.occupied_seconds_sum), (0, 0))
        self.assertEqual((row.queue_wait_count, row.queue_wait_seconds_sum), (0, 0))

    def test_station_inventory_interfaces_preserve_counts_and_rated_power(self):
        from data_analysis.spark_jobs.advanced_analysis import build_station_inventory
        chargers = [dict(charger) for charger in self.rows["chargers"]]
        for charger in chargers:
            if charger["charger_id"] == "b3":
                charger.update(connector_type="AC", rated_power_kw=7.0)
        station = build_station_inventory(dict(self.tables, chargers=self.frame("chargers", chargers)))\
            .filter("station_id = 'b'").first().asDict(recursive=True)
        self.assertEqual(station["capacity"], 3)
        self.assertEqual(station["rated_capacity_kw"], 247.0)
        self.assertEqual(station["interfaces"], [dict(connector_type="AC", charger_count=1, rated_power_kw=7.0),
                                                dict(connector_type="DC", charger_count=2, rated_power_kw=240.0)])

    def test_segments_jointly_group_and_conserve_sessions_energy_bills(self):
        rows = self.frames["session_segments"].collect()
        self.assertEqual(sum(row.session_count for row in rows), 3)
        self.assertEqual(sum(row.energy_wh for row in rows), 6000)
        self.assertEqual(sum(row.billed_cents for row in rows), 600)
        self.assertEqual(sum(row.occupied_seconds_sum for row in rows), 3 * 15 * 60)
        self.assertTrue(all(row.user_segment == "COMMUTER" and row.battery_capacity_band == "50_TO_69"
                            and row.connector_type == "DC" for row in rows))
        self.assertNotIn("vehicle_class", self.frames["session_segments"].columns)
        midnight = next(row for row in rows if row.energy_wh == 3000)
        self.assertEqual(midnight.business_date, date(2025, 12, 2))

    def test_capacity_dimension_uses_hardware_not_copied_user_segment(self):
        from data_analysis.spark_jobs.advanced_analysis import build_session_segments
        vehicles, sessions = [], []
        for index, capacity in enumerate([40.0, 50.0, 69.0, 70.0, 90.0, None, float("nan"), -1.0]):
            identifier = "v" + str(index)
            vehicles.append(dict(vehicle_id=identifier, user_id="u1", vehicle_class="COMMUTER", battery_capacity_kwh=capacity))
            sessions.append(dict(self.rows["charging_sessions"][0], session_id="cap" + str(index), vehicle_id=identifier))
        tables = dict(self.tables, vehicles=self.frame("vehicles", vehicles),
                      charging_sessions=self.frame("charging_sessions", sessions))
        actual = {row.battery_capacity_band: row.session_count for row in build_session_segments(tables).collect()}
        self.assertEqual(actual, {"LT50": 1, "50_TO_69": 2, "GE70": 2, "UNKNOWN": 3})

    def test_timestamp_export_uses_spark_utc_not_python_host_timezone(self):
        import time
        from pyspark.sql import functions as F
        from data_analysis.spark_jobs.advanced_analysis import export_scalars, _json_value
        frame = self.spark.range(1).select(F.to_timestamp(F.lit("2025-11-30T16:00:00Z")).alias("recorded_at"))
        previous = os.environ.get("TZ")
        try:
            for host_zone in ["Asia/Shanghai", "America/Los_Angeles"]:
                os.environ["TZ"] = host_zone
                if hasattr(time, "tzset"):
                    time.tzset()
                self.assertEqual(export_scalars(frame).first().recorded_at, "2025-11-30T16:00:00.000000Z")
        finally:
            if previous is None:
                os.environ.pop("TZ", None)
            else:
                os.environ["TZ"] = previous
            if hasattr(time, "tzset"):
                time.tzset()
        with self.assertRaisesRegex(ValueError, "naive Python"):
            _json_value(datetime(2025, 12, 1))

    def test_hour_weather_join_is_unique_preserves_gaps_and_local_hour(self):
        rows = self.frames["station_hour"].filter("station_id = 'a'").collect()
        self.assertEqual(len(rows), 48)
        observed = next(row for row in rows if row.is_complete)
        self.assertEqual(observed.local_hour, 8)
        self.assertEqual(observed.temperature_c, 12.5)
        self.assertEqual(observed.day_type, "WORKDAY")
        missing = next(row for row in rows if not row.has_observation)
        self.assertIsNone(missing.mean_power_kw)
        self.assertFalse(missing.is_complete)

    def test_station_day_does_not_multiply_payments_sessions_or_queues(self):
        row = self.frames["station_day"].filter("station_id = 'a' and business_date = '2025-12-02'").first()
        self.assertEqual(row.paid_cents, 1000)
        self.assertEqual(row.refund_cents, 100)
        self.assertEqual(row.session_count, 1)
        self.assertEqual(row.queues_resolved_count, 3)
        self.assertEqual(row.attempt_count, 1)

    def test_retention_deduplicates_users_fills_observed_zeros_censors_future(self):
        from data_analysis.spark_jobs.advanced_analysis import build_retention
        rows = []
        for user, station, month, day in [("u1", "a", 12, 2), ("u1", "a", 12, 3),
            ("u1", "b", 12, 4), ("u1", "a", 1, 2), ("u2", "a", 2, 2), ("u3", "c", 12, 2),
            ("u4", "a", 5, 2)]:
            rows.append(dict(session_id=str(len(rows)), user_id=user, station_id=station,
                started_at=datetime(2025 if month == 12 else 2026, month, day, tzinfo=timezone.utc)))
        tables = dict(self.tables, charging_sessions=self.frame("charging_sessions", rows))
        actual = build_retention(tables, "2025-11-30T16:00:00Z", "2026-05-14T16:00:00Z").collect()
        december = sorted([row for row in actual if row.scope_type == "ALL"
                           and row.cohort_month == date(2025, 12, 1)], key=lambda row: row.month_offset)
        self.assertEqual([row.n for row in december], [2, 1, 0, 0, 0])
        self.assertEqual({row.cohort_size for row in december}, {2})
        self.assertFalse(any(row.cohort_month.month == 5 for row in actual))
        station = next(row for row in actual if row.scope_type == "STATION" and row.scope_id == "b"
                       and row.month_offset == 1)
        self.assertEqual(station.n, 0)  # u1 returned elsewhere, not to station b.
        self.assertEqual(station.cohort_size, 1)

    def test_ambiguous_queue_and_reservation_request_is_rejected(self):
        from data_analysis.spark_jobs.advanced_analysis import build_attempt_flow
        from pyspark.sql import functions as F
        tables = dict(self.tables)
        tables["charging_attempts"] = tables["charging_attempts"].withColumn("queue_id", F.lit("q"))\
            .withColumn("reservation_id", F.lit("r"))
        with self.assertRaisesRegex(ValueError, "simultaneously"):
            build_attempt_flow(tables)

    def test_duplicate_weather_key_is_rejected_before_chart_join(self):
        from data_analysis.spark_jobs.advanced_analysis import build_advanced_frames
        tables = dict(self.tables)
        tables["weather_hourly"] = tables["weather_hourly"].unionByName(tables["weather_hourly"])
        with self.assertRaisesRegex(ValueError, "weather_hourly"):
            build_advanced_frames(tables, "2025-11-30T16:00:00Z", "2025-12-31T16:00:00Z")

    def test_invalid_energy_cannot_publish_misleading_behavior_mean(self):
        from data_analysis.spark_jobs.advanced_analysis import build_advanced_frames
        for value in [None, -1]:
            tables = dict(self.tables, charging_sessions=self.frame("charging_sessions", [
                dict(self.rows["charging_sessions"][0], energy_wh=value)]))
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "nonnegative session energy_wh"):
                build_advanced_frames(tables, "2025-11-30T16:00:00Z", "2025-12-31T16:00:00Z")


if __name__ == "__main__":
    unittest.main()
