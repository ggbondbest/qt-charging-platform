"""Verify completed Spark output against independent Python control aggregates.

This is a separate, read-only comparison phase. The production batch never
reads reference_aggregates when it calculates its statistics. Only an optional
new report directory is written by this verifier.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
from functools import reduce
import json
import time

from data_analysis.charging_data.schema import SUMMARY_TABLES
from data_analysis.spark_jobs.pipeline import _filesystem, _spark_imports


def _reference_table(spark, dataset, table):
    _, _, _, T = _spark_imports()
    fields = []
    for name in SUMMARY_TABLES[table]:
        if name in {"station_id", "city_id"}:
            data_type = T.StringType()
        elif name == "recorded_at":
            data_type = T.TimestampType()
        elif name == "business_date":
            data_type = T.DateType()
        elif name == "mean_power_kw":
            data_type = T.DoubleType()
        else:
            data_type = T.LongType()
        fields.append(T.StructField(name, data_type))
    return (spark.read.schema(T.StructType(fields)).option("header", True)
            .option("enforceSchema", False).option("mode", "FAILFAST")
            .option("timestampFormat", "yyyy-MM-dd'T'HH:mm:ssXXX")
            .option("escape", '"').option("encoding", "UTF-8")
            .csv(dataset.rstrip("/") + "/reference_aggregates/" + table + "/part-*.csv.gz"))


def compare_table(spark, dataset, processed, table):
    """Check primary-key sets and every field, with exact integer comparisons."""
    _, _, F, _ = _spark_imports()
    keys = ["station_id", "recorded_at" if table == "station_hourly" else "business_date"]
    expected = _reference_table(spark, dataset, table).cache()
    actual = spark.read.parquet(processed.rstrip("/") + "/statistics/" + table).cache()
    cached_expected, cached_actual = expected, actual
    try:
        if set(actual.columns) != set(SUMMARY_TABLES[table]):
            raise ValueError("Unexpected output columns for " + table)
        counts = {}
        for name, frame in [("reference", expected), ("actual", actual)]:
            counts[name] = frame.count()
            if frame.filter(reduce(lambda a, b: a | b,
                                   [F.col(column).isNull() for column in SUMMARY_TABLES[table]])).limit(1).count():
                raise ValueError(table + " contains null statistics in " + name)
            if frame.groupBy(*keys).count().filter("count > 1").limit(1).count():
                raise ValueError(table + " contains duplicate keys in " + name)
        expected = expected.withColumn("_reference_present", F.lit(True)).alias("r")
        actual = actual.withColumn("_actual_present", F.lit(True)).alias("a")
        joined = actual.join(expected, keys, "full")
        missing = (F.col("_reference_present").isNull() | F.col("_actual_present").isNull())
        aggregates = [F.sum(F.when(missing, 1).otherwise(0)).alias("missing_keys")]
        fields = [field for field in SUMMARY_TABLES[table] if field not in keys]
        for field in fields:
            left, right = F.col("a." + field), F.col("r." + field)
            if field == "mean_power_kw":
                difference = ((F.abs(left - right) > F.lit(1e-9)) |
                              F.isnan(left) | F.isnan(right))
            else:
                difference = ~left.eqNullSafe(right)
            aggregates.append(F.sum(F.when(~missing & difference, 1).otherwise(0)).alias(field))
        differences = joined.agg(*aggregates).first().asDict()
        differences = {name: (count or 0) for name, count in differences.items()}
        if counts["reference"] != counts["actual"] or any(differences.values()):
            raise ValueError("Aggregate mismatch for " + table + ": " + json.dumps({
                "rows": counts, "different_rows_per_field": differences}, sort_keys=True))
        return {
            "rows": counts["actual"], "key_columns": keys,
            "compared_fields": fields, "different_rows_per_field": differences,
            "floating_tolerance": 1e-9 if "mean_power_kw" in fields else 0,
        }
    finally:
        cached_expected.unpersist()
        cached_actual.unpersist()


def verify_aggregates(spark, dataset, processed, report_path=None):
    started = time.monotonic()
    spark.conf.set("spark.sql.session.timeZone", "UTC")
    filesystem, marker = _filesystem(spark, processed.rstrip("/") + "/_SUCCESS")
    if not filesystem.exists(marker):
        raise ValueError("Refusing to verify an incomplete Spark batch without root _SUCCESS")
    manifests = spark.read.option("multiLine", True).json(dataset.rstrip("/") + "/manifest.json").collect()
    reports = spark.read.json(processed.rstrip("/") + "/reports/quality_report").collect()
    if len(manifests) != 1 or len(reports) != 1:
        raise ValueError("Expected exactly one input manifest and one Spark quality report")
    manifest = manifests[0].asDict(recursive=True)
    quality = reports[0].asDict(recursive=True)
    if quality.get("dataset_id") != manifest.get("dataset_id"):
        raise ValueError("Dataset ID mismatch between input and completed batch")
    if (quality.get("reference_aggregates_used_as_input") is not False or
            quality.get("manifest_row_counts_verified") is not True):
        raise ValueError("Batch provenance or manifest row validation is missing")
    comparison = {}
    for table in SUMMARY_TABLES:
        comparison[table] = compare_table(spark, dataset, processed, table)
        declared = manifest.get("reference_aggregates", {}).get(table, {}).get("rows")
        if declared != comparison[table]["rows"]:
            raise ValueError("Reference row count differs from its manifest: " + table)
    report = {
        "dataset_id": manifest["dataset_id"], "status": "PASSED",
        "verification": "all primary keys and all summary fields compared",
        "comparison": comparison,
        "reference_used_only_for_post_computation_verification": True,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "elapsed_seconds": round(time.monotonic() - started, 3),
    }
    if report_path:
        # Do not overwrite reports and never write into an input dataset.
        from data_analysis.spark_jobs.pipeline import _qualified
        reference_root = _qualified(spark, dataset)
        destination = _qualified(spark, report_path)
        if destination == reference_root or destination.startswith(reference_root + "/"):
            raise ValueError("Verification report cannot be placed inside the input dataset")
        (spark.read.json(spark.sparkContext.parallelize([json.dumps(report)]))
         .coalesce(1).write.mode("errorifexists").json(report_path))
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="Original dataset root")
    parser.add_argument("--processed", required=True, help="Completed Spark output root")
    parser.add_argument("--report", help="Optional new JSON report directory; must not exist")
    parser.add_argument("--master", default="local[2]")
    args = parser.parse_args(argv)
    SparkSession, _, _, _ = _spark_imports()
    spark = (SparkSession.builder.appName("charging-aggregate-verification").master(args.master)
             .config("spark.sql.session.timeZone", "UTC")
             .config("spark.sql.shuffle.partitions", 8).getOrCreate())
    try:
        print(json.dumps(verify_aggregates(spark, args.input, args.processed, args.report),
                         ensure_ascii=False, indent=2))
    finally:
        spark.stop()


if __name__ == "__main__":
    main()
