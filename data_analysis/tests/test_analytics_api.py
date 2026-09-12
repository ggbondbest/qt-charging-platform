"""HTTP tests against independent tiny SQLite snapshots, without Spark/network.

Install requirements-api.txt before running this module. The general stdlib
suite explicitly skips it when optional HTTP dependencies are unavailable.
"""

import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

HAS_API = all(importlib.util.find_spec(name) is not None for name in ("fastapi", "httpx"))
if HAS_API:
    from fastapi.testclient import TestClient
    from data_analysis.backend.app import create_app, export_openapi
    from data_analysis.backend.database import open_snapshot
    from data_analysis.backend.service import COHORT_COLUMNS, DAILY_COLUMNS, SERVICE_COLUMNS, STATE_COLUMNS


def fixture(path):
    connection = sqlite3.connect(path)
    connection.execute("CREATE TABLE __metadata (key TEXT PRIMARY KEY, value TEXT)")
    metadata = dict(schemaVersion="1.0.0", datasetId="fixture", publishedBatchId="batch-1", pipelineRunId="run-1",
        generatedAt="2026-01-01T00:00:00Z", startDate="2025-12-01", endDate="2025-12-04", source="SIMULATED",
        sourceManifestSha256="f"*64, tables={}, qualityReport={"input_rows": {"charging_sessions": 100},
        "rejected_session_rows": 2, "normalized_enum_rows": {"charging_sessions": 1},
        "clean_session_rows": 98, "rejection_reasons": {"DUPLICATE_SESSION": 2},
        "rejection_samples": [{"session_id": "SES-1", "rejection_reason": "DUPLICATE_SESSION", "raw_json": "/private/never-expose-row"}],
        "input": "/private/never-expose-source", "output": "/private/never-expose-output"})
    connection.executemany("INSERT INTO __metadata VALUES (?, ?)", [(key, json.dumps(value)) for key, value in metadata.items()])
    connection.execute("CREATE TABLE cities (city_id TEXT, city_name TEXT, latitude REAL, longitude REAL, timezone TEXT)")
    connection.executemany("INSERT INTO cities VALUES (?,?,?,?,?)", [("C1","Alpha",39.1,121.1,"Asia/Shanghai"),("C2","Beta",40.1,122.1,"Asia/Shanghai")])
    snapshot_fields = {"station_id": "TEXT", "city_id": "TEXT", "station_name": "TEXT", "city_name": "TEXT", "site_type": "TEXT",
        "latitude": "REAL", "longitude": "REAL", "city_latitude": "REAL", "city_longitude": "REAL", "capacity": "INTEGER",
        "rated_capacity_kw": "REAL", "transformer_kw": "REAL", "snapshot_at": "TEXT", "data_as_of": "TEXT",
        "observed_pile_count": "INTEGER", "available_count": "INTEGER", "charging_count": "INTEGER", "reserved_count": "INTEGER",
        "occupied_count": "INTEGER", "maintenance_count": "INTEGER", "offline_count": "INTEGER", "unknown_count": "INTEGER",
        "is_current": "INTEGER", "is_complete": "INTEGER"}
    connection.execute("CREATE TABLE station_snapshot ("+", ".join(f"{k} {v}" for k,v in snapshot_fields.items())+")")
    for sid, cid in (("S1","C1"),("S2","C1"),("S3","C2")):
        values = dict.fromkeys(snapshot_fields, 0)
        values.update(station_id=sid, city_id=cid, station_name=sid+" station", city_name="Alpha" if cid=="C1" else "Beta",
            site_type="OFFICE", latitude=39.1, longitude=121.1, city_latitude=39, city_longitude=121,
            capacity=2, rated_capacity_kw=100, transformer_kw=200, snapshot_at="2025-12-03T15:55:00Z",
            data_as_of="2025-12-03T15:55:00Z", observed_pile_count=2, available_count=1, charging_count=1, is_current=1, is_complete=1)
        connection.execute("INSERT INTO station_snapshot VALUES ("+",".join("?" for _ in values)+")", tuple(values.values()))
    for table, columns in (("station_metrics_daily",DAILY_COLUMNS),("station_cohorts_daily",COHORT_COLUMNS),("station_service_daily",SERVICE_COLUMNS)):
        connection.execute(f"CREATE TABLE {table} (station_id TEXT, city_id TEXT, business_date TEXT,"+",".join(f"{key} INTEGER" for key in columns)+")")

    def insert(table, columns, station, day, **changes):
        values = dict.fromkeys(columns, 0)
        values.update(changes)
        connection.execute(f"INSERT INTO {table} VALUES ("+",".join("?" for _ in range(len(columns)+3))+")",
                           (station,"C2" if station=="S3" else "C1",day,*values.values()))

    insert("station_metrics_daily",DAILY_COLUMNS,"S1","2025-12-01", energy_wh=1000, paid_cents=1000, refund_cents=100, net_paid_cents=900,
        started_sessions=1, completed_sessions=1, observed_hours=1, complete_hours=1, charging_samples=1, occupied_samples=1, sample_count=2,
        expected_sample_count=2, complete_charging_samples=1, complete_sample_count=2)
    insert("station_metrics_daily",DAILY_COLUMNS,"S1","2025-12-02", energy_wh=2000, paid_cents=2000, net_paid_cents=2000,
        started_sessions=1, completed_sessions=1, observed_hours=1, complete_hours=1, charging_samples=1, sample_count=4,
        expected_sample_count=4, complete_charging_samples=1, complete_sample_count=4)
    insert("station_metrics_daily",DAILY_COLUMNS,"S2","2025-12-01", energy_wh=7000, paid_cents=7000, net_paid_cents=7000,
        started_sessions=1, completed_sessions=1, observed_hours=1, incomplete_hours=1, charging_samples=1, sample_count=1,
        expected_sample_count=2, missing_sample_count=1)
    insert("station_metrics_daily",DAILY_COLUMNS,"S3","2025-12-01", energy_wh=3000, paid_cents=3000, net_paid_cents=3000,
        observed_hours=1, complete_hours=1, charging_samples=2, sample_count=2, expected_sample_count=2,
        complete_charging_samples=2, complete_sample_count=2)
    insert("station_metrics_daily",DAILY_COLUMNS,"S1","2025-12-03", expected_sample_count=2,missing_sample_count=2,incomplete_hours=1)
    insert("station_cohorts_daily",COHORT_COLUMNS,"S1","2025-12-01",reservations_created_count=2,reservation_cancelled_count=1,
        reservation_confirmed_count=1,queues_joined_count=3,queue_served_count=2,queue_abandoned_count=1)
    insert("station_service_daily",SERVICE_COLUMNS,"S1","2025-12-01",queues_resolved_count=2,queue_wait_count=2,
        queue_wait_seconds_sum=120,queue_sojourn_seconds_sum=180,rating_count=2,rating_sum=9)
    insert("station_service_daily",SERVICE_COLUMNS,"S2","2025-12-01",queues_resolved_count=1,queue_wait_count=1,
        queue_wait_seconds_sum=360,queue_sojourn_seconds_sum=360,rating_count=1,rating_sum=1)
    connection.execute("CREATE TABLE user_activity_daily (station_id TEXT, city_id TEXT, business_date TEXT, user_id TEXT, session_count INTEGER, first_started_at TEXT, last_started_at TEXT)")
    connection.executemany("INSERT INTO user_activity_daily VALUES (?,?,?,?,?,?,?)", [
        ("S1","C1","2025-12-01","U1",1,"",""),("S2","C1","2025-12-01","U1",1,"",""),
        ("S1","C1","2025-12-02","U2",1,"",""),("S3","C2","2025-12-01","U3",2,"","")])
    hourly_columns = ["energy_wh","sample_count","expected_sample_count","missing_sample_count"]+STATE_COLUMNS+["is_complete"]
    connection.execute("CREATE TABLE station_hourly_metrics (station_id TEXT,city_id TEXT,business_date TEXT,recorded_at TEXT,"+",".join(f"{key} INTEGER" for key in hourly_columns)+")")
    for station, day, hour, changes in [
        ("S1","2025-12-01","2025-11-30T16:00:00Z",dict(energy_wh=1000,sample_count=2,expected_sample_count=2,charging_samples=1,occupied_samples=1,is_complete=1)),
        ("S1","2025-12-02","2025-12-01T16:00:00Z",dict(energy_wh=2000,sample_count=4,expected_sample_count=4,charging_samples=1,is_complete=1)),
        ("S2","2025-12-01","2025-11-30T16:00:00Z",dict(energy_wh=7000,sample_count=1,expected_sample_count=2,missing_sample_count=1,charging_samples=1)),
        ("S3","2025-12-01","2025-11-30T16:00:00Z",dict(energy_wh=3000,sample_count=2,expected_sample_count=2,charging_samples=2,is_complete=1)),
        ("S1","2025-12-03","2025-12-02T16:00:00Z",dict(energy_wh=None,expected_sample_count=2,missing_sample_count=2))]:
        values = dict.fromkeys(hourly_columns,0)
        values.update(changes)
        connection.execute("INSERT INTO station_hourly_metrics VALUES ("+",".join("?" for _ in range(len(hourly_columns)+4))+")",
            (station,"C2" if station=="S3" else "C1",day,hour,*values.values()))
    connection.execute("ALTER TABLE station_hourly_metrics ADD COLUMN mean_power_kw REAL")
    connection.execute("UPDATE station_hourly_metrics SET mean_power_kw=energy_wh/1000.0")
    connection.commit()
    connection.close()
    return metadata


