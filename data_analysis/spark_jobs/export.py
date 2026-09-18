"""Compute dashboard/ML tables and export a complete versioned data handoff.

Input raw and processed roots may be local paths or hdfs:// URIs. Output is
new-only. CSV.gz is portable for Vue/API/ML consumers; Parquet remains the
distributed computation artifact. Only the small metadata/report is collected.
"""

import argparse
import json
import uuid
from datetime import datetime, timezone

from data_analysis.charging_data.schema import TABLES
from data_analysis.contracts import CONTRACT_VERSION, FEATURE_VERSION
from data_analysis.contracts.serving import TABLE_KEYS
from data_analysis.spark_jobs import fs
from data_analysis.spark_jobs.dashboard_aggregates import build_dashboard_frames
from data_analysis.spark_jobs.ml_features import build_ml_frames, split_dates
from data_analysis.spark_jobs.pipeline import _filesystem, _spark_imports, _qualified


def _unit(name):
    if name.endswith("_cents"):
        return "CNY_cents"
    if name.endswith("_wh"):
        return "Wh"
    if "_kw" in name or name.endswith("_kw"):
        return "kW"
    if "seconds" in name:
        return "seconds"
    if name == "charging_utilization":
        return "ratio_0_1"
    return None


def describe_frame(name, frame):
    types = {"string": "string", "date": "date", "timestamp": "timestamp", "long": "integer",
             "integer": "integer", "double": "number", "float": "number", "boolean": "boolean"}
    columns = []
    for field in frame.schema.fields:
        kind = types.get(field.dataType.typeName())
        if not kind:
            raise ValueError("Unsupported export type: " + field.name)
        item = {"name": field.name, "type": kind,
                "nullable": field.nullable and field.name not in TABLE_KEYS[name]}
        if _unit(field.name):
            item["unit"] = _unit(field.name)
        columns.append(item)
    return {"columns": columns, "primaryKey": TABLE_KEYS[name]}


