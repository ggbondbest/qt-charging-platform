"""Actual Spark business aggregates over a completed, cleaned batch.

``build_dashboard_frames`` accepts DataFrames loaded from local or HDFS Parquet;
it never collects business records or reads Python reference aggregates. The
caller owns completion-marker, batch-identity and publishing checks.

Amounts stay integer CNY cents; energy stays integer Wh. UTC timestamp columns
describe instants and ``business_date`` is an Asia/Shanghai date. Reservation
and queue cohorts describe their FINAL OUTCOME IN THIS BATCH, not the state that
was known on a historical cohort date. They must not become forecasting inputs.

Charging utilization uses CHARGING samples / all pile samples in complete hours
only; maintenance remains in the denominator. Missing hours are not zero-load
hours. Coverage fields accompany daily totals so consumers can disclose gaps.
"""

from data_analysis.charging_data.schema import STATE_VALUES, SUMMARY_TABLES
from data_analysis.spark_jobs.pipeline import _spark_imports, business_date


STATE_SAMPLE_COLUMNS = [state.lower() + "_samples" for state in (
    "AVAILABLE", "CHARGING", "RESERVED", "OCCUPIED", "MAINTENANCE", "OFFLINE")]
DAILY_FACT_COLUMNS = [field for field in SUMMARY_TABLES["station_daily"]
                      if field not in {"station_id", "city_id", "business_date"}]
COVERAGE_COLUMNS = [
    "sample_count", "expected_sample_count", "missing_sample_count",
    "observed_hours", "complete_hours", "incomplete_hours",
    "complete_charging_samples", "complete_sample_count",
]
ADDITIVE_DAILY_COLUMNS = DAILY_FACT_COLUMNS + ["net_paid_cents", "started_sessions"] + \
    STATE_SAMPLE_COLUMNS + COVERAGE_COLUMNS
COHORT_COLUMNS = [
    "reservations_created_count", "reservation_confirmed_count", "reservation_cancelled_count",
    "reservation_expired_count", "reservation_other_status_count",
    "queues_joined_count", "queue_served_count", "queue_abandoned_count",
    "queue_call_expired_count", "queue_waiting_count", "queue_other_status_count",
]
SERVICE_COLUMNS = [
    "queues_resolved_count", "queue_wait_count", "queue_wait_seconds_sum",
    "queue_sojourn_seconds_sum", "queue_invalid_timing_count",
    "repairs_restored_count", "repair_resolution_count", "repair_resolution_seconds_sum",
    "repair_work_count", "repair_work_seconds_sum", "repair_invalid_timing_count",
    "rating_count", "rating_sum", "invalid_rating_count",
]
REQUIRED_TABLES = {
    "cities", "stations", "chargers", "calendar", "charging_sessions",
    "reservations", "queue_entries", "maintenance_tickets", "reviews", "charger_telemetry",
}


def _count_when(condition, alias):
    _, _, F, _ = _spark_imports()
    return F.sum(F.when(condition, 1).otherwise(0)).cast("long").alias(alias)


def _dimensions(tables):
    _, _, F, _ = _spark_imports()
    capacities = tables["chargers"].groupBy("station_id").agg(
        F.count("*").alias("capacity"),
        F.sum("rated_power_kw").alias("rated_capacity_kw"))
    cities = tables["cities"].select(
        "city_id", "city_name", F.col("latitude").alias("city_latitude"),
        F.col("longitude").alias("city_longitude"))
    return (tables["stations"].select("station_id", "city_id", "station_name", "site_type",
                                      "latitude", "longitude", "transformer_kw")
            .join(cities, "city_id").join(capacities, "station_id", "left")
            .fillna({"capacity": 0, "rated_capacity_kw": 0.0}))


def _day_grid(tables, daily):
    """Keep payment-only dates, including those outside telemetry coverage."""
    calendar = (tables["calendar"].select("city_id", "business_date").distinct()
                .join(tables["stations"].select("station_id", "city_id"), "city_id")
                .select("station_id", "city_id", "business_date"))
    return calendar.unionByName(daily.select("station_id", "city_id", "business_date")).distinct()


