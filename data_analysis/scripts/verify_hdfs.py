"""Read-only acceptance checks against actual HDFS, never a local substitute.

Requires uploaded raw data and a completed Spark pipeline on that HDFS input.
It does not install Hadoop, format NameNode, upload, overwrite or delete data.
"""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
from urllib.parse import urlsplit


def require_hdfs_uri(value):
    parsed = urlsplit(value)
    if (parsed.scheme != "hdfs" or not parsed.netloc or parsed.username is not None or
            parsed.password is not None or parsed.query or parsed.fragment or
            not parsed.path.startswith("/") or parsed.path in ("", "/") or
            any(part in (".", "..") for part in parsed.path.split("/")) or
            any(char in value for char in ("\\", "%", "\n", "\r", "\t"))):
        raise ValueError("Use an explicit hdfs://namenode[:port]/dataset URI, not a local path or filesystem root")
    return value.rstrip("/")


def validate_export_metadata(serving, schemas, export_quality, expected_quality):
    """Pure contract preflight; actual HDFS existence and hashes are checked next.

    Empty *rows* are valid, but an empty table/file inventory is not a completed
    portable export. Keep future-label tables in this integrity check even
    though the publisher deliberately excludes them from the query database.
    """
    from data_analysis.contracts import CONTRACT_VERSION, FEATURE_VERSION
    from data_analysis.contracts.serving import SERVING_TABLES, TABLE_KEYS, validate_table_contract
    from data_analysis.spark_jobs.fs import safe_relative

    if (not isinstance(serving, dict) or serving.get("schemaVersion") != CONTRACT_VERSION or
            serving.get("featureVersion") != FEATURE_VERSION):
        raise ValueError("Unsupported HDFS export schema/feature version")
    if serving.get("source") != "SIMULATED" or serving.get("businessTimezone") != "Asia/Shanghai":
        raise ValueError("Unsupported HDFS export source or business timezone")
    if not isinstance(expected_quality, dict) or not isinstance(export_quality, dict):
        raise ValueError("HDFS export requires a cleaning quality report")
    for serving_key, quality_key in (("datasetId", "dataset_id"), ("pipelineRunId", "pipeline_run_id"),
                                     ("sourceManifestSha256", "source_manifest_sha256")):
        value = serving.get(serving_key)
        if not isinstance(value, str) or not value or value != expected_quality.get(quality_key):
            raise ValueError("HDFS export belongs to another batch")
    if not re.fullmatch(r"[0-9a-f]{64}", serving["sourceManifestSha256"]):
        raise ValueError("Invalid HDFS export source hash")
    # export_data deliberately removes machine-specific paths and appends a
    # bounded rejection preview. Compare the actual exported representation,
    # not the unsanitized processed report, without admitting arbitrary extras.
    expected_export = {key: value for key, value in expected_quality.items() if key not in {"input", "output"}}
    actual_export = {key: value for key, value in export_quality.items() if key != "rejection_samples"}
    if "rejection_samples" in export_quality:
        samples = export_quality["rejection_samples"]
        if (not isinstance(samples, list) or len(samples) > 12 or any(
                not isinstance(sample, dict) or set(sample) != {"session_id", "rejection_reason"} or
                sample["session_id"] is not None and not isinstance(sample["session_id"], str) or
                not isinstance(sample["rejection_reason"], str) or not sample["rejection_reason"]
                for sample in samples)):
            raise ValueError("Invalid HDFS export quality report rejection samples")
    # JSON comparison also distinguishes booleans from integers (True != 1
    # in the public schema), unlike Python's ordinary dictionary equality.
    quality_matches = (json.dumps(actual_export, sort_keys=True, allow_nan=False) ==
                       json.dumps(expected_export, sort_keys=True, allow_nan=False))
    if (not isinstance(serving.get("publishedBatchId"), str) or not serving["publishedBatchId"] or
            not quality_matches or
            export_quality.get("raw_file_checksums_verified") is not True or
            export_quality.get("manifest_row_counts_verified") is not True or
            export_quality.get("reference_aggregates_used_as_input") is not False):
        raise ValueError("HDFS export quality report differs from its completed cleaning batch")
    if (serving.get("qualityReportPath") != "quality_report.json" or
            not isinstance(serving.get("qualityReportSha256"), str) or
            not re.fullmatch(r"[0-9a-f]{64}", serving["qualityReportSha256"])):
        raise ValueError("Invalid HDFS export quality report path/hash")
    tables = serving.get("tables")
    if not isinstance(tables, dict) or set(tables) != set(TABLE_KEYS):
        raise ValueError("HDFS export must contain exactly the contracted ten tables")
    declared = {}
    for name, metadata in tables.items():
        if (not isinstance(metadata, dict) or not isinstance(metadata.get("columns"), list) or
                not all(isinstance(column, dict) for column in metadata["columns"])):
            raise ValueError("Invalid HDFS export table metadata: " + name)
        validate_table_contract(name, metadata)
        names = {column["name"] for column in metadata["columns"]}
        if name in SERVING_TABLES and name != "cities" and "city_id" not in names:
            raise ValueError("HDFS serving table lacks city ownership: " + name)
        for column in metadata["columns"]:
            if column["name"] in TABLE_KEYS[name] and column["nullable"]:
                raise ValueError("HDFS export primary keys must be non-nullable")
            if name in SERVING_TABLES and column["name"].startswith(("label_", "target_", "split_")):
                raise ValueError("Future labels must not enter a serving table")
        if not isinstance(metadata.get("files"), list) or not metadata["files"]:
            raise ValueError("Every HDFS export table requires a CSV schema shard")
        for shard in metadata["files"]:
            if not isinstance(shard, dict):
                raise ValueError("Invalid HDFS export shard metadata")
            relative = safe_relative(shard.get("path"))
            if not re.fullmatch(r"csv/" + re.escape(name) + r"/part-[^/]+\.csv\.gz", relative):
                raise ValueError("HDFS export CSV shard does not belong to its table")
            if relative in declared:
                raise ValueError("HDFS export CSV shard is declared more than once")
            if (type(shard.get("bytes")) is not int or shard["bytes"] <= 0 or
                    not isinstance(shard.get("sha256"), str) or
                    not re.fullmatch(r"[0-9a-f]{64}", shard["sha256"])):
                raise ValueError("Invalid HDFS export CSV size/hash")
            declared[relative] = shard
    expected_schemas = {"schemaVersion": CONTRACT_VERSION, "tables": {
        name: {key: metadata[key] for key in ("columns", "primaryKey")}
        for name, metadata in tables.items()}}
    if schemas != expected_schemas:
        raise ValueError("HDFS table_schemas.json differs from the manifest")
    return declared


