"""Independent Spark acceptance analysis: 11 grouping dimensions, 3 comparisons.

This does not replace the serving/API contract. Facts and dimension snapshots
come from the same completed clean Parquet batch, not Python reference totals.
No raw fact table is collected to the driver; only <=12 preview rows per result.
"""

import argparse
from datetime import date, datetime, timezone
import json

from data_analysis.charging_data.schema import SCHEMA_VERSION
from data_analysis.spark_jobs import fs
from data_analysis.spark_jobs.pipeline import _filesystem, _qualified, _spark_imports, business_date, create_spark

ANALYSIS_VERSION = "1.0.0"
PREVIEW_LIMIT = 12
DIMENSION_TABLES = {"cities": "city_id", "stations": "station_id", "users": "user_id",
                    "vehicles": "vehicle_id", "chargers": "charger_id"}
INPUT_TABLES = tuple(DIMENSION_TABLES) + ("charging_sessions", "payments", "maintenance_tickets")

# A city ID and its display name are ONE semantic dimension, not two.
DIMENSIONS = {
    "city": ["city_id", "city_name"], "station": ["station_id", "station_name"],
    "business_date": ["business_date"], "start_hour": ["start_hour"],
    "site_type": ["site_type"], "user_segment": ["user_segment"],
    "vehicle_class": ["vehicle_class"], "connector_type": ["connector_type"],
    "day_type": ["day_type"], "payment_channel": ["payment_channel"], "fault_type": ["fault_type"],
}
ANALYSES = {
    **{"sessions_by_"+dimension: {"fact": "sessions", "dimensions": [dimension]}
       for dimension in ("city", "station", "business_date", "start_hour", "site_type", "user_segment",
                         "vehicle_class", "connector_type", "day_type")},
    "payments_by_channel": {"fact": "payments", "dimensions": ["payment_channel"]},
    "payments_by_business_date": {"fact": "payments", "dimensions": ["business_date"]},
    "repairs_by_fault_type": {"fact": "repairs", "dimensions": ["fault_type"]},
    "comparison_city_hour": {"fact": "sessions", "dimensions": ["city", "start_hour"]},
    "comparison_site_day_type": {"fact": "sessions", "dimensions": ["site_type", "day_type"]},
    "comparison_city_payment_channel": {"fact": "payments", "dimensions": ["city", "payment_channel"]},
}
TIME_SEMANTICS = {
    "sessions": "Shanghai started_at date/hour cohort: whole-session energy, bill and duration are attributed to its START, not interval-day energy or payment receipts",
    "payments": "Shanghai occurred_at transaction date: only successful PAYMENT/REFUND amounts enter cash receipts/refunds; failed requests are counted separately",
    "repairs": "reported_at cohort with final outcomes observed in the completed batch; repair duration/cost include only valid restored tickets, not historical as-of status",
    "day_type": "MON_FRI versus SAT_SUN using literal weekdays; public holidays and adjusted working days are NOT reclassified",
}
METRICS = {
    "session_count": ("sessions", "number of clean sessions in this group"),
    "distinct_users": ("users", "COUNT DISTINCT user_id within this group; do not sum across overlapping groups"),
    "energy_wh": ("Wh", "sum of whole-session delivered energy"),
    "grid_energy_wh": ("Wh", "sum of whole-session grid energy"),
    "billed_cents": ("CNY cents", "sum of session total_fee_cents; bills are NOT cash receipts"),
    "active_duration_count": ("sessions", "sessions with nonnegative ended_at-started_at"),
    "active_seconds_sum": ("seconds", "sum over active_duration_count"),
    "connected_duration_count": ("sessions", "sessions with nonnegative unplugged_at-started_at"),
    "connected_seconds_sum": ("seconds", "sum over connected_duration_count; connection includes occupied parking"),
    "mean_energy_wh": ("Wh/session", "energy_wh / session_count"),
    "mean_active_seconds": ("seconds/session", "active_seconds_sum / active_duration_count; empty denominator is null"),
    "mean_connected_seconds": ("seconds/session", "connected_seconds_sum / connected_duration_count; empty denominator is null"),
    "transaction_count": ("records", "all payment/refund records, including invalid/failed records"),
    "payment_request_count": ("requests", "transaction_type=PAYMENT; refund requests excluded"),
    "successful_payment_count": ("requests", "successful PAYMENT with valid amount and timestamp"),
    "failed_payment_count": ("requests", "PAYMENT status=FAILED; not unique failed orders"),
    "successful_refund_count": ("requests", "successful REFUND with valid amount and timestamp"),
    "invalid_transaction_count": ("records", "unknown kind/status, missing/negative amount or missing occurred_at"),
    "paid_cents": ("CNY cents", "sum of valid successful PAYMENT amounts by transaction time"),
    "refund_cents": ("CNY cents", "sum of valid successful REFUND amounts by transaction time"),
    "net_paid_cents": ("CNY cents", "paid_cents-refund_cents; may be negative, not profit"),
    "payment_success_rate": ("fraction 0..1", "successful_payment_count / payment_request_count; null if no payment requests"),
    "reported_count": ("tickets", "all tickets in this reported-at cohort"),
    "restored_count": ("tickets", "non-null restored_at >= reported_at"),
    "unresolved_count": ("tickets", "restored_at is null at batch completion"),
    "invalid_timing_count": ("tickets", "missing reported_at or restored_at earlier than reported_at"),
    "resolution_seconds_sum": ("seconds", "sum reported_at→restored_at over restored_count"),
    "repair_cost_cents": ("CNY cents", "labor+parts of restored tickets in reported cohort; not a daily cashflow series"),
    "mean_resolution_seconds": ("seconds/ticket", "resolution_seconds_sum / restored_count; null if no restored tickets"),
}


