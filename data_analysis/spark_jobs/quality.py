"""Bounded, versioned quality audits. Rule compliance is not real-world accuracy."""

import hashlib
import json

from data_analysis.charging_data.schema import TABLES
from data_analysis.spark_jobs.normalization import BOUNDARY_PATTERN, RULES, RULE_VERSION

AUDIT_VERSION = "cleaning-audit-1.0.0"
RULE_SHA256 = hashlib.sha256(json.dumps(RULES, ensure_ascii=False, sort_keys=True,
                                      separators=(",", ":")).encode("utf-8")).hexdigest()
PRIMARY_KEYS = {table: [fields[0]] for table, fields in TABLES.items()}
PRIMARY_KEYS.update(calendar=["city_id", "business_date"], weather_hourly=["city_id", "recorded_at"],
                    operating_costs=["station_id", "business_date"],
                    charger_telemetry=["charger_id", "recorded_at"],
                    battery_samples=["session_id", "recorded_at"])
SESSION_REQUIRED = {"session_id", "attempt_id", "user_id", "vehicle_id", "station_id", "charger_id",
                    "started_at", "ended_at", "unplugged_at", "status", "start_soc_pct", "end_soc_pct",
                    "target_mode", "target_value"} | {
                        name for name in TABLES["charging_sessions"] if name.endswith(("_cents", "_wh"))}


