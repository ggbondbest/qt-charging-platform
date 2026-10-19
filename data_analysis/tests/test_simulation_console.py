"""Console access changes do not remove user-session authentication."""
import importlib.util
import os
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch

HAS_API = all(importlib.util.find_spec(name) for name in ("fastapi", "httpx"))


@unittest.skipUnless(HAS_API, "Install requirements-api.txt for console API tests")
class SimulationConsoleTests(unittest.TestCase):
    def setUp(self):
        from fastapi.testclient import TestClient
        from data_analysis.chargepilot.app import create_app
        from data_analysis.chargepilot.settings import Settings
        self.store = MagicMock()
        clock = dict(time="2026-05-05T00:00:00Z", speed=10, paused=True)
        self.store.observed_clock.return_value = clock
        self.store.admin.return_value = dict(baselineInitialized=True, clock=clock, config={}, stations=[], counts={})
        self.store.configure.return_value = {"weights": {}}
        self.store.control_clock.return_value = clock
        self.store.set_background.return_value = {"stationId": "S1"}
        self.store.feedback.return_value = []
        predictor = SimpleNamespace(catalog=[], cities=[], bounds={}, metadata={})
        application = create_app(Settings(admin_token="obsolete-ignored-value"), store=self.store,
                                 predictor=predictor, routing=MagicMock(), load_adapter=SimpleNamespace(report={"status": "READY"}))
        self.client = TestClient(application)
        self.addCleanup(self.client.close)

    def test_console_routes_open_without_token_and_ignore_legacy_header(self):
        for headers in ({}, {"X-Admin-Token": "incorrect-old-token"}):
            with self.subTest(headers=bool(headers)):
                self.assertEqual(self.client.get("/api/v1/chargepilot/admin", headers=headers).status_code, 200)
                self.assertEqual(self.client.get("/api/v1/chargepilot/admin/feedback", headers=headers).status_code, 200)
                self.assertEqual(self.client.patch("/api/v1/chargepilot/admin/config", headers=headers, json={"weights": {}}).status_code, 200)
                self.assertEqual(self.client.post("/api/v1/chargepilot/admin/clock", headers=headers,
                                                  json={"action": "configure", "paused": True}).status_code, 200)
                self.assertEqual(self.client.post("/api/v1/chargepilot/admin/stations/S1/background", headers=headers,
                                                  json={"busyCount": 0, "releaseAfterSeconds": 1}).status_code, 200)
        schema = self.client.get("/api/v1/chargepilot/openapi.json").json()
        for path, methods in schema["paths"].items():
            if "/admin" in path:
                for operation in methods.values():
                    self.assertFalse(any(parameter.get("in") == "header" for parameter in operation.get("parameters", [])))

    def test_user_identity_still_required_and_console_validation_remains(self):
        for method, path, body in (("GET", "/me", None), ("POST", "/trips/unknown/pay", {}),
                                    ("POST", "/route", {"origin": {"latitude": 39, "longitude": 116}, "stationId": "S1"})):
            response = self.client.request(method, "/api/v1/chargepilot" + path, json=body)
            self.assertEqual(response.status_code, 401, response.text)
        bad_clock = self.client.post("/api/v1/chargepilot/admin/clock", json={"action": "advance"})
        self.assertEqual(bad_clock.status_code, 422)
        self.store.control_clock.assert_not_called()

    def test_short_legacy_environment_token_is_not_a_startup_requirement(self):
        from data_analysis.chargepilot.settings import Settings
        with patch.dict(os.environ, {"CHARGEPILOT_MYSQL_USER": "test_user",
                                     "CHARGEPILOT_MYSQL_PASSWORD": "test_password", "CHARGEPILOT_ADMIN_TOKEN": "x"}):
            self.assertEqual(Settings.from_env().admin_token, "")
