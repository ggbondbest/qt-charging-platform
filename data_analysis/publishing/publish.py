"""Validate portable Spark exports and atomically publish one read-only SQLite.

Run ``python -m data_analysis.publishing.publish --input EXPORT --output NEW_DB``.
Only aggregate/feature serving tables are imported. Future ML targets are fully
checked but never persisted in the API database. No Spark runtime is required.
"""

from __future__ import annotations

import argparse
import csv
from datetime import date, timedelta, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import sqlite3
import stat
import tempfile

from data_analysis.contracts import CONTRACT_VERSION
from data_analysis.contracts.serving import (
    SERVING_TABLES, SQL_TYPES, TABLE_KEYS, convert_csv_value, parse_utc, validate_table_contract,
)


LABEL_TABLE = "ml_targets_hourly"
TARGET_KEYS_TABLE = "__validation_ml_target_keys"
STATE_NAMES = ("available", "charging", "reserved", "occupied", "maintenance", "offline")


def _hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _json(path):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("Duplicate JSON key: " + key)
            result[key] = value
        return result
    def nonfinite(value):
        raise ValueError("Non-finite JSON value: " + value)
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=pairs, parse_constant=nonfinite)


def _file(root, relative):
    if (not isinstance(relative, str) or not relative or "\\" in relative or
            PurePosixPath(relative).is_absolute() or PureWindowsPath(relative).drive or
            any(part in {".", ".."} for part in relative.split("/"))):
        raise ValueError("Export file path must be relative and contained")
    path = root
    for part in relative.split("/"):
        if not part:
            raise ValueError("Empty export path component")
        path = path / part
        if path.is_symlink():
            raise ValueError("Export symlink traversal is forbidden")
    if not path.resolve().is_relative_to(root) or not path.is_file():
        raise ValueError("Missing or escaping export file: " + relative)
    return path


