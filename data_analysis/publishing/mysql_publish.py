"""Validate a portable export and publish one new, immutable MySQL batch schema.

Run ``python -m data_analysis.publishing.mysql_publish --input EXPORT --report NEW_JSON``.
Connection settings come only from ``ANALYTICS_MYSQL_*``. This command never
overwrites/drops a database or grants privileges. Switch the API to the new
schema only after verification; an older published schema remains available.
"""

from __future__ import annotations

import argparse
import csv
import gzip
import json
from pathlib import Path
import re

from data_analysis.contracts import CONTRACT_VERSION
from data_analysis.contracts.serving import SERVING_TABLES, TABLE_KEYS
from data_analysis.mysql_support import MySQLSettings, connect
from data_analysis.publishing.publish import (
    LABEL_TABLE, TARGET_KEYS_TABLE, _file, _hash, _row_values, inspect_export,
)


COLLATION = "utf8mb4_0900_bin"
IDENTIFIER_LIMIT = 160
TEXT_LIMIT = 4096
PUBLICATION_METADATA = {"publicationStatus": "PUBLISHED", "storageBackend": "mysql"}
_SQL_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,63}\Z")


def _quote(identifier):
    # Contracts and settings are validated again at the SQL boundary; SQL
    # values, including names in information_schema predicates, are parameters.
    if not isinstance(identifier, str) or not _SQL_IDENTIFIER.fullmatch(identifier):
        raise ValueError("Invalid or overlong MySQL identifier")
    return "`" + identifier + "`"


def _execute(connection, statement, parameters=None):
    with connection.cursor() as cursor:
        cursor.execute(statement, parameters)


def _one(connection, statement, parameters=None):
    with connection.cursor() as cursor:
        cursor.execute(statement, parameters)
        return cursor.fetchone()


def _many(connection, statement, rows):
    with connection.cursor() as cursor:
        cursor.executemany(statement, rows)


def _string_limit(column, primary_keys):
    if column["type"] == "date":
        return 10
    if column["type"] == "timestamp":
        return 32
    if column["name"] in primary_keys or column["name"].endswith("_id"):
        return IDENTIFIER_LIMIT
    return TEXT_LIMIT


def _column_type(column, primary_keys):
    kind = column["type"]
    if kind in {"integer", "number", "boolean"}:
        return {"integer": "BIGINT", "number": "DOUBLE", "boolean": "TINYINT"}[kind]
    if kind not in {"string", "date", "timestamp"}:
        raise ValueError("Unsupported MySQL column type")
    limit = _string_limit(column, primary_keys)
    # TEXT avoids MySQL's 65KB in-row VARCHAR limit on wide feature tables.
    return ("TEXT" if limit == TEXT_LIMIT else "VARCHAR(" + str(limit) + ")") + \
        " CHARACTER SET utf8mb4 COLLATE " + COLLATION


def _bounded_row_values(row, columns, primary_keys):
    values = _row_values(row, columns)
    for column, value in zip(columns, values):
        if value is None or column["type"] not in {"string", "date", "timestamp"}:
            continue
        limit = _string_limit(column, primary_keys)
        if len(value) > limit or len(value.encode("utf-8")) > limit * 4:
            raise ValueError("String exceeds MySQL storage contract: " + column["name"])
    return values


