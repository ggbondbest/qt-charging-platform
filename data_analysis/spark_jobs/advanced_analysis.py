"""Multidimensional, auditable Spark summaries for the decision-analysis board.

Build from the immutable CLEAN Parquet batch, never the Python generator's
reference aggregates. Seven reduced tables support load surfaces, station
efficiency, mutually exclusive request flows, behavior distributions, service
bottlenecks, segment drilldown and legacy retention. JSON exports contain no
user IDs or raw charging records.
"""

import argparse
from datetime import date, datetime, timedelta, timezone
import gzip
import hashlib
import json
from pathlib import Path
import re
from zoneinfo import ZoneInfo

from data_analysis.charging_data.schema import SCHEMA_VERSION
from data_analysis.contracts import CONTRACT_VERSION
from data_analysis.spark_jobs import fs
from data_analysis.spark_jobs.dashboard_aggregates import (
    COHORT_COLUMNS, SERVICE_COLUMNS, build_dashboard_frames,
)
from data_analysis.spark_jobs.pipeline import business_date, create_spark, station_daily, station_hourly

ANALYSIS_VERSION = "2.0.0"
MAX_EXPORT_ROWS = 250000
TABLE_NAMES = ("station_day", "station_hour", "attempt_flow", "session_segments", "retention",
               "user_behavior", "service_hour")
INPUT_TABLES = (
    "cities", "stations", "chargers", "users", "vehicles", "calendar", "weather_hourly",
    "charging_attempts", "charging_sessions", "payments", "operating_costs",
    "maintenance_tickets", "queue_entries", "reservations", "reviews", "charger_telemetry",
)
DEFINITIONS = {
    "station_day": "Station/business-day panel. Energy and grid costs use telemetry intervals; receipts/refunds use transaction date; repair costs use restoration date. Net receipts minus listed costs is an operating-contribution proxy, not accounting profit. Means must be recomputed from sums and counts.",
    "station_hour": "Station/hour panel with Shanghai hour, operational day type, site type and observed weather. Complete-hour utilization denominator includes all piles and maintenance. Missing telemetry is null load, never zero. Weather is observed context, not a causal effect.",
    "attempt_flow": "One row per disjoint station/Shanghai-day/hour/access-path/outcome/final-session-status/failure-reason group. Each request enters DIRECT, QUEUE or RESERVATION exactly once. Final outcome is observed at batch completion, not historical as-of state or a forecasting feature. Concrete failure reasons must not be collapsed into a generic abandonment label when diagnosing bottlenecks.",
    "session_segments": "Whole-session energy, bill and durations assigned to Shanghai START date; site type × user segment × battery capacity band × connector type are jointly grouped, not separate marginals. Capacity bands use actual vehicle battery_capacity_kwh: LT50 (<50), 50_TO_69 (50<=kWh<70), GE70 (>=70), UNKNOWN (missing/nonpositive/nonfinite). vehicle_class is deliberately excluded because the source duplicates user segment. Do not compare start-cohort energy with interval-day electricity as if equal.",
    "retention": "First OBSERVED charging month within the selected ALL/CITY/STATION scope, not true acquisition. A user returns if they charge at least once in the observation month in that same scope. Cohort size is fixed; complete calendar months only. Missing future cells are censored, observed zero-return cells are explicit zero. Site-type filtering does not apply.",
    "user_behavior": "Session-event frequency distribution, not distinct users: each start is assigned to its station, Shanghai date, user segment, prior-start interval band and delivered-energy band. Prior start is computed over each user's FULL source history across ALL stations BEFORE query filters. FIRST_OBSERVED is left-censored (not zero interval, acquisition, churn or non-return); it does not enter interval means. Interval bands are [0,1), [1,3), [3,7), [7,14), [14,infinity) days; energy bands are [0,10), [10,20), [20,40), [40,infinity) kWh. Same-time starts are ordered by session_id for deterministic zero intervals. Frequent users contribute more session events; no user-level percentile/frequency or retention inference is valid from these counts.",
    "service_hour": "Station/Shanghai-date/hour resource-context panel. Resolved queue wait ends at first call, or exit if never called, assigned to JOIN hour (not resolution hour); waiting/in-progress or invalid chronology are excluded from the mean. Whole-session post-charge and connected durations are assigned to START hour, not interval-hour occupancy; only jointly valid durations enter the paired ratio. Event aggregates join once to the complete station-hour grid and do not multiply. Correlations with attempt success are descriptive associations, never causal effects.",
    "weather": "City weather is repeated for each station for matched comparisons. Do not sum rainfall or weather-hour counts across stations and call it city weather; deduplicate city/date or use station-hour matched averages.",
    "day_type": "HOLIDAY for reference-calendar PUBLIC_HOLIDAY events; WORKDAY for ADJUSTED_WORKDAY; otherwise literal Saturday/Sunday WEEKEND or WORKDAY. LOCAL_EXPO stays an additional scenario_event category.",
    "causality": "Associations and simulated observed cohorts only: no causal weather, price-elasticity, campaign uplift or real-world performance claim.",
}