def group_columns(spec):
    return [column for dimension in spec["dimensions"] for column in DIMENSIONS[dimension]]


def prepare_facts(tables):
    _, _, F, _ = _spark_imports()
    stations = tables["stations"].select("station_id", "city_id", "station_name", "site_type")
    cities = tables["cities"].select("city_id", "city_name")
    sessions = (tables["charging_sessions"].join(stations, "station_id", "left")
        .join(cities, "city_id", "left")
        .join(tables["users"].select("user_id", F.col("segment").alias("user_segment")), "user_id", "left")
        .join(tables["vehicles"].select("vehicle_id", "vehicle_class"), "vehicle_id", "left")
        .join(tables["chargers"].select("charger_id", "connector_type"), "charger_id", "left")
        .withColumn("business_date", business_date("started_at"))
        .withColumn("start_hour", F.hour(F.from_utc_timestamp("started_at", "Asia/Shanghai")))
        .withColumn("day_type", F.when(F.dayofweek("business_date").isin(1, 7), "SAT_SUN").otherwise("MON_FRI"))
        .withColumn("active_seconds", F.col("ended_at").cast("long")-F.col("started_at").cast("long"))
        .withColumn("connected_seconds", F.col("unplugged_at").cast("long")-F.col("started_at").cast("long")))
    session_locations = tables["charging_sessions"].select("session_id", "station_id").join(stations, "station_id", "left")
    payments = (tables["payments"].join(session_locations.select("session_id", "city_id"), "session_id", "left")
        .join(cities, "city_id", "left").withColumnRenamed("channel", "payment_channel")
        .withColumn("business_date", business_date("occurred_at")))
    repairs = tables["maintenance_tickets"].withColumn("resolution_seconds",
        F.col("restored_at").cast("long")-F.col("reported_at").cast("long"))
    # UNKNOWN is visible, not silently dropped. Unique dimension IDs are checked
    # before export; empty categories or unavailable lookup labels remain explicit.
    return {"sessions": sessions.fillna("UNKNOWN", subset=["city_id", "city_name", "station_name", "site_type",
                "user_segment", "vehicle_class", "connector_type"]),
            "payments": payments.fillna("UNKNOWN", subset=["city_id", "city_name", "payment_channel"]),
            "repairs": repairs.fillna("UNKNOWN", subset=["fault_type"])}


