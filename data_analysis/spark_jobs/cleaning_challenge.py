"""Isolated, labeled fault-injection exercise using the production Spark cleaner.

This is NOT a replacement for the published dataset and does not estimate its
natural error rate. Only a bounded deterministic sample is collected to build
test cases; validation, normalization and deduplication run in PySpark.
"""

from __future__ import annotations

import argparse
from collections import Counter
import csv
from datetime import datetime, timedelta, timezone
import gzip
import hashlib
import json
from pathlib import Path

from data_analysis.charging_data.schema import TABLES
from data_analysis.spark_jobs.pipeline import clean_sessions, create_spark, read_table


CHALLENGE_VERSION = "cleaning-challenge-1.0.0"
INPUT_TABLES = ("charging_sessions", "stations", "chargers", "users", "vehicles", "charging_attempts")
MAX_SAMPLE_ROWS = 4096

# The expected outcome is a test oracle, not the reported cleaning result.
# Report success is calculated from the actual production cleaner outputs.
CASES = (
    ("DUPLICATE", "DEDUPLICATE", "DUPLICATE_SESSION_ID", ()),
    ("ENUM_WHITESPACE", "NORMALIZE", None, ("N001", "N004")),
    ("FULLWIDTH_NUMBER", "NORMALIZE", None, ("N003",)),
    ("ZERO_WIDTH_ID", "NORMALIZE", None, ("N002",)),
    ("EXPLICIT_TIMEZONE", "EQUIVALENT_TIMESTAMP", None, ()),
    ("INVALID_NUMERIC", "QUARANTINE", "INVALID_TYPE_OR_CSV", ()),
    ("INTEGER_OVERFLOW", "QUARANTINE", "INVALID_TYPE_OR_CSV", ()),
    ("MISSING_TIMEZONE", "QUARANTINE", "INVALID_TYPE_OR_CSV", ()),
    ("NONFINITE_SOC", "QUARANTINE", "INVALID_TYPE_OR_CSV", ()),
    ("NEGATIVE_FEE", "QUARANTINE", "INVALID_NONNEGATIVE_TOTAL_FEE_CENTS", ()),
    ("REVERSED_TIME", "QUARANTINE", "INVALID_TIME_ORDER", ()),
    ("SOC_RANGE", "QUARANTINE", "INVALID_END_SOC_PCT", ()),
    ("FEE_IDENTITY", "QUARANTINE", "INCONSISTENT_TOTAL_FEE", ()),
    ("MISSING_SESSION_ID", "QUARANTINE", "MISSING_SESSION_ID", ()),
    ("UNKNOWN_STATION", "QUARANTINE", "UNKNOWN_STATION_ID", ()),
    ("CHARGER_STATION_MISMATCH", "QUARANTINE", "INCONSISTENT_CHARGERS_STATION_ID", ()),
    ("VEHICLE_USER_MISMATCH", "QUARANTINE", "INCONSISTENT_VEHICLES_USER_ID", ()),
    ("INVALID_STATUS", "QUARANTINE", "INVALID_SESSION_STATUS", ()),
    ("INVALID_TEXT", "QUARANTINE", "INVALID_TEXT_ENCODING", ()),
    ("INVALID_TARGET", "QUARANTINE", "INVALID_ENERGY_TARGET", ()),
)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def _json(path, value):
    with Path(path).open("x", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False)
        stream.write("\n")


def validate_paths(source, destination):
    source_arg, destination_arg = Path(source), Path(destination)
    if source_arg.is_symlink() or destination_arg.is_symlink():
        raise ValueError("Symlink roots are not accepted")
    source, destination = source_arg.resolve(), destination_arg.resolve()
    if not source.is_dir():
        raise ValueError("Input must be an existing acceptance bundle")
    if source == destination or source.is_relative_to(destination) or destination.is_relative_to(source):
        raise ValueError("Source and output must be independent, non-nested directories")
    if destination.exists():
        raise FileExistsError("Output already exists; choose a new challenge directory")
    return source, destination