def complete_months_through(period_end_exclusive):
    """Latest fully observed Shanghai month, honoring a precise exclusive end."""
    stamp = datetime.fromisoformat(period_end_exclusive.replace("Z", "+00:00"))
    if stamp.tzinfo is None:
        raise ValueError("period_end_exclusive needs an explicit timezone")
    local = stamp.astimezone(ZoneInfo("Asia/Shanghai"))
    previous = date(local.year, local.month, 1) - timedelta(days=1)
    return previous.replace(day=1)


def first_complete_month(period_start):
    stamp = datetime.fromisoformat(period_start.replace("Z", "+00:00"))
    if stamp.tzinfo is None:
        raise ValueError("period_start needs an explicit timezone")
    local = stamp.astimezone(ZoneInfo("Asia/Shanghai"))
    first = date(local.year, local.month, 1)
    if (local.day, local.hour, local.minute, local.second, local.microsecond) != (1, 0, 0, 0, 0):
        first = (first + timedelta(days=32)).replace(day=1)
    return first


def _assert_unique(frame, keys, name):
    from functools import reduce
    from pyspark.sql import functions as F
    missing = reduce(lambda left, right: left | right, [F.col(key).isNull() for key in keys])
    if (frame.filter(missing).limit(1).count() or
            frame.groupBy(*keys).count().filter("count > 1").limit(1).count()):
        raise ValueError("Missing or duplicate join key: " + name)


def _calendar(tables):
    from pyspark.sql import functions as F
    frame = tables["calendar"].select("city_id", "business_date", "scenario_event")
    event = F.coalesce(F.col("scenario_event"), F.lit("NONE"))
    return frame.withColumn("day_type", F.when(event.startswith("PUBLIC_HOLIDAY"), "HOLIDAY")
        .when(event == "ADJUSTED_WORKDAY", "WORKDAY")
        .when(F.dayofweek("business_date").isin(1, 7), "WEEKEND").otherwise("WORKDAY"))


def _decorate(frame, tables, calendar):
    from pyspark.sql import functions as F
    result = (frame.join(tables["stations"].select("station_id", "site_type"), "station_id", "left")
              .join(calendar, ["city_id", "business_date"], "left"))
    # Payment-only tail dates can fall after the telemetry/calendar window.
    return result.withColumn("day_type", F.coalesce("day_type",
        F.when(F.dayofweek("business_date").isin(1, 7), "WEEKEND").otherwise("WORKDAY")))\
        .fillna("NONE", subset=["scenario_event"])


def build_session_segments(tables):
    from pyspark.sql import functions as F
    capacity = F.col("battery_capacity_kwh")
    vehicles = tables["vehicles"].select("vehicle_id", "battery_capacity_kwh").withColumn("battery_capacity_band",
        F.when(capacity.isNull() | F.isnan(capacity) | (F.abs(capacity) == float("inf")) | (capacity <= 0), "UNKNOWN")
         .when(capacity < 50, "LT50").when(capacity < 70, "50_TO_69").otherwise("GE70"))
    sessions = (tables["charging_sessions"]
        .join(tables["stations"].select("station_id", "city_id", "site_type"), "station_id", "left")
        .join(tables["users"].select("user_id", F.col("segment").alias("user_segment")), "user_id", "left")
        .join(vehicles.select("vehicle_id", "battery_capacity_band"), "vehicle_id", "left")
        .join(tables["chargers"].select("charger_id", "connector_type"), "charger_id", "left")
        .withColumn("business_date", business_date("started_at"))
        .withColumn("_active", F.col("ended_at").cast("long") - F.col("started_at").cast("long"))
        .withColumn("_connected", F.col("unplugged_at").cast("long") - F.col("started_at").cast("long"))
        .withColumn("_occupied", F.col("unplugged_at").cast("long") - F.col("ended_at").cast("long"))
        .fillna("UNKNOWN", subset=["site_type", "user_segment", "battery_capacity_band", "connector_type"]))
    keys = ["station_id", "city_id", "business_date", "site_type", "user_segment", "battery_capacity_band", "connector_type"]
    aggregates = [F.count("*").alias("session_count"), F.sum("energy_wh").alias("energy_wh"),
                  F.sum("total_fee_cents").alias("billed_cents")]
    for field in ("active", "connected", "occupied"):
        valid = F.col("_" + field) >= 0
        aggregates.extend([
            F.sum(F.when(valid, F.col("_" + field)).otherwise(0)).cast("long").alias(field + "_seconds_sum"),
            F.sum(F.when(valid, 1).otherwise(0)).cast("long").alias(field + "_duration_count"),
        ])
    return sessions.groupBy(*keys).agg(*aggregates)