def inspect_export(input_root, *, require_complete=True):
    """Read-only metadata/hash preflight; row/type checks occur during publication.

    Return ``root, manifest, quality, files, manifestSha256``. ``files`` maps
    declared relative paths to verified local Paths; no Parquet/raw is included.
    """
    supplied = Path(input_root)
    if supplied.is_symlink():
        raise ValueError("Export root must not be a symlink")
    root = supplied.resolve()
    if not root.is_dir():
        raise ValueError("Input must be an existing export directory")
    files = {"serving_manifest.json": _file(root, "serving_manifest.json"),
             "table_schemas.json": _file(root, "table_schemas.json")}
    if require_complete:
        if (root / "_RUNNING").exists() or (root / "_RUNNING").is_symlink():
            raise ValueError("Export is not complete: _RUNNING is present")
        files["_SUCCESS"] = _file(root, "_SUCCESS")
    else:
        # Internal bundle precommit check. No consumer may use this mode to
        # publish SQLite: the public publishing path always requires completion.
        _file(root, "_RUNNING")
        if (root / "_SUCCESS").exists() or (root / "_SUCCESS").is_symlink():
            raise ValueError("Staged bundle must not advertise completion")
    manifest = _json(files["serving_manifest.json"])
    if not isinstance(manifest, dict) or manifest.get("schemaVersion") != CONTRACT_VERSION:
        raise ValueError("Unsupported serving schemaVersion")
    if manifest.get("source") != "SIMULATED" or manifest.get("businessTimezone") != "Asia/Shanghai":
        raise ValueError("Unsupported source or business timezone")
    for key in ["datasetId", "publishedBatchId", "pipelineRunId", "featureVersion", "generatedAt"]:
        if not isinstance(manifest.get(key), str) or not 0 < len(manifest[key]) <= 160:
            raise ValueError("Missing or invalid publication identity: " + key)
    parse_utc(manifest["generatedAt"])
    start, end = date.fromisoformat(manifest["startDate"]), date.fromisoformat(manifest["endDate"])
    if start.isoformat() != manifest["startDate"] or end.isoformat() != manifest["endDate"] or start >= end:
        raise ValueError("Invalid publication date range")
    begin, finish = parse_utc(manifest["periodStart"]), parse_utc(manifest["periodEndExclusive"])
    business_tz = timezone(timedelta(hours=8))
    if begin >= finish or begin.astimezone(business_tz).date() != start or finish.astimezone(business_tz).date() != end:
        raise ValueError("Publication instant/date ranges do not agree")
    if not isinstance(manifest.get("tables"), dict) or set(manifest["tables"]) != set(TABLE_KEYS):
        raise ValueError("Export must contain exactly the contracted ten tables")
    if not re.fullmatch(r"[0-9a-f]{64}", manifest.get("sourceManifestSha256", "")):
        raise ValueError("Missing source manifest SHA256")
    quality_path = _file(root, manifest["qualityReportPath"])
    if _hash(quality_path) != manifest.get("qualityReportSha256"):
        raise ValueError("Quality report checksum mismatch")
    files[manifest["qualityReportPath"]] = quality_path
    quality = _json(quality_path)
    if (not isinstance(quality, dict) or quality.get("dataset_id") != manifest["datasetId"] or
            quality.get("pipeline_run_id") != manifest["pipelineRunId"] or
            quality.get("source_manifest_sha256") != manifest["sourceManifestSha256"] or
            quality.get("raw_file_checksums_verified") is not True or
            quality.get("manifest_row_counts_verified") is not True or
            quality.get("reference_aggregates_used_as_input") is not False):
        raise ValueError("Quality report does not certify this source/pipeline batch")
    if "qualityReport" in manifest:
        raise ValueError("qualityReport is a reserved database metadata key")
    declared_csv = set()
    for name, metadata in manifest["tables"].items():
        validate_table_contract(name, metadata)
        names = {column["name"] for column in metadata["columns"]}
        if name in SERVING_TABLES and name != "cities" and "city_id" not in names:
            raise ValueError("Serving table lacks city ownership: " + name)
        for column in metadata["columns"]:
            if column["name"] in TABLE_KEYS[name] and column["nullable"]:
                raise ValueError("Primary keys must be non-nullable")
            if name in SERVING_TABLES and column["name"].startswith(("label_", "target_", "split_")):
                raise ValueError("Future labels/splits must not enter a serving table")
        if not isinstance(metadata.get("files"), list) or not metadata["files"]:
            raise ValueError("Every table requires at least one CSV schema shard")
        for shard in metadata["files"]:
            path = _file(root, shard["path"])
            if path.parent != root / "csv" / name or not path.name.startswith("part-") or not path.name.endswith(".csv.gz"):
                raise ValueError("CSV shard does not belong to its declared table")
            if shard["path"] in declared_csv:
                raise ValueError("A CSV shard is declared more than once")
            if type(shard.get("bytes")) is not int or shard["bytes"] < 0 or path.stat().st_size != shard["bytes"]:
                raise ValueError("CSV shard byte count mismatch")
            if _hash(path) != shard.get("sha256"):
                raise ValueError("CSV shard checksum mismatch")
            declared_csv.add(shard["path"])
            files[shard["path"]] = path
    actual_csv = {path.relative_to(root).as_posix() for path in (root / "csv").rglob("*.csv.gz")}
    if actual_csv != declared_csv:
        raise ValueError("CSV inventory differs from the serving manifest")
    schemas = _json(files["table_schemas.json"])
    expected_schemas = {"schemaVersion": CONTRACT_VERSION, "tables": {
        name: {key: metadata[key] for key in ["columns", "primaryKey"]}
        for name, metadata in manifest["tables"].items()}}
    if schemas != expected_schemas:
        raise ValueError("table_schemas.json differs from the manifest")
    return {"root": root, "manifest": manifest, "quality": quality, "files": files,
            "manifestSha256": _hash(files["serving_manifest.json"]),
            "fileSha256": {relative: _hash(path) for relative, path in files.items()}}


def _quote(identifier):
    # All identifiers passed here come from validated contracts or constants.
    return '"' + identifier + '"'


