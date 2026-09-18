"""Scoring/routing units and full HTTP workflow on an isolated MySQL schema."""
from datetime import datetime, timedelta, timezone
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import uuid

from data_analysis.chargepilot.ranking import adjusted_prediction, score_candidates, DEFAULT_WEIGHTS

try:
    import httpx
    from fastapi.testclient import TestClient
    from data_analysis.chargepilot.routing import RoutePlanner, haversine, wgs_to_gcj, gcj_to_wgs
    from data_analysis.chargepilot.app import create_app
    from data_analysis.chargepilot.settings import Settings
    from data_analysis.chargepilot.ml.predict import ArrivalPredictor
    from data_analysis.chargepilot.store import ChargePilotStore
    from data_analysis.mysql_support import MySQLSettings, connect
    HAS_API = True
except ImportError:
    HAS_API = False


class ScoringTests(unittest.TestCase):
    def candidate(self, sid="S", **values):
        return {"stationId": sid, "currentFree": 1, "expectedFree": 1., "availableProbability": .8,
                "waitMinutes": 5, "etaMinutes": 10, "pricePerKwh": 1.2, "powerKw": 60,
                "loadRatio": .6, "chargingMinutes": 20, **values}

    def test_weighted_sum_and_reward_order_are_explainable(self):
        ranked = score_candidates([self.candidate("B"), self.candidate("A"), self.candidate("C")])
        self.assertEqual([r["stationId"] for r in ranked], ["A", "B", "C"])
        self.assertAlmostEqual(ranked[0]["score"], sum(ranked[0]["scoreBreakdown"].values()), places=2)
        self.assertGreater(ranked[0]["rewardPoints"], ranked[1]["rewardPoints"])
        self.assertEqual(ranked[2]["rewardPoints"], 0)
        almost_certain = score_candidates([self.candidate(availableProbability=.99999)])[0]
        self.assertIn(">99.9%", almost_certain["reasons"][0])
        self.assertNotIn("100.0%", almost_certain["reasons"][0])

    def test_bad_weights_rejected(self):
        with self.assertRaises(ValueError):
            score_candidates([self.candidate()], {"weights": {**DEFAULT_WEIGHTS, "wait": .5}})

    def test_pending_users_shift_count_distribution_consistently(self):
        prediction = {"currentFree": 2, "availabilityDistribution": [.1,.2,.3,.4],
                      "expectedFree": 2, "availableProbability": .9, "waitMinutes": 3,
                      "waitP90Minutes": 10, "serviceProbability": .8}
        station = {"capacity": 3, "currentFree": 2, "enRoute": 1, "queued": 1}
        result = adjusted_prediction(prediction, station)
        for actual, expected in zip(result["availabilityDistribution"], [.6,.4,0,0]):
            self.assertAlmostEqual(actual, expected)
        self.assertAlmostEqual(result["expectedFree"], .4)
        self.assertAlmostEqual(result["availableProbability"], .4)
        self.assertEqual(result["rawModelPrediction"]["availableProbability"], .9)


@unittest.skipUnless(HAS_API, "HTTP/ML optional dependencies absent")
class RoutingTests(unittest.TestCase):
    def setUp(self):
        self.origin = {"latitude": 39.9, "longitude": 116.4}
        self.station = {"stationId": "S", "latitude": 39.92, "longitude": 116.42}

    def test_matrix_seconds_and_cache(self):
        calls = []
        def handler(request):
            calls.append(request)
            return httpx.Response(200, json={"status": 0, "result": {"rows": [{"elements": [{"distance": 4200, "duration": 720}]}]}})
        planner = RoutePlanner("dummy-test-key", client=httpx.Client(transport=httpx.MockTransport(handler)))
        first = planner.routes(self.origin, [self.station])[0]
        self.assertEqual((first["etaMinutes"], first["distanceKm"]), (12, 4.2))
        self.assertNotIn("key", json.dumps(first))
        planner.routes(self.origin, [self.station])
        self.assertEqual(len(calls), 1)

    def test_provider_error_is_explicit_bounded_fallback(self):
        calls = []
        def handler(request):
            calls.append(1)
            return httpx.Response(200, json={"status": 121, "message": "private provider details"})
        planner = RoutePlanner("dummy-test-key", client=httpx.Client(transport=httpx.MockTransport(handler)))
        for _ in range(5):
            result = planner.routes(self.origin, [self.station])[0]
            self.assertEqual(result["routeSource"], "ESTIMATED_DISTANCE_SPEED")
        self.assertEqual(len(calls), 1)

    def test_gcj_conversion_roundtrip_and_nonchina_noop(self):
        gcj = wgs_to_gcj(39.9, 116.4)
        self.assertNotEqual(gcj, (39.9,116.4))
        wgs = gcj_to_wgs(*gcj)
        self.assertAlmostEqual(wgs[0], 39.9, places=6)
        self.assertAlmostEqual(wgs[1], 116.4, places=6)
        self.assertEqual(wgs_to_gcj(40,-100), (40,-100))


MODEL_DIR = Path(__file__).resolve().parents[1]/"outputs"/"chargepilot"


@unittest.skipUnless(HAS_API and os.environ.get("RUN_MYSQL_TESTS") == "1" and (MODEL_DIR/"arrival.metadata.json").exists(),
                     "Requires trained arrival model and isolated real MySQL")
class HttpWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = "cp_http_test_"+uuid.uuid4().hex[:16]
        cls.mysql = dict(host=os.getenv("MYSQL_TEST_HOST", "127.0.0.1"), port=int(os.getenv("MYSQL_TEST_PORT", "3306")),
                         user=os.getenv("MYSQL_TEST_USER", "root"), password=os.getenv("MYSQL_TEST_PASSWORD", ""), database=cls.schema)
        cls.admin = {"X-Admin-Token": "test-only-admin-token-123"}
        connection = connect(MySQLSettings(**cls.mysql), database="")
        with connection.cursor() as cursor:
            cursor.execute(f"CREATE DATABASE `{cls.schema}`")
        connection.close()
        cls.store = ChargePilotStore(cls.mysql)
        cls.predictor = ArrivalPredictor(MODEL_DIR)
        cls.tmp = tempfile.TemporaryDirectory()
        settings = Settings(mysql=cls.mysql, admin_token=cls.admin["X-Admin-Token"], model_dir=MODEL_DIR,
                            load_dir=Path(cls.tmp.name))
        cls.app = create_app(settings, predictor=cls.predictor, store=cls.store)
        cls.client = TestClient(cls.app)
        cls.store.control_clock({"action": "configure", "paused": True})

    @classmethod
    def tearDownClass(cls):
        cls.client.close(); cls.tmp.cleanup()
        connection = connect(MySQLSettings(**cls.mysql), database="")
        with connection.cursor() as cursor:
            cursor.execute(f"DROP DATABASE `{cls.schema}`")
        connection.close()

    def session(self, name):
        response = self.client.post("/api/v1/chargepilot/sessions", json={"name":name})
        self.assertEqual(response.status_code, 200, response.text)
        return {"Authorization": "Bearer "+response.json()["data"]["token"]}

    def test_auth_validation_safe_errors(self):
        self.assertEqual(self.client.get("/api/v1/chargepilot/me").status_code, 401)
        self.assertEqual(self.client.get("/api/v1/chargepilot/admin").status_code, 403)
        headers = self.session("输入测试")
        response = self.client.post("/api/v1/chargepilot/recommendations", headers=headers,
            json={"cityId":"DL","origin":{"latitude":400,"longitude":121},"energyKwh":20,"awardPoints":1000})
        self.assertEqual(response.status_code, 422)
        response = self.client.post("/api/v1/chargepilot/trips/unknown/pay", headers=headers, json={"amount": 0})
        self.assertEqual(response.status_code, 422)

    def test_actual_models_recommend_charge_pay_export(self):
        prefix = "/api/v1/chargepilot"
        headers = self.session("完整演示")
        city = next(c for c in self.predictor.cities if c["cityId"] == "DL")
        response = self.client.post(prefix+"/recommendations", headers=headers,
            json={"cityId":"DL","origin":{k:city[k] for k in ("latitude","longitude")},"energyKwh":5,"maxEtaMinutes":60})
        self.assertEqual(response.status_code, 200, response.text)
        rec = response.json()["data"]
        self.assertEqual(len(rec["candidates"]), 5)
        chosen = rec["candidates"][0]
        self.assertTrue(0 <= chosen["availableProbability"] <= 1)
        response = self.client.post(prefix+f"/recommendations/{rec['recommendationId']}/select", headers=headers,
                                    json={"stationId": chosen["stationId"]})
        self.assertEqual(response.status_code, 200, response.text)
        trip = response.json()["data"]
        other = self.session("其他用户")
        self.assertEqual(self.client.post(prefix+f"/trips/{trip['tripId']}/arrive", headers=other).status_code, 404)
        self.store.control_clock({"action":"advance","seconds":3601})
        self.store.set_background(chosen["stationId"],0,1)
        for action in ("arrive","start"):
            response = self.client.post(prefix+f"/trips/{trip['tripId']}/{action}", headers=headers)
            self.assertEqual(response.status_code, 200, response.text)
        self.store.control_clock({"action":"advance","seconds":1800})
        response = self.client.get(prefix+"/me", headers=headers)
        self.assertEqual(response.json()["data"]["trips"][0]["status"], "PENDING_PAYMENT")
        paid = self.client.post(prefix+f"/trips/{trip['tripId']}/pay", headers=headers).json()["data"]
        self.assertEqual(paid["status"], "COMPLETED")
        self.assertEqual(paid["awardedPoints"], chosen["rewardPoints"])
        self.assertGreater(paid["amount"], 0)
        again = self.client.post(prefix+f"/trips/{trip['tripId']}/pay", headers=headers).json()["data"]
        self.assertEqual(again["awardedPoints"], paid["awardedPoints"])
        feedback = self.client.get(prefix+"/admin/feedback", headers=self.admin).json()["data"]["records"]
        self.assertTrue(any(row["tripId"] == trip["tripId"] and row["status"] == "COMPLETED" for row in feedback))

    def test_no_load_artifact_no_fake_prediction(self):
        response = self.client.get("/api/v1/chargepilot/load?stationId=ST-DL-01&horizonHours=6")
        self.assertEqual(response.status_code, 503)
        self.assertEqual(response.json()["error"]["code"], "MODEL_NOT_READY")

    def test_restart_skips_reinitializing_baseline_and_optional_load_failure(self):
        settings = Settings(mysql=self.mysql, admin_token=self.admin["X-Admin-Token"], model_dir=MODEL_DIR,
                            load_dir=Path(self.tmp.name))
        with patch.object(self.predictor, "snapshot", side_effect=AssertionError("Must not reread initial baseline")), \
                patch("data_analysis.chargepilot.app.LoadAdapter.__init__", side_effect=ValueError("invalid optional artifact")):
            app = create_app(settings, predictor=self.predictor, store=self.store)
        with TestClient(app) as client:
            response = client.get("/api/v1/chargepilot/bootstrap")
            self.assertEqual(response.status_code, 200)
            response = client.get("/api/v1/chargepilot/models")
            self.assertEqual(response.json()["data"]["load"]["status"], "NOT_READY")


if __name__ == "__main__":
    unittest.main()