def build_attempt_flow(tables):
    from pyspark.sql import functions as F
    attempts = tables["charging_attempts"]
    if attempts.filter(F.col("queue_id").isNotNull() & F.col("reservation_id").isNotNull()).limit(1).count():
        raise ValueError("A request cannot enter queue and reservation paths simultaneously")
    flow = (attempts.join(tables["stations"].select("station_id", "city_id"), "station_id", "left")
        .join(tables["charging_sessions"].select("session_id", F.col("status").alias("session_status")), "session_id", "left")
        .withColumn("business_date", business_date("attempted_at"))
        .withColumn("local_hour", F.hour(F.from_utc_timestamp("attempted_at", "Asia/Shanghai")))
        .withColumn("access_path", F.when(F.col("reservation_id").isNotNull(), "RESERVATION")
                    .when(F.col("queue_id").isNotNull(), "QUEUE").otherwise("DIRECT"))
        .withColumn("session_status", F.coalesce("session_status", F.lit("NO_SESSION")))
        .fillna("UNKNOWN", subset=["outcome"]).fillna("NONE", subset=["failure_reason"]))
    return flow.groupBy("station_id", "city_id", "business_date", "local_hour", "access_path", "outcome",
                        "session_status", "failure_reason").agg(F.count("*").alias("attempt_count"))


def build_user_behavior(tables):
    """Preserve cross-station/history intervals before any dashboard filtering."""
    from pyspark.sql import Window, functions as F
    history = Window.partitionBy("user_id").orderBy("started_at", "session_id")
    sessions = (tables["charging_sessions"]
        .withColumn("_previous_start", F.lag("started_at").over(history))
        .withColumn("_interval", F.col("started_at").cast("long") - F.col("_previous_start").cast("long"))
        .join(tables["stations"].select("station_id", "city_id"), "station_id", "left")
        .join(tables["users"].select("user_id", F.col("segment").alias("user_segment")), "user_id", "left")
        .withColumn("business_date", business_date("started_at"))
        .withColumn("gap_bucket", F.when(F.col("_previous_start").isNull(), "FIRST_OBSERVED")
            .when(F.col("_interval") < 86400, "LT1D").when(F.col("_interval") < 3 * 86400, "1_TO_3D")
            .when(F.col("_interval") < 7 * 86400, "3_TO_7D")
            .when(F.col("_interval") < 14 * 86400, "7_TO_14D").otherwise("GE14D"))
        .withColumn("energy_bucket", F.when(F.col("energy_wh").isNull() | (F.col("energy_wh") < 0), "UNKNOWN")
            .when(F.col("energy_wh") < 10000, "LT10").when(F.col("energy_wh") < 20000, "10_TO_20")
            .when(F.col("energy_wh") < 40000, "20_TO_40").otherwise("GE40"))
        .fillna("UNKNOWN", subset=["user_segment"]))
    valid_interval = F.col("_previous_start").isNotNull() & (F.col("_interval") >= 0)
    return sessions.groupBy("station_id", "city_id", "business_date", "user_segment", "gap_bucket", "energy_bucket").agg(
        F.count("*").alias("session_count"), F.sum("energy_wh").alias("energy_wh"),
        F.sum(F.when(valid_interval, F.col("_interval")).otherwise(0)).cast("long").alias("interval_seconds_sum"),
        F.sum(F.when(valid_interval, 1).otherwise(0)).cast("long").alias("interval_count"),
        F.sum(F.when(F.col("_previous_start").isNull(), 1).otherwise(0)).cast("long").alias("first_observed_count"))