def _row_values(row, columns):
    values = []
    for column in columns:
        value = convert_csv_value(row[column["name"]], column)
        if value is not None and column["type"] == "timestamp":
            parsed = parse_utc(value)
            value = parsed.isoformat(timespec="microseconds" if parsed.microsecond else "seconds").replace("+00:00", "Z")
        values.append(value)
    record = dict(zip((column["name"] for column in columns), values))
    for column, value in zip(columns, values):
        name = column["name"]
        if value is None or column["type"] not in {"integer", "number"}:
            continue
        signed = name.startswith("net_") or name in {"cash_contribution_cents", "profit_cents", "margin_cents"}
        coordinate = name.endswith("latitude") or name.endswith("longitude")
        if not signed and not coordinate and "temperature" not in name and value < 0:
            raise ValueError("Negative count/energy/cost/measurement: " + name)
        if coordinate and not -(90 if name.endswith("latitude") else 180) <= value <= (90 if name.endswith("latitude") else 180):
            raise ValueError("Coordinate outside geographic bounds")
        if column.get("unit") == "ratio_0_1" or name == "charging_utilization" or name.endswith("_ratio"):
            if not 0 <= value <= 1:
                raise ValueError("Ratio outside [0,1]: " + name)
        if name in {"hour_of_day", "day_of_week"} and not 0 <= value <= (23 if name == "hour_of_day" else 6):
            raise ValueError("Invalid calendar feature: " + name)
    def present(*keys):
        return all(key in record and record[key] is not None for key in keys)
    if present("paid_cents", "refund_cents", "net_paid_cents") and record["net_paid_cents"] != record["paid_cents"] - record["refund_cents"]:
        raise ValueError("Net receipts do not equal payments minus refunds")
    if present("complete_charging_samples", "complete_sample_count"):
        numerator, denominator = record["complete_charging_samples"], record["complete_sample_count"]
        if numerator > denominator:
            raise ValueError("Charging samples exceed their denominator")
        if "charging_utilization" in record:
            ratio = record["charging_utilization"]
            if denominator == 0 and ratio is not None or denominator > 0 and (ratio is None or abs(ratio - numerator / denominator) > 1e-9):
                raise ValueError("Utilization does not match complete sample counts")
    state_fields = [state + "_samples" for state in STATE_NAMES]
    if present("sample_count", *state_fields) and sum(record[key] for key in state_fields) != record["sample_count"]:
        raise ValueError("State sample counts do not sum to sample_count")
    for field in ["end_available_count", "last_available_count", "observed_pile_count"]:
        if present(field, "capacity") and record[field] > record["capacity"]:
            raise ValueError("Availability/observations exceed station capacity")
    snapshot_fields = [state + "_count" for state in STATE_NAMES]
    if present("observed_pile_count", *snapshot_fields) and sum(record[key] for key in snapshot_fields) != record["observed_pile_count"]:
        raise ValueError("Snapshot states do not sum to observed piles")
    if present("unknown_count", "capacity", "observed_pile_count") and record["unknown_count"] != record["capacity"] - record["observed_pile_count"]:
        raise ValueError("Unknown pile count does not match missing coverage")
    if present("rating_count", "rating_sum") and not record["rating_count"] <= record["rating_sum"] <= 5 * record["rating_count"]:
        raise ValueError("Rating sum/count are inconsistent")
    return values


def _create_table(connection, name, metadata, temporary=False):
    columns = metadata["columns"]
    definitions = [f'{_quote(column["name"])} {SQL_TYPES[column["type"]]}' +
                   (" NOT NULL" if not column["nullable"] else "") for column in columns]
    definitions.append("PRIMARY KEY (" + ",".join(_quote(key) for key in metadata["primaryKey"]) + ")")
    connection.execute("CREATE " + ("TEMP " if temporary else "") + "TABLE " + _quote(name) + " (" + ",".join(definitions) + ")")


def _import_table(connection, inspected, name):
    metadata = inspected["manifest"]["tables"][name]
    columns = metadata["columns"]
    names = [column["name"] for column in columns]
    if name == LABEL_TABLE:
        target = TARGET_KEYS_TABLE
        keys = TABLE_KEYS[name]
        key_columns = [next(column for column in columns if column["name"] == key) for key in keys]
        _create_table(connection, target, {"columns": key_columns, "primaryKey": keys}, temporary=True)
        selected = [names.index(key) for key in keys]
    else:
        target, selected = name, list(range(len(columns)))
        _create_table(connection, name, metadata)
    statement = "INSERT INTO " + _quote(target) + " VALUES (" + ",".join("?" for _ in selected) + ")"
    count, batch = 0, []
    for shard in metadata["files"]:
        with gzip.open(inspected["files"][shard["path"]], "rt", encoding="utf-8", newline="") as stream:
            reader = csv.DictReader(stream, strict=True)
            if reader.fieldnames != names:
                raise ValueError("CSV header/order mismatch: " + name)
            for row in reader:
                if None in row or None in row.values():
                    raise ValueError("Malformed CSV record: " + name)
                values = _row_values(row, columns)
                batch.append(tuple(values[index] for index in selected))
                count += 1
                if len(batch) == 1000:
                    connection.executemany(statement, batch)
                    batch.clear()
    if batch:
        connection.executemany(statement, batch)
    if count != metadata["rows"]:
        raise ValueError("CSV row count mismatch: " + name)
    return count


