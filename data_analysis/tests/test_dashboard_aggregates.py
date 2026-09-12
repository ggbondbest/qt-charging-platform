"""Opt-in real Spark tests of business dates, denominators and final cohorts."""

from datetime import date, datetime, timedelta, timezone
import os
import unittest

from data_analysis.charging_data.schema import TABLES


@unittest.skipUnless(os.environ.get("RUN_SPARK_TESTS") == "1",
                     "Requires RUN_SPARK_TESTS=1, PySpark 3.5.6 and Java 17")
class DashboardAggregatesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("SPARK_LOCAL_IP", "127.0.0.1")
        from pyspark.sql import SparkSession
        from data_analysis.spark_jobs.dashboard_aggregates import build_dashboard_frames
        from data_analysis.spark_jobs.pipeline import station_daily, station_hourly
        cls.spark = (SparkSession.builder.master("local[2]").appName("dashboard-aggregate-tests")
                     .config("spark.ui.enabled", "false")
                     .config("spark.sql.shuffle.partitions", "2")
                     .config("spark.sql.session.timeZone", "UTC").getOrCreate())
        cls.spark.sparkContext.setLogLevel("ERROR")
        cls.rows = cls.fixture_records()
        cls.tables = {table: cls.frame(table, records) for table, records in cls.rows.items()}
        cls.hourly = station_hourly(cls.tables["charger_telemetry"], cls.tables["stations"], cls.tables["chargers"])
        cls.daily = station_daily(cls.tables)
        cls.results = build_dashboard_frames(cls.tables, cls.hourly, cls.daily)
        for frame in cls.results.values():
            frame.cache()

    @classmethod
    def tearDownClass(cls):
        for frame in cls.results.values():
            frame.unpersist()
        cls.spark.stop()

    @classmethod
    def frame(cls, table, records):
        from pyspark.sql import types as T
        from data_analysis.spark_jobs.pipeline import field_type
        fields = TABLES[table]
        return cls.spark.createDataFrame([tuple(row.get(field) for field in fields) for row in records],
            T.StructType([T.StructField(field, field_type(field)) for field in fields]))

    @staticmethod
    def fixture_records():
        stamp = datetime(2025, 12, 1, tzinfo=timezone.utc)
        rows = {name: [] for name in TABLES}
        rows["cities"] = [dict(city_id="c1", city_name="大连市", latitude=38.9, longitude=121.6),
                          dict(city_id="c2", city_name="北京市", latitude=39.9, longitude=116.4)]
        rows["stations"] = [dict(station_id=name, city_id=city, station_name=name, site_type="TRANSIT",
                                  latitude=lat, longitude=lon, transformer_kw=500.0)
                            for name, city, lat, lon in [("a", "c1", 38.8, 121.5),
                                                        ("b", "c1", 38.7, 121.4),
                                                        ("c", "c2", 39.8, 116.3)]]
        rows["chargers"] = [dict(charger_id=name, station_id=station, rated_power_kw=120.0)
                             for station, names in [("a", ["a1"]), ("b", ["b1", "b2", "b3"]),
                                                     ("c", ["c1"])] for name in names]
        rows["calendar"] = [dict(city_id=city, business_date=day)
                            for city in ["c1", "c2"] for day in [date(2025, 12, 1), date(2025, 12, 2)]]
        for minute in range(0, 60, 5):
            for station, charger, state in [("a", "a1", "CHARGING"), ("b", "b1", "CHARGING"),
                                             ("b", "b2", "AVAILABLE"), ("b", "b3", "MAINTENANCE")]:
                energy = 500 if state == "CHARGING" else 0
                rows["charger_telemetry"].append(dict(
                    charger_id=charger, station_id=station, recorded_at=stamp + timedelta(minutes=minute),
                    interval_seconds=300, state=state, energy_wh=energy, power_kw=energy / 1000 * 12,
                    grid_energy_wh=energy, grid_cost_cents=20 if energy else 0))
        rows["charging_sessions"] = [dict(session_id=name, station_id=station, user_id="u1",
                                           started_at=stamp + timedelta(hours=hour),
                                           ended_at=stamp + timedelta(hours=hour, minutes=55),
                                           status="WAITING_PAYMENT" if name == "s2" else "COMPLETED")
                                     for name, station, hour in [("s1", "a", 0), ("s2", "b", 0), ("s3", "a", 17)]]
        rows["payments"] = [dict(payment_id=name, session_id="s1", occurred_at=stamp + timedelta(hours=16, minutes=minute),
                                  status=status, transaction_type=kind, amount_cents=amount)
                            for name, minute, status, kind, amount in [("p1", 1, "SUCCESS", "PAYMENT", 1000),
                                                                      ("p2", 2, "SUCCESS", "REFUND", 100),
                                                                      ("p3", 3, "FAILED", "PAYMENT", 1000)]]
        rows["reservations"] = [dict(reservation_id="r1", station_id="a", created_at=stamp + timedelta(hours=15, minutes=55),
                                      resolved_at=stamp + timedelta(hours=16, minutes=2), status="CONFIRMED"),
                                dict(reservation_id="r2", station_id="a", created_at=stamp + timedelta(hours=16, minutes=10),
                                      resolved_at=stamp + timedelta(hours=17), status="CANCELLED")]
        rows["queue_entries"] = [
            dict(queue_id="q1", station_id="a", joined_at=stamp + timedelta(hours=15, minutes=50),
                 called_at=stamp + timedelta(hours=15, minutes=58), resolved_at=stamp + timedelta(hours=16, minutes=2), status="SERVED"),
            dict(queue_id="q2", station_id="a", joined_at=stamp + timedelta(hours=15, minutes=55),
                 called_at=stamp + timedelta(hours=16, minutes=5), resolved_at=stamp + timedelta(hours=16, minutes=10), status="CALL_EXPIRED"),
            dict(queue_id="q3", station_id="a", joined_at=stamp + timedelta(hours=15, minutes=59), status="WAITING"),
            dict(queue_id="q4", station_id="a", joined_at=stamp + timedelta(hours=16, minutes=10),
                 resolved_at=stamp + timedelta(hours=16, minutes=13), status="ABANDONED"),
        ]
        rows["maintenance_tickets"] = [
            dict(ticket_id="t1", station_id="b", status="RESOLVED", reported_at=stamp + timedelta(hours=15),
                 work_started_at=stamp + timedelta(hours=15, minutes=30), restored_at=stamp + timedelta(hours=17),
                 labor_cost_cents=200, parts_cost_cents=300),
            dict(ticket_id="t2", station_id="b", status="IN_PROGRESS", reported_at=stamp,
                 work_started_at=stamp + timedelta(hours=1), labor_cost_cents=99999, parts_cost_cents=99999),
        ]
        rows["reviews"] = [dict(review_id="v1", station_id="a", rating=5, created_at=stamp + timedelta(hours=15, minutes=59)),
                           dict(review_id="v2", station_id="a", rating=1, created_at=stamp + timedelta(hours=16, minutes=1)),
                           dict(review_id="v3", station_id="a", rating=9, created_at=stamp + timedelta(hours=16, minutes=2))]
        return rows

    def test_complete_hour_denominator_and_weighted_city_utilization(self):
        daily = {row.station_id: row for row in self.results["station_metrics_daily"]
                 .filter("business_date = '2025-12-01'").collect()}
        self.assertEqual(daily["a"].charging_samples, 12)
        self.assertEqual(daily["a"].complete_hours, 1)
        self.assertEqual(daily["a"].incomplete_hours, 23)
        self.assertEqual(daily["a"].expected_sample_count, 24 * 12)
        self.assertEqual(daily["a"].missing_sample_count, 23 * 12)
        self.assertEqual(daily["a"].charging_utilization, 1.0)
        self.assertAlmostEqual(daily["b"].charging_utilization, 1 / 3)
        self.assertEqual(daily["b"].maintenance_samples, 12)
        self.assertIsNone(daily["c"].charging_utilization)
        self.assertFalse(daily["a"].is_complete)
        city = self.results["city_daily"].filter("city_id = 'c1' AND business_date = '2025-12-01'").first()
        self.assertEqual(city.complete_sample_count, 48)
        self.assertEqual(city.complete_charging_samples, 24)
        self.assertEqual(city.station_count, 2)
        self.assertEqual(city.charging_utilization, .5)
        self.assertEqual(city.energy_wh, 12000)

    def test_missing_hours_keep_load_null_and_mark_coverage(self):
        rows = self.results["station_hourly_metrics"].filter("station_id = 'c'").collect()
        self.assertEqual(len(rows), 48)
        self.assertTrue(all(not row.has_observation and not row.is_complete for row in rows))
        self.assertTrue(all(row.energy_wh is None and row.mean_power_kw is None for row in rows))
        self.assertTrue(all(row.sample_count == 0 and row.expected_sample_count == 12 for row in rows))
        self.assertTrue(all(row.end_available_count is None for row in rows))

    def test_receipts_use_payment_day_and_users_are_not_daily_distinct_sums(self):
        daily = {str(row.business_date): row for row in self.results["station_metrics_daily"]
                 .filter("station_id = 'a'").collect()}
        self.assertEqual(daily["2025-12-01"].paid_cents, 0)
        self.assertEqual(daily["2025-12-02"].paid_cents, 1000)
        self.assertEqual(daily["2025-12-02"].refund_cents, 100)
        self.assertEqual(daily["2025-12-02"].net_paid_cents, 900)
        ended_unpaid = self.results["station_metrics_daily"].filter(
            "station_id = 'b' AND business_date = '2025-12-01'").first()
        self.assertEqual(ended_unpaid.completed_sessions, 1)
        self.assertEqual(ended_unpaid.paid_cents, 0)
        from pyspark.sql import functions as F
        users = self.results["user_activity_daily"].groupBy("user_id").agg(F.sum("session_count").alias("starts"))
        self.assertEqual(users.count(), 1)
        self.assertEqual(users.filter("starts >= 2").count(), 1)
        self.assertEqual(users.first().starts, 3)

    def test_final_cohorts_differ_from_resolution_dates_and_waiting_is_excluded(self):
        cohorts = {str(row.business_date): row for row in self.results["station_cohorts_daily"]
                   .filter("station_id = 'a'").collect()}
        self.assertEqual(cohorts["2025-12-01"].reservation_confirmed_count, 1)
        self.assertEqual(cohorts["2025-12-01"].queues_joined_count, 3)
        self.assertEqual(cohorts["2025-12-01"].queue_waiting_count, 1)
        self.assertIn("FINAL_OUTCOME_IN_BATCH", cohorts["2025-12-01"].outcome_semantics)
        services = {str(row.business_date): row for row in self.results["station_service_daily"]
                    .filter("station_id = 'a'").collect()}
        self.assertEqual(services["2025-12-01"].queues_resolved_count, 0)
        completed = services["2025-12-02"]
        self.assertEqual(completed.queues_resolved_count, 3)
        self.assertEqual(completed.queue_wait_count, 3)
        self.assertEqual(completed.queue_wait_seconds_sum, (8 + 10 + 3) * 60)
        self.assertEqual(completed.queue_sojourn_seconds_sum, (12 + 15 + 3) * 60)
        self.assertEqual(completed.rating_count, 1)
        self.assertEqual(completed.rating_sum, 1)
        self.assertEqual(completed.invalid_rating_count, 1)
        repair = self.results["station_service_daily"].filter("station_id = 'b'").first()
        self.assertEqual(str(repair.business_date), "2025-12-02")
        self.assertEqual(repair.repairs_restored_count, 1)
        self.assertEqual(repair.repair_resolution_seconds_sum, 7200)
        self.assertEqual(repair.repair_work_seconds_sum, 5400)

    def test_snapshot_never_backfills_missing_piles_or_hides_staleness(self):
        from data_analysis.spark_jobs.dashboard_aggregates import _snapshot
        changed = dict(self.tables)
        records = list(self.rows["charger_telemetry"])
        records.append(dict(charger_id="b1", station_id="b", state="OFFLINE", interval_seconds=300,
                            recorded_at=datetime(2025, 12, 1, 1, tzinfo=timezone.utc)))
        changed["charger_telemetry"] = self.frame("charger_telemetry", records)
        states = {row.station_id: row for row in _snapshot(changed).collect()}
        self.assertEqual(states["b"].capacity, 3)
        self.assertEqual(states["b"].rated_capacity_kw, 360.0)
        self.assertEqual(states["b"].observed_pile_count, 1)
        self.assertEqual(states["b"].unknown_count, 2)
        self.assertEqual(states["b"].offline_count, 1)
        self.assertEqual(states["b"].maintenance_count, 0)
        self.assertTrue(states["b"].is_current)
        self.assertFalse(states["b"].is_complete)
        self.assertFalse(states["a"].is_current)
        self.assertIsNone(states["c"].snapshot_at)
        self.assertEqual(states["c"].unknown_count, 1)
        self.assertEqual(states["c"].city_name, "北京市")

    def test_wrong_cadence_and_off_grid_timestamps_are_rejected(self):
        from data_analysis.spark_jobs.dashboard_aggregates import build_station_hourly_metrics
        with self.assertRaisesRegex(ValueError, "positive integer divisor"):
            build_station_hourly_metrics(self.tables, self.hourly, self.daily, 7)
        with self.assertRaisesRegex(ValueError, "cadence"):
            build_station_hourly_metrics(self.tables, self.hourly, self.daily, 600)
        from pyspark.sql import functions as F
        changed = dict(self.tables)
        changed["charger_telemetry"] = self.tables["charger_telemetry"].withColumn(
            "recorded_at", F.col("recorded_at") + F.expr("INTERVAL 1 SECOND"))
        with self.assertRaisesRegex(ValueError, "alignment"):
            build_station_hourly_metrics(changed, self.hourly, self.daily)


if __name__ == "__main__":
    unittest.main()
