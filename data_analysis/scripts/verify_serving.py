"""Read-only SQL/API reconciliation of an actual published analytics bundle."""

import argparse
import hashlib
import json
from pathlib import Path
import sqlite3

from data_analysis.publishing.publish import inspect_export


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify(database, bundle):
    from fastapi.testclient import TestClient
    from data_analysis.backend.app import create_app

    inspected = inspect_export(bundle)
    manifest = inspected["manifest"]
    database = Path(database).resolve()
    before = digest(database)
    connection = sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row
    requests = 0
    try:
        tables = {row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        require("ml_targets_hourly" not in tables, "Labels leaked into serving database")
        metadata = {row["key"]: json.loads(row["value"]) for row in connection.execute("SELECT * FROM __metadata")}
        for key in ("datasetId", "pipelineRunId", "publishedBatchId", "sourceManifestSha256", "schemaVersion"):
            require(metadata[key] == manifest[key], "Database/bundle identity differs: " + key)
        batch = {key: manifest[key] for key in ("datasetId", "publishedBatchId")}
        numeric = ["energy_wh", "grid_cost_cents", "completed_sessions", "paid_cents", "refund_cents",
                   "operating_cost_cents", "maintenance_cost_cents", "net_paid_cents"]
        totals = dict(connection.execute("SELECT " + ",".join("SUM(" + name + ") AS " + name for name in numeric)
                                         + " FROM station_metrics_daily").fetchone())
        def camel(name):
            first, *parts = name.split("_")
            return first + "".join(part.title() for part in parts)
        with TestClient(create_app(str(database))) as client:
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
                city_sum = connection.execute("SELECT SUM(energy_wh) FROM station_metrics_daily WHERE city_id=?", (city["cityId"],)).fetchone()[0]
                require(city_sum == city_overview["energyWh"], "API city energy differs from SQL")
                for chart in ("energy", "revenue", "utilization", "states", "service", "cohorts"):
                    request("/api/v1/dashboard/charts", dict(selected, chart=chart, granularity="day"))
                request("/api/v1/dashboard/charts", dict(selected, chart="energy", granularity="hour", limit=1000))
                load = request("/api/v1/dashboard/charts", dict(selected, chart="load", granularity="hour", limit=1000))["data"]
                for point in load["items"]:
                    source = connection.execute("SELECT SUM(mean_power_kw), MIN(is_complete) FROM station_hourly_metrics WHERE city_id=? AND recorded_at=?",
                                                (city["cityId"], point["time"])).fetchone()
                    if point["isComplete"]:
                        require(source[1] == 1 and abs(point["meanPowerKw"] - source[0]) < 1e-8, "API load differs from summed station means")
                    else:
                        require(point["meanPowerKw"] is None, "Incomplete power must not be advertised as full load")
            request("/api/v1/pipeline/runs", batch)
            models = request("/api/v1/models", batch)["data"]
            require(models["implementedPrediction"] is False and not models["models"], "Untrained model advertised as ready")
            station = connection.execute("SELECT station_id FROM station_snapshot ORDER BY station_id LIMIT 1").fetchone()[0]
            for target in ("load", "availability"):
                result = request("/api/v1/predict/" + target, status=503, body=dict(
                    batch, stationId=station, referenceTime=manifest["startDate"] + "T00:00:00Z", modelId="not-published", horizonHours=6))
                require(result["code"] == "MODEL_NOT_READY", "Prediction must not fabricate values")
            require(request("/api/v1/dashboard/overview", dict(batch, publishedBatchId="wrong-batch"), 409)["code"] == "BATCH_MISMATCH", "Batch mismatch not rejected")
        require(digest(database) == before, "Read-only API changed the database")
        return {"datasetId": manifest["datasetId"], "publishedBatchId": manifest["publishedBatchId"],
                "pipelineRunId": manifest["pipelineRunId"], "sourceManifestSha256": manifest["sourceManifestSha256"],
                "databaseSha256": before, "databaseBytes": database.stat().st_size,
                "cities": len(cities), "stations": station_count, "checkedApiRequests": requests,
                "sqlApiTotalsMatch": True, "readOnlyDatabaseUnchanged": True,
                "targetLabelsImported": False, "realPredictionsImplemented": False, "totals": totals,
                "tables": {name: value["rows"] for name, value in manifest["tables"].items()}}
    finally:
        connection.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True)
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--output", required=True, help="New JSON report")
    args = parser.parse_args(argv)
    path = Path(args.output)
    if path.exists() or path.is_symlink():
        parser.error("Refusing to overwrite validation report")
    report = verify(args.database, args.bundle)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    main()
