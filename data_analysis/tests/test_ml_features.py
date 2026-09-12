"""Past-only handoff checks; Spark cases run only with RUN_SPARK_TESTS=1."""

from datetime import date, datetime, timedelta, timezone
import os
import unittest

from data_analysis.spark_jobs.ml_features import split_dates


class FeatureSplitDatesTests(unittest.TestCase):
    def test_full_split_is_118_30_30_days_with_exclusive_boundaries(self):
        result = split_dates("2025-12-01", "2026-05-30")
        self.assertEqual(result, {"start": "2025-12-02", "trainEnd": "2026-03-30",
                                  "validationEnd": "2026-04-29", "end": "2026-05-29", "usable": True})
        boundaries = [date.fromisoformat(result[key]) for key in ("start", "trainEnd", "validationEnd", "end")]
        self.assertEqual([(right - left).days for left, right in zip(boundaries, boundaries[1:])], [118, 30, 30])

    def test_sample_and_unusable_tiny_batches(self):
        sample = split_dates("2025-12-01", "2025-12-08")
        self.assertEqual(sample, {"start": "2025-12-02", "trainEnd": "2025-12-05",
                                 "validationEnd": "2025-12-06", "end": "2025-12-07", "usable": True})
        for length in (1, 2, 3, 4):
            tiny = split_dates("2025-12-01", (date(2025, 12, 1) + timedelta(days=length)).isoformat())
            self.assertFalse(tiny["usable"])
            self.assertLessEqual(tiny["start"], tiny["end"])
        with self.assertRaisesRegex(ValueError, "exclusive"):
            split_dates("2025-12-01", "2025-12-01")


@unittest.skipUnless(os.environ.get("RUN_SPARK_TESTS") == "1",
                     "Requires RUN_SPARK_TESTS=1, PySpark 3.5.6 and Java 17")
class MLFeatureSparkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("SPARK_LOCAL_IP", "127.0.0.1")
        from pyspark.sql import SparkSession
        from data_analysis.spark_jobs.dashboard_aggregates import build_station_hourly_metrics
        from data_analysis.spark_jobs.pipeline import station_hourly
        from data_analysis.spark_jobs.ml_features import build_ml_frames
        cls.spark = (SparkSession.builder.master("local[2]").appName("past-only-feature-tests")
                     .config("spark.ui.enabled", "false")
                     .config("spark.sql.shuffle.partitions", "2")
                     .config("spark.sql.session.timeZone", "UTC").getOrCreate())
        cls.spark.sparkContext.setLogLevel("ERROR")
        # All six business days start at UTC 16:00 on the previous date.
        cls.start = datetime(2025, 11, 30, 16, tzinfo=timezone.utc)
        cls.splits = split_dates("2025-12-01", "2025-12-07")
        rows = {
            "stations": [dict(station_id=name, city_id="city") for name in ("a", "b")],
            "chargers": [dict(charger_id="a1", station_id="a", rated_power_kw=2000.0),
                         dict(charger_id="b1", station_id="b", rated_power_kw=3000.0),
                         dict(charger_id="b2", station_id="b", rated_power_kw=3000.0)],
            "calendar": [dict(city_id="city", business_date=date(2025, 12, 1) + timedelta(days=i),
                              is_weekend=int(i == 5), scenario_event=("PUBLIC_HOLIDAY_FIXTURE" if i == 1 else
                                  "ADJUSTED_WORKDAY" if i == 5 else "NORMAL")) for i in range(6)],
            "charger_telemetry": [],
        }
        for hour in range(144):
            for minute in range(0, 60, 5):
                for station, charger, offset in [("a", "a1", 1), ("b", "b1", 101), ("b", "b2", 101)]:
                    rows["charger_telemetry"].append(dict(
                        charger_id=charger, station_id=station,
                        recorded_at=cls.start + timedelta(hours=hour, minutes=minute),
                        interval_seconds=300, state="CHARGING", energy_wh=1000 * (hour + offset),
                        grid_energy_wh=1000 * (hour + offset), grid_cost_cents=1,
                        power_kw=float(12 * (hour + offset))))
        cls.tables = {name: cls.frame(name, records) for name, records in rows.items()}
        hourly = station_hourly(cls.tables["charger_telemetry"], cls.tables["stations"], cls.tables["chargers"])
        cls.hours = build_station_hourly_metrics(cls.tables, hourly, cls.frame("station_daily", []))
        cls.hours.cache()
        cls.results = build_ml_frames(cls.hours, cls.tables, cls.splits)
        for frame in cls.results.values():
            frame.cache()

    @classmethod
    def tearDownClass(cls):
        for frame in cls.results.values():
            frame.unpersist()
        cls.hours.unpersist()
        cls.spark.stop()

    @classmethod
    def frame(cls, table, records):
        from pyspark.sql import types as T
        from data_analysis.charging_data.schema import TABLES, SUMMARY_TABLES
        from data_analysis.spark_jobs.pipeline import field_type
        fields = (TABLES if table in TABLES else SUMMARY_TABLES)[table]
        return cls.spark.createDataFrame([tuple(row.get(field) for field in fields) for row in records],
            T.StructType([T.StructField(field, field_type(field)) for field in fields]))

    def row(self, frame, hour=24, station="a"):
        from pyspark.sql import functions as F
        return frame.filter((F.col("station_id") == station) &
                            (F.col("reference_time") == F.lit(self.start + timedelta(hours=hour)))).first()

    def test_first_prediction_uses_exactly_previous_24_hours_without_cross_station(self):
        frame = self.results["ml_features_hourly"]
        a, b = self.row(frame), self.row(frame, station="b")
        self.assertIsNone(self.row(frame, hour=23))
        self.assertEqual(a.lag_power_kw_h01, 12 * 24)
        self.assertEqual(a.lag_power_kw_h24, 12)
        self.assertEqual(a.rolling_mean_kw_3h, 12 * 23)
        self.assertEqual(a.rolling_mean_kw_24h, 12 * 12.5)
        self.assertEqual(b.lag_power_kw_h01, 24 * 124)
        self.assertEqual(b.lag_power_kw_h24, 24 * 101)
        self.assertEqual(a.capacity, 1)
        self.assertEqual(a.rated_capacity_kw, 2000.0)
        self.assertEqual(b.capacity, 2)
        self.assertEqual(b.rated_capacity_kw, 6000.0)
        self.assertTrue(a.history_complete)
        self.assertEqual((a.history_end_at - a.history_start_at).total_seconds(), 24 * 3600)

    def test_labels_are_separate_and_future_changes_do_not_change_features(self):
        from pyspark.sql import functions as F
        from data_analysis.spark_jobs.ml_features import build_ml_frames
        features, targets = self.results["ml_features_hourly"], self.results["ml_targets_hourly"]
        self.assertFalse(any(name.startswith(("label_", "split_")) for name in features.columns))
        self.assertFalse(any(name.startswith(("lag_", "rolling_")) for name in targets.columns))
        self.assertFalse(any("weather" in name or "temperature" in name or "anomaly" in name for name in features.columns))
        target = self.row(targets)
        self.assertEqual(target.label_power_kw_h01, 12 * 25)
        self.assertEqual(target.label_power_kw_h24, 12 * 48)
        self.assertEqual(target.label_available_count_h01, 0)
        self.assertEqual(target.label_available_count_h24, 0)
        self.assertEqual(len([name for name in targets.columns if name.startswith("label_available_count_")]), 24)
        changed = self.hours.withColumn("mean_power_kw", F.when(
            F.col("recorded_at") >= F.lit(self.start + timedelta(hours=24)), F.col("mean_power_kw") + 100)
            .otherwise(F.col("mean_power_kw")))
        changed = changed.withColumn("end_available_count", F.when(
            F.col("recorded_at") >= F.lit(self.start + timedelta(hours=24)), F.col("capacity"))
            .otherwise(F.col("end_available_count")))
        other = build_ml_frames(changed, self.tables, self.splits)
        self.assertEqual(self.row(features).asDict(), self.row(other["ml_features_hourly"]).asDict())
        self.assertEqual(self.row(other["ml_targets_hourly"]).label_power_kw_h01,
                         target.label_power_kw_h01 + 100)
        self.assertEqual(self.row(other["ml_targets_hourly"]).label_available_count_h01, 1)
        self.assertEqual(self.row(other["ml_targets_hourly"]).label_available_count_h24, 1)
        self.assertEqual(self.row(other["ml_targets_hourly"], station="b").label_available_count_h24, 2)
        tail = self.row(targets, hour=144)
        self.assertIsNone(tail.label_power_kw_h24)
        self.assertIsNone(tail.label_available_count_h01)
        self.assertIsNone(tail.label_available_count_h24)

    def test_calendar_is_reference_day_in_shanghai_and_flags_are_boolean(self):
        frame = self.results["ml_features_hourly"]
        a = self.row(frame, hour=24)
        self.assertEqual(a.business_date, date(2025, 12, 2))
        self.assertEqual(a.hour_of_day, 0)
        self.assertEqual(a.day_of_week, 1)  # Monday=0, Tuesday=1.
        self.assertIs(a.is_public_holiday, True)
        self.assertIs(a.is_adjusted_workday, False)
        weekend = self.row(frame, hour=120)
        self.assertIs(weekend.is_weekend, True)
        self.assertIs(weekend.is_adjusted_workday, True)
        for name in ("is_weekend", "is_public_holiday", "is_adjusted_workday", "history_complete"):
            self.assertEqual(frame.schema[name].dataType.simpleString(), "boolean")
        self.assertEqual(frame.schema["business_date"].dataType.simpleString(), "date")

    def test_labels_cannot_cross_split_boundaries_and_unusable_is_excluded(self):
        from data_analysis.spark_jobs.ml_features import build_ml_frames
        # Small-batch train [Dec2, Dec4), validation [Dec4, Dec5), test [Dec5, Dec6).
        targets = self.results["ml_targets_hourly"]
        edge = self.row(targets, hour=71)  # Dec3 23:00.
        self.assertEqual(edge.split_1h, "TRAIN")
        self.assertEqual(edge.split_6h, "EXCLUDED")
        self.assertEqual(edge.split_24h, "EXCLUDED")
        self.assertEqual(self.row(targets, hour=72).split_1h, "VALIDATION")
        self.assertEqual(self.row(targets, hour=96).split_24h, "TEST")
        self.assertEqual(self.row(targets, hour=120).split_1h, "EXCLUDED")
        unusable = split_dates("2025-12-01", "2025-12-05")
        other = build_ml_frames(self.hours, self.tables, unusable)["ml_targets_hourly"]
        self.assertEqual(other.filter("split_1h != 'EXCLUDED' OR split_6h != 'EXCLUDED' OR split_24h != 'EXCLUDED'").count(), 0)

    def test_missing_or_incomplete_hour_never_becomes_zero_training_history(self):
        from pyspark.sql import functions as F
        from data_analysis.spark_jobs.ml_features import build_ml_frames
        absent_at = self.start + timedelta(hours=5)
        missing = (F.col("station_id") == "a") & (F.col("recorded_at") == F.lit(absent_at))
        for changed in [self.hours.filter(~missing),
                        self.hours.withColumn("is_complete", F.when(missing, False).otherwise(F.col("is_complete")))
                        .withColumn("mean_power_kw", F.when(missing, F.lit(None).cast("double")).otherwise(F.col("mean_power_kw")))]:
            with self.subTest(kind="deleted" if changed.columns == self.hours.columns else "incomplete"):
                other = build_ml_frames(changed, self.tables, self.splits)["ml_features_hourly"]
                self.assertIsNone(self.row(other, hour=24))
                self.assertIsNone(self.row(other, hour=29))
                self.assertIsNotNone(self.row(other, hour=30))
                self.assertIsNotNone(self.row(other, hour=24, station="b"))
        # Duplicating hour 8 while losing hour 9 keeps a 24-row/23h span, but
        # must still fail the internal consecutive-hour check.
        omitted = (F.col("station_id") == "a") & (F.col("recorded_at") == F.lit(self.start + timedelta(hours=9)))
        duplicate = self.hours.filter((F.col("station_id") == "a") &
                                      (F.col("recorded_at") == F.lit(self.start + timedelta(hours=8))))
        changed = self.hours.filter(~omitted).unionByName(duplicate)
        other = build_ml_frames(changed, self.tables, self.splits)["ml_features_hourly"]
        self.assertIsNone(self.row(other, hour=24))

    def test_invalid_power_or_availability_does_not_enter_history_or_target(self):
        from pyspark.sql import functions as F
        from data_analysis.spark_jobs.ml_features import build_ml_frames
        future = (F.col("station_id") == "a") & (F.col("recorded_at") == F.lit(self.start + timedelta(hours=30)))
        changed = (self.hours.withColumn("mean_power_kw", F.when(future, F.lit(float("inf")))
                                         .otherwise(F.col("mean_power_kw")))
                   .withColumn("end_available_count", F.when(future, F.col("capacity") + 1)
                               .otherwise(F.col("end_available_count"))))
        results = build_ml_frames(changed, self.tables, self.splits)
        labels = self.row(results["ml_targets_hourly"], hour=24)
        self.assertEqual(labels.split_1h, "TRAIN")
        self.assertEqual(labels.split_6h, "TRAIN")
        self.assertEqual(labels.split_24h, "EXCLUDED")
        self.assertIsNone(labels.label_power_kw_h07)
        self.assertIsNone(labels.label_available_count_h07)
        self.assertIsNotNone(labels.label_available_count_h24)
        self.assertIsNotNone(labels.label_power_kw_h08)
        self.assertIsNone(self.row(results["ml_features_hourly"], hour=31))


if __name__ == "__main__":
    unittest.main()