def build_service_hour(tables, hours):
    """Join one row per event cohort to one row per station/date/hour grid."""
    from pyspark.sql import functions as F
    keys = ["station_id", "city_id", "business_date", "local_hour"]
    dimensions = tables["stations"].select("station_id", "city_id")
    sessions = (tables["charging_sessions"].join(dimensions, "station_id", "left")
        .withColumn("business_date", business_date("started_at"))
        .withColumn("local_hour", F.hour(F.from_utc_timestamp("started_at", "Asia/Shanghai"))))
    duration_valid = ((F.col("ended_at") >= F.col("started_at")) &
                      (F.col("unplugged_at") >= F.col("ended_at")))
    session_groups = sessions.groupBy(*keys).agg(F.count("*").alias("session_count"),
        F.sum(F.when(duration_valid, F.col("unplugged_at").cast("long") - F.col("ended_at").cast("long"))
              .otherwise(0)).cast("long").alias("occupied_seconds_sum"),
        F.sum(F.when(duration_valid, F.col("unplugged_at").cast("long") - F.col("started_at").cast("long"))
              .otherwise(0)).cast("long").alias("connected_seconds_sum"))
    queues = (tables["queue_entries"]
        .filter(F.col("status").isin("SERVED", "ABANDONED", "CALL_EXPIRED") & F.col("resolved_at").isNotNull())
        .join(dimensions, "station_id", "left")
        .withColumn("business_date", business_date("joined_at"))
        .withColumn("local_hour", F.hour(F.from_utc_timestamp("joined_at", "Asia/Shanghai"))))
    wait_end = F.coalesce("called_at", "resolved_at")
    valid_wait = F.col("joined_at").isNotNull() & (wait_end >= F.col("joined_at")) & (wait_end <= F.col("resolved_at"))
    queue_groups = queues.groupBy(*keys).agg(
        F.sum(F.when(valid_wait, wait_end.cast("long") - F.col("joined_at").cast("long"))
              .otherwise(0)).cast("long").alias("queue_wait_seconds_sum"),
        F.sum(F.when(valid_wait, 1).otherwise(0)).cast("long").alias("queue_wait_count"))
    # Outer joins retain source event cohorts even if telemetry/calendar has an
    # uncovered boundary hour; resource telemetry then remains missing, not zero.
    return (hours.select(*keys).join(session_groups, keys, "full").join(queue_groups, keys, "full")
        .fillna(0, subset=["session_count", "occupied_seconds_sum", "connected_seconds_sum",
                           "queue_wait_seconds_sum", "queue_wait_count"]))


def build_station_inventory(tables):
    from pyspark.sql import functions as F
    interfaces = (tables["chargers"].fillna("UNKNOWN", subset=["connector_type"])
        .groupBy("station_id", "connector_type").agg(F.count("*").alias("charger_count"),
                                                   F.sum("rated_power_kw").alias("rated_power_kw")))
    capacities = interfaces.groupBy("station_id").agg(F.sum("charger_count").alias("capacity"),
        F.sum("rated_power_kw").alias("rated_capacity_kw"),
        F.sort_array(F.collect_list(F.struct("connector_type", "charger_count", "rated_power_kw"))).alias("interfaces"))
    return (tables["stations"].join(tables["cities"].select("city_id", "city_name"), "city_id")
        .join(capacities, "station_id").select("station_id", "city_id", "city_name", "station_name", "site_type",
            "latitude", "longitude", "transformer_kw", "capacity", "rated_capacity_kw", "interfaces"))


def build_retention(tables, period_start, period_end_exclusive):
    from pyspark.sql import functions as F
    last_month = complete_months_through(period_end_exclusive)
    first_month = first_complete_month(period_start)
    sessions = (tables["charging_sessions"].filter(
        (F.col("started_at") >= F.to_timestamp(F.lit(period_start))) &
        (F.col("started_at") < F.to_timestamp(F.lit(period_end_exclusive))))
        .join(tables["stations"].select("station_id", "city_id"), "station_id")
        .withColumn("month", F.trunc(business_date("started_at"), "month")))
    scopes = []
    for scope, identifier in [("ALL", F.lit("ALL")), ("CITY", F.col("city_id")), ("STATION", F.col("station_id"))]:
        scopes.append(sessions.select("user_id", "month", F.lit(scope).alias("scope_type"), identifier.alias("scope_id")))
    activity = scopes[0].unionByName(scopes[1]).unionByName(scopes[2]).distinct()
    membership = activity.groupBy("scope_type", "scope_id", "user_id").agg(F.min("month").alias("cohort_month"))
    cohort_keys = ["scope_type", "scope_id", "cohort_month"]
    cohorts = (membership.filter(F.col("cohort_month").between(F.lit(first_month), F.lit(last_month)))
               .groupBy(*cohort_keys).agg(F.count("*").alias("cohort_size")))
    observations = (activity.filter(F.col("month") <= F.lit(last_month))
        .join(membership, ["scope_type", "scope_id", "user_id"])
        .withColumn("month_offset", F.months_between("month", "cohort_month").cast("int"))
        .groupBy(*cohort_keys, "month_offset").agg(F.countDistinct("user_id").alias("n")))
    grid = cohorts.withColumn("month_offset", F.explode(F.sequence(F.lit(0),
        F.months_between(F.lit(last_month), "cohort_month").cast("int"))))
    return (grid.join(observations, cohort_keys + ["month_offset"], "left").fillna({"n": 0})
            .select(*cohort_keys, "month_offset", "n", "cohort_size"))