def verify_source(root):
    """Verify only consumed CLEAN tables plus their immutable manifest binding."""
    root = Path(root).resolve()
    if not (root / "_SUCCESS").is_file():
        raise ValueError("Source has no acceptance success marker")
    manifest_path = root / "acceptance_manifest.json"
    serving_path = root / "serving_manifest.json"
    if manifest_path.is_symlink() or serving_path.is_symlink():
        raise ValueError("Manifest symlinks are not accepted")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    serving = json.loads(serving_path.read_text(encoding="utf-8"))
    bindings = ("datasetId", "publishedBatchId", "pipelineRunId", "sourceManifestSha256")
    if (any(not manifest.get(key) or manifest.get(key) != serving.get(key) for key in bindings) or
            manifest.get("servingManifestSha256") != digest(serving_path) or
            not isinstance(manifest.get("files"), list)):
        raise ValueError("Acceptance and serving dataset identities do not match")
    declared = {}
    for entry in manifest["files"]:
        relative = entry.get("path", "")
        if not isinstance(relative, str) or relative in declared:
            raise ValueError("Invalid or repeated acceptance inventory path")
        path = root / relative
        if Path(relative).is_absolute() or ".." in Path(relative).parts or not path.resolve().is_relative_to(root):
            raise ValueError("Unsafe acceptance inventory path")
        declared[relative] = entry
    wanted = set()
    for table in INPUT_TABLES:
        directory = root / "clean" / table
        files = sorted(directory.glob("part-*.parquet"))
        if not files or directory.is_symlink():
            raise ValueError("Source is missing a regular CLEAN table: " + table)
        wanted.update(path.relative_to(root).as_posix() for path in files)
        expected = {name for name in declared if name.startswith(f"clean/{table}/part-") and name.endswith(".parquet")}
        actual = {path.relative_to(root).as_posix() for path in files}
        if actual != expected:
            raise ValueError("Source CLEAN inventory mismatch: " + table)
    verified = []
    for relative in sorted(wanted):
        entry, path = declared.get(relative), root / relative
        if (not entry or path.is_symlink() or not path.is_file() or
                path.stat().st_size != entry.get("bytes") or digest(path) != entry.get("sha256")):
            raise ValueError("Source checksum/size mismatch: " + relative)
        verified.append(dict(path=relative, sha256=entry["sha256"], bytes=entry["bytes"]))
    return serving, {"acceptanceManifestSha256": digest(manifest_path),
                     "servingManifestSha256": digest(serving_path), "verifiedInputFiles": verified}


def _string(value):
    if value is None:
        return ""
    if isinstance(value, datetime):
        value = value.replace(tzinfo=timezone.utc) if value.tzinfo is None else value.astimezone(timezone.utc)
        return value.isoformat().replace("+00:00", "Z")
    return str(value)


def _raw_hash(row):
    # Matches Spark to_json(struct(schema-ordered CSV strings)), including nulls.
    canonical = {name: row.get(name) or None for name in TABLES["charging_sessions"]}
    return hashlib.sha256(json.dumps(canonical, ensure_ascii=False, separators=(",", ":")).encode("utf-8")).hexdigest()