def _create_table(connection, name, metadata, *, temporary=False):
    keys = metadata["primaryKey"]
    definitions = [
        _quote(column["name"]) + " " + _column_type(column, keys) +
        (" NOT NULL" if not column["nullable"] else "")
        for column in metadata["columns"]
    ]
    definitions.append("PRIMARY KEY (" + ",".join(_quote(key) for key in keys) + ")")
    column_names = {column["name"] for column in metadata["columns"]}
    if not temporary and name not in {"cities", "station_snapshot"} and "business_date" in column_names:
        definitions.append("KEY " + _quote("idx_" + name + "_city_date") + " (`city_id`,`business_date`)")
    _execute(connection, "CREATE " + ("TEMPORARY " if temporary else "") + "TABLE " + _quote(name) +
             " (" + ",".join(definitions) + ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE " + COLLATION)


def _prepare_schema(connection, database, manifest):
    version = str(_one(connection, "SELECT VERSION()")[0])
    match = re.match(r"(\d+)\.(\d+)\.", version)
    if not match or tuple(map(int, match.groups())) < (8, 4) or "mariadb" in version.lower():
        raise ValueError("Publication requires MySQL 8.4 or newer; MariaDB is not validated")
    collation = _one(connection, "SELECT PAD_ATTRIBUTE FROM information_schema.COLLATIONS WHERE COLLATION_NAME=%s",
                     (COLLATION,))
    if collation is None or collation[0] != "NO PAD":
        raise ValueError("MySQL must provide utf8mb4_0900_bin with NO PAD comparisons")
    _execute(connection, "SET SESSION sql_mode='STRICT_ALL_TABLES,ONLY_FULL_GROUP_BY,NO_ZERO_IN_DATE,NO_ZERO_DATE,"
             "NO_ENGINE_SUBSTITUTION,ERROR_FOR_DIVISION_BY_ZERO'")
    # CREATE DATABASE, not IF NOT EXISTS: even an empty existing schema is not
    # ours to reuse. MySQL's server-side name collision check is race-safe.
    _execute(connection, "CREATE DATABASE " + _quote(database) + " CHARACTER SET utf8mb4 COLLATE " + COLLATION)
    _execute(connection, "USE " + _quote(database))
    for name in sorted(SERVING_TABLES):
        _create_table(connection, name, manifest["tables"][name])
    target = manifest["tables"][LABEL_TABLE]
    keys = target["primaryKey"]
    _create_table(connection, TARGET_KEYS_TABLE, {
        "columns": [next(column for column in target["columns"] if column["name"] == key) for key in keys],
        "primaryKey": keys,
    }, temporary=True)
    _execute(connection, "CREATE TABLE `__metadata` (`key` VARCHAR(160) CHARACTER SET utf8mb4 COLLATE " +
             COLLATION + " PRIMARY KEY NOT NULL, `value` LONGTEXT NOT NULL) ENGINE=InnoDB "
             "DEFAULT CHARACTER SET utf8mb4 COLLATE " + COLLATION)


def _import_table(connection, inspected, name):
    metadata = inspected["manifest"]["tables"][name]
    columns, keys = metadata["columns"], metadata["primaryKey"]
    names = [column["name"] for column in columns]
    target = TARGET_KEYS_TABLE if name == LABEL_TABLE else name
    selected = [names.index(key) for key in keys] if name == LABEL_TABLE else list(range(len(columns)))
    statement = "INSERT INTO " + _quote(target) + " VALUES (" + ",".join("%s" for _ in selected) + ")"
    count, batch = 0, []
    for shard in metadata["files"]:
        with gzip.open(inspected["files"][shard["path"]], "rt", encoding="utf-8", newline="") as stream:
            reader = csv.DictReader(stream, strict=True)
            if reader.fieldnames != names:
                raise ValueError("CSV header/order mismatch: " + name)
            for row in reader:
                if None in row or None in row.values():
                    raise ValueError("Malformed CSV record: " + name)
                # Future target values are checked, but only the two key
                # columns enter a private, connection-scoped temporary table.
                values = _bounded_row_values(row, columns, keys)
                batch.append(tuple(values[index] for index in selected))
                count += 1
                if len(batch) == 1000:
                    _many(connection, statement, batch)
                    batch.clear()
    if batch:
        _many(connection, statement, batch)
    if count != metadata["rows"]:
        raise ValueError("CSV row count mismatch: " + name)
    return count


def _relationships(connection, manifest):
    def absent(statement, message):
        if _one(connection, statement + " LIMIT 1") is not None:
            raise ValueError(message)

    for name in sorted(SERVING_TABLES - {"cities"}):
        absent("SELECT 1 FROM " + _quote(name) + " t LEFT JOIN cities c ON t.city_id=c.city_id WHERE c.city_id IS NULL",
               "Unknown/null city in " + name)
        if name not in {"city_daily", "station_snapshot"}:
            absent("SELECT 1 FROM " + _quote(name) + " t LEFT JOIN station_snapshot s ON t.station_id=s.station_id "
                   "WHERE s.station_id IS NULL OR NOT(t.city_id <=> s.city_id)",
                   "Unknown station or city ownership in " + name)
    absent("SELECT 1 FROM " + _quote(TARGET_KEYS_TABLE) +
           " t LEFT JOIN station_snapshot s ON t.station_id=s.station_id WHERE s.station_id IS NULL",
           "Unknown station in ML target keys")
    # Anti-joins work on every supported MySQL 8 version and avoid SQLite's
    # EXCEPT syntax. Each statement references the temporary table only once.
    for left, right in [(TARGET_KEYS_TABLE, "ml_features_hourly"), ("ml_features_hourly", TARGET_KEYS_TABLE)]:
        absent("SELECT 1 FROM " + _quote(left) + " a LEFT JOIN " + _quote(right) +
               " b ON a.station_id=b.station_id AND a.reference_time=b.reference_time WHERE b.station_id IS NULL",
               "ML feature/target key sets differ")
    tables = manifest["tables"]
    station = {column["name"]: column for column in tables["station_metrics_daily"]["columns"]}
    city = {column["name"]: column for column in tables["city_daily"]["columns"]}
    additive = sorted(name for name in station.keys() & city.keys()
                      if station[name]["type"] == "integer" and name not in {"station_id", "city_id", "business_date"})
    sums = ["SUM(" + _quote(name) + ") AS " + _quote(name) for name in additive]
    grouped = "SELECT city_id,business_date,COUNT(*) AS station_count" + ("," + ",".join(sums) if sums else "") + \
        " FROM station_metrics_daily GROUP BY city_id,business_date"
    absent("SELECT 1 FROM (" + grouped + ") d LEFT JOIN city_daily c "
           "ON c.city_id=d.city_id AND c.business_date=d.business_date WHERE c.city_id IS NULL",
           "Missing city-day aggregate")
    absent("SELECT 1 FROM city_daily c LEFT JOIN (" + grouped + ") d "
           "ON c.city_id=d.city_id AND c.business_date=d.business_date WHERE d.city_id IS NULL",
           "Unexpected city-day aggregate")
    comparisons = ["NOT(c." + _quote(name) + " <=> d." + _quote(name) + ")" for name in additive]
    if "station_count" in city:
        comparisons.append("NOT(c.station_count <=> d.station_count)")
    if comparisons:
        absent("SELECT 1 FROM city_daily c JOIN (" + grouped + ") d "
               "ON c.city_id=d.city_id AND c.business_date=d.business_date WHERE " + " OR ".join(comparisons),
               "City aggregate does not equal its station-day sums")


def publish_mysql(input_root, settings=None):
    """Publish all validated rows and the ready marker in one InnoDB commit.

    DDL is deliberately finished before the transaction starts because MySQL
    DDL can implicitly commit. Failure leaves an empty, unpublished schema for
    diagnosis; never reuse it or delete it automatically. A new unique schema
    is required for a retry. No API configuration is changed by this function.
    """
    inspected = inspect_export(input_root)
    if set(PUBLICATION_METADATA) & set(inspected["manifest"]):
        raise ValueError("Export uses a reserved MySQL publication metadata key")
    settings = settings or MySQLSettings.from_env()
    _quote(settings.database)
    # Preflight all SQL identifiers/types before creating a server-side schema.
    for name, metadata in inspected["manifest"]["tables"].items():
        _quote(name)
        for column in metadata["columns"]:
            _quote(column["name"])
            _column_type(column, metadata["primaryKey"])
    connection = connect(settings, database="")
    try:
        connection.autocommit(True)
        _prepare_schema(connection, settings.database, inspected["manifest"])
        connection.begin()
        counts = {name: _import_table(connection, inspected, name) for name in sorted(TABLE_KEYS)}
        _relationships(connection, inspected["manifest"])
        if (inspected["root"] / "_RUNNING").exists() or (inspected["root"] / "_RUNNING").is_symlink():
            raise ValueError("Export changed while publishing")
        for relative, digest in inspected["fileSha256"].items():
            if _hash(_file(inspected["root"], relative)) != digest:
                raise ValueError("Export changed while publishing: " + relative)
        metadata = dict(inspected["manifest"], qualityReport=inspected["quality"], **PUBLICATION_METADATA)
        for key in metadata:
            if not isinstance(key, str) or not 0 < len(key) <= IDENTIFIER_LIMIT:
                raise ValueError("Invalid MySQL metadata key")
        _many(connection, "INSERT INTO `__metadata` (`key`,`value`) VALUES (%s,%s)", [
            (key, json.dumps(value, ensure_ascii=False, sort_keys=True, allow_nan=False))
            for key, value in metadata.items()
        ])
        connection.commit()
        # A failed/uncertain commit raises instead of returning PUBLISHED. If
        # connectivity is lost at commit, inspect this schema before retrying
        # with another name: it might have committed even without an ACK.
        return {
            "status": "PUBLISHED", "storageBackend": "mysql", "schemaVersion": CONTRACT_VERSION,
            "datasetId": inspected["manifest"]["datasetId"],
            "publishedBatchId": inspected["manifest"]["publishedBatchId"],
            "servingManifestSha256": inspected["manifestSha256"], "databaseSchema": settings.database,
            "importedTables": {name: counts[name] for name in sorted(SERVING_TABLES)},
            "validatedTargetRows": counts[LABEL_TABLE], "targetLabelsImported": False,
            "fileChecksumsVerified": True, "rowCountsVerified": True,
            "foreignKeysAndCityTotalsVerified": True, "singleTransactionCommitted": True,
            "apiConfigurationChanged": False,
        }
    except Exception:
        try:
            connection.rollback()
        except Exception:
            pass  # Preserve the original import/commit error, never report success.
        raise
    finally:
        connection.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args(argv)
    if args.report.exists() or args.report.is_symlink():
        parser.exit(1, "Publication report already exists; choose a new report path.\n")
    if args.report.resolve().is_relative_to(args.input.resolve()):
        parser.exit(1, "Publication report must not be written inside the source export.\n")
    try:
        report = publish_mysql(args.input)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
    except Exception as exc:
        # Drivers can include usernames, hosts or SQL row contents in errors.
        # Deliberately keep the CLI summary credential/data-free; Python callers
        # retain the original exception for controlled diagnostics.
        parser.exit(1, "Publication failed (" + type(exc).__name__ + "). No API configuration was changed. "
                    "Inspect the target schema before retrying; an interrupted commit may need verification.\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
