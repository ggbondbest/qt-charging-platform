"""Real unified delivery boundary: MySQL publication, all models and live demo.

Opt in with RUN_MYSQL_TESTS=1 and MYSQL_TEST_HOST/PORT/USER/PASSWORD.
Once opted in, missing dependencies/models are an ERROR, never a silent skip.
Only UUID-named schemas/users successfully created by this test are removed.
"""
from dataclasses import replace
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
import uuid

DEPS = ("fastapi", "httpx", "pymysql", "pandas", "numpy", "sklearn", "joblib", "pyarrow")
HAS_DEPS = all(importlib.util.find_spec(name) is not None for name in DEPS)
ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.environ.get("RUN_MYSQL_TESTS") == "1", "Set RUN_MYSQL_TESTS=1 for the full delivery integration")
class UnifiedDeliveryIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not HAS_DEPS:
            raise RuntimeError("Formal delivery integration requires all API/MySQL/ML dependencies")
        from fastapi.testclient import TestClient
        from data_analysis.mysql_support import MySQLSettings, connect
        from data_analysis.chargepilot.settings import Settings
        from data_analysis.chargepilot.store import ChargePilotStore
        from data_analysis.chargepilot import app as cp_module
        from data_analysis.delivery.app import create_app
        from data_analysis.delivery.models import ForecastService
        from data_analysis.publishing import mysql_publish

        required = [ROOT / "outputs/chargepilot/arrival.metadata.json",
                    ROOT / "outputs/ml_load/hgb-q50-history24-v1.joblib",
                    ROOT / "outputs/ml_insights_delivery/metadata.json"]
        required.extend(ROOT / f"outputs/ml_availability_delivery/h{h:02}/model_metadata.json" for h in (1, 6, 24))
        missing = [str(path.relative_to(ROOT)) for path in required if not path.is_file()]
        if missing:
            raise RuntimeError("Train the complete delivery models before this test: " + ", ".join(missing))

        cls.connect = staticmethod(connect)
        suffix = uuid.uuid4().hex[:18]
        cls.analytics_schema = "delivery_stats_" + suffix
        cls.operations_schema = "delivery_ops_" + suffix
        cls.reader = "delivery_ro_" + suffix
        cls.reader_password = uuid.uuid4().hex + "Aa9!"
        cls.created_analytics = cls.created_operations = cls.created_reader = False
        cls.client = None
        cls.client_entered = False
        cls.temporary = tempfile.TemporaryDirectory(prefix="delivery-http-test-")
        cls.admin = MySQLSettings(host=os.getenv("MYSQL_TEST_HOST", "127.0.0.1"),
            port=int(os.getenv("MYSQL_TEST_PORT", "3306")), user=os.getenv("MYSQL_TEST_USER", "root"),
            password=os.getenv("MYSQL_TEST_PASSWORD", ""), database=cls.analytics_schema)
        cls.admin_header = {"X-Admin-Token": "delivery-test-admin-token-only"}
        cls.tick_observed = threading.Event()

        class ObservedStore(ChargePilotStore):
            def clock(self):
                result = super().clock()
                cls.tick_observed.set()
                return result

        try:
            # Observe successful CREATE, including when publication fails later;
            # cleanup never guesses ownership from a schema that merely exists.
            actual_execute = mysql_publish._execute
            def observe_execute(connection, statement, parameters=None):
                result = actual_execute(connection, statement, parameters)
                if statement.startswith(f"CREATE DATABASE `{cls.analytics_schema}` "):
                    cls.created_analytics = True
                return result
            with patch.object(mysql_publish, "_execute", side_effect=observe_execute):
                cls.publication = mysql_publish.publish_mysql(ROOT / "datasets/analytics_full_180d_v1", cls.admin)
            connection = cls.connect(cls.admin, database="")
            try:
                with connection.cursor() as cursor:
                    cursor.execute(f"CREATE DATABASE `{cls.operations_schema}` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin")
                    cls.created_operations = True
                    cursor.execute("CREATE USER %s@'%%' IDENTIFIED BY %s", (cls.reader, cls.reader_password))
                    cls.created_reader = True
                    escaped = cls.analytics_schema.replace("_", "\\_")
                    cursor.execute(f"GRANT SELECT ON `{escaped}`.* TO %s@'%%'", (cls.reader,))
            finally:
                connection.close()
            cls.reader_settings = replace(cls.admin, user=cls.reader, password=cls.reader_password)
            mysql = dict(host=cls.admin.host, port=cls.admin.port, user=cls.admin.user,
                         password=cls.admin.password, database=cls.operations_schema)
            cls.settings = Settings(mysql=mysql, admin_token=cls.admin_header["X-Admin-Token"],
                                    model_dir=ROOT / "outputs/chargepilot", load_dir=ROOT / "outputs/ml_load")
            with patch.dict(os.environ, {"AVAILABILITY_MODEL_DIR": str(ROOT / "outputs/ml_availability_delivery"),
                                         "INSIGHTS_MODEL_DIR": str(ROOT / "outputs/ml_insights_delivery")}):
                cls.provider = ForecastService.from_local(cls.settings)
            cls.store = ObservedStore(mysql)
            # Exercise actual StaticFiles routing independently of whether this
            # CI job also builds Vue. This is a temporary test asset, not Mock API.
            static_root = Path(cls.temporary.name)
            dist = static_root / "frontend/dist"
            dist.mkdir(parents=True)
            (dist / "index.html").write_text("<!doctype html><title>Delivery integration</title><main>delivery-static-probe</main>", encoding="utf8")
            with patch.object(cp_module, "ROOT", static_root):
                operational = cp_module.create_app(cls.settings, predictor=cls.provider.arrival,
                    store=cls.store, load_adapter=cls.provider.load)
            cls.store.control_clock({"action": "configure", "paused": True})
            cls.app = create_app(settings=cls.settings, mysql_settings=cls.reader_settings,
                                 provider=cls.provider, operational_app=operational)
            cls.tick_observed.clear()
            cls.client = TestClient(cls.app)
            cls.client.__enter__()
            cls.client_entered = True
            if not cls.tick_observed.wait(timeout=5):
                raise AssertionError("Mounted ChargePilot lifespan did not start its real MySQL clock task")
            registry = cls.client.get("/api/v1/intelligence/models")
            if registry.status_code != 200:
                raise AssertionError(f"Unified model registry failed: {registry.status_code} {registry.text}")
            cls.registry = registry.json()["data"]
            for name in ("load", "availability", "insights", "arrival"):
                if cls.registry[name]["status"] != "READY":
                    raise AssertionError(f"Required delivery model {name} is not READY; formal integration must not skip it")
        except Exception:
            cls.tearDownClass()
            raise

    @classmethod
    def tearDownClass(cls):
        try:
            if cls.client is not None:
                if cls.client_entered:
                    cls.client.__exit__(None, None, None)
                else:
                    cls.client.close()
        finally:
            connection = cls.connect(cls.admin, database="")
            try:
                with connection.cursor() as cursor:
                    if cls.created_reader:
                        cursor.execute("DROP USER %s@'%%'", (cls.reader,))
                    if cls.created_operations:
                        cursor.execute(f"DROP DATABASE `{cls.operations_schema}`")
                    if cls.created_analytics:
                        cursor.execute(f"DROP DATABASE `{cls.analytics_schema}`")
            finally:
                connection.close()
                cls.temporary.cleanup()

    def envelope(self, response, status=200, code="OK"):
        self.assertEqual(response.status_code, status, response.text)
        payload = response.json()
        self.assertEqual(set(payload), {"code", "message", "data", "meta"})
        self.assertEqual(payload["code"], code)
        self.assertEqual(payload["meta"]["requestId"], response.headers["X-Request-ID"])
        self.assertEqual(response.headers["Cache-Control"], "no-store")
        if status == 200:
            self.assertEqual(payload["meta"]["publishedBatchId"], self.publication["publishedBatchId"])
            self.assertEqual(payload["meta"]["datasetId"], self.publication["datasetId"])
        for secret in (self.reader_password, self.analytics_schema, self.operations_schema):
            self.assertNotIn(secret, response.text)
        return payload["data"]

    def body(self, target, horizon=6):
        model = next(model for model in self.registry[target]["models"] if model["horizonHours"] == horizon)
        return dict(target=target, modelId=model["modelId"], stationId="ST-DL-01", cityId="DL",
                    referenceTime="2026-05-05T00:00:00Z", horizonHours=horizon,
                    datasetId=self.publication["datasetId"], publishedBatchId=self.publication["publishedBatchId"])

    def test_real_statistics_publication_and_readonly_connection(self):
        data = self.envelope(self.client.get("/api/v1/dashboard/overview"))
        self.assertGreater(data["metrics"]["energyWh"], 0)
        self.assertGreater(data["metrics"]["activeUsers"], 0)
        cities = self.envelope(self.client.get("/api/v1/cities"))
        self.assertEqual(cities["total"], 5)
        stations = self.envelope(self.client.get("/api/v1/stations", params={"cityId": "DL"}))
        self.assertEqual(stations["total"], 5)
        connection = self.connect(self.reader_settings)
        try:
            with connection.cursor() as cursor:
                with self.assertRaises(Exception):
                    cursor.execute("UPDATE __metadata SET value=value WHERE 1=0")
        finally:
            connection.close()

    def test_all_models_and_detailed_forecasts_use_true_targets(self):
        for target in ("load", "availability"):
            for horizon in (1, 6, 24):
                with self.subTest(target=target, horizon=horizon):
                    data = self.envelope(self.client.post("/api/v1/intelligence/forecast", json=self.body(target, horizon)))
                    self.assertEqual(len(data["points"]), horizon)
                    self.assertEqual(data["points"][0]["timestamp"], "2026-05-05T00:00:00Z")
                    self.assertEqual(data["unit"], "kW" if target == "load" else "chargers")
                    self.assertEqual(data["targetSemantics"], "MEAN_POWER_DURING_HOUR" if target == "load" else "LAST_SAMPLE_IN_HOUR")
                    self.assertEqual(data["historyHours"], 24)
                    for point in data["points"]:
                        self.assertGreaterEqual(point["value"], 0)
                        if target == "availability":
                            self.assertAlmostEqual(sum(point["distribution"].values()), 1, places=6)
                            self.assertTrue(0 <= point["probabilityNoFree"] <= 1)
                            self.assertEqual(point["sampleTimestamp"], point["timestamp"].replace(":00:00Z", ":55:00Z"))

    def test_legacy_prediction_routes_keep_strict_contract(self):
        capabilities = self.envelope(self.client.get("/api/v1/models"))
        self.assertTrue(capabilities["implementedPrediction"])
        self.assertTrue(capabilities["models"])
        for target in ("load", "availability"):
            body = self.body(target)
            body.pop("target")
            data = self.envelope(self.client.post("/api/v1/predict/" + target, json=body))
            self.assertEqual(set(data), {"schemaVersion", "featureVersion", "modelId", "modelVersion", "unit", "points"})
            self.assertTrue(all(set(point) == {"timestamp", "value"} for point in data["points"]))

    def test_insight_lists_details_and_unknown_id_errors(self):
        for kind, id_name, score_name in (("churn", "userId", "riskScore"), ("anomalies", "sessionId", "anomalyScore")):
            route = "/api/v1/intelligence/insights/" + kind
            data = self.envelope(self.client.get(route, params={"limit": 2}))
            self.assertEqual(len(data["items"]), 2)
            self.assertEqual(data["evaluationSplit"], "TEST")
            item = data["items"][0]
            detail = self.envelope(self.client.get(route + "/" + item[id_name]))
            self.assertEqual(item[score_name], detail[score_name])
            self.assertTrue(detail["explanations"])
            self.envelope(self.client.get(route + "/unknown-synthetic-id"), 404, "NOT_FOUND")
        self.envelope(self.client.get("/api/v1/intelligence/insights/churn", params={"limit": 101}), 422, "INVALID_ARGUMENT")
        self.envelope(self.client.get("/api/v1/intelligence/insights/churn", params={"limit": 0}), 422, "INVALID_ARGUMENT")

    def test_batch_guard_and_parameter_validation_never_fabricate(self):
        self.envelope(self.client.get("/api/v1/intelligence/models", params={"publishedBatchId": "stale-batch"}), 409, "BATCH_MISMATCH")
        body = self.body("load")
        self.envelope(self.client.post("/api/v1/intelligence/forecast", json={**body, "publishedBatchId": "stale-batch"}), 409, "BATCH_MISMATCH")
        for changes in ({"horizonHours": True}, {"horizonHours": 2}, {"referenceTime": "2026-05-05T00:30:00Z"},
                        {"target": "invented"}, {"extraPayload": "not-allowed"}):
            self.envelope(self.client.post("/api/v1/intelligence/forecast", json={**body, **changes}), 422, "INVALID_ARGUMENT")
        self.envelope(self.client.post("/api/v1/intelligence/forecast", json={**body, "modelId": "wrong-model"}), 422, "MODEL_INCOMPATIBLE")
        self.envelope(self.client.post("/api/v1/intelligence/forecast", json={**body, "referenceTime": "2030-01-01T00:00:00Z"}), 409, "HISTORY_TOO_SHORT")
        with patch.object(self.provider, "manifest", {**self.provider.manifest, "sourceManifestSha256": "0" * 64}):
            self.envelope(self.client.get("/api/v1/intelligence/models"), 409, "BATCH_MISMATCH")

    def test_real_operational_lifespan_auth_and_static_routes_coexist(self):
        self.assertTrue(self.tick_observed.is_set())
        response = self.client.get("/")
        self.assertEqual(response.status_code, 200)
        self.assertIn("delivery-static-probe", response.text)
        self.assertEqual(self.client.get("/api/v1/chargepilot/me").status_code, 401)
        bootstrap = self.client.get("/api/v1/chargepilot/bootstrap")
        self.assertEqual(bootstrap.status_code, 200, bootstrap.text)
        self.assertEqual(bootstrap.json()["meta"]["mode"], "REPLAY_DEMO")
        session = self.client.post("/api/v1/chargepilot/sessions", json={"name": "统一入口验证"})
        self.assertEqual(session.status_code, 200, session.text)
        headers = {"Authorization": "Bearer " + session.json()["data"]["token"]}
        self.assertEqual(self.client.get("/api/v1/chargepilot/me", headers=headers).status_code, 200)
        unknown = self.client.get("/api/v1/not-a-real-api")
        self.assertEqual(unknown.status_code, 404)
        self.assertNotIn("delivery-static-probe", unknown.text)
        # Public analytics still reads the immutable independent database.
        self.envelope(self.client.get("/api/v1/health"))

    def test_mounted_openapi_and_docs_have_distinct_working_urls(self):
        docs = self.client.get("/api/v1/chargepilot/docs")
        self.assertEqual(docs.status_code, 200, docs.text)
        self.assertIn("/api/v1/chargepilot/openapi.json", docs.text)
        cp_schema = self.client.get("/api/v1/chargepilot/openapi.json")
        self.assertEqual(cp_schema.status_code, 200, cp_schema.text)
        self.assertIn("/api/v1/chargepilot/recommendations", cp_schema.json()["paths"])
        self.assertIn("/api/v1/chargepilot/admin/config", cp_schema.json()["paths"])
        self.assertIn("ChargePilot", cp_schema.json()["info"]["title"])
        main_schema = self.client.get("/openapi.json")
        self.assertEqual(main_schema.status_code, 200, main_schema.text)
        self.assertIn("/api/v1/intelligence/forecast", main_schema.json()["paths"])
        self.assertNotIn("/api/v1/chargepilot/recommendations", main_schema.json()["paths"])
        self.assertEqual(self.client.get("/docs").status_code, 200)

    def test_outer_cors_allows_operational_patch_but_not_untrusted_origin(self):
        route = "/api/v1/chargepilot/admin/config"
        headers = {"Origin": "http://127.0.0.1:5173", "Access-Control-Request-Method": "PATCH",
                   "Access-Control-Request-Headers": "x-admin-token,content-type"}
        allowed = self.client.options(route, headers=headers)
        self.assertEqual(allowed.status_code, 200, allowed.text)
        self.assertEqual(allowed.headers.get("Access-Control-Allow-Origin"), headers["Origin"])
        self.assertIn("PATCH", allowed.headers.get("Access-Control-Allow-Methods", ""))
        self.assertIn("x-admin-token", allowed.headers.get("Access-Control-Allow-Headers", "").lower())
        rejected = self.client.options(route, headers={**headers, "Origin": "https://unexpected-origin.invalid"})
        self.assertEqual(rejected.status_code, 400, rejected.text)
        self.assertNotIn("Access-Control-Allow-Origin", rejected.headers)


if __name__ == "__main__":
    unittest.main()