def make_cases(records, station_ids, user_ids):
    """Create one labeled case per baseline row; invalid clones cannot erase truth."""
    if len(records) < len(CASES):
        raise ValueError(f"At least {len(CASES)} clean sessions are required to exercise all cases")
    baseline, dirty, labels = [], [], []
    station_ids, user_ids = sorted(station_ids), sorted(user_ids)
    if len(station_ids) < 2 or len(user_ids) < 2:
        raise ValueError("The challenge needs at least two stations and users for ownership checks")
    for index, record in enumerate(records):
        row = {name: _string(record.get(name)) for name in TABLES["charging_sessions"]}
        baseline.append(row)
        changed = dict(row)
        kind, action, reason, normalization = CASES[index % len(CASES)]
        if kind == "ENUM_WHITESPACE":
            changed["status"] = "\u3000" + row["status"].lower() + "\u00a0"
        elif kind == "FULLWIDTH_NUMBER":
            changed["total_fee_cents"] = row["total_fee_cents"].translate(str.maketrans("0123456789", "０１２３４５６７８９"))
        elif kind == "ZERO_WIDTH_ID":
            changed["user_id"] = row["user_id"][:1] + "\u200b" + row["user_id"][1:]
        elif kind == "EXPLICIT_TIMEZONE":
            stamp = datetime.fromisoformat(row["started_at"].replace("Z", "+00:00"))
            changed["started_at"] = stamp.astimezone(timezone(timedelta(hours=8))).isoformat()
        elif kind == "INVALID_NUMERIC":
            changed["energy_wh"] = "2O000"  # letter O, not zero: cannot infer intent
        elif kind == "INTEGER_OVERFLOW":
            changed["energy_wh"] = "9223372036854775808"
        elif kind == "MISSING_TIMEZONE":
            changed["started_at"] = row["started_at"].removesuffix("Z")
        elif kind == "NONFINITE_SOC":
            changed["end_soc_pct"] = "NaN"
        elif kind == "NEGATIVE_FEE":
            changed["total_fee_cents"] = "-100"
        elif kind == "REVERSED_TIME":
            changed["ended_at"] = row["started_at"]
        elif kind == "SOC_RANGE":
            changed["end_soc_pct"] = "150"
        elif kind == "FEE_IDENTITY":
            changed["total_fee_cents"] = str(int(row["total_fee_cents"]) + 12345)
        elif kind == "MISSING_SESSION_ID":
            changed["session_id"] = ""
        elif kind == "UNKNOWN_STATION":
            unknown = "CHALLENGE-NONEXISTENT-STATION"
            if unknown in station_ids:
                raise ValueError("Unknown-station sentinel collision")
            changed["station_id"] = unknown
        elif kind == "CHARGER_STATION_MISMATCH":
            changed["station_id"] = next(key for key in station_ids if key != row["station_id"])
        elif kind == "VEHICLE_USER_MISMATCH":
            changed["user_id"] = next(key for key in user_ids if key != row["user_id"])
        elif kind == "INVALID_STATUS":
            changed["status"] = "SUSPICIOUS_COMPLETED"
        elif kind == "INVALID_TEXT":
            changed["stop_reason"] = "ENERGY_\ufffdREACHED"
        elif kind == "INVALID_TARGET":
            changed["target_value"] = "0"
        if action in ("NORMALIZE", "EQUIVALENT_TIMESTAMP"):
            dirty.append(changed)  # the same event, only a different presentation
        else:
            dirty.extend([row, changed])  # original truth remains recoverable
        labels.append({"caseId": f"case-{index + 1:05d}", "kind": kind,
            "expectedAction": action, "expectedReason": reason,
            "expectedNormalizationRules": list(normalization), "rawRecordSha256": _raw_hash(changed),
            "sourceSessionSha256": hashlib.sha256(row["session_id"].encode()).hexdigest()})
    return baseline, dirty, labels


def _csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as binary, gzip.GzipFile(fileobj=binary, filename="", mode="wb", mtime=0) as compressed:
        import io
        with io.TextIOWrapper(compressed, encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=TABLES["charging_sessions"], lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)