def build_station_hourly_metrics(tables, hourly, daily, sample_interval_seconds=300):
    """Expose absent hours and cadence/capacity mismatches rather than filling load.

    The completed source currently uses a fixed five-minute cadence. A caller
    processing another fixed cadence must explicitly supply that interval.
    """
    _, _, F, _ = _spark_imports()
    if (type(sample_interval_seconds) is not int or sample_interval_seconds <= 0 or
            3600 % sample_interval_seconds):
        raise ValueError("sample_interval_seconds must be a positive integer divisor of 3600")
    bad_interval = (F.col("interval_seconds").isNull() |
                    (F.col("interval_seconds") != sample_interval_seconds) |
                    F.col("recorded_at").isNull() |
                    (F.pmod(F.col("recorded_at").cast("long"), F.lit(sample_interval_seconds)) != 0))
    if tables["charger_telemetry"].filter(bad_interval).limit(1).count():
        raise ValueError("Clean telemetry cadence or alignment differs from sample_interval_seconds")
    capacity = tables["chargers"].groupBy("station_id").agg(F.count("*").alias("capacity"))
    grid = (_day_grid(tables, daily).crossJoin(hourly.sparkSession.range(24)
                                              .select(F.col("id").alias("_hour")))
            .withColumn("recorded_at", F.to_utc_timestamp(F.to_timestamp(F.concat(
                F.col("business_date").cast("string"), F.lit(" "),
                F.format_string("%02d:00:00", F.col("_hour")))), "Asia/Shanghai"))
            .drop("_hour").join(capacity, "station_id", "left").fillna({"capacity": 0}))
    observations = hourly.drop("city_id").withColumnRenamed("capacity", "_reported_capacity")
    result = (grid.join(observations, ["station_id", "recorded_at"], "left")
              .withColumn("has_observation", F.col("sample_count").isNotNull())
              .withColumn("expected_sample_count", (F.col("capacity") *
                                                      (3600 // sample_interval_seconds)).cast("long"))
              .withColumn("sample_count", F.coalesce("sample_count", F.lit(0)).cast("long")))
    for field in STATE_SAMPLE_COLUMNS:
        result = result.withColumn(field, F.coalesce(field, F.lit(0)).cast("long"))
    result = (result.withColumn("missing_sample_count", F.greatest(
        F.col("expected_sample_count") - F.col("sample_count"), F.lit(0)).cast("long"))
        .withColumn("is_complete", F.coalesce(
            F.col("has_observation") & (F.col("capacity") > 0) &
            (F.col("_reported_capacity") == F.col("capacity")) &
            (F.col("sample_count") == F.col("expected_sample_count")) &
            (sum(F.col(field) for field in STATE_SAMPLE_COLUMNS) == F.col("sample_count")),
            F.lit(False))))
    return result.select(*SUMMARY_TABLES["station_hourly"], "business_date", "has_observation",
                         "expected_sample_count", "missing_sample_count", "is_complete")


def _user_activity(tables):
    _, _, F, _ = _spark_imports()
    return (tables["charging_sessions"]
            .withColumn("business_date", business_date("started_at"))
            .groupBy("station_id", "business_date", "user_id")
            .agg(F.count("*").alias("session_count"),
                 F.min("started_at").alias("first_started_at"),
                 F.max("started_at").alias("last_started_at"))
            .join(tables["stations"].select("station_id", "city_id"), "station_id")
            .select("station_id", "city_id", "business_date", "user_id", "session_count",
                    "first_started_at", "last_started_at"))


def _daily_metrics(tables, daily, hours, user_activity):
    _, _, F, _ = _spark_imports()
    keys = ["station_id", "business_date"]
    sums = [F.sum(field).cast("long").alias(field)
            for field in STATE_SAMPLE_COLUMNS + ["sample_count", "expected_sample_count", "missing_sample_count"]]
    sums += [_count_when(F.col("has_observation"), "observed_hours"),
             _count_when(F.col("is_complete"), "complete_hours"),
             _count_when(~F.col("is_complete"), "incomplete_hours"),
             F.sum(F.when(F.col("is_complete"), F.col("charging_samples")).otherwise(0))
             .cast("long").alias("complete_charging_samples"),
             F.sum(F.when(F.col("is_complete"), F.col("sample_count")).otherwise(0))
             .cast("long").alias("complete_sample_count")]
    coverage = hours.groupBy(*keys).agg(*sums)
    started = user_activity.groupBy(*keys).agg(F.sum("session_count").cast("long").alias("started_sessions"))
    result = (_day_grid(tables, daily).join(daily.drop("city_id"), keys, "left")
              .join(coverage, keys, "left").join(started, keys, "left"))
    for field in DAILY_FACT_COLUMNS + ["started_sessions"]:
        result = result.withColumn(field, F.coalesce(field, F.lit(0)).cast("long"))
    result = (result.withColumn("net_paid_cents", F.col("paid_cents") - F.col("refund_cents"))
              .withColumn("charging_utilization", F.when(F.col("complete_sample_count") > 0,
                  F.col("complete_charging_samples") / F.col("complete_sample_count")))
              .withColumn("is_complete", F.col("incomplete_hours") == 0))
    return result.select("station_id", "city_id", "business_date", *ADDITIVE_DAILY_COLUMNS,
                         "charging_utilization", "is_complete")


def _cohorts(tables):
    """Eventual statuses grouped by creation/join date, NOT as-of status history."""
    _, _, F, _ = _spark_imports()
    keys = ["station_id", "business_date"]
    reservation_statuses = {"CONFIRMED": "confirmed", "CANCELLED": "cancelled", "EXPIRED": "expired"}
    queue_statuses = {"SERVED": "served", "ABANDONED": "abandoned",
                      "CALL_EXPIRED": "call_expired", "WAITING": "waiting"}
    reservation_agg = [F.count("*").alias("reservations_created_count")]
    reservation_agg += [_count_when(F.col("status") == state, "reservation_" + name + "_count")
                        for state, name in reservation_statuses.items()]
    reservation_agg += [_count_when(F.col("status").isNull() |
                                    ~F.col("status").isin(list(reservation_statuses)),
                                    "reservation_other_status_count")]
    queue_agg = [F.count("*").alias("queues_joined_count")]
    queue_agg += [_count_when(F.col("status") == state, "queue_" + name + "_count")
                  for state, name in queue_statuses.items()]
    queue_agg += [_count_when(F.col("status").isNull() | ~F.col("status").isin(list(queue_statuses)),
                              "queue_other_status_count")]
    reservations = (tables["reservations"].withColumn("business_date", business_date("created_at"))
                    .groupBy(*keys).agg(*reservation_agg))
    queues = (tables["queue_entries"].withColumn("business_date", business_date("joined_at"))
              .groupBy(*keys).agg(*queue_agg))
    return (reservations.join(queues, keys, "full").fillna(0)
            .join(tables["stations"].select("station_id", "city_id"), "station_id")
            .withColumn("outcome_semantics", F.lit("FINAL_OUTCOME_IN_BATCH_BY_CREATED_OR_JOINED_DATE"))
            .select("station_id", "city_id", "business_date", *COHORT_COLUMNS, "outcome_semantics"))


def _services(tables):
    _, _, F, _ = _spark_imports()
    keys = ["station_id", "business_date"]
    queue = (tables["queue_entries"].filter(F.col("status").isin("SERVED", "ABANDONED", "CALL_EXPIRED") &
                                           F.col("resolved_at").isNotNull())
             .withColumn("business_date", business_date("resolved_at")))
    wait_end = F.coalesce("called_at", "resolved_at")
    valid_queue = (F.col("joined_at").isNotNull() &
                   (wait_end >= F.col("joined_at")) & (wait_end <= F.col("resolved_at")))
    queue = queue.groupBy(*keys).agg(
        F.count("*").alias("queues_resolved_count"),
        _count_when(valid_queue, "queue_wait_count"),
        F.sum(F.when(valid_queue, wait_end.cast("long") - F.col("joined_at").cast("long"))
              .otherwise(0)).cast("long").alias("queue_wait_seconds_sum"),
        F.sum(F.when(valid_queue, F.col("resolved_at").cast("long") - F.col("joined_at").cast("long"))
              .otherwise(0)).cast("long").alias("queue_sojourn_seconds_sum"),
        _count_when(~F.coalesce(valid_queue, F.lit(False)), "queue_invalid_timing_count"))
    repairs = (tables["maintenance_tickets"].filter((F.col("status") == "RESOLVED") &
                                                   F.col("restored_at").isNotNull())
               .withColumn("business_date", business_date("restored_at")))
    valid_resolution = F.col("reported_at").isNotNull() & (F.col("restored_at") >= F.col("reported_at"))
    valid_work = (valid_resolution & F.col("work_started_at").isNotNull() &
                  (F.col("work_started_at") >= F.col("reported_at")) &
                  (F.col("restored_at") >= F.col("work_started_at")))
    repairs = repairs.groupBy(*keys).agg(
        F.count("*").alias("repairs_restored_count"),
        _count_when(valid_resolution, "repair_resolution_count"),
        F.sum(F.when(valid_resolution, F.col("restored_at").cast("long") - F.col("reported_at").cast("long"))
              .otherwise(0)).cast("long").alias("repair_resolution_seconds_sum"),
        _count_when(valid_work, "repair_work_count"),
        F.sum(F.when(valid_work, F.col("restored_at").cast("long") - F.col("work_started_at").cast("long"))
              .otherwise(0)).cast("long").alias("repair_work_seconds_sum"),
        _count_when(~F.coalesce(valid_resolution &
                    (F.col("work_started_at").isNull() | valid_work), F.lit(False)), "repair_invalid_timing_count"))
    valid_rating = F.col("rating").between(1, 5)
    reviews = (tables["reviews"].withColumn("business_date", business_date("created_at"))
               .groupBy(*keys).agg(_count_when(valid_rating, "rating_count"),
                   F.sum(F.when(valid_rating, F.col("rating")).otherwise(0)).cast("long").alias("rating_sum"),
                   _count_when(~F.coalesce(valid_rating, F.lit(False)), "invalid_rating_count")))
    return (queue.join(repairs, keys, "full").join(reviews, keys, "full").fillna(0)
            .join(tables["stations"].select("station_id", "city_id"), "station_id")
            .select("station_id", "city_id", "business_date", *SERVICE_COLUMNS))


def _snapshot(tables):
    """Never mix a missing pile's older state into a newer station snapshot."""
    _, _, F, _ = _spark_imports()
    telemetry = tables["charger_telemetry"]
    latest = telemetry.groupBy("station_id").agg(F.max("recorded_at").alias("snapshot_at"))
    current = telemetry.join(latest, "station_id").filter(F.col("recorded_at") == F.col("snapshot_at"))
    states = current.groupBy("station_id", "snapshot_at").agg(
        F.countDistinct("charger_id").alias("observed_pile_count"),
        *[_count_when(F.col("state") == state, state.lower() + "_count")
          for state in ("AVAILABLE", "CHARGING", "RESERVED", "OCCUPIED", "MAINTENANCE", "OFFLINE")])
    result = (_dimensions(tables).join(states, "station_id", "left")
              .crossJoin(telemetry.agg(F.max("recorded_at").alias("data_as_of"))))
    for field in ["observed_pile_count"] + [state.lower() + "_count" for state in STATE_VALUES]:
        result = result.withColumn(field, F.coalesce(field, F.lit(0)).cast("long"))
    return (result.withColumn("unknown_count", F.greatest(F.col("capacity") - F.col("observed_pile_count"), F.lit(0)))
            .withColumn("is_current", F.coalesce(F.col("snapshot_at") == F.col("data_as_of"), F.lit(False)))
            .withColumn("is_complete", (F.col("capacity") > 0) & F.col("is_current") &
                        (F.col("observed_pile_count") == F.col("capacity")))
            .select("station_id", "city_id", "station_name", "city_name", "site_type",
                    "latitude", "longitude", "city_latitude", "city_longitude", "capacity",
                    "rated_capacity_kw", "transformer_kw", "snapshot_at", "data_as_of",
                    "observed_pile_count", "available_count", "charging_count", "reserved_count",
                    "occupied_count", "maintenance_count", "offline_count", "unknown_count",
                    "is_current", "is_complete"))


def build_dashboard_frames(tables, hourly, daily, sample_interval_seconds=300):
    """Return seven Spark DataFrames; writes/publication are the caller's job.

    Repeat users over arbitrary filters must be calculated from
    user_activity_daily: group the filtered rows by user_id, sum session_count,
    and count users with at least two starts. Never sum daily distinct counts.
    A cohort's business_date is not its event-resolution or knowledge date.
    Queue waiting ends at the first call, or at exit if never called; sojourn
    ends at resolution. Repair resolution is report-to-restoration; work time
    is work_started-to-restoration. In-progress cases never enter these means.
    """
    _, _, F, _ = _spark_imports()
    missing = REQUIRED_TABLES - set(tables)
    if missing:
        raise ValueError("Missing cleaned tables: " + ", ".join(sorted(missing)))
    hourly.sparkSession.conf.set("spark.sql.session.timeZone", "UTC")
    hours = build_station_hourly_metrics(tables, hourly, daily, sample_interval_seconds)
    users = _user_activity(tables)
    stations = _daily_metrics(tables, daily, hours, users)
    cities = (stations.groupBy("city_id", "business_date")
              .agg(F.countDistinct("station_id").alias("station_count"),
                   *[F.sum(field).cast("long").alias(field) for field in ADDITIVE_DAILY_COLUMNS])
              .withColumn("charging_utilization", F.when(F.col("complete_sample_count") > 0,
                  F.col("complete_charging_samples") / F.col("complete_sample_count")))
              .withColumn("is_complete", F.col("incomplete_hours") == 0)
              .select("city_id", "business_date", "station_count", *ADDITIVE_DAILY_COLUMNS,
                      "charging_utilization", "is_complete"))
    return {"station_hourly_metrics": hours, "station_metrics_daily": stations,
            "city_daily": cities, "station_snapshot": _snapshot(tables),
            "user_activity_daily": users, "station_cohorts_daily": _cohorts(tables),
            "station_service_daily": _services(tables)}