def export_data(spark, input_root, processed_root, output_root):
    _, _, F, _ = _spark_imports()
    spark.conf.set("spark.sql.session.timeZone", "UTC")
    fs.independent_paths(spark, input_root, processed_root, output_root)
    if fs.exists(spark, output_root):
        raise FileExistsError("Output exists; choose a new batch directory")
    fs.require_success(spark, processed_root)
    raw_manifest = fs.read_json(spark, input_root.rstrip("/") + "/manifest.json")
    fs.require_raw_complete(spark, input_root, raw_manifest)
    source_digest = fs.checksum(spark, input_root.rstrip("/") + "/manifest.json")
    report_files = fs.glob(spark, processed_root.rstrip("/") + "/reports/quality_report/part-*.json")
    if len(report_files) != 1:
        raise ValueError("Exactly one successful quality report is required")
    quality = fs.read_json(spark, report_files[0])
    if (quality.get("dataset_id") != raw_manifest.get("dataset_id") or
            quality.get("source_manifest_sha256") != source_digest or
            quality.get("raw_file_checksums_verified") is not True or
            quality.get("manifest_row_counts_verified") is not True or
            not quality.get("pipeline_run_id") or
            quality.get("reference_aggregates_used_as_input") is not False or
            quality.get("input") != _qualified(spark, input_root)):
        raise ValueError("Processed batch provenance mismatch; rerun current pipeline on this raw dataset")
    if not fs.verify_raw_files(spark, input_root, raw_manifest):
        raise ValueError("Export requires a complete versioned manifest with shard checksums")
    root = output_root.rstrip("/")
    fs.write_text(spark, root + "/_RUNNING", "")
    cached = []
    try:
        tables = {table: spark.read.parquet(processed_root.rstrip("/") + "/clean/" + table) for table in TABLES}
        hourly = spark.read.parquet(processed_root.rstrip("/") + "/statistics/station_hourly")
        daily = spark.read.parquet(processed_root.rstrip("/") + "/statistics/station_daily")
        if hourly.count() != quality["station_hourly_rows"] or daily.count() != quality["station_daily_rows"]:
            raise ValueError("Processed statistics row count differs from completed quality report")
        cadence = raw_manifest.get("config", {}).get("interval_minutes", 5) * 60
        frames = build_dashboard_frames(tables, hourly, daily, sample_interval_seconds=cadence)
        frames["cities"] = tables["cities"]
        start_date = raw_manifest["config"]["start_date"]
        from datetime import date, timedelta
        end_date = (date.fromisoformat(start_date) + timedelta(days=raw_manifest["config"]["days"])).isoformat()
        splits = split_dates(start_date, end_date)
        frames["station_hourly_metrics"] = frames["station_hourly_metrics"].cache()
        cached.append(frames["station_hourly_metrics"])
        frames.update(build_ml_frames(frames["station_hourly_metrics"], tables, splits))
        exports = {}
        for name, frame in frames.items():
            frame = frame.cache()
            cached.append(frame)
            rows = frame.count()
            if frame.groupBy(*TABLE_KEYS[name]).count().filter("count > 1").limit(1).count():
                raise ValueError("Duplicate export primary key: " + name)
            for key in TABLE_KEYS[name]:
                if frame.filter(F.col(key).isNull()).limit(1).count():
                    raise ValueError("Missing export primary key: " + name)
            metadata = describe_frame(name, frame)
            ordered = frame.orderBy(*TABLE_KEYS[name])
            ordered.write.mode("errorifexists").parquet(root + "/parquet/" + name)
            (ordered.coalesce(1 if rows < 25000 else 4).write.mode("errorifexists")
             .option("header", True).option("encoding", "UTF-8").option("compression", "gzip")
             .option("nullValue", "").option("emptyValue", "").option("escape", '"')
             .option("timestampFormat", "yyyy-MM-dd'T'HH:mm:ss'Z'").option("dateFormat", "yyyy-MM-dd")
             .csv(root + "/csv/" + name))
            shards = fs.glob(spark, root + "/csv/" + name + "/part-*.csv.gz")
            if not shards:
                raise ValueError("No CSV schema shard produced: " + name)
            metadata.update(rows=rows, parquetPath="parquet/" + name,
                            files=[{"path": "csv/" + name + "/" + path.rsplit("/", 1)[-1],
                                    "bytes": fs.file_size(spark, path),
                                    "sha256": fs.checksum(spark, path)} for path in shards])
            exports[name] = metadata
        sanitized_quality = {key: value for key, value in quality.items() if key not in {"input", "output"}}
        rejected = spark.read.parquet(processed_root.rstrip("/") + "/rejected/charging_sessions")
        samples = rejected.select("session_id", "rejection_reason").orderBy("rejection_reason", "session_id").limit(12)
        sanitized_quality["rejection_samples"] = [row.asDict() for row in samples.collect()]
        fs.write_json(spark, root + "/quality_report.json", sanitized_quality)
        manifest = {
            "schemaVersion": CONTRACT_VERSION, "featureVersion": FEATURE_VERSION,
            "datasetId": raw_manifest["dataset_id"], "source": raw_manifest.get("source", "SIMULATED"),
            "publishedBatchId": "analytics-" + uuid.uuid4().hex,
            "pipelineRunId": quality["pipeline_run_id"], "sourceManifestSha256": source_digest,
            "generatedAt": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "startDate": start_date, "endDate": end_date, "businessTimezone": "Asia/Shanghai",
            "periodStart": raw_manifest["period_start"], "periodEndExclusive": raw_manifest["period_end_exclusive"],
            "tables": exports, "mlSplits": splits, "mlHistoryHours": 24,
            "definitions": {"money": "integer CNY cents", "energy": "integer Wh",
                            "dates": "Asia/Shanghai, start inclusive and end exclusive",
                            "timestamps": "UTC ISO8601 Z; hourly records label interval START",
                            "chargingUtilization": "CHARGING samples / all samples of complete hours; includes maintenance in denominator",
                            "netPaid": "successful payment receipts minus refunds on transaction business date",
                            "completedSessions": "charging ended; includes unpaid finished sessions",
                            "cohorts": "final outcomes in this batch grouped by creation/join date; not historical as-of status",
                            "snapshot": "latest state in batch, separate from date-filtered period metrics",
                            "mlFeatures": "known information from 24 complete hours before reference_time; no future weather or labels",
                            "mlTargets": "separate future labels; NEVER import into serving/API database",
                            "simulation": "business records and illustrative locations are simulated; weather provenance follows raw manifest"},
            "qualityReportPath": "quality_report.json",
            "qualityReportSha256": fs.checksum(spark, root + "/quality_report.json"),
        }
        fs.write_json(spark, root + "/serving_manifest.json", manifest)
        fs.write_json(spark, root + "/table_schemas.json", {"schemaVersion": CONTRACT_VERSION, "tables": {
            name: {key: meta[key] for key in ("columns", "primaryKey")} for name, meta in exports.items()}})
        fs.write_text(spark, root + "/_SUCCESS", "")
        filesystem, marker = _filesystem(spark, root + "/_RUNNING")
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
    args = parser.parse_args(argv)
    if args.shuffle_partitions < 1:
        parser.error("shuffle-partitions must be positive")
    SparkSession, _, _, _ = _spark_imports()
    spark = (SparkSession.builder.appName("charging-data-handoff").master(args.master)
             .config("spark.sql.session.timeZone", "UTC")
             .config("spark.sql.shuffle.partitions", args.shuffle_partitions).getOrCreate())
    try:
        manifest = export_data(spark, args.input, args.processed, args.output)
        print(json.dumps({"datasetId": manifest["datasetId"], "publishedBatchId": manifest["publishedBatchId"],
                          "rows": {name: meta["rows"] for name, meta in manifest["tables"].items()}}, indent=2))
    finally:
        spark.stop()


if __name__ == "__main__":
    main()