def build_advanced_frames(tables, period_start, period_end_exclusive, sample_interval_seconds=300):
    """Return the seven bounded-grain DataFrames, not collected records."""
    from pyspark.sql import functions as F
    missing = set(INPUT_TABLES) - set(tables)
    if missing:
        raise ValueError("Missing clean advanced inputs: " + ", ".join(sorted(missing)))
    spark = tables["stations"].sparkSession
    spark.conf.set("spark.sql.session.timeZone", "UTC")
    energy = F.col("energy_wh")
    if tables["charging_sessions"].filter(energy.isNull() | F.isnan(energy) |
            (F.abs(energy) == float("inf")) | (energy < 0)).limit(1).count():
        raise ValueError("Behavior analysis requires finite, nonnegative session energy_wh")
    for table, keys in [("stations", ["station_id"]), ("cities", ["city_id"]),
                        ("chargers", ["charger_id"]), ("users", ["user_id"]), ("vehicles", ["vehicle_id"]),
                        ("calendar", ["city_id", "business_date"]), ("weather_hourly", ["city_id", "recorded_at"]),
                        ("charging_sessions", ["session_id"]), ("charging_attempts", ["attempt_id"]),
                        ("queue_entries", ["queue_id"])]:
        _assert_unique(tables[table], keys, table)
    calendar = _calendar(tables)
    hourly = station_hourly(tables["charger_telemetry"], tables["stations"], tables["chargers"])
    daily = station_daily(tables)
    base = build_dashboard_frames(tables, hourly, daily, sample_interval_seconds)
    hours = (_decorate(base["station_hourly_metrics"], tables, calendar)
        .withColumn("local_hour", F.hour(F.from_utc_timestamp("recorded_at", "Asia/Shanghai")))
        .join(tables["weather_hourly"].select("city_id", "recorded_at", "temperature_c", "rainfall_mm", "weather"),
              ["city_id", "recorded_at"], "left"))
    flow = build_attempt_flow(tables)
    segments = build_session_segments(tables)
    keys = ["station_id", "city_id", "business_date"]
    attempt_daily = flow.groupBy(*keys).agg(F.sum("attempt_count").alias("attempt_count"),
        F.sum(F.when(F.col("outcome") == "STARTED", F.col("attempt_count")).otherwise(0)).alias("successful_attempt_count"))
    session_daily = segments.groupBy(*keys).agg(*[F.sum(field).alias(field) for field in
        ["session_count", "active_seconds_sum", "connected_seconds_sum", "occupied_seconds_sum",
         "active_duration_count", "connected_duration_count", "occupied_duration_count"]])
    weather_daily = (tables["weather_hourly"].withColumn("business_date", business_date("recorded_at"))
        .groupBy("city_id", "business_date").agg(F.sum("temperature_c").alias("weather_temperature_sum"),
            F.count("temperature_c").alias("weather_hour_count"), F.sum("rainfall_mm").alias("rainfall_mm_sum")))
    days = (_decorate(base["station_metrics_daily"], tables, calendar)
        .join(base["station_cohorts_daily"].drop("outcome_semantics"), keys, "left")
        .join(base["station_service_daily"], keys, "left").join(attempt_daily, keys, "left")
        .join(session_daily, keys, "left").join(weather_daily, ["city_id", "business_date"], "left")
        .fillna(0, subset=COHORT_COLUMNS + SERVICE_COLUMNS + ["attempt_count", "successful_attempt_count",
            "session_count", "active_seconds_sum", "connected_seconds_sum", "occupied_seconds_sum",
            "active_duration_count", "connected_duration_count", "occupied_duration_count"]))
    return {"station_day": days, "station_hour": hours, "attempt_flow": flow,
            "session_segments": segments, "retention": build_retention(tables, period_start, period_end_exclusive),
            "user_behavior": build_user_behavior(tables), "service_hour": build_service_hour(tables, hours)}


def _json_value(value):
    if isinstance(value, datetime):
        if value.tzinfo is None:
            raise ValueError("Serialize Spark timestamps inside the UTC Spark session, not as naive Python datetimes")
        return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
    if isinstance(value, date):
        return value.isoformat()
    raise TypeError("Unsupported export scalar: " + type(value).__name__)


def export_scalars(frame):
    """Avoid host-TZ shifts in PySpark TimestampType -> Python datetime conversion.

    SQL timestamp formatting happens in the UTC Spark session before records
    cross the JVM/Python boundary. A naive Python datetime must never be marked
    with an invented Z suffix. Null timestamps remain JSON null.
    """
    from pyspark.sql import functions as F, types as T
    if frame.sparkSession.conf.get("spark.sql.session.timeZone") != "UTC":
        raise ValueError("Advanced timestamp export requires Spark UTC timezone")
    return frame.select(*[
        F.date_format(F.col(field.name), "yyyy-MM-dd'T'HH:mm:ss.SSSSSS'Z'").alias(field.name)
        if isinstance(field.dataType, T.TimestampType) else F.col(field.name)
        for field in frame.schema.fields
    ])