def aggregate(fact, family, keys):
    _, _, F, _ = _spark_imports()
    def count_if(condition):
        return F.sum(F.when(condition, 1).otherwise(0)).cast("long")
    def sum_if(condition, column):
        return F.sum(F.when(condition, F.col(column)).otherwise(0)).cast("long")
    if family == "sessions":
        result = fact.groupBy(*keys).agg(F.count("*").alias("session_count"),
            F.countDistinct("user_id").alias("distinct_users"), F.sum("energy_wh").alias("energy_wh"),
            F.sum("grid_energy_wh").alias("grid_energy_wh"), F.sum("total_fee_cents").alias("billed_cents"),
            count_if(F.col("active_seconds") >= 0).alias("active_duration_count"),
            sum_if(F.col("active_seconds") >= 0, "active_seconds").alias("active_seconds_sum"),
            count_if(F.col("connected_seconds") >= 0).alias("connected_duration_count"),
            sum_if(F.col("connected_seconds") >= 0, "connected_seconds").alias("connected_seconds_sum"))
        ratios = {"mean_energy_wh": ("energy_wh", "session_count"),
                  "mean_active_seconds": ("active_seconds_sum", "active_duration_count"),
                  "mean_connected_seconds": ("connected_seconds_sum", "connected_duration_count")}
    elif family == "payments":
        valid = (F.col("amount_cents").isNotNull() & (F.col("amount_cents") >= 0) & F.col("occurred_at").isNotNull()
                 & F.col("transaction_type").isin("PAYMENT", "REFUND") & F.col("status").isin("SUCCESS", "FAILED"))
        paid = valid & (F.col("transaction_type") == "PAYMENT") & (F.col("status") == "SUCCESS")
        refunded = valid & (F.col("transaction_type") == "REFUND") & (F.col("status") == "SUCCESS")
        result = fact.groupBy(*keys).agg(F.count("*").alias("transaction_count"),
            count_if(F.col("transaction_type") == "PAYMENT").alias("payment_request_count"),
            count_if(paid).alias("successful_payment_count"), count_if(refunded).alias("successful_refund_count"),
            count_if((F.col("transaction_type") == "PAYMENT") & (F.col("status") == "FAILED")).alias("failed_payment_count"),
            count_if(~F.coalesce(valid, F.lit(False))).alias("invalid_transaction_count"),
            sum_if(paid, "amount_cents").alias("paid_cents"), sum_if(refunded, "amount_cents").alias("refund_cents"))
        result = result.withColumn("net_paid_cents", F.col("paid_cents")-F.col("refund_cents"))
        ratios = {"payment_success_rate": ("successful_payment_count", "payment_request_count")}
    elif family == "repairs":
        restored = F.col("reported_at").isNotNull() & (F.col("resolution_seconds") >= 0)
        result = fact.groupBy(*keys).agg(F.count("*").alias("reported_count"),
            count_if(restored).alias("restored_count"), count_if(F.col("restored_at").isNull()).alias("unresolved_count"),
            count_if(F.col("reported_at").isNull() | (F.col("resolution_seconds") < 0)).alias("invalid_timing_count"),
            sum_if(restored, "resolution_seconds").alias("resolution_seconds_sum"),
            F.sum(F.when(restored, F.coalesce("labor_cost_cents", F.lit(0))+F.coalesce("parts_cost_cents", F.lit(0)))
                .otherwise(0)).cast("long").alias("repair_cost_cents"))
        ratios = {"mean_resolution_seconds": ("resolution_seconds_sum", "restored_count")}
    else:
        raise ValueError("Unknown acceptance fact family")
    for name, (numerator, denominator) in ratios.items():
        result = result.withColumn(name, F.when(F.col(denominator) > 0, F.col(numerator)/F.col(denominator)))
    return result


def build_analysis_frames(tables):
    facts = prepare_facts(tables)
    return {name: aggregate(facts[spec["fact"]], spec["fact"], group_columns(spec)) for name, spec in ANALYSES.items()}