@unittest.skipUnless(HAS_API, "Install optional requirements-api.txt to run HTTP API tests")
class AnalyticsApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="analytics-api-tests-")
        cls.root = Path(cls.temporary.name)
        cls.path = cls.root/"published.sqlite"
        cls.metadata = fixture(cls.path)
        cls.client = TestClient(create_app(cls.path))

    @classmethod
    def tearDownClass(cls):
        cls.client.close()
        cls.temporary.cleanup()

    def assert_envelope(self, response, status=200, code="OK"):
        self.assertEqual(response.status_code,status,response.text)
        body=response.json()
        self.assertEqual(set(body),{"code","message","data","meta"})
        self.assertEqual(body["code"],code)
        self.assertEqual(body["meta"]["requestId"],response.headers["X-Request-ID"])
        self.assertEqual(body["meta"]["datasetId"],"fixture")
        self.assertEqual(body["meta"]["publishedBatchId"],"batch-1")
        self.assertNotIn(str(self.root),response.text)
        return body["data"]

    def test_all_get_endpoints_trace_same_publication(self):
        for endpoint in ("health","datasets","cities","stations","pipeline/runs","dashboard/overview","dashboard/charts","models"):
            with self.subTest(endpoint=endpoint):
                self.assert_envelope(self.client.get("/api/v1/"+endpoint))

    def test_overview_deduplicates_users_and_uses_weighted_denominators(self):
        data=self.assert_envelope(self.client.get("/api/v1/dashboard/overview",params={"cityId":"C1"}))
        m=data["metrics"]
        self.assertEqual(m["energyWh"],10000)
        self.assertEqual(m["netPaidCents"],9900)
        self.assertEqual((m["activeUsers"],m["repeatUsers"]),(2,1))
        self.assertAlmostEqual(m["chargingUtilizationRate"],2/6)
        self.assertAlmostEqual(m["chargingAndOccupiedRate"],3/6)
        self.assertEqual(m["queueMeanWaitSeconds"],160)
        self.assertAlmostEqual(m["meanRating"],10/3)

    def test_exclusive_dates_and_station_filter_apply_consistently(self):
        params={"cityId":"C1","stationId":"S1","startDate":"2025-12-01","endDate":"2025-12-02"}
        overview=self.assert_envelope(self.client.get("/api/v1/dashboard/overview",params=params))
        stations=self.assert_envelope(self.client.get("/api/v1/stations",params=params))
        charts=self.assert_envelope(self.client.get("/api/v1/dashboard/charts",params=params))
        self.assertEqual(overview["metrics"]["energyWh"],1000)
        self.assertEqual(stations["items"][0]["periodMetrics"]["energyWh"],1000)
        self.assertEqual(charts["items"][0]["energyWh"],1000)
        self.assertEqual(stations["items"][0]["snapshotAt"],"2025-12-03T15:55:00Z")
        self.assertEqual(stations["items"][0]["snapshotSemantics"],"LATEST_IN_BATCH")
        self.assertIs(type(stations["items"][0]["isCurrent"]),bool)

    def test_invalid_filters_and_injection_fail_safely(self):
        tests=[({"cityId":"unknown"},404,"CITY_NOT_FOUND"),({"stationId":"unknown"},404,"STATION_NOT_FOUND"),
            ({"cityId":"C2","stationId":"S1"},400,"FILTER_MISMATCH"),({"datasetId":"other"},404,"DATASET_NOT_FOUND"),
            ({"publishedBatchId":"old"},409,"BATCH_MISMATCH"),({"startDate":"2025-11-30"},422,"DATE_OUT_OF_RANGE"),
            ({"startDate":"2025-12-03","endDate":"2025-12-02"},422,"INVALID_DATE_RANGE"),
            ({"startDate":"2025-02-30"},422,"INVALID_ARGUMENT"),({"cityId":"C1\u0027 OR 1=1 --"},404,"CITY_NOT_FOUND")]
        for params,status,code in tests:
            with self.subTest(params=params):
                self.assert_envelope(self.client.get("/api/v1/dashboard/overview",params=params),status,code)
        for params in ({"pageSize":101},{"page":0},{"sortOrder":"sideways"},{"unknown":1},{"sortBy":"station_id;DROP TABLE cities"}):
            response=self.client.get("/api/v1/stations",params=params)
            self.assertEqual(response.status_code,422,response.text)
        self.assertEqual(self.client.get("/api/v1/cities").json()["data"]["total"],2)

    def test_pagination_sort_and_out_of_range_page(self):
        data=self.assert_envelope(self.client.get("/api/v1/stations",params={"pageSize":1,"page":2,"sortOrder":"desc"}))
        self.assertEqual(data["items"][0]["stationId"],"S2")
        self.assertTrue(data["hasNext"])
        empty=self.assert_envelope(self.client.get("/api/v1/stations",params={"page":10}))
        self.assertEqual(empty["items"],[])
        self.assertFalse(empty["hasNext"])

    def test_station_ranking_uses_period_metrics_before_pagination_and_nulls_last(self):
        for sort, order in (("energy", ["S2", "S1", "S3"]), ("netPaid", ["S2", "S3", "S1"]),
                            ("utilization", ["S3", "S1", "S2"])):
            with self.subTest(sort=sort):
                data=self.assert_envelope(self.client.get("/api/v1/stations",params={"sortBy":sort,"sortOrder":"desc"}))
                self.assertEqual([row["stationId"] for row in data["items"]], order)
        data=self.assert_envelope(self.client.get("/api/v1/stations",params={"sortBy":"energy","sortOrder":"desc",
            "startDate":"2025-12-02","endDate":"2025-12-03","pageSize":1}))
        self.assertEqual(data["items"][0]["stationId"],"S1")
        self.assertEqual(data["items"][0]["periodMetrics"]["energyWh"],2000)
        data=self.assert_envelope(self.client.get("/api/v1/stations",params={"sortBy":"utilization","sortOrder":"asc"}))
        self.assertEqual([row["stationId"] for row in data["items"]],["S1","S3","S2"])

    def test_empty_metrics_are_null_and_no_missing_load_is_filled(self):
        params={"stationId":"S1","startDate":"2025-12-03","endDate":"2025-12-04"}
        metrics=self.assert_envelope(self.client.get("/api/v1/dashboard/overview",params=params))["metrics"]
        self.assertIsNone(metrics["energyWh"])
        self.assertIsNone(metrics["chargingUtilizationRate"])
        self.assertIsNone(metrics["meanRating"])
        self.assertEqual(metrics["activeUsers"],0)
        data=self.assert_envelope(self.client.get("/api/v1/dashboard/charts",params=dict(params,granularity="hour")))
        self.assertIsNone(data["items"][0]["energyWh"])

    def test_chart_types_limits_and_sparse_event_zero_days(self):
        for chart in ("energy","revenue","utilization","states","service","cohorts"):
            self.assert_envelope(self.client.get("/api/v1/dashboard/charts",params={"chart":chart}))
        data=self.assert_envelope(self.client.get("/api/v1/dashboard/charts",params={"chart":"service","limit":2}))
        self.assertTrue(data["truncated"])
        self.assertEqual(data["items"][1]["ratingCount"],0)
        self.assertIsNone(data["items"][1]["meanRating"])
        for params in ({"limit":1001},{"chart":"not-a-chart"},{"chart":"revenue","granularity":"hour"}):
            self.assertEqual(self.client.get("/api/v1/dashboard/charts",params=params).status_code,422)

    def test_load_sums_station_power_and_never_labels_partial_load_complete(self):
        base={"chart":"load","granularity":"hour","startDate":"2025-12-01","endDate":"2025-12-02"}
        single=self.assert_envelope(self.client.get("/api/v1/dashboard/charts",params=dict(base,stationId="S1")))["items"][0]
        self.assertEqual(single["meanPowerKw"],1.0)
        self.assertEqual(single["completeStationCount"],1)
        self.assertTrue(single["isComplete"])
        partial=self.assert_envelope(self.client.get("/api/v1/dashboard/charts",params=dict(base,cityId="C1")))["items"][0]
        self.assertIsNone(partial["meanPowerKw"])
        self.assertFalse(partial["isComplete"])
        self.assertEqual((partial["stationCount"],partial["completeStationCount"],partial["incompleteStationCount"]),(2,1,1))
        self.assertEqual(partial["missingSampleCount"],1)
        # Separate disposable database: make both C1 observations complete.
        path=self.root/"complete-load.sqlite"
        fixture(path)
        with sqlite3.connect(path) as connection:
            connection.execute("UPDATE station_hourly_metrics SET is_complete=1,missing_sample_count=0,expected_sample_count=sample_count WHERE station_id='S2'")
        with TestClient(create_app(path)) as client:
            combined=self.assert_envelope(client.get("/api/v1/dashboard/charts",params=dict(base,cityId="C1")))["items"][0]
            self.assertEqual(combined["meanPowerKw"],8.0)  # SUM(1,7), not AVG(1,7)=4
            self.assertEqual(combined["capacity"],4)
            self.assertTrue(combined["isComplete"])
        absent=self.assert_envelope(self.client.get("/api/v1/dashboard/charts",params={
            "chart":"load","granularity":"hour","cityId":"C1","startDate":"2025-12-02","endDate":"2025-12-03"}))["items"][0]
        self.assertIsNone(absent["meanPowerKw"])  # no row for S2 is not zero city load
        self.assertEqual(absent["incompleteStationCount"],1)
        self.assertEqual(self.client.get("/api/v1/dashboard/charts",params={"chart":"load"}).status_code,422)

    def test_model_endpoints_never_make_predictions(self):
        models=self.assert_envelope(self.client.get("/api/v1/models"))
        self.assertEqual(models["models"],[])
        self.assertFalse(models["implementedPrediction"])
        for target in ("load","availability"):
            endpoint="/api/v1/predict/"+target
            body={"datasetId":"fixture","stationId":"S1","referenceTime":"2025-12-04T00:00:00Z","modelId":"candidate-model","horizonHours":6}
            self.assert_envelope(self.client.post(endpoint,json=body),503,"MODEL_NOT_READY")
            self.assertEqual(self.client.post(endpoint,json=dict(body,horizonHours=9)).status_code,422)
            for horizon in (True,False,1.0,6.0,"1"):
                self.assertEqual(self.client.post(endpoint,json=dict(body,horizonHours=horizon)).status_code,422)
            for stamp in ("2025-12-04 08:00","2025-12-04T00:01:00Z","2025-12-04T00:00:01Z","2025-12-04T00:00:00.001Z",
                          "2025-12-04T08:00:00+08:00","2025-12-04T00:00:00","2025-12-04T00Z","2025-02-30T00:00:00Z"):
                self.assertEqual(self.client.post(endpoint,json=dict(body,referenceTime=stamp)).status_code,422)
            self.assertEqual(self.client.post(endpoint,json=dict(body,modelId=" ")).status_code,422)
            self.assertEqual(self.client.post(endpoint,json=dict(body,forecastStart="2025-12-04T00:00:00Z")).status_code,422)
            self.assertEqual(self.client.post(endpoint,json=dict(body,unknown="input")).status_code,422)

    def test_data_unavailable_has_no_physical_paths(self):
        with TestClient(create_app(self.root/"sensitive-nonexistent.sqlite")) as client:
            response=client.get("/api/v1/health")
            self.assertEqual(response.status_code,503)
            self.assertEqual(response.json()["code"],"DATA_NOT_READY")
            self.assertIsNone(response.json()["meta"]["publishedBatchId"])
            self.assertNotIn("sensitive",response.text)

    def test_queries_and_connections_cannot_modify_database(self):
        before=hashlib.sha256(self.path.read_bytes()).hexdigest()
        self.client.get("/api/v1/dashboard/overview")
        with open_snapshot(self.path) as snapshot:
            with self.assertRaises(sqlite3.OperationalError):
                snapshot.connection.execute("DELETE FROM cities")
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).hexdigest(),before)

    def test_pipeline_quality_sanitizes_private_provenance(self):
        response=self.client.get("/api/v1/pipeline/runs")
        data=self.assert_envelope(response)
        quality=data["items"][0]["quality"]
        self.assertEqual({key:quality[key] for key in ("rawRows","cleanRows","rejectedRows","normalizedRows","cleanSessionRows")},
                         {"rawRows":100,"cleanRows":98,"rejectedRows":2,"normalizedRows":1,"cleanSessionRows":98})
        self.assertEqual(quality["tables"],[{"tableName":"charging_sessions","rawRows":100}])
        self.assertEqual(quality["normalizedByTable"],[{"tableName":"charging_sessions","normalizedRows":1}])
        self.assertEqual(quality["rejectionReasons"],[{"reason":"DUPLICATE_SESSION","rowCount":2}])
        self.assertEqual(quality["rejectionSamples"],[{"sessionId":"SES-1","rejectionReason":"DUPLICATE_SESSION"}])
        self.assertEqual(quality["rejectionSampleLimit"],100)
        self.assertNotIn("raw_json",response.text)
        self.assertNotIn("/private",response.text)

    def test_incomplete_metadata_is_unavailable_instead_of_internal_error(self):
        path=self.root/"invalid-metadata.sqlite"
        fixture(path)
        connection=sqlite3.connect(path)
        connection.execute("DELETE FROM __metadata WHERE key='sourceManifestSha256'")
        connection.commit()
        connection.close()
        with TestClient(create_app(path)) as client:
            response=client.get("/api/v1/datasets")
            self.assertEqual(response.status_code,503)
            self.assertEqual(response.json()["code"],"DATA_NOT_READY")

    def test_openapi_is_database_free_strict_and_exclusive(self):
        path=self.root/"openapi.json"
        schema=export_openapi(path)
        self.assertEqual(len(schema["paths"]),10)
        prediction=schema["components"]["schemas"]["PredictionRequest"]
        self.assertFalse(prediction["additionalProperties"])
        self.assertIn("stationId",prediction["required"])
        self.assertIn("referenceTime",prediction["required"])
        self.assertIn("modelId",prediction["required"])
        published=schema["components"]["schemas"]["PublishedModel"]
        self.assertIn("modelVersion",published["required"])
        self.assertNotIn("version",published["properties"])
        self.assertIn("503",schema["paths"]["/api/v1/predict/load"]["post"]["responses"])
        success=schema["paths"]["/api/v1/predict/load"]["post"]["responses"]["200"]
        self.assertIn("Not implemented",success["description"])
        self.assertIn("PredictionResult",success["content"]["application/json"]["schema"]["$ref"])
        self.assertIn("500",schema["paths"]["/api/v1/predict/load"]["post"]["responses"])
        with self.assertRaises(FileExistsError):
            export_openapi(path)

    def test_unrecognized_route_and_method_use_envelope(self):
        response=self.client.get("/api/v1/not-real")
        self.assertEqual(response.status_code,404)
        self.assertEqual(response.json()["code"],"NOT_FOUND")
        response=self.client.post("/api/v1/cities")
        self.assertEqual(response.status_code,405)
        self.assertEqual(response.json()["code"],"METHOD_NOT_ALLOWED")


if __name__ == "__main__":
    unittest.main()