def verify_hdfs_batch(spark, input_root, processed_root, export_root=None):
    from data_analysis.spark_jobs import fs
    from data_analysis.spark_jobs.pipeline import _filesystem, _qualified
    roots = [require_hdfs_uri(input_root), require_hdfs_uri(processed_root)]
    if export_root is not None:
        roots.append(require_hdfs_uri(export_root))
    fs.independent_paths(spark, *roots)
    storage = []
    for root in roots:
        filesystem, path = _filesystem(spark, root)
        if str(filesystem.getUri().getScheme()) != "hdfs":
            raise ValueError("Resolved filesystem is not HDFS")
        if not filesystem.isDirectory(path):
            raise ValueError("HDFS dataset directory does not exist: " + root)
        summary = filesystem.getContentSummary(path)
        storage.append({"uri": _qualified(spark, root),
            "filesystemClass": str(filesystem.getClass().getName()),
            "fileCount": summary.getFileCount(), "directoryCount": summary.getDirectoryCount(),
            "contentBytes": summary.getLength()})
    raw, processed = roots[:2]
    manifest = fs.read_json(spark, raw + "/manifest.json")
    fs.require_raw_complete(spark, raw, manifest)
    if not fs.verify_raw_files(spark, raw, manifest):
        raise ValueError("HDFS verification requires declared raw shard hashes")
    fs.require_success(spark, processed)
    quality_files = fs.glob(spark, processed + "/reports/quality_report/part-*.json")
    if len(quality_files) != 1:
        raise ValueError("Exactly one completed cleaning quality report is required")
    quality = fs.read_json(spark, quality_files[0])
    source_hash = fs.checksum(spark, raw + "/manifest.json")
    if (quality.get("dataset_id") != manifest.get("dataset_id") or
            quality.get("source_manifest_sha256") != source_hash or
            quality.get("input") != _qualified(spark, raw) or
            quality.get("output") != _qualified(spark, processed) or
            quality.get("raw_file_checksums_verified") is not True or
            quality.get("manifest_row_counts_verified") is not True or
            quality.get("reference_aggregates_used_as_input") is not False):
        raise ValueError("HDFS cleaning lineage mismatch; rerun Spark with these actual HDFS paths")
    observed = spark.read.parquet(processed + "/statistics/station_hourly").count()
    if observed != quality.get("station_hourly_rows"):
        raise ValueError("HDFS Parquet row count differs from completed quality report")
    export_verified = False
    if export_root is not None:
        export = roots[2]
        fs.require_success(spark, export)
        serving = fs.read_json(spark, export + "/serving_manifest.json")
        schemas = fs.read_json(spark, export + "/table_schemas.json")
        report_path = export + "/quality_report.json"
        export_quality = fs.read_json(spark, report_path)
        shards = validate_export_metadata(serving, schemas, export_quality, quality)
        declared_paths = {_qualified(spark, export + "/" + relative) for relative in shards}
        actual_paths = {_qualified(spark, path) for path in fs.glob(spark, export + "/csv/*/*.csv.gz")}
        if actual_paths != declared_paths:
            raise ValueError("HDFS exported CSV inventory differs from the serving manifest")
        for relative, shard in shards.items():
            path = export + "/" + relative
            if (not fs.is_file(spark, path) or fs.file_size(spark, path) != shard["bytes"] or
                    fs.checksum(spark, path) != shard["sha256"]):
                raise ValueError("HDFS exported CSV checksum/size mismatch")
        if fs.checksum(spark, report_path) != serving["qualityReportSha256"]:
            raise ValueError("HDFS export quality report checksum mismatch")
        export_verified = True
    return {"status": "VERIFIED_HDFS", "verifiedAt": datetime.now(timezone.utc).isoformat(),
        "datasetId": manifest["dataset_id"], "pipelineRunId": quality["pipeline_run_id"],
        "sourceManifestSha256": source_hash, "storage": storage,
        "rawFileHashesVerified": True, "processedParquetRows": observed,
        "exportVerified": export_verified, "sparkVersion": spark.version,
        "hadoopClientVersion": str(spark._jvm.org.apache.hadoop.util.VersionInfo.getVersion()),
        "hadoopServerVersion": "NOT_DETECTED: record hadoop version separately on the target server",
        "sparkMaster": spark.sparkContext.master,
        "scope": "Real HDFS storage/read and completed batch lineage; local Spark master is not a multi-node computation cluster"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True)
    parser.add_argument("--processed", required=True)
    parser.add_argument("--export", dest="export_root")
    parser.add_argument("--report", required=True, help="New local JSON evidence file")
    parser.add_argument("--master", default="local[2]")
    args = parser.parse_args(argv)
    report_path = Path(args.report)
    try:
        for root in (args.input, args.processed, args.export_root):
            if root is not None:
                require_hdfs_uri(root)
        if "://" in args.report or report_path.exists() or report_path.is_symlink():
            raise ValueError("Report must be a new local file")
    except ValueError as exc:
        parser.error(str(exc))
    from data_analysis.spark_jobs.pipeline import create_spark
    spark = None
    try:
        spark = create_spark("charging-hdfs-acceptance", args.master)
        report = verify_hdfs_batch(spark, args.input, args.processed, args.export_root)
    except Exception as exc:
        report = {"status": "NOT_VERIFIED", "errorType": type(exc).__name__,
            "reason": str(exc), "note": "A local run or failed HDFS connection is not an HDFS acceptance pass"}
        raise
    finally:
        if spark is not None:
            spark.stop()
        if "report" in locals():
            report_path.parent.mkdir(parents=True, exist_ok=True)
            with report_path.open("x", encoding="utf-8") as stream:
                json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
                stream.write("\n")
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    main()