def evaluate_frames(input_frame, baseline_frame, clean, rejected, labels):
    """Compute outcomes from actual frames, including false deletion and drift."""
    from pyspark.sql import functions as F
    rejection_counts = {(row["_raw_record_sha256"], row["rejection_reason"]): row["count"]
                        for row in rejected.groupBy("_raw_record_sha256", "rejection_reason").count().collect()}
    observed = {row["_raw_record_sha256"]: row.asDict() for row in input_frame.groupBy("_raw_record_sha256").agg(
        F.count("*").alias("inputCount"),
        F.array_distinct(F.flatten(F.collect_list("_normalization_actions"))).alias("normalizationRules")).collect()}
    results = []
    for label in labels:
        raw_hash = label["rawRecordSha256"]
        incoming = observed.get(raw_hash, {})
        reasons = {reason: count for (fingerprint, reason), count in rejection_counts.items() if fingerprint == raw_hash}
        expected_reason = label["expectedReason"]
        correct = (incoming.get("inputCount", 0) > 0 and
                   (reasons == {expected_reason: 1} if expected_reason else not reasons) and
                   set(label["expectedNormalizationRules"]).issubset(incoming.get("normalizationRules", [])))
        results.append(dict(label, observedInputRows=incoming.get("inputCount", 0), observedRejections=reasons,
                            observedNormalizationRules=sorted(incoming.get("normalizationRules", [])), passed=correct))
    expected = baseline_frame.select(*TABLES["charging_sessions"])
    missing = expected.exceptAll(clean).count()
    unexpected = clean.exceptAll(expected).count()
    input_count, clean_count, rejected_count = input_frame.count(), clean.count(), rejected.count()
    def metrics(frame):
        row = frame.agg(F.count("*").alias("rows"), F.sum("total_fee_cents").alias("billedCents"),
                        F.sum("energy_wh").alias("energyWh"),
                        F.sum(F.when(F.col("total_fee_cents").isNull(), 1).otherwise(0)).alias("nullFeeRows"),
                        F.sum(F.when(F.col("energy_wh").isNull(), 1).otherwise(0)).alias("nullEnergyRows")).first()
        return row.asDict()
    before, baseline, after = metrics(input_frame), metrics(expected), metrics(clean)
    def bill_distortion(values):
        # Still emit a failed audit when a regression removes every valid row;
        # unknown/all-null sums must not be silently converted into zero.
        if values["billedCents"] is None or baseline["billedCents"] is None:
            return None
        return values["billedCents"] - baseline["billedCents"]
    reasons = Counter()
    for (_, reason), count in rejection_counts.items():
        reasons[reason] += count
    case_results = []
    for kind, action, reason, _ in CASES:
        subset = [result for result in results if result["kind"] == kind]
        case_results.append(dict(kind=kind, action=action, expectedReason=reason, cases=len(subset),
                                 passed=sum(result["passed"] for result in subset)))
    all_passed = (all(result["passed"] for result in results) and missing == unexpected == 0 and
                  input_count == clean_count + rejected_count)
    return {"passed": all_passed, "sampleRows": baseline["rows"], "inputRows": input_count,
        "cleanRows": clean_count, "rejectedRows": rejected_count,
        "testedCaseTypes": len(case_results), "testedCases": len(results),
        "passedCases": sum(row["passed"] for row in results), "byCaseType": case_results,
        "rejectionReasons": dict(sorted(reasons.items())),
        "invariants": {"rowConservation": input_count == clean_count + rejected_count,
                       "missingOrChangedBaselineRows": missing, "unexpectedCleanRows": unexpected,
                       "exactBaselineRestored": missing == unexpected == 0},
        "metricImpact": {"beforeNaiveParsedSum": before, "expectedBaseline": baseline, "afterCleaning": after,
            "beforeBillDistortionCents": bill_distortion(before),
            "afterBillDistortionCents": bill_distortion(after),
            "explanation": "Billed amounts, NOT cash revenue. The naive before sum includes invalid/duplicate rows; unparseable numeric values are NULL, not zero."},
        "cases": results}