def _relationships(connection, manifest):
    tables = manifest["tables"]
    def absent(sql, message):
        if connection.execute(sql + " LIMIT 1").fetchone() is not None:
            raise ValueError(message)
    for name in sorted(SERVING_TABLES - {"cities"}):
        absent(f'SELECT 1 FROM {_quote(name)} t LEFT JOIN cities c ON t.city_id=c.city_id WHERE c.city_id IS NULL',
               "Unknown/null city in " + name)
        if name != "city_daily" and name != "station_snapshot":
            absent(f'SELECT 1 FROM {_quote(name)} t LEFT JOIN station_snapshot s ON t.station_id=s.station_id '
                   'WHERE s.station_id IS NULL OR t.city_id<>s.city_id', "Unknown station or city ownership in " + name)
    absent(f'SELECT 1 FROM {_quote(TARGET_KEYS_TABLE)} t LEFT JOIN station_snapshot s ON t.station_id=s.station_id '
           'WHERE s.station_id IS NULL', "Unknown station in ML target keys")
    for left, right in [(TARGET_KEYS_TABLE, "ml_features_hourly"), ("ml_features_hourly", TARGET_KEYS_TABLE)]:
        absent(f'SELECT station_id,reference_time FROM {_quote(left)} EXCEPT '
               f'SELECT station_id,reference_time FROM {_quote(right)}', "ML feature/target key sets differ")
    station = {column["name"]: column for column in tables["station_metrics_daily"]["columns"]}
    city = {column["name"]: column for column in tables["city_daily"]["columns"]}
    additive = sorted(name for name in station.keys() & city.keys()
                      if station[name]["type"] == "integer" and name not in {"station_id", "city_id", "business_date"})
    sums = ["SUM(" + _quote(name) + ") AS " + _quote(name) for name in additive]
    grouped = 'SELECT city_id,business_date,COUNT(*) AS station_count' + ("," + ",".join(sums) if sums else "") + \
              ' FROM station_metrics_daily GROUP BY city_id,business_date'
    absent('SELECT d.city_id,d.business_date FROM (' + grouped + ') d LEFT JOIN city_daily c '
           'ON c.city_id=d.city_id AND c.business_date=d.business_date WHERE c.city_id IS NULL',
           "Missing city-day aggregate")
    absent('SELECT c.city_id,c.business_date FROM city_daily c LEFT JOIN (' + grouped + ') d '
           'ON c.city_id=d.city_id AND c.business_date=d.business_date WHERE d.city_id IS NULL',
           "Unexpected city-day aggregate")
    comparisons = [f'c.{_quote(name)} IS NOT d.{_quote(name)}' for name in additive]
    if "station_count" in city:
        comparisons.append('c.station_count IS NOT d.station_count')
    if comparisons:
        absent('SELECT 1 FROM city_daily c JOIN (' + grouped + ') d '
               'ON c.city_id=d.city_id AND c.business_date=d.business_date WHERE ' + ' OR '.join(comparisons),
               "City aggregate does not equal its station-day sums")


