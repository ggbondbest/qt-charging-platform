"""Schema-aware, auditable batch processing for the synthetic charging dataset.

Run from the repository root (local paths or hdfs:// paths are supported)::

    python -m data_analysis.spark_jobs.pipeline \
        --input data_analysis/datasets/charging_sample_7d_v2 \
        --output data_analysis/outputs/charging_sample_7d_spark_v2

The output must not exist. An incomplete run has no root _SUCCESS marker and
must never be published as a successful dataset. The raw input is never changed.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import uuid
from datetime import datetime, timezone

from data_analysis.charging_data.schema import SCHEMA_VERSION, STATE_VALUES, TABLES


ENUM_FIELDS = {
    "state", "status", "outcome", "transaction_type", "stop_reason", "target_mode",
    "failure_reason", "connector_type", "period", "membership", "segment",
    "site_type", "vehicle_class", "fault_type", "severity", "anomaly_type",
}
INTEGER_FIELDS = {
    "hour", "is_weekend", "position_at_join", "interval_seconds", "online", "rating",
}
FLOAT_FIELDS = {
    "latitude", "longitude", "transformer_kw", "rated_power_kw",
    "battery_capacity_kwh", "max_charge_kw", "demand_multiplier", "temperature_c",
    "humidity_pct", "rainfall_mm", "start_soc_pct", "end_soc_pct", "target_value",
    "power_kw", "soc_pct", "pack_voltage_v", "charge_current_a",
    "max_cell_voltage_v", "min_cell_voltage_v", "max_temperature_c",
    "min_temperature_c",
}


def _spark_imports():
    try:
        from pyspark.sql import SparkSession, Window, functions as F, types as T
    except ImportError as exc:
        raise SystemExit(
            "PySpark is optional. Install data_analysis/requirements-spark.txt "
            "and Java 17 before running this batch job."
        ) from exc
    return SparkSession, Window, F, T


def configure_driver_memory(driver_memory=None):
    """Configure the launcher, not an already running JVM.

    An explicit CLI value takes precedence; otherwise respect a heap size set
    in PYSPARK_SUBMIT_ARGS and use 2g only when none was provided. Retain all
    unrelated submit options (including deployment/security options).
    """
    arguments = shlex.split(os.environ.get("PYSPARK_SUBMIT_ARGS", "pyspark-shell"))
    retained = []
    configured = None
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--driver-memory":
            if index + 1 == len(arguments):
                raise ValueError("PYSPARK_SUBMIT_ARGS --driver-memory requires a value")
            configured = arguments[index + 1]
            index += 2
            continue
        if argument.startswith("--driver-memory="):
            configured = argument.split("=", 1)[1]
            index += 1
            continue
        if argument == "--conf" and index + 1 < len(arguments):
            if arguments[index + 1].startswith("spark.driver.memory="):
                configured = arguments[index + 1].split("=", 1)[1]
                index += 2
                continue
        if argument.startswith("--conf=spark.driver.memory="):
            configured = argument.split("=", 2)[2]
            index += 1
            continue
        retained.append(argument)
        index += 1
    selected = driver_memory if driver_memory is not None else (configured or "2g")
    if not isinstance(selected, str) or not re.fullmatch(r"[1-9][0-9]*[mMgG]", selected):
        raise ValueError("driver-memory must be a positive size such as 2g, 4g or 2048m")
    selected = selected.lower()
    # Spark's options must precede its application resource.
    os.environ["PYSPARK_SUBMIT_ARGS"] = shlex.join(["--driver-memory", selected, *retained])
    return selected


def create_spark(app_name, master="local[2]", shuffle_partitions=8, driver_memory=None):
    """Start a fresh batch JVM with resource options applied before launch."""
    if shuffle_partitions < 1:
        raise ValueError("shuffle-partitions must be positive")
    SparkSession, _, _, _ = _spark_imports()
    from pyspark import SparkContext
    if SparkContext._gateway is not None:
        raise RuntimeError("create_spark must run before the first Spark JVM; restart the Python process to change its heap")
    selected = configure_driver_memory(driver_memory)
    return (SparkSession.builder.appName(app_name).master(master)
            .config("spark.driver.memory", selected)
            .config("spark.sql.session.timeZone", "UTC")
            .config("spark.sql.shuffle.partitions", shuffle_partitions).getOrCreate())


def field_type(field):
    """One explicit type contract; never infer types from a CSV sample."""
    _, _, _, T = _spark_imports()
    if field.endswith("_at"):
        return T.TimestampType()
    if field.endswith("_date"):
        return T.DateType()
    if (field.endswith("_cents") or field.endswith("_cents_per_kwh") or
            field.endswith("_wh") or field in INTEGER_FIELDS):
        return T.LongType()
    if field in FLOAT_FIELDS:
        return T.DoubleType()
    return T.StringType()


def read_table(spark, dataset_root, table):
    """Preserve parse failures for quarantine rather than silently dropping them.

    CSV is initially read against exact string headers, then explicitly cast.
    This lets the rejection dataset retain both raw text and a reason. A header
    mismatch aborts the job instead of assigning a column to the wrong field.
    """
    _, _, F, T = _spark_imports()
    fields = TABLES[table]
    schema = T.StructType([T.StructField(name, T.StringType()) for name in fields])
    schema.add("_corrupt_record", T.StringType())
    raw = (spark.read.schema(schema).option("header", True)
           .option("enforceSchema", False).option("mode", "PERMISSIVE")
           .option("columnNameOfCorruptRecord", "_corrupt_record")
           .option("encoding", "UTF-8").option("escape", '"')
           .csv(dataset_root.rstrip("/") + "/raw/" + table + "/part-*.csv.gz"))
    from data_analysis.spark_jobs.normalization import (
        BAD_TEXT_PATTERN, BOUNDARY_PATTERN, invalid_type, named_flags, normalize_value,
    )
    expressions, changed = [], []
    raw_missing, raw_blanks, raw_invalid, invalid, text_errors, normalized_fields = {}, {}, {}, {}, {}, {}
    actions = {name: F.lit(False) for name in ["N001", "N002", "N003", "N004", "N005"]}
    for field in fields:
        source = F.col(field)
        kind = field_type(field)
        normalized, field_actions = normalize_value(source, field, kind, field in ENUM_FIELDS)
        raw_missing[field] = source.isNull() | (source == "")
        raw_blanks[field] = source.isNotNull() & (source != "") & (F.regexp_replace(source, BOUNDARY_PATTERN, "") == "")
        raw_invalid[field] = invalid_type(source, kind)
        invalid[field] = invalid_type(normalized, kind)
        text_errors[field] = F.coalesce(source.rlike(BAD_TEXT_PATTERN), F.lit(False))
        normalized_fields[field] = ~source.eqNullSafe(normalized)
        for rule, condition in field_actions.items():
            actions[rule] = actions[rule] | condition
        if field in ENUM_FIELDS:
            changed.append(~source.eqNullSafe(normalized))
        expressions.append(normalized.cast(kind).alias(field))
    normalized_enum = F.lit(False)
    for flag in changed:
        normalized_enum = normalized_enum | flag
    raw_json = F.to_json(F.struct(*[F.col(name) for name in fields]), options={"ignoreNullFields": "false"})
    frame = raw.select(
        *expressions,
        normalized_enum.alias("_normalized_enum"),
        named_flags(raw_missing).alias("_raw_missing_fields"),
        named_flags(raw_blanks).alias("_raw_blank_fields"),
        named_flags(raw_invalid).alias("_raw_type_error_fields"),
        named_flags(invalid).alias("_type_error_fields"),
        named_flags(text_errors).alias("_text_error_fields"),
        named_flags(normalized_fields).alias("_normalized_fields"),
        named_flags(actions).alias("_normalization_actions"),
        F.col("_corrupt_record").alias("_corrupt_csv_record"),
        raw_json.alias("_raw_json"), F.sha2(raw_json, 256).alias("_raw_record_sha256"),
        F.input_file_name().alias("_source_file"),
    )
    return (frame.withColumn("_parse_error", F.col("_corrupt_csv_record").isNotNull() |
                             (F.size("_type_error_fields") > 0) | (F.size("_text_error_fields") > 0))
            .withColumn("_parse_error_before", F.col("_corrupt_csv_record").isNotNull() |
                        (F.size("_raw_type_error_fields") > 0) | (F.size("_text_error_fields") > 0)))


def _add_reason(frame, condition, reason):
    _, _, F, _ = _spark_imports()
    return frame.withColumn(
        "rejection_reason", F.when(F.col("rejection_reason").isNull() & condition,
                                   F.lit(reason)).otherwise(F.col("rejection_reason")))


def clean_sessions(sessions, tables):
    """Validate before deterministic deduplication, so a corrupt clone cannot win.

    All original fields plus raw JSON and one stable rejection reason are kept in
    quarantine. Identical duplicates are indistinguishable, but the retained
    canonical contents are deterministic even after a shuffle or repartition.
    """
    _, Window, F, _ = _spark_imports()
    frame = sessions.withColumn("rejection_reason", F.lit(None).cast("string"))
    if "_text_error_fields" in sessions.columns:
        frame = _add_reason(frame, F.size("_text_error_fields") > 0, "INVALID_TEXT_ENCODING")
    frame = _add_reason(frame, F.col("_parse_error"), "INVALID_TYPE_OR_CSV")
    required = ["session_id", "attempt_id", "user_id", "vehicle_id", "station_id",
                "charger_id", "started_at", "ended_at", "unplugged_at", "status"]
    for field in required:
        frame = _add_reason(frame, F.col(field).isNull(), "MISSING_" + field.upper())
    numeric = [name for name in TABLES["charging_sessions"]
               if name.endswith("_cents") or name.endswith("_wh")]
    for field in numeric:
        frame = _add_reason(frame, F.col(field).isNull() | (F.col(field) < 0),
                            "INVALID_NONNEGATIVE_" + field.upper())
    frame = _add_reason(frame,
                        (F.col("ended_at") <= F.col("started_at")) |
                        (F.col("unplugged_at") < F.col("ended_at")), "INVALID_TIME_ORDER")
    for field in ["start_soc_pct", "end_soc_pct"]:
        frame = _add_reason(frame, F.col(field).isNull() | ~F.col(field).between(0, 100),
                            "INVALID_" + field.upper())
    frame = _add_reason(frame, F.col("target_mode").isNull() | (F.col("target_mode") != "ENERGY") |
                        F.col("target_value").isNull() | (F.col("target_value") <= 0), "INVALID_ENERGY_TARGET")
    frame = _add_reason(frame, ~F.col("status").isin("COMPLETED", "WAITING_PAYMENT"),
                        "INVALID_SESSION_STATUS")
    frame = _add_reason(frame,
                        F.col("total_fee_cents") != (
                            F.col("electricity_fee_cents") + F.col("service_fee_cents") +
                            F.col("parking_fee_cents") - F.col("discount_cents")),
                        "INCONSISTENT_TOTAL_FEE")
    for table, key in [("stations", "station_id"), ("chargers", "charger_id"),
                       ("users", "user_id"), ("vehicles", "vehicle_id"),
                       ("charging_attempts", "attempt_id")]:
        lookup = tables[table].select(key).distinct().withColumn("_fk_exists", F.lit(1))
        frame = frame.join(lookup, key, "left")
        frame = _add_reason(frame, F.col("_fk_exists").isNull(), "UNKNOWN_" + key.upper())
        frame = frame.drop("_fk_exists")
    relationships = [
        ("chargers", "charger_id", ["station_id"]),
        ("vehicles", "vehicle_id", ["user_id"]),
        ("charging_attempts", "attempt_id",
         ["session_id", "user_id", "vehicle_id", "station_id", "charger_id"]),
    ]
    for table, key, related_fields in relationships:
        lookup = tables[table].select(key, *[
            F.col(field).alias("_expected_" + field) for field in related_fields])
        frame = frame.join(lookup, key, "left")
        for field in related_fields:
            frame = _add_reason(
                frame, ~F.col(field).eqNullSafe(F.col("_expected_" + field)),
                "INCONSISTENT_" + table.upper() + "_" + field.upper())
        frame = frame.drop(*["_expected_" + field for field in related_fields])
    rejected = frame.filter(F.col("rejection_reason").isNotNull())
    valid = frame.filter(F.col("rejection_reason").isNull())
    fingerprint = F.to_json(F.struct(*[
        F.col(name) for name in TABLES["charging_sessions"]]),
        options={"ignoreNullFields": "false"})
    window = Window.partitionBy("session_id").orderBy(fingerprint, F.col("_raw_json"))
    ranked = valid.withColumn("_duplicate_rank", F.row_number().over(window))
    duplicates = (ranked.filter(F.col("_duplicate_rank") > 1)
                  .drop("_duplicate_rank")
                  .withColumn("rejection_reason", F.lit("DUPLICATE_SESSION_ID")))
    clean = ranked.filter(F.col("_duplicate_rank") == 1).select(*TABLES["charging_sessions"])
    return clean, rejected.unionByName(duplicates)


def assert_telemetry_contract(telemetry, stations, chargers):
    """Fail closed on non-session corruption; never make plausible wrong charts."""
    _, _, F, _ = _spark_imports()
    stamp = F.col("recorded_at").cast("long")
    duration = F.col("interval_seconds")
    invalid = (F.col("recorded_at").isNull() | duration.isNull() | (duration <= 0) |
               F.col("energy_wh").isNull() | (F.col("energy_wh") < 0) |
               F.col("state").isNull() | ~F.col("state").isin(sorted(STATE_VALUES)) |
               F.col("power_kw").isNull() |
               (F.abs(F.col("power_kw") - F.col("energy_wh") * 3.6 / duration) > 0.00011) |
               (F.floor(stamp / 3600) != F.floor((stamp + duration - 1) / 3600)))
    if "grid_energy_wh" in telemetry.columns:
        invalid = invalid | F.col("grid_energy_wh").isNull() | (
            F.col("grid_energy_wh") < F.col("energy_wh"))
    if "grid_cost_cents" in telemetry.columns:
        invalid = invalid | F.col("grid_cost_cents").isNull() | (F.col("grid_cost_cents") < 0)
    if telemetry.filter(invalid).limit(1).count():
        raise ValueError("Telemetry violates energy/state/time contract; no statistics published")
    if telemetry.groupBy("charger_id", "recorded_at").count().filter("count > 1").limit(1).count():
        raise ValueError("Duplicate charger telemetry timestamp; refusing double-counted energy")
    if telemetry.join(stations.select("station_id"), "station_id", "left_anti").limit(1).count():
        raise ValueError("Telemetry contains an unknown station")
    if telemetry.join(chargers.select("charger_id", "station_id"),
                      ["charger_id", "station_id"], "left_anti").limit(1).count():
        raise ValueError("Telemetry charger does not belong to the referenced station")


def station_hourly(telemetry, stations, chargers):
    """Hourly mean kW = integer Wh / 1000, not the sum of sampled kW.

    end_available_count means the AVAILABLE state in the last observed sample
    per hour. It is not an hourly mean and must not be derived from power.
    A partially observed hour still divides energy by one hour; sample_count and
    capacity make incomplete coverage visible to later consumers.
    """
    _, Window, F, _ = _spark_imports()
    timed = telemetry.withColumn("_hour", F.date_trunc("hour", "recorded_at"))
    hour_window = Window.partitionBy("station_id", "_hour")
    timed = timed.withColumn("_last_at", F.max("recorded_at").over(hour_window))
    expressions = [F.sum("energy_wh").cast("long").alias("energy_wh")]
    for state in sorted(STATE_VALUES):
        expressions.append(F.sum(F.when(F.col("state") == state, 1).otherwise(0))
                           .cast("long").alias(state.lower() + "_samples"))
    expressions += [
        F.count("*").alias("sample_count"),
        F.sum(F.when((F.col("recorded_at") == F.col("_last_at")) &
                     (F.col("state") == "AVAILABLE"), 1).otherwise(0))
        .cast("long").alias("end_available_count"),
    ]
    result = timed.groupBy("station_id", "_hour").agg(*expressions)
    capacity = chargers.groupBy("station_id").agg(F.count("*").alias("capacity"))
    return (result.withColumn("mean_power_kw", F.col("energy_wh") / F.lit(1000.0))
            .withColumnRenamed("_hour", "recorded_at")
            .join(stations.select("station_id", "city_id"), "station_id")
            .join(capacity, "station_id")
            .select("station_id", "city_id", "recorded_at", "energy_wh", "mean_power_kw",
                    "available_samples", "charging_samples", "reserved_samples",
                    "occupied_samples", "maintenance_samples", "offline_samples",
                    "sample_count", "capacity", "end_available_count"))


def business_date(column):
    _, _, F, _ = _spark_imports()
    return F.to_date(F.from_utc_timestamp(F.col(column), "Asia/Shanghai"))


def station_daily(tables):
    """Accrual energy/cost and cashflow dates deliberately have separate meanings.

    Payments/refunds use successful transaction time; maintenance uses restored
    time; completed_sessions uses charging end time. Never equate cash receipts
    with order creation, nor sum electricity cost from cross-midnight sessions.
    """
    _, _, F, _ = _spark_imports()
    telemetry = tables["charger_telemetry"]
    if "grid_cost_cents" not in telemetry.columns:
        raise ValueError("Daily costs require interval-level telemetry.grid_cost_cents")
    keys = ["station_id", "business_date"]
    energy = (telemetry.withColumn("business_date", business_date("recorded_at"))
              .groupBy(*keys).agg(F.sum("energy_wh").alias("energy_wh"),
                                 F.sum("grid_cost_cents").alias("grid_cost_cents")))
    sessions = tables["charging_sessions"]
    completed = (sessions.withColumn("business_date", business_date("ended_at"))
                 .groupBy(*keys).agg(F.count("*").alias("completed_sessions")))
    payments = (tables["payments"].filter(F.col("status") == "SUCCESS")
                .join(sessions.select("session_id", "station_id"), "session_id")
                .withColumn("business_date", business_date("occurred_at"))
                .groupBy(*keys).agg(
                    F.sum(F.when(F.col("transaction_type") == "PAYMENT",
                                 F.col("amount_cents")).otherwise(0)).alias("paid_cents"),
                    F.sum(F.when(F.col("transaction_type") == "REFUND", F.col("amount_cents"))
                          .otherwise(0)).alias("refund_cents")))
    operating = (tables["operating_costs"].groupBy(*keys).agg(
        F.sum(F.col("rent_cents") + F.col("labor_cents") + F.col("network_cents"))
        .alias("operating_cost_cents")))
    maintenance = (tables["maintenance_tickets"].filter(F.col("restored_at").isNotNull())
                   .withColumn("business_date", business_date("restored_at"))
                   .groupBy(*keys).agg(F.sum(F.col("labor_cost_cents") +
                                           F.col("parts_cost_cents"))
                                      .alias("maintenance_cost_cents")))
    result = energy
    for metric in [completed, payments, operating, maintenance]:
        result = result.join(metric, keys, "full")
    return (result.join(tables["stations"].select("station_id", "city_id"), "station_id")
            .fillna(0).select("station_id", "city_id", "business_date", "energy_wh",
                             "grid_cost_cents", "completed_sessions", "paid_cents",
                             "refund_cents", "operating_cost_cents", "maintenance_cost_cents"))


def _filesystem(spark, path):
    uri = spark._jvm.org.apache.hadoop.fs.Path(path)
    return uri.getFileSystem(spark._jsc.hadoopConfiguration()), uri


def _qualified(spark, path):
    filesystem, uri = _filesystem(spark, path)
    return filesystem.makeQualified(uri).toString().rstrip("/")


def run_pipeline(spark, input_root, output_root):
    """Write a new versioned result tree. Existing output is never overwritten."""
    _, _, F, _ = _spark_imports()
    from pyspark import StorageLevel
    spark.conf.set("spark.sql.session.timeZone", "UTC")
    spark.conf.set("spark.sql.ansi.enabled", "false")
    source = _qualified(spark, input_root)
    destination = _qualified(spark, output_root)
    if source == destination or destination.startswith(source + "/") or source.startswith(destination + "/"):
        raise ValueError("Input and output must be independent, non-nested directories")
    filesystem, output_path = _filesystem(spark, output_root)
    if filesystem.exists(output_path):
        raise FileExistsError("Output already exists; use a fresh run directory: " + output_root)
    from data_analysis.spark_jobs.fs import read_json, checksum, verify_raw_files, require_raw_complete, write_json
    from data_analysis.spark_jobs.quality import (
        AUDIT_VERSION, RULE_SHA256, audit_summary, build_audit, profile_table,
    )
    from data_analysis.spark_jobs.normalization import RULES, RULE_VERSION
    manifest_path = input_root.rstrip("/") + "/manifest.json"
    manifest = read_json(spark, manifest_path)
    if not isinstance(manifest, dict):
        raise ValueError("Expected exactly one dataset manifest object")
    require_raw_complete(spark, input_root, manifest)
    source_digest = checksum(spark, manifest_path)
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("Dataset schema_version is not supported by this pipeline")
    declared_tables = manifest.get("tables")
    if not isinstance(declared_tables, dict) or set(declared_tables) != set(TABLES):
        raise ValueError("Manifest must declare exactly all required raw tables")
    for table, metadata in declared_tables.items():
        count = metadata.get("rows") if isinstance(metadata, dict) else None
        if type(count) is not int or count < 0:
            raise ValueError("Manifest contains an invalid raw row count: " + table)
    verified_files = verify_raw_files(spark, input_root, manifest)
    pipeline_run_id = "spark-" + uuid.uuid4().hex
    filesystem.mkdirs(output_path)
    marker_path = spark._jvm.org.apache.hadoop.fs.Path(output_root.rstrip("/") + "/_RUNNING")
    marker = filesystem.create(marker_path, False)
    marker.close()
    cached = []
    before_profiles = {}
    write_json(spark, output_root.rstrip("/") + "/reports/cleaning_rules.json",
               {"rule_version": RULE_VERSION, "rules_sha256": RULE_SHA256, "rules": RULES})
    try:
        raw = {}
        counts = {}
        normalizations = {}
        for table in TABLES:
            # Parsed raw rows retain a full forensic JSON copy. Telemetry and
            # battery data together contain millions of rows, so retaining all
            # 23 tables in the JVM heap can starve validation/shuffle execution.
            # Disk persistence still avoids reparsing compressed CSV on every
            # action and works with Spark's local scratch storage for HDFS input.
            frame = read_table(spark, input_root, table).persist(StorageLevel.DISK_ONLY)
            cached.append(frame)
            quality = profile_table(frame, table, before=True, manifest=manifest)
            before_profiles[table] = quality
            counts[table] = quality["row_count"]
            normalizations[table] = quality["normalized_enum_rows"]
            if counts[table] != declared_tables[table]["rows"]:
                raise ValueError(
                    "Manifest row count mismatch for " + table + ": expected " +
                    str(declared_tables[table]["rows"]) + ", actually read " + str(counts[table]))
            if table != "charging_sessions" and quality["normalized_type_invalid_rows"]:
                # Preserve offending rows for investigation, but do not publish
                # partial business tables with silently missing source facts.
                (frame.filter("_parse_error").withColumn("rejection_reason", F.when(
                    F.size("_text_error_fields") > 0, "INVALID_TEXT_ENCODING").otherwise("INVALID_TYPE_OR_CSV"))
                 .write.mode("errorifexists").parquet(output_root.rstrip("/") + "/rejected/" + table))
                raise ValueError("Unexpected invalid types outside session corruption fixture: " + table)
            raw[table] = frame
        clean = {table: frame.select(*TABLES[table]) for table, frame in raw.items()}
        cleaned_sessions, rejected = clean_sessions(raw["charging_sessions"], clean)
        cleaned_sessions = cleaned_sessions.cache()
        rejected = rejected.cache()
        cached.extend([cleaned_sessions, rejected])
        clean["charging_sessions"] = cleaned_sessions
        clean_count = cleaned_sessions.count()
        if ("canonical_session_count" in manifest and
                clean_count != manifest["canonical_session_count"]):
            raise ValueError("Clean session count does not match manifest canonical_session_count")
        assert_telemetry_contract(clean["charger_telemetry"], clean["stations"], clean["chargers"])
        reasons = {row["rejection_reason"]: row["count"]
                   for row in rejected.groupBy("rejection_reason").count().collect()}
        audit = build_audit(raw, clean, before_profiles, reasons, rejected, manifest,
                            pipeline_run_id, source_digest)
        write_json(spark, output_root.rstrip("/") + "/reports/cleaning_audit.json", audit)
        cleaning_summary = audit_summary(audit)
        cleaning_summary["audit_sha256"] = checksum(spark, output_root.rstrip("/") + "/reports/cleaning_audit.json")
        cleaning_summary["rules_file_sha256"] = checksum(spark, output_root.rstrip("/") + "/reports/cleaning_rules.json")
        hourly = station_hourly(clean["charger_telemetry"], clean["stations"], clean["chargers"])
        daily = station_daily(clean)
        for table, frame in clean.items():
            frame.write.mode("errorifexists").parquet(output_root.rstrip("/") + "/clean/" + table)
        rejected.write.mode("errorifexists").parquet(output_root.rstrip("/") + "/rejected/charging_sessions")
        hourly.write.mode("errorifexists").parquet(output_root.rstrip("/") + "/statistics/station_hourly")
        daily.write.mode("errorifexists").parquet(output_root.rstrip("/") + "/statistics/station_daily")
        report = {
            "dataset_id": manifest.get("dataset_id"), "schema_version": SCHEMA_VERSION,
            "pipeline_run_id": pipeline_run_id,
            "source_manifest_sha256": source_digest,
            "raw_file_checksums_verified": verified_files,
            "source": manifest.get("source", "SIMULATED"),
            "engine": "PySpark", "spark_version": spark.version,
            "created_at": datetime.now(timezone.utc).isoformat(),
            "business_timezone": "Asia/Shanghai", "input": source, "output": destination,
            "input_rows": counts, "normalized_enum_rows": normalizations,
            "manifest_row_counts_verified": True,
            "clean_session_rows": clean_count,
            "rejected_session_rows": sum(reasons.values()), "rejection_reasons": reasons,
            "station_hourly_rows": hourly.count(), "station_daily_rows": daily.count(),
            "reference_aggregates_used_as_input": False,
            "cleaning_audit_summary": cleaning_summary,
        }
        (spark.read.json(spark.sparkContext.parallelize([json.dumps(report)]))
         .coalesce(1).write.mode("errorifexists")
         .json(output_root.rstrip("/") + "/reports/quality_report"))
        success_path = spark._jvm.org.apache.hadoop.fs.Path(output_root.rstrip("/") + "/_SUCCESS")
        stream = filesystem.create(success_path, False)
        stream.close()
        filesystem.delete(marker_path, False)
        return report
    except Exception as exc:
        # Best effort only: a dead JVM cannot write a failure report. Never mask
        # the original failure or mark the batch successful in that situation.
        try:
            write_json(spark, output_root.rstrip("/") + "/reports/cleaning_failure.json", {
                "audit_version": AUDIT_VERSION, "rule_version": RULE_VERSION, "status": "FAILED",
                "dataset_id": manifest.get("dataset_id"), "pipeline_run_id": pipeline_run_id,
                "source_manifest_sha256": source_digest, "source_layer_modified": False,
                "error_type": type(exc).__name__, "message": str(exc)[:1000],
                "completed_before_profiles": before_profiles,
                "note": "Partial audit only; no completed batch or post-cleaning success assessment is claimed.",
            })
        except Exception:
            pass
        raise
    finally:
        for frame in cached:
            frame.unpersist()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="Dataset root containing raw/ and manifest.json")
    parser.add_argument("--output", required=True, help="New independent output directory; never overwritten")
    parser.add_argument("--master", default="local[2]", help="Spark master; default local[2]")
    parser.add_argument("--shuffle-partitions", type=int, default=8)
    parser.add_argument("--driver-memory", help="JVM heap before startup, e.g. 2g or 4g; default: submit environment or 2g")
    args = parser.parse_args(argv)
    if args.shuffle_partitions < 1:
        parser.error("--shuffle-partitions must be positive")
    try:
        spark = create_spark("charging-analysis-batch", args.master, args.shuffle_partitions, args.driver_memory)
    except ValueError as exc:
        parser.error(str(exc))
    try:
        report = run_pipeline(spark, args.input, args.output)
        print(json.dumps(report, ensure_ascii=False, indent=2))
    finally:
        spark.stop()


if __name__ == "__main__":
    main()