def run_challenge(spark, source, destination, sample_rows=256, seed=42):
    if type(sample_rows) is not int or not len(CASES) <= sample_rows <= MAX_SAMPLE_ROWS:
        raise ValueError(f"sample-rows must be in [{len(CASES)}, {MAX_SAMPLE_ROWS}]")
    if type(seed) is not int or not 0 <= seed <= 2**32 - 1:
        raise ValueError("seed must be an integer in [0, 2**32-1]")
    source, destination = validate_paths(source, destination)
    serving, provenance = verify_source(source)
    from pyspark.sql import functions as F
    if spark.conf.get("spark.sql.session.timeZone") != "UTC":
        raise ValueError("Spark session timezone must be UTC")
    tables = {name: spark.read.parquet(str(source / "clean" / name)).select(*TABLES[name]) for name in INPUT_TABLES}
    sample = (tables["charging_sessions"].orderBy(
        F.sha2(F.concat(F.lit(str(seed) + ":"), F.col("session_id")), 256), "session_id").limit(sample_rows).cache())
    # Format timestamps inside Spark's asserted UTC session. PySpark's Python
    # Row datetime conversion can otherwise use the host timezone despite the
    # SQL session timezone, silently shifting the supposedly identical source.
    records = [row.asDict() for row in sample.select(*[
        (F.date_format(F.col(name), "yyyy-MM-dd'T'HH:mm:ss.SSSSSS'Z'") if name.endswith("_at")
         else F.col(name).cast("string")).alias(name) for name in TABLES["charging_sessions"]]).collect()]
    if len(records) != sample_rows:
        sample.unpersist()
        raise ValueError("Source has fewer sessions than the requested bounded sample")
    baseline, dirty, labels = make_cases(records,
        [row["station_id"] for row in tables["stations"].select("station_id").collect()],
        [row["user_id"] for row in tables["users"].select("user_id").orderBy("user_id").limit(2).collect()])
    destination.mkdir(parents=True, exist_ok=False)
    running = destination / "_RUNNING"
    running.touch(exist_ok=False)
    persisted = [sample]
    try:
        _csv(destination / "baseline" / "charging_sessions.csv.gz", baseline)
        _csv(destination / "raw" / "charging_sessions" / "part-00000.csv.gz", dirty)
        _json(destination / "injection_plan.json", {"version": CHALLENGE_VERSION, "seed": seed,
            "selection": "smallest SHA256(seed:session_id), tie-break session_id", "cases": labels})
        # Dimensions are not mutated. Retain only selected attempt rows in the
        # plan to make the relational validation smaller without weakening it.
        dimensions = dict(tables)
        dimensions["charging_attempts"] = tables["charging_attempts"].join(sample.select("attempt_id"), "attempt_id", "left_semi")
        raw = read_table(spark, str(destination), "charging_sessions").cache()
        persisted.append(raw)
        clean, rejected = clean_sessions(raw, dimensions)
        clean, rejected = clean.cache(), rejected.cache()
        persisted.extend([clean, rejected])
        report = evaluate_frames(raw, sample, clean, rejected, labels)
        clean.write.mode("errorifexists").parquet(str(destination / "clean" / "charging_sessions"))
        rejected.write.mode("errorifexists").parquet(str(destination / "quarantine" / "charging_sessions"))
        report.update(version=CHALLENGE_VERSION, generatedAt=datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            engine={"name": "PySpark", "version": spark.version, "sessionTimezone": "UTC"},
            datasetId=serving["datasetId"], publishedBatchId=serving["publishedBatchId"],
            pipelineRunId=serving["pipelineRunId"], sourceManifestSha256=serving["sourceManifestSha256"],
            source=provenance, seed=seed, scope="ISOLATED_LABELED_CHALLENGE_NOT_PRODUCTION_ERROR_RATE",
            privacy="No raw user/session IDs in this report; evidence files retain synthetic IDs for relational audit.",
            limitations=["Cases are deliberately balanced exercises, not an estimate of real-world defect prevalence.",
                "One primary rejection reason per row; a row can violate additional checks.",
                "Only recoverable presentation is normalized. Missing facts, ambiguous times and invalid values are quarantined.",
                "This exercise reuses existing production cleaner; it does not modify the published analytics or ML training batches."])
        report["evidenceFiles"] = [{"path": path.relative_to(destination).as_posix(), "bytes": path.stat().st_size,
                                    "sha256": digest(path)} for path in sorted(destination.rglob("*"))
                                   if path.is_file() and path.name != "_RUNNING" and not path.name.startswith(".")]
        _json(destination / "report.json", report)
        if not report["passed"]:
            raise ValueError("Challenge failed; inspect report.json and quarantine. No _SUCCESS marker was written.")
        running.unlink()
        (destination / "_SUCCESS").touch(exist_ok=False)
        return report
    finally:
        for frame in reversed(persisted):
            frame.unpersist()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="Verified local acceptance bundle containing CLEAN Parquet")
    parser.add_argument("--output", required=True, help="New independent local evidence directory")
    parser.add_argument("--sample-rows", type=int, default=256)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--master", default="local[2]")
    parser.add_argument("--driver-memory", default="2g")
    args = parser.parse_args()
    spark = create_spark("charging-cleaning-challenge", master=args.master, shuffle_partitions=2,
                         driver_memory=args.driver_memory)
    spark.sparkContext.setLogLevel("WARN")
    try:
        result = run_challenge(spark, args.input, args.output, args.sample_rows, args.seed)
        print(json.dumps({key: result[key] for key in ("passed", "sampleRows", "testedCaseTypes", "testedCases", "passedCases", "inputRows", "cleanRows", "rejectedRows")}, ensure_ascii=False))
    finally:
        spark.stop()


if __name__ == "__main__":
    main()