def publish_dataset(input_root, output_db):
    """Publish an independently validated, single-batch query-only snapshot."""
    output_arg = Path(output_db)
    if output_arg.is_symlink():
        raise FileExistsError("Output must be a fresh database path")
    output = output_arg.resolve()
    report_path = output.with_name(output.name + ".publication_report.json")
    if output.exists() or report_path.exists() or report_path.is_symlink():
        raise FileExistsError("Database/report already exists; existing publications are never replaced")
    inspected = inspect_export(input_root)
    if output.is_relative_to(inspected["root"]):
        raise ValueError("Output database must not be written inside its source export")
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=".analytics-publish-", suffix=".sqlite3", dir=output.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    temporary_report = None
    connection = None
    report_linked = database_linked = False
    try:
        connection = sqlite3.connect(str(temporary))
        connection.execute("PRAGMA journal_mode=DELETE")
        connection.execute("PRAGMA synchronous=FULL")
        connection.execute("PRAGMA trusted_schema=OFF")
        connection.execute("PRAGMA user_version=1")
        connection.execute("BEGIN IMMEDIATE")
        counts = {name: _import_table(connection, inspected, name) for name in sorted(TABLE_KEYS)}
        _relationships(connection, inspected["manifest"])
        # Recheck the pinned batch after streaming: a producer changing files
        # during publication must not produce a mixed-source query snapshot.
        if (inspected["root"] / "_RUNNING").exists():
            raise ValueError("Export changed while publishing")
        for relative, digest in inspected["fileSha256"].items():
            if _hash(_file(inspected["root"], relative)) != digest:
                raise ValueError("Export changed while publishing: " + relative)
        connection.execute("DROP TABLE " + _quote(TARGET_KEYS_TABLE))
        connection.execute("CREATE TABLE __metadata (key TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL)")
        metadata = dict(inspected["manifest"], qualityReport=inspected["quality"])
        connection.executemany("INSERT INTO __metadata VALUES (?,?)", [
            (key, json.dumps(value, ensure_ascii=False, sort_keys=True, allow_nan=False)) for key, value in metadata.items()])
        for name in sorted(SERVING_TABLES - {"cities", "station_snapshot"}):
            columns = {column["name"] for column in inspected["manifest"]["tables"][name]["columns"]}
            if "business_date" in columns:
                connection.execute(f'CREATE INDEX {_quote("idx_" + name + "_city_date")} ON {_quote(name)} (city_id,business_date)')
        connection.commit()
        if connection.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
            raise ValueError("SQLite integrity check failed")
        connection.close()
        connection = None
        with temporary.open("rb") as stream:
            os.fsync(stream.fileno())
        report = {"status": "PUBLISHED", "schemaVersion": CONTRACT_VERSION,
            "datasetId": inspected["manifest"]["datasetId"], "publishedBatchId": inspected["manifest"]["publishedBatchId"],
            "servingManifestSha256": inspected["manifestSha256"], "databaseFile": output.name,
            "databaseSha256": _hash(temporary), "databaseBytes": temporary.stat().st_size,
            "importedTables": {name: counts[name] for name in sorted(SERVING_TABLES)},
            "validatedTargetRows": counts[LABEL_TABLE], "targetLabelsImported": False,
            "fileChecksumsVerified": True, "rowCountsVerified": True,
            "foreignKeysAndCityTotalsVerified": True}
        descriptor, report_name = tempfile.mkstemp(prefix=".analytics-publication-report-", suffix=".json", dir=output.parent)
        temporary_report = Path(report_name)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(report, stream, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        # Hard-link creation is atomic and refuses an existing target. Unlike
        # os.replace(), it cannot overwrite a publication created by a racer.
        os.link(temporary_report, report_path)
        report_linked = True
        os.link(temporary, output)
        database_linked = True
        temporary.unlink()
        temporary_report.unlink()
        os.chmod(output, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
        os.chmod(report_path, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
        return report
    except sqlite3.IntegrityError as exc:
        raise ValueError("Duplicate primary key or null required field in export") from exc
    finally:
        if connection is not None:
            connection.close()
        if report_linked and not database_linked and temporary_report is not None and temporary_report.exists():
            if report_path.exists() and report_path.samefile(temporary_report):
                report_path.unlink()
        temporary.unlink(missing_ok=True)
        if temporary_report is not None:
            temporary_report.unlink(missing_ok=True)
        # A rollback journal belongs only to this uniquely named temporary DB.
        Path(str(temporary) + "-journal").unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        report = publish_dataset(args.input, args.output)
    except (OSError, ValueError, KeyError, TypeError, csv.Error, sqlite3.Error) as exc:
        parser.exit(1, "Publication failed: " + str(exc) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