def profile_table(frame, table, *, before, manifest=None):
    """One aggregate action per table/stage, including exact primary-key duplicates.

    Before means original CSV strings, not silently standardized values. A blank
    text cell and a literal null are reported separately. Optional null columns
    are measured but are NOT all treated as missing required data.
    """
    from functools import reduce
    from pyspark.sql import functions as F, types as T
    fields, keys = TABLES[table], PRIMARY_KEYS[table]
    if before:
        frame = frame.withColumn("_audit_original", F.from_json("_raw_json", T.MapType(T.StringType(), T.StringType())))
    key_values = [F.col("_audit_original")[key] if before else F.col(key) for key in keys]
    missing_key = reduce(lambda a, b: a | b, [value.isNull() | (F.regexp_replace(value.cast("string"), BOUNDARY_PATTERN, "") == "") for value in key_values])
    summation = lambda condition: F.coalesce(F.sum(condition.cast("long")), F.lit(0))
    expressions = [F.count("*").alias("rows"), summation(missing_key).alias("missing_keys"),
                   F.countDistinct(F.when(~missing_key, F.struct(*key_values))).alias("distinct_keys")]
    row_invalid = F.lit(False)
    for field in fields:
        if before:
            missing = F.array_contains("_raw_missing_fields", field)
            blank = F.array_contains("_raw_blank_fields", field)
            invalid = F.array_contains("_raw_type_error_fields", field) | F.array_contains("_text_error_fields", field)
        else:
            missing, blank = F.col(field).isNull(), F.lit(False)
            invalid = (F.isnan(field) | (F.abs(F.col(field)) == float("inf"))
                       if isinstance(frame.schema[field].dataType, T.DoubleType) else F.lit(False))
        expressions.extend([summation(missing).alias(field + "__missing"),
                            summation(blank).alias(field + "__blank"), summation(invalid).alias(field + "__invalid")])
        row_invalid = row_invalid | invalid
        kind = frame.schema[field].dataType
        if isinstance(kind, (T.LongType, T.DoubleType, T.DateType, T.TimestampType)):
            parsable = (~F.array_contains("_type_error_fields", field) & ~F.array_contains("_text_error_fields", field)
                        if before else ~invalid)
            value = F.when(parsable, F.col(field))
            minimum, maximum = F.min(value), F.max(value)
            if isinstance(kind, (T.DateType, T.TimestampType)):
                minimum, maximum = minimum.cast("string"), maximum.cast("string")
            expressions.extend([minimum.alias(field + "__min"), maximum.alias(field + "__max")])
    if before:
        expressions.extend([summation(F.col("_parse_error_before")).alias("type_invalid_rows"),
                            summation(F.col("_parse_error")).alias("normalized_type_invalid_rows"),
                            summation(F.col("_normalized_enum")).alias("normalized_enum_rows"),
                            summation(F.size("_normalized_fields") > 0).alias("normalized_rows"),
                            F.coalesce(F.sum(F.size("_normalized_fields")), F.lit(0)).alias("normalized_cells")])
        for rule in ["N001", "N002", "N003", "N004", "N005"]:
            expressions.append(summation(F.array_contains("_normalization_actions", rule)).alias(rule))
    else:
        expressions.append(summation(row_invalid).alias("type_invalid_rows"))
    window = (manifest or {}).get("period_start"), (manifest or {}).get("period_end_exclusive")
    if table == "charger_telemetry":
        expressions.extend([F.min("recorded_at").cast("string").alias("first_observed_at_utc"),
                            F.max("recorded_at").cast("string").alias("last_observed_at_utc"),
                            summation(F.col("recorded_at").isNull()).alias("missing_or_unparseable_timestamp_rows")])
        if all(window):
            outside = ((F.col("recorded_at") < F.to_timestamp(F.lit(window[0]))) |
                       (F.col("recorded_at") >= F.to_timestamp(F.lit(window[1]))))
            expressions.append(summation(outside).alias("outside_batch_window_rows"))
    result = frame.agg(*expressions).first().asDict()
    field_profiles = {field: {"missing_rows": result[field + "__missing"],
                              "blank_text_rows": result[field + "__blank"],
                              "type_or_encoding_invalid_rows": result[field + "__invalid"]} for field in fields}
    for field in fields:
        if field + "__min" in result:
            field_profiles[field]["observed_range"] = {"min": result[field + "__min"], "max": result[field + "__max"]}
    required = SESSION_REQUIRED if table == "charging_sessions" else set(keys)
    profile = {
        "row_count": result["rows"], "field_profiles": field_profiles,
        "range_basis": "successfully parsed normalized finite/date/time values, not raw text distribution; before can include later business-rule rejects",
        "required_fields": sorted(required),
        "required_missing_cells": sum(field_profiles[field]["missing_rows"] + field_profiles[field]["blank_text_rows"] for field in required),
        "required_cell_count": len(required) * result["rows"],
        "primary_key": {"columns": keys, "missing_key_rows": result["missing_keys"],
                        "distinct_key_count": result["distinct_keys"],
                        "duplicate_excess_rows": result["rows"] - result["missing_keys"] - result["distinct_keys"]},
    }
    if before:
        profile.update({name: result[name] for name in ["type_invalid_rows", "normalized_type_invalid_rows",
                         "normalized_enum_rows", "normalized_rows", "normalized_cells"]})
        profile["normalization_action_rows"] = {rule: result[rule] for rule in ["N001", "N002", "N003", "N004", "N005"]}
    else:
        profile["type_invalid_rows"] = result["type_invalid_rows"]
        profile["type_assessment"] = "retained schema plus finite-number checks; original lexical checks occur before retention"
    if table == "charger_telemetry":
        profile["event_window"] = {key: result.get(key) for key in ["first_observed_at_utc", "last_observed_at_utc", "outside_batch_window_rows", "missing_or_unparseable_timestamp_rows"]}
    return profile


def _rate(passed, total):
    return {"passed": passed, "assessed": total, "rate": passed / total if total else None}