def _digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def accepted_clean_inventory(acceptance, serving, serving_sha256):
    """Bind the files to the previously completed acceptance bundle, not today.

    Merely hashing whatever files exist and comparing their row counts would
    allow a same-size replacement to masquerade as the published CLEAN batch.
    The acceptance manifest is the existing inventory of that original batch.
    """
    if (acceptance.get("bundleVersion") != "acceptance-bundle-1.0.0"
            or acceptance.get("servingManifestSha256") != serving_sha256
            or any(not serving.get(key) or acceptance.get(key) != serving.get(key)
                   for key in ("datasetId", "publishedBatchId", "pipelineRunId", "sourceManifestSha256"))):
        raise ValueError("Acceptance manifest does not bind this published CLEAN batch")
    entries = acceptance.get("files")
    if not isinstance(entries, list) or not entries:
        raise ValueError("Acceptance manifest has no authenticated file inventory")
    declared = {}
    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError("Invalid acceptance file metadata")
        relative = fs.safe_relative(entry.get("path"))
        if (relative in declared or type(entry.get("bytes")) is not int or entry["bytes"] < 0
                or not isinstance(entry.get("sha256"), str) or not re.fullmatch(r"[a-f0-9]{64}", entry["sha256"])):
            raise ValueError("Duplicate or invalid acceptance file metadata")
        declared[relative] = entry
    inventory = {}
    for name in INPUT_TABLES:
        table = acceptance.get("cleanTables", {}).get(name, {})
        if (table.get("path") != "clean/" + name or type(table.get("rowCount")) is not int or table["rowCount"] < 0):
            raise ValueError("Missing authenticated clean table: " + name)
        prefix = "clean/" + name + "/"
        files = {relative: entry for relative, entry in declared.items() if relative.startswith(prefix)
                 and relative != prefix + "_SUCCESS"}
        if (not files or any(not relative[len(prefix):].startswith("part-")
                or not relative.endswith(".parquet") or "/" in relative[len(prefix):] for relative in files)):
            raise ValueError("Invalid authenticated clean shard inventory: " + name)
        inventory[name] = files
    return inventory


def verify_clean_shard(expected, digest, size):
    if digest != expected["sha256"] or size != expected["bytes"]:
        raise ValueError("CLEAN shard differs from authenticated acceptance inventory: " + expected["path"])


