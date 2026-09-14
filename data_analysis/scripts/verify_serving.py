"""Read-only SQL/API reconciliation of an actual published analytics bundle."""

import argparse
import hashlib
import json
from pathlib import Path
from decimal import Decimal
import re

from data_analysis.publishing.publish import inspect_export
from data_analysis.contracts.serving import SERVING_TABLES, TABLE_KEYS


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def mysql_content_digest(snapshot):
    """Hash ordered logical values in bounded chunks, never server data files."""
    import pymysql
    digest_value = hashlib.sha256()
    for table in sorted(SERVING_TABLES | {"__metadata"}):
        keys = TABLE_KEYS[table] if table != "__metadata" else ["key"]
        statement = "SELECT * FROM `" + table + "` ORDER BY " + ",".join("`" + key + "`" for key in keys)
        digest_value.update((table + "\n").encode())
        with snapshot.connection.cursor(pymysql.cursors.SSDictCursor) as cursor:
            cursor.execute(statement)
            while True:
                rows = cursor.fetchmany(500)
                if not rows:
                    break
                for row in rows:
                    values = {key: (int(value) if value.as_tuple().exponent >= 0 else float(value))
                              if isinstance(value, Decimal) else value for key, value in row.items()}
                    digest_value.update(json.dumps(values, sort_keys=True, ensure_ascii=False,
                        separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n")
    return digest_value.hexdigest()


def require_mysql_readonly_account(settings):
    """Require a direct SELECT-only account, rejecting roles and grant options.

    Roles/dynamic privileges are deliberately not inferred from one failed
    write: UPDATE denied does not prove INSERT/DELETE/DDL are also denied.
    A safe zero-row UPDATE is an additional check outside the API transaction.
    """
    import pymysql
    from data_analysis.mysql_support import connect
    connection = connect(settings)
    try:
        with connection.cursor() as cursor:
            cursor.execute("SELECT CURRENT_ROLE()")
            if cursor.fetchone()[0] != "NONE":
                raise ValueError("Verification requires a direct SELECT-only account without active roles")
            cursor.execute("SHOW GRANTS")
            grants = [row[0] for row in cursor.fetchall()]
            if not grants or any(not re.fullmatch(r"GRANT (USAGE|SELECT) ON .+ TO .+", grant)
                                 or " WITH GRANT OPTION" in grant for grant in grants):
                raise ValueError("Verification requires a direct SELECT-only account without roles or grant options")
            try:
                cursor.execute("UPDATE __metadata SET `value`=`value` WHERE 1=0")
            except pymysql.err.OperationalError as exc:
                if exc.args[0] in {1142, 1143}:
                    return True
                raise
        raise ValueError("API account has UPDATE privilege; configure a SELECT-only account")
    finally:
        connection.rollback()
        connection.close()


def verify(database, bundle, *, mysql_settings=None):
    from fastapi.testclient import TestClient
    from data_analysis.backend.app import create_app
    from data_analysis.backend.database import open_snapshot

    inspected = inspect_export(bundle)
    manifest = inspected["manifest"]
    if database is not None and mysql_settings is not None:
        raise ValueError("Choose one database backend")
    mysql = database is None
    if mysql:
        from data_analysis.mysql_support import MySQLSettings
        mysql_settings = mysql_settings or MySQLSettings.from_env()
        require_mysql_readonly_account(mysql_settings)
    else:
        database = Path(database).resolve()
    before = None if mysql else digest(database)
    requests = 0
    with open_snapshot(database, mysql_settings=mysql_settings) as snapshot:
        tables = {row["name"] for row in snapshot.rows(
            "SELECT TABLE_NAME AS name FROM information_schema.tables WHERE TABLE_SCHEMA=DATABASE() AND TABLE_TYPE='BASE TABLE'"
            if mysql else "SELECT name FROM sqlite_master WHERE type='table'")}
        require(tables == SERVING_TABLES | {"__metadata"}, "Unexpected/missing serving tables")
        require("ml_targets_hourly" not in tables, "Labels leaked into serving database")
        metadata = snapshot.metadata
        if mysql:
            before = mysql_content_digest(snapshot)
        for key in ("datasetId", "pipelineRunId", "publishedBatchId", "sourceManifestSha256", "schemaVersion"):
            require(metadata[key] == manifest[key], "Database/bundle identity differs: " + key)
        batch = {key: manifest[key] for key in ("datasetId", "publishedBatchId")}
        numeric = ["energy_wh", "grid_cost_cents", "completed_sessions", "paid_cents", "refund_cents",
                   "operating_cost_cents", "maintenance_cost_cents", "net_paid_cents"]
        totals = snapshot.one("SELECT " + ",".join("SUM(" + name + ") AS " + name for name in numeric)
                              + " FROM station_metrics_daily")
        def camel(name):
            first, *parts = name.split("_")
            return first + "".join(part.title() for part in parts)
        with TestClient(create_app(None if mysql else str(database), mysql_settings=mysql_settings)) as client:
            def request(path, params=None, status=200, body=None):
                nonlocal requests
                requests += 1
                response = client.get(path, params=params) if body is None else client.post(path, json=body)
                require(response.status_code == status, "Unexpected HTTP status: " + path)
                result = response.json()
                require(set(result) == {"code", "message", "data", "meta"}, "Response envelope mismatch")
                require(result["meta"]["publishedBatchId"] == batch["publishedBatchId"], "API mixed batches")
                require(response.headers["X-Request-ID"] == result["meta"]["requestId"], "Request ID mismatch")
                return result
            request("/api/v1/health", batch)
            request("/api/v1/datasets", batch)
            cities = request("/api/v1/cities", batch)["data"]["items"]
            all_overview = request("/api/v1/dashboard/overview", batch)["data"]["metrics"]
            require(all(all_overview[camel(name)] == value for name, value in totals.items()), "API global totals differ from SQL")
            station_count = 0
            for city in cities:
                selected = dict(batch, cityId=city["cityId"])
                stations = request("/api/v1/stations", dict(selected, pageSize=100, sortBy="energy", sortOrder="desc"))["data"]
                station_count += stations["total"]
                require(len(stations["items"]) == stations["total"], "Unexpected truncated city station list")
                require(all(station["cityId"] == city["cityId"] for station in stations["items"]), "Station/city mismatch")
                city_overview = request("/api/v1/dashboard/overview", selected)["data"]["metrics"]
                city_sum = snapshot.one("SELECT SUM(energy_wh) AS energy FROM station_metrics_daily WHERE city_id=?", (city["cityId"],))["energy"]
                require(city_sum == city_overview["energyWh"], "API city energy differs from SQL")
                for chart in ("energy", "revenue", "utilization", "states", "service", "cohorts"):
                    request("/api/v1/dashboard/charts", dict(selected, chart=chart, granularity="day"))
                request("/api/v1/dashboard/charts", dict(selected, chart="energy", granularity="hour", limit=1000))
                load = request("/api/v1/dashboard/charts", dict(selected, chart="load", granularity="hour", limit=1000))["data"]
                for point in load["items"]:
                    source = snapshot.one("SELECT SUM(mean_power_kw) AS power, MIN(is_complete) AS complete FROM station_hourly_metrics WHERE city_id=? AND recorded_at=?",
                                          (city["cityId"], point["time"]))
                    if point["isComplete"]:
                        require(source["complete"] == 1 and abs(point["meanPowerKw"] - source["power"]) < 1e-8, "API load differs from summed station means")
                    else:
                        require(point["meanPowerKw"] is None, "Incomplete power must not be advertised as full load")
            request("/api/v1/pipeline/runs", batch)
            models = request("/api/v1/models", batch)["data"]
            require(models["implementedPrediction"] is False and not models["models"], "Untrained model advertised as ready")
            station = snapshot.one("SELECT station_id FROM station_snapshot ORDER BY station_id LIMIT 1")["station_id"]
            for target in ("load", "availability"):
                result = request("/api/v1/predict/" + target, status=503, body=dict(
                    batch, stationId=station, referenceTime=manifest["startDate"] + "T00:00:00Z", modelId="not-published", horizonHours=6))
                require(result["code"] == "MODEL_NOT_READY", "Prediction must not fabricate values")
            require(request("/api/v1/dashboard/overview", dict(batch, publishedBatchId="wrong-batch"), 409)["code"] == "BATCH_MISMATCH", "Batch mismatch not rejected")
    if mysql:
        # Fresh transaction: a repeatable-read snapshot alone would conceal
        # writes committed by another connection during the API checks.
        with open_snapshot(mysql_settings=mysql_settings) as snapshot:
            after = mysql_content_digest(snapshot)
    else:
        after = digest(database)
    require(after == before, "Published database changed during verification")
    storage = ({"storageBackend": "mysql", "database": mysql_settings.database,
                "logicalContentSha256": before, "readonlyAccountVerified": True} if mysql else
               {"storageBackend": "sqlite", "databaseSha256": before, "databaseBytes": database.stat().st_size})
    return {"datasetId": manifest["datasetId"], "publishedBatchId": manifest["publishedBatchId"],
                "pipelineRunId": manifest["pipelineRunId"], "sourceManifestSha256": manifest["sourceManifestSha256"],
                **storage,
                "cities": len(cities), "stations": station_count, "checkedApiRequests": requests,
                "sqlApiTotalsMatch": True, "readOnlyDatabaseUnchanged": True,
                "targetLabelsImported": False, "realPredictionsImplemented": False, "totals": totals,
                "tables": {name: value["rows"] for name, value in manifest["tables"].items()}}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", help="Explicit legacy offline SQLite path, not a connection URL")
    parser.add_argument("--database-backend", choices=["mysql", "sqlite"], default=None,
        help="Defaults to mysql; explicit --database retains offline SQLite compatibility")
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--output", required=True, help="New JSON report")
    args = parser.parse_args(argv)
    backend = args.database_backend or ("sqlite" if args.database else "mysql")
    if (backend == "mysql" and args.database) or (backend == "sqlite" and not args.database):
        parser.error("MySQL uses environment configuration; SQLite requires --database PATH")
    path = Path(args.output)
    if path.exists() or path.is_symlink():
        parser.error("Refusing to overwrite validation report")
    try:
        report = verify(args.database, args.bundle)
    except Exception as exc:
        parser.exit(1, "Database verification failed (" + type(exc).__name__ + "). Check configuration, permissions and batch data; credentials are not printed.\n")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    main()
