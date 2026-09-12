"""Capture real API handoff examples from one read-only published database."""

import argparse
import json
from datetime import date, timedelta
from pathlib import Path


def export_examples(database, output):
    from fastapi.testclient import TestClient
    from data_analysis.backend.app import create_app

    target = Path(output)
    if target.exists() or target.is_symlink():
        raise FileExistsError("Choose a new examples directory")
    with TestClient(create_app(str(database))) as client:
        response = client.get("/api/v1/datasets")
        response.raise_for_status()
        dataset = response.json()["data"]["items"][0]
        batch = {key: dataset[key] for key in ("datasetId", "publishedBatchId")}
        start = date.fromisoformat(dataset["startDate"])
        end = min(start + timedelta(days=2), date.fromisoformat(dataset["endDate"]))
        cities = client.get("/api/v1/cities", params=batch).json()["data"]["items"]
        city = cities[0]["cityId"]
        stations = client.get("/api/v1/stations", params=dict(batch, cityId=city)).json()["data"]["items"]
        filters = dict(batch, cityId=city, stationId=stations[0]["stationId"],
                       startDate=start.isoformat(), endDate=end.isoformat())
        prediction = dict(batch, stationId=stations[0]["stationId"], modelId="not-yet-published",
                          referenceTime=(start + timedelta(days=1)).isoformat() + "T00:00:00Z", horizonHours=6)
        cases = [
            ("datasets", "GET", "/api/v1/datasets", {}, 200),
            ("cities", "GET", "/api/v1/cities", batch, 200),
            ("stations", "GET", "/api/v1/stations", dict(batch, cityId=city, sortBy="netPaid", sortOrder="desc"), 200),
            ("overview", "GET", "/api/v1/dashboard/overview", filters, 200),
            ("energy_hourly", "GET", "/api/v1/dashboard/charts", dict(filters, chart="energy", granularity="hour"), 200),
            ("load_hourly", "GET", "/api/v1/dashboard/charts", dict(filters, chart="load", granularity="hour"), 200),
            ("quality", "GET", "/api/v1/pipeline/runs", batch, 200),
            ("models", "GET", "/api/v1/models", batch, 200),
            ("prediction_not_ready", "POST", "/api/v1/predict/load", prediction, 503),
            ("batch_mismatch", "GET", "/api/v1/dashboard/overview", dict(batch, publishedBatchId="wrong-batch"), 409),
        ]
        results = {}
        for name, method, route, values, status in cases:
            response = (client.get(route, params=values) if method == "GET" else client.post(route, json=values))
            if response.status_code != status:
                raise ValueError("Example endpoint returned unexpected status: " + route)
            results[name] = {"method": method, "path": route,
                             "query" if method == "GET" else "body": values,
                             "httpStatus": response.status_code, "response": response.json()}
    target.mkdir(parents=True, exist_ok=False)
    for name, value in results.items():
        with (target / (name + ".json")).open("x", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
    return {"datasetId": dataset["datasetId"], "publishedBatchId": dataset["publishedBatchId"], "examples": len(results)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    print(json.dumps(export_examples(args.database, args.output), ensure_ascii=False))


if __name__ == "__main__":
    main()