def export_advanced_analysis(spark, input_root, output_root, max_rows=MAX_EXPORT_ROWS):
    """New-only portable output; local/HDFS CLEAN input, bounded local JSON.gz.

    Aggregate records stream through toLocalIterator after a hard row limit.
    Only station labels and scalar invariants are collected. The exact input
    Parquet inventory is fingerprinted both before and after the export.
    """
    from pyspark import StorageLevel
    from pyspark.sql import functions as F
    if type(max_rows) is not int or max_rows < 1 or max_rows > MAX_EXPORT_ROWS:
        raise ValueError("max_rows must be within the fixed export safety bound")
    target = Path(output_root)
    if target.exists() or target.is_symlink():
        raise FileExistsError("Advanced output exists; choose a new directory")
    target = target.resolve()
    fs.independent_paths(spark, input_root, str(target))
    fs.require_success(spark, input_root)
    root = input_root.rstrip("/")
    manifest_path = root + "/serving_manifest.json"
    source = fs.read_json(spark, manifest_path)
    quality_relative = fs.safe_relative(source["qualityReportPath"])
    quality_path = root + "/" + quality_relative
    if fs.checksum(spark, quality_path) != source["qualityReportSha256"]:
        raise ValueError("Quality report checksum differs from published manifest")
    quality = fs.read_json(spark, quality_path)
    if (source.get("schemaVersion") != CONTRACT_VERSION or quality.get("schema_version") != SCHEMA_VERSION
            or quality.get("engine") != "PySpark"
            or quality.get("dataset_id") != source.get("datasetId")
            or quality.get("pipeline_run_id") != source.get("pipelineRunId")
            or quality.get("source_manifest_sha256") != source.get("sourceManifestSha256")
            or quality.get("reference_aggregates_used_as_input") is not False
            or quality.get("manifest_row_counts_verified") is not True
            or quality.get("raw_file_checksums_verified") is not True):
        raise ValueError("Published CLEAN provenance is incomplete or inconsistent")
    inputs = {"serving_manifest.json": fs.checksum(spark, manifest_path),
              quality_relative: fs.checksum(spark, quality_path)}
    acceptance_path = root + "/acceptance_manifest.json"
    acceptance = fs.read_json(spark, acceptance_path)
    expected_inventory = accepted_clean_inventory(acceptance, source, inputs["serving_manifest.json"])
    inputs["acceptance_manifest.json"] = fs.checksum(spark, acceptance_path)
    inventory = {}
    for table in INPUT_TABLES:
        paths = fs.glob(spark, root + "/clean/" + table + "/*.parquet")
        actual = {"clean/" + table + "/" + path.rsplit("/", 1)[-1] for path in paths}
        if actual != set(expected_inventory[table]):
            raise ValueError("CLEAN shard inventory differs from authenticated acceptance batch: " + table)
        inventory[table] = paths
        for path in paths:
            relative = "clean/" + table + "/" + path.rsplit("/", 1)[-1]
            digest = fs.checksum(spark, path)
            verify_clean_shard(expected_inventory[table][relative], digest, fs.file_size(spark, path))
            inputs[relative] = digest
    target.mkdir(parents=True, exist_ok=False)
    (target / "_RUNNING").touch(exist_ok=False)
    cached = []
    try:
        tables, counts = {}, {}
        for name in INPUT_TABLES:
            tables[name] = spark.read.parquet(root + "/clean/" + name).persist(StorageLevel.DISK_ONLY)
            cached.append(tables[name])
            counts[name] = tables[name].count()
            expected = quality["clean_session_rows"] if name == "charging_sessions" else quality["input_rows"][name]
            if counts[name] != expected or counts[name] != acceptance["cleanTables"][name]["rowCount"]:
                raise ValueError("Clean input row-count mismatch: " + name)
        frames = build_advanced_frames(tables, source["periodStart"], source["periodEndExclusive"])
        metadata, checks = {}, {}
        sort_keys = {"station_day": ["city_id", "station_id", "business_date"],
            "station_hour": ["city_id", "station_id", "recorded_at"],
            "attempt_flow": ["city_id", "station_id", "business_date", "local_hour", "access_path", "outcome", "session_status", "failure_reason"],
            "session_segments": ["city_id", "station_id", "business_date", "user_segment", "battery_capacity_band", "connector_type"],
            "retention": ["scope_type", "scope_id", "cohort_month", "month_offset"],
            "user_behavior": ["city_id", "station_id", "business_date", "user_segment", "gap_bucket", "energy_bucket"],
            "service_hour": ["city_id", "station_id", "business_date", "local_hour"]}
        plan_dir = target / "spark_plans"
        plan_dir.mkdir()
        for name in TABLE_NAMES:
            frame = frames[name].persist(StorageLevel.DISK_ONLY)
            try:
                rows = frame.count()
                if rows > max_rows:
                    raise ValueError("Aggregate export exceeds row bound: " + name)
                if name == "attempt_flow":
                    checks["attemptsConserved"] = frame.agg(F.sum("attempt_count")).first()[0] == counts["charging_attempts"]
                elif name == "session_segments":
                    sums = frame.agg(F.sum("session_count"), F.sum("energy_wh"), F.sum("billed_cents")).first()
                    expected = tables["charging_sessions"].agg(F.count("*"), F.sum("energy_wh"), F.sum("total_fee_cents")).first()
                    checks["sessionsEnergyBillsConserved"] = tuple(sums) == tuple(expected)
                elif name == "retention":
                    checks["retentionDenominatorsValid"] = not frame.filter(
                        (F.col("cohort_size") <= 0) | (F.col("n") < 0) | (F.col("n") > F.col("cohort_size")) |
                        ((F.col("month_offset") == 0) & (F.col("n") != F.col("cohort_size")))).limit(1).count()
                elif name == "station_day":
                    checks["stationDayUnique"] = not frame.groupBy("station_id", "business_date").count().filter("count > 1").limit(1).count()
                elif name == "user_behavior":
                    if frame.filter(F.col("energy_bucket") == "UNKNOWN").limit(1).count():
                        raise ValueError("Invalid energy bucket in authenticated behavior output")
                    sums = frame.agg(F.sum("session_count"), F.sum("energy_wh"),
                        F.sum("interval_count"), F.sum("first_observed_count")).first()
                    expected = tables["charging_sessions"].agg(F.count("*"), F.sum("energy_wh"), F.countDistinct("user_id")).first()
                    checks["behaviorSessionsEnergyConserved"] = tuple(sums[:2]) == tuple(expected[:2])
                    checks["behaviorIntervalsCensoredCorrectly"] = sums[2] + sums[3] == expected[0] and sums[3] == expected[2]
                elif name == "service_hour":
                    _assert_unique(frame, ["station_id", "business_date", "local_hour"], "service_hour")
                    checks["serviceHourUnique"] = True
                    checks["serviceSessionsConserved"] = frame.agg(F.sum("session_count")).first()[0] == counts["charging_sessions"]
                    checks["serviceDurationRatioValid"] = not frame.filter(
                        (F.col("occupied_seconds_sum") < 0) | (F.col("connected_seconds_sum") < F.col("occupied_seconds_sum"))
                        | (F.col("queue_wait_seconds_sum") < 0) | (F.col("queue_wait_count") < 0)).limit(1).count()
                if checks and not all(checks.values()):
                    raise ValueError("Advanced aggregate invariant failed: " + name)
                filename = name + ".json.gz"
                output = target / filename
                with output.open("xb") as binary, gzip.GzipFile(filename="", mode="wb", fileobj=binary, mtime=0) as stream:
                    stream.write(b"[")
                    for index, row in enumerate(export_scalars(frame.orderBy(*sort_keys[name])).toLocalIterator()):
                        if index:
                            stream.write(b",\n")
                        stream.write(json.dumps(row.asDict(), ensure_ascii=False, allow_nan=False,
                                                default=_json_value, separators=(",", ":")).encode("utf-8"))
                    stream.write(b"]\n")
                plan = spark._jvm.PythonSQLUtils.explainString(frame._jdf.queryExecution(), "formatted")
                # Share the actual plan without embedding a developer's home
                # directory. Only source-root text changes, not plan operators.
                for source_root in sorted({str(input_root), str(Path(input_root).resolve())}, key=len, reverse=True):
                    plan = plan.replace(source_root, "${CLEAN_ROOT}")
                (plan_dir / (name + ".txt")).write_text(plan, encoding="utf-8")
                metadata[name] = {"file": filename, "rows": rows, "bytes": output.stat().st_size,
                    "sha256": _digest(output), "schema": [{"name": field.name, "type": field.dataType.simpleString(),
                    "nullable": field.nullable} for field in frame.schema.fields]}
            finally:
                frame.unpersist()
        dimensions = build_station_inventory(tables)
        if dimensions.count() > 1000:
            raise ValueError("Station dimension exceeds export limit")
        for relative, digest in inputs.items():
            if fs.checksum(spark, root + "/" + relative) != digest:
                raise ValueError("CLEAN source changed during advanced export")
        for name, paths in inventory.items():
            if fs.glob(spark, root + "/clean/" + name + "/*.parquet") != paths:
                raise ValueError("CLEAN Parquet inventory changed during export")
        manifest = {"schemaVersion": CONTRACT_VERSION, "analysisVersion": ANALYSIS_VERSION,
            "analysisId": "advanced-v2-" + source["publishedBatchId"],
            "datasetId": source["datasetId"], "publishedBatchId": source["publishedBatchId"],
            "pipelineRunId": source["pipelineRunId"], "sourceManifestSha256": source["sourceManifestSha256"],
            "generatedAt": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "engine": "PySpark", "sparkVersion": spark.version, "source": source["source"],
            "periodStart": source["periodStart"], "periodEnd": source["periodEndExclusive"],
            "periodEndExclusive": source["periodEndExclusive"],
            "completeMonthsThrough": complete_months_through(source["periodEndExclusive"]).isoformat(),
            "completeMonthsFrom": first_complete_month(source["periodStart"]).isoformat(),
            "stations": [row.asDict(recursive=True) for row in dimensions.orderBy("city_id", "station_id").collect()],
            "tables": metadata, "inputSha256": inputs, "sourceCounts": counts, "invariants": checks,
            "definitions": DEFINITIONS, "maxExportRows": max_rows, "rawFactsCollected": False,
            "retentionSupportsSiteType": False, "referenceAggregatesUsedAsInput": False,
            "authenticatedCleanInventoryVerified": True,
            "sparkPlans": {name: "spark_plans/" + name + ".txt" for name in TABLE_NAMES}}
        (target / "advanced_manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2,
            allow_nan=False, default=_json_value) + "\n", encoding="utf-8")
        (target / "_SUCCESS").touch(exist_ok=False)
        (target / "_RUNNING").unlink()
        return manifest
    finally:
        for frame in cached:
            frame.unpersist()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", default="data_analysis/datasets/analytics_full_180d_v1")
    parser.add_argument("--output", default="data_analysis/datasets/advanced_analytics_v2")
    parser.add_argument("--master", default="local[2]")
    parser.add_argument("--shuffle-partitions", type=int, default=8)
    parser.add_argument("--driver-memory", default="2g")
    args = parser.parse_args(argv)
    spark = create_spark("charging-advanced-analysis", args.master, args.shuffle_partitions, args.driver_memory)
    try:
        result = export_advanced_analysis(spark, args.input, args.output)
        print(json.dumps({"analysisId": result["analysisId"], "rows": {name: meta["rows"]
            for name, meta in result["tables"].items()}, "invariants": result["invariants"]}))
    finally:
        spark.stop()


if __name__ == "__main__":
    main()