def export_acceptance_analysis(spark, input_root, processed_root, output_root):
    """New-only local/HDFS export, independently traceable to the clean batch."""
    from pyspark import StorageLevel
    _, _, F, _ = _spark_imports()
    spark.conf.set("spark.sql.session.timeZone", "UTC")
    fs.independent_paths(spark, input_root, processed_root, output_root)
    if fs.exists(spark, output_root):
        raise FileExistsError("Acceptance output exists; choose a new directory")
    fs.require_success(spark, processed_root)
    raw_manifest_path = input_root.rstrip("/")+"/manifest.json"
    raw = fs.read_json(spark, raw_manifest_path)
    fs.require_raw_complete(spark, input_root, raw)
    raw_hash = fs.checksum(spark, raw_manifest_path)
    reports = fs.glob(spark, processed_root.rstrip("/")+"/reports/quality_report/part-*.json")
    if len(reports) != 1:
        raise ValueError("Expected one completed clean quality report")
    quality = fs.read_json(spark, reports[0])
    if (raw.get("schema_version") != SCHEMA_VERSION or quality.get("dataset_id") != raw.get("dataset_id")
            or quality.get("source_manifest_sha256") != raw_hash or not quality.get("pipeline_run_id")
            or quality.get("input") != _qualified(spark, input_root)
            or quality.get("output") != _qualified(spark, processed_root)
            or quality.get("raw_file_checksums_verified") is not True
            or quality.get("manifest_row_counts_verified") is not True
            or quality.get("reference_aggregates_used_as_input") is not False):
        raise ValueError("Raw/clean batch provenance mismatch")
    if not fs.verify_raw_files(spark, input_root, raw):
        raise ValueError("Versioned raw checksums are required")
    tables = {table: spark.read.parquet(processed_root.rstrip("/")+"/clean/"+table) for table in INPUT_TABLES}
    for table, key in DIMENSION_TABLES.items():
        if (tables[table].filter(F.col(key).isNull()).limit(1).count()
                or tables[table].groupBy(key).count().filter("count > 1").limit(1).count()):
            raise ValueError("Missing/duplicate dimension key: "+table)
    if tables["charging_sessions"].count() != quality["clean_session_rows"]:
        raise ValueError("Clean session count differs from quality report")
    root = output_root.rstrip("/")
    fs.write_text(spark, root+"/_RUNNING", "")
    cached = []
    try:
        facts = prepare_facts(tables)
        for name, frame in facts.items():
            facts[name] = frame.persist(StorageLevel.DISK_ONLY)
            cached.append(facts[name])
        manifests, previews = {}, {}
        for name, spec in ANALYSES.items():
            keys = group_columns(spec)
            frame = aggregate(facts[spec["fact"]], spec["fact"], keys).persist(StorageLevel.DISK_ONLY)
            try:
                rows = frame.count()
                ordered = frame.orderBy(*keys)
                (ordered.coalesce(1 if rows < 25000 else 4).write.mode("errorifexists")
                    .option("header", True).option("encoding", "UTF-8").option("compression", "gzip")
                    .option("nullValue", "").option("escape", '"').option("dateFormat", "yyyy-MM-dd")
                    .csv(root+"/csv/"+name))
                files = fs.glob(spark, root+"/csv/"+name+"/part-*.csv.gz")
                # Limit BEFORE collecting; Spark's default toJSON drops null
                # fields, which would obscure explicitly empty denominators.
                previews[name] = [{key: value.isoformat() if isinstance(value, (date, datetime)) else value
                                   for key, value in row.asDict().items()}
                                  for row in ordered.limit(PREVIEW_LIMIT).collect()]
                manifests[name] = dict(spec, groupColumns=keys, rows=rows, timeSemantics=TIME_SEMANTICS[spec["fact"]],
                    schema=[dict(name=field.name, sparkType=field.dataType.simpleString(), nullable=field.nullable,
                                 **({"unit": METRICS[field.name][0], "definition": METRICS[field.name][1]} if field.name in METRICS else {}))
                            for field in frame.schema.fields],
                    files=[dict(path="csv/"+name+"/"+path.rsplit("/",1)[-1], bytes=fs.file_size(spark,path),
                                sha256=fs.checksum(spark,path)) for path in files])
            finally:
                frame.unpersist()
        manifest = dict(analysisVersion=ANALYSIS_VERSION, source=raw.get("source", "SIMULATED"),
            datasetId=raw["dataset_id"], pipelineRunId=quality["pipeline_run_id"], sourceManifestSha256=raw_hash,
            generatedAt=datetime.now(timezone.utc).isoformat().replace("+00:00","Z"), businessTimezone="Asia/Shanghai",
            semanticDimensionCount=len(DIMENSIONS), comparisonCount=sum(len(spec["dimensions"]) >= 2 for spec in ANALYSES.values()),
            dimensionDefinitions=DIMENSIONS, timeSemantics=TIME_SEMANTICS, analyses=manifests,
            engine="PySpark", sparkVersion=spark.version, previewLimit=PREVIEW_LIMIT,
            referenceAggregatesUsedAsInput=False,
            note="SIMULATED acceptance statistics; session-cohort bills are not cash revenue; no training or causal claims")
        fs.write_json(spark, root+"/preview.json", previews)
        fs.write_json(spark, root+"/analysis_manifest.json", manifest)
        fs.write_text(spark, root+"/_SUCCESS", "")
        filesystem, marker = _filesystem(spark, root+"/_RUNNING")
        filesystem.delete(marker, False)
        return manifest
    finally:
        for frame in cached:
            frame.unpersist()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True)
    parser.add_argument("--processed", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--master", default="local[2]")
    parser.add_argument("--shuffle-partitions", type=int, default=8)
    parser.add_argument("--driver-memory", default=None)
    args = parser.parse_args(argv)
    spark = create_spark("charging-acceptance-analysis", args.master, args.shuffle_partitions, args.driver_memory)
    try:
        result = export_acceptance_analysis(spark,args.input,args.processed,args.output)
        print(json.dumps({"datasetId":result["datasetId"],"semanticDimensions":result["semanticDimensionCount"],
                          "comparisons":result["comparisonCount"],"analyses":len(result["analyses"])}))
    finally:
        spark.stop()


if __name__ == "__main__":
    main()