def six_dimensions(tables, reasons, manifest):
    """No invented accuracy percentage, wall-clock freshness score or overall grade."""
    output = {
        "accuracy": {"status": "NOT_MEASURED", "rate": None,
                     "reason": "No independent real-world ground truth. Synthetic reference reconciliation proves computational agreement, not factual accuracy."},
        "completeness": {"scope": "declared primary keys and required charging-session fields; optional nulls are not scored as errors"},
        "validity": {"scope": "explicit type/lexical/encoding checks; not an exhaustive business truth test"},
        "uniqueness": {"scope": "excess copies per declared primary key, excluding missing keys; raw keys before versus retained normalized keys after"},
        "consistency": {"scope": "session business checks after deterministic standardization, before quarantine; one first-failure reason per row"},
        "timeliness": {"scope": "historical charger telemetry against declared batch window, not current wall-clock age",
                       "period_start": manifest.get("period_start"), "period_end_exclusive": manifest.get("period_end_exclusive"),
                       "limitation": "In-window events do not prove complete cadence or current live-device state."},
    }
    for stage in ("before", "after"):
        profiles = [item[stage] for item in tables.values()]
        rows = sum(profile["row_count"] for profile in profiles)
        required = sum(profile["required_cell_count"] for profile in profiles)
        missing = sum(profile["required_missing_cells"] for profile in profiles)
        invalid = sum(profile["type_invalid_rows"] for profile in profiles)
        unique_rows = sum(profile["row_count"] - profile["primary_key"]["missing_key_rows"] for profile in profiles)
        duplicates = sum(profile["primary_key"]["duplicate_excess_rows"] for profile in profiles)
        output["completeness"][stage] = _rate(required - missing, required)
        output["validity"][stage] = _rate(rows - invalid, rows)
        output["uniqueness"][stage] = _rate(unique_rows - duplicates, unique_rows)
        sessions = tables["charging_sessions"][stage]["row_count"]
        failures = (sum(count for reason, count in reasons.items() if reason not in
                        {"DUPLICATE_SESSION_ID", "INVALID_TYPE_OR_CSV", "INVALID_TEXT_ENCODING"}) if stage == "before" else 0)
        assessed = (sessions - reasons.get("INVALID_TYPE_OR_CSV", 0) - reasons.get("INVALID_TEXT_ENCODING", 0)
                    if stage == "before" else sessions)
        output["consistency"][stage] = _rate(assessed - failures, assessed)
        telemetry = tables["charger_telemetry"][stage]
        outside = telemetry["event_window"].get("outside_batch_window_rows")
        missing_time = telemetry["event_window"]["missing_or_unparseable_timestamp_rows"]
        output["timeliness"][stage] = (_rate(telemetry["row_count"] - outside - missing_time, telemetry["row_count"])
                                             if outside is not None else {"status": "NOT_MEASURED", "rate": None, "reason": "Batch window absent"})
    return output


def build_audit(raw, clean, before_profiles, reasons, rejected, manifest, pipeline_run_id, source_digest):
    from functools import reduce
    from pyspark.sql import functions as F, types as T
    tables = {}
    for table in TABLES:
        after = profile_table(clean[table], table, before=False, manifest=manifest)
        quarantined = sum(count for reason, count in reasons.items() if reason != "DUPLICATE_SESSION_ID") if table == "charging_sessions" else 0
        duplicates = reasons.get("DUPLICATE_SESSION_ID", 0) if table == "charging_sessions" else 0
        source_count = before_profiles[table]["row_count"]
        conserved = source_count == after["row_count"] + quarantined + duplicates
        tables[table] = {"before": before_profiles[table], "after": after,
                         "row_conservation": {"input": source_count, "retained": after["row_count"],
                                              "quarantined": quarantined, "duplicate_copies": duplicates, "passed": conserved}}
        if not conserved:
            raise ValueError("Cleaning row conservation failed: " + table)
        if after["primary_key"]["duplicate_excess_rows"]:
            raise ValueError("Duplicate primary key after normalization: " + table)
        if after["primary_key"]["missing_key_rows"]:
            raise ValueError("Missing primary key after normalization: " + table)
    samples = []
    for table, frame in raw.items():
        samples.append(frame.filter(F.size("_normalized_fields") > 0).select(
            F.lit(table).alias("table"), F.col("_raw_record_sha256").alias("raw_record_sha256"),
            F.regexp_replace("_source_file", r"^.*/raw/", "raw/").alias("source_file"),
            "_raw_json", "_normalized_fields", "_normalization_actions",
            F.create_map(*[item for field in TABLES[table] for item in (F.lit(field), F.col(field).cast("string"))]).alias("_after")))
    normalized = reduce(lambda left, right: left.unionByName(right), samples)
    # Only a small, deterministically ordered sample crosses into Python; raw tables do not.
    normalized = (normalized.orderBy("table", "raw_record_sha256").limit(8)
                  .withColumn("_before", F.from_json("_raw_json", T.MapType(T.StringType(), T.StringType()))))
    corrections = []
    def preview(value):
        return None if value is None else str(value)[:120]
    for row in normalized.collect():
        corrections.append({"table": row["table"], "source_file": row["source_file"],
                            "raw_record_sha256": row["raw_record_sha256"], "rule_ids": row["_normalization_actions"],
                            "changes": [{"field": field, "before": preview(row["_before"].get(field)),
                                         "after": preview(row["_after"].get(field))} for field in row["_normalized_fields"]]})
    error_fields = [
        (F.col(source) if source in rejected.columns else F.array().cast("array<string>")).alias(target)
        for source, target in [("_type_error_fields", "type_error_fields"), ("_text_error_fields", "text_error_fields")]
    ]
    issue_rows = (rejected.orderBy("rejection_reason", "_raw_record_sha256").limit(12)
                  .select("session_id", "rejection_reason", "_raw_record_sha256",
                          F.regexp_replace("_source_file", r"^.*/raw/", "raw/").alias("source_file"), *error_fields))
    issues = [{**row.asDict(), "action": "DISCARD_DUPLICATE_COPY" if row.rejection_reason == "DUPLICATE_SESSION_ID" else "QUARANTINE"}
              for row in issue_rows.collect()]
    totals = {key: sum(item["row_conservation"][key] for item in tables.values())
              for key in ["input", "retained", "quarantined", "duplicate_copies"]}
    totals["passed"] = totals["input"] == totals["retained"] + totals["quarantined"] + totals["duplicate_copies"]
    return {
        "audit_version": AUDIT_VERSION, "rule_version": RULE_VERSION, "rules_sha256": RULE_SHA256,
        "lineage": {"dataset_id": manifest.get("dataset_id"), "pipeline_run_id": pipeline_run_id,
                    "source_manifest_sha256": source_digest, "source_layer_modified": False,
                    "raw_identity": "manifest/shard checksums plus original field-JSON SHA256; fingerprint is not a unique physical row offset"},
        "workflow": ["PROFILE_RAW", "ASSESS_SIX_DIMENSIONS", "APPLY_VERSIONED_RULES", "QUARANTINE_AND_DEDUP", "VERIFY_CONSERVATION", "REPORT"],
        "tables": tables, "row_conservation": totals,
        "six_dimensions": six_dimensions(tables, reasons, manifest),
        "correction_samples": corrections, "issue_samples": issues,
        "limitations": ["Optional missing values are preserved; no key/amount/energy imputation or unit guessing.",
                        "Business outliers are not blindly clipped or deleted; only explicit contract violations are rejected.",
                        "Encoding detection catches replacement characters/prohibited controls, not every possible mojibake string.",
                        "Only charging sessions are repaired/quarantined as a recoverable batch; invalid other-table data fails closed.",
                        "Sample limits are 8 corrected and 12 rejected rows; full rejected rows remain in Parquet."],
    }


def audit_summary(audit):
    before = [item["before"] for item in audit["tables"].values()]
    return {"audit_version": AUDIT_VERSION, "rule_version": RULE_VERSION, "rules_sha256": RULE_SHA256,
            "audit_path": "reports/cleaning_audit.json", "rules_path": "reports/cleaning_rules.json",
            "row_conservation_passed": audit["row_conservation"]["passed"],
            "normalized_rows": sum(profile["normalized_rows"] for profile in before),
            "normalized_cells": sum(profile["normalized_cells"] for profile in before),
            "normalization_action_rows": {rule: sum(profile["normalization_action_rows"][rule] for profile in before)
                                          for rule in ["N001", "N002", "N003", "N004", "N005"]}}
