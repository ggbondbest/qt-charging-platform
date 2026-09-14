"""MySQL adapter checks plus opt-in real-server HTTP/permission tests.

RUN_MYSQL_TESTS=1 requires an isolated test server with MYSQL_TEST_HOST,
MYSQL_TEST_PORT, MYSQL_TEST_USER and MYSQL_TEST_PASSWORD. A unique schema and
read-only user are created and removed; existing databases are never reused.
"""

from dataclasses import replace
from decimal import Decimal
import importlib.util
import json
import os
from pathlib import Path
import re
import sqlite3
import tempfile
import unittest
from unittest.mock import patch
import uuid

from data_analysis.backend.database import _value, mysql_parameters

HAS_API = all(importlib.util.find_spec(name) is not None for name in ("fastapi", "httpx"))
HAS_MYSQL = importlib.util.find_spec("pymysql") is not None
if HAS_API:
    from fastapi.testclient import TestClient
    from data_analysis.backend.app import create_app
    from data_analysis.backend.database import open_snapshot
    from data_analysis.tests.test_analytics_api import fixture


class MySQLAdapterTests(unittest.TestCase):
    def test_only_qmark_tokens_are_converted(self):
        sql = "SELECT '?', `?`, \"?\", 'a''?b', '20%', ? /* ? */ -- ?\n WHERE id = ?"
        self.assertEqual(mysql_parameters(sql),
            "SELECT '?', `?`, \"?\", 'a''?b', '20%%', %s /* ? */ -- ?\n WHERE id = %s")
        self.assertEqual(mysql_parameters("SELECT 'a\\\'?b', ? # ?\n"),
                         "SELECT 'a\\\'?b', %s # ?\n")

    def test_big_integer_sums_are_not_rounded_through_float(self):
        value = _value(Decimal("9007199254740993"))
        self.assertIs(type(value), int)
        self.assertEqual(value, 9007199254740993)
        self.assertIs(type(_value(Decimal("0.5000"))), float)
        self.assertIsNone(_value(None))

    @unittest.skipUnless(HAS_API, "Install API dependencies")
    def test_default_does_not_use_legacy_sqlite_environment(self):
        with patch.dict(os.environ, {"ANALYTICS_DB": "/private/old.sqlite"}, clear=True):
            with TestClient(create_app()) as client:
                response = client.get("/api/v1/health")
        self.assertEqual(response.status_code, 503)
        self.assertEqual(response.json()["code"], "DATA_NOT_READY")
        self.assertNotIn("private", response.text)

    @unittest.skipUnless(HAS_API, "Install API dependencies")
    def test_invalid_mysql_environment_does_not_break_startup_or_leak(self):
        with patch.dict(os.environ, {"ANALYTICS_MYSQL_HOST": "host-secret",
                "ANALYTICS_MYSQL_PORT": "invalid-private", "ANALYTICS_MYSQL_PASSWORD": "secret-value"}, clear=True):
            with TestClient(create_app()) as client:
                response = client.get("/api/v1/health")
        self.assertEqual(response.status_code, 503)
        self.assertEqual(response.json()["code"], "DATA_NOT_READY")
        for private in ("host-secret", "invalid-private", "secret-value"):
            self.assertNotIn(private, response.text)


@unittest.skipUnless(os.environ.get("RUN_MYSQL_TESTS") == "1" and HAS_API and HAS_MYSQL,
                     "Set RUN_MYSQL_TESTS=1 with API/MySQL dependencies and a test server")
class MySQLApiIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from data_analysis.mysql_support import MySQLSettings, connect
        cls.connect = staticmethod(connect)
        cls.temporary = tempfile.TemporaryDirectory(prefix="mysql-api-test-")
        cls.path = Path(cls.temporary.name) / "independent.sqlite3"
        cls.metadata = fixture(cls.path)
        cls.schema = "analytics_api_test_" + uuid.uuid4().hex[:20]
        cls.reader = "api_ro_" + uuid.uuid4().hex[:20]
        cls.reader_password = uuid.uuid4().hex + "Aa9!"
        cls.admin = MySQLSettings(host=os.environ.get("MYSQL_TEST_HOST", "127.0.0.1"),
            port=int(os.environ.get("MYSQL_TEST_PORT", "3306")),
            user=os.environ.get("MYSQL_TEST_USER", "root"),
            password=os.environ.get("MYSQL_TEST_PASSWORD", ""), database=cls.schema)
        cls.created_schema, cls.created_reader = False, False
        cls.sqlite_client, cls.mysql_client = None, None
        try:
            cls._seed_server()
            cls.reader_settings = replace(cls.admin, user=cls.reader, password=cls.reader_password)
            cls.sqlite_client = TestClient(create_app(cls.path))
            cls.mysql_client = TestClient(create_app(mysql_settings=cls.reader_settings))
        except Exception:
            cls.tearDownClass()
            raise

    @classmethod
    def _seed_server(cls):
        connection = cls.connect(cls.admin, database="")
        try:
            with connection.cursor() as cursor:
                cursor.execute(f"CREATE DATABASE `{cls.schema}` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin")
                cls.created_schema = True
                cursor.execute(f"USE `{cls.schema}`")
                with sqlite3.connect(cls.path) as source:
                    tables = [row[0] for row in source.execute("SELECT name FROM sqlite_master WHERE type='table'")]
                    for table in tables:
                        if not re.fullmatch(r"[a-z_]+", table):
                            raise ValueError("Unexpected independent fixture table")
                        fields = list(source.execute(f"PRAGMA table_info({table})"))
                        definitions = []
                        for field in fields:
                            name, kind = field[1:3]
                            if not re.fullmatch(r"[a-z_]+", name):
                                raise ValueError("Unexpected independent fixture column")
                            sql_type = {"TEXT": "VARCHAR(512)", "INTEGER": "BIGINT", "REAL": "DOUBLE"}[kind]
                            if table == "__metadata" and name == "value":
                                sql_type = "LONGTEXT"
                            definitions.append(f"`{name}` {sql_type}")
                        cursor.execute(f"CREATE TABLE `{table}` ({', '.join(definitions)}) ENGINE=InnoDB")
                        rows = list(source.execute(f"SELECT * FROM {table}"))
                        if rows:
                            cursor.executemany(f"INSERT INTO `{table}` VALUES ({','.join(['%s'] * len(fields))})", rows)
                cursor.executemany("INSERT INTO __metadata (`key`, `value`) VALUES (%s, %s)",
                    [("storageBackend", json.dumps("mysql")), ("publicationStatus", json.dumps("PUBLISHED"))])
                cursor.execute("CREATE USER %s@'%%' IDENTIFIED BY %s", (cls.reader, cls.reader_password))
                cls.created_reader = True
                grant_schema = cls.schema.replace("_", "\\_")
                cursor.execute(f"GRANT SELECT ON `{grant_schema}`.* TO %s@'%%'", (cls.reader,))
            connection.commit()
        finally:
            connection.close()

    @classmethod
    def tearDownClass(cls):
        for client in (cls.mysql_client, cls.sqlite_client):
            if client is not None:
                client.close()
        connection = cls.connect(cls.admin, database="")
        try:
            with connection.cursor() as cursor:
                if cls.created_reader:
                    cursor.execute("DROP USER %s@'%%'", (cls.reader,))
                if cls.created_schema:
                    cursor.execute(f"DROP DATABASE `{cls.schema}`")
        finally:
            connection.close()
            cls.temporary.cleanup()

    def assert_same_response(self, route, params=None):
        sqlite = self.sqlite_client.get("/api/v1/" + route, params=params)
        mysql = self.mysql_client.get("/api/v1/" + route, params=params)
        self.assertEqual(mysql.status_code, sqlite.status_code, mysql.text)
        expected, actual = sqlite.json(), mysql.json()
        for body in (expected, actual):
            body["meta"].pop("requestId")
        self.assertEqual(actual, expected)
        return mysql.json()

    def test_existing_get_contracts_filters_sorting_and_nulls_match(self):
        for route in ("health", "datasets", "cities", "stations", "pipeline/runs", "dashboard/overview", "models"):
            with self.subTest(route=route):
                self.assert_same_response(route)
        cases = [{"cityId": "C1"}, {"stationId": "S2"}, {"cityId": "c1"}, {"cityId": "C1 "},
            {"publishedBatchId": "previous"}, {"cityId": "C1' OR 1=1 --"},
            {"stationId": "S1", "startDate": "2025-12-03", "endDate": "2025-12-04"},
            {"stationId": "S1", "startDate": "2025-12-01", "endDate": "2025-12-02"}]
        for params in cases:
            for route in ("dashboard/overview", "stations"):
                with self.subTest(route=route, params=params):
                    self.assert_same_response(route, params)
        for sort in ("id", "name", "capacity", "city", "energy", "netPaid", "utilization"):
            for order in ("asc", "desc"):
                with self.subTest(sort=sort, order=order):
                    self.assert_same_response("stations", {"sortBy": sort, "sortOrder": order, "pageSize": 1, "page": 2})
        metrics = self.mysql_client.get("/api/v1/dashboard/overview").json()["data"]["metrics"]
        self.assertIs(type(metrics["paidCents"]), int)
        self.assertIs(type(metrics["energyWh"]), int)
        self.assertIs(type(metrics["chargingUtilizationRate"]), float)

    def test_all_chart_types_and_predictions_match_existing_semantics(self):
        for chart in ("energy", "revenue", "utilization", "states", "service", "cohorts", "load"):
            granularities = ("hour",) if chart == "load" else ("day", "hour")
            for granularity in granularities:
                for filters in ({}, {"cityId": "C1"}, {"stationId": "S1", "startDate": "2025-12-03", "endDate": "2025-12-04"}):
                    with self.subTest(chart=chart, granularity=granularity, filters=filters):
                        self.assert_same_response("dashboard/charts", dict(filters, chart=chart, granularity=granularity, limit=2))
        for target in ("load", "availability"):
            body = {"stationId": "S1", "referenceTime": "2025-12-04T00:00:00Z", "horizonHours": 6,
                    "modelId": "candidate-model"}
            response = self.mysql_client.post("/api/v1/predict/" + target, json=body)
            self.assertEqual(response.status_code, 503)
            self.assertEqual(response.json()["code"], "MODEL_NOT_READY")

    def test_read_only_user_and_transaction_both_reject_writes(self):
        import pymysql
        connection = self.connect(self.reader_settings)
        try:
            with connection.cursor() as cursor:
                with self.assertRaises(pymysql.Error):
                    cursor.execute("DELETE FROM cities")
        finally:
            connection.close()
        # Even a privileged account must be unable to mutate through the
        # request transaction (the deployment still uses a SELECT-only user).
        with open_snapshot(mysql_settings=self.admin) as snapshot:
            with snapshot.connection.cursor() as cursor:
                with self.assertRaises(pymysql.Error):
                    cursor.execute("DELETE FROM cities")
            self.assertEqual(snapshot.one("SELECT COUNT(*) AS n FROM cities")["n"], 2)

    def test_unpublished_or_wrong_backend_never_serves_partial_batch(self):
        connection = self.connect(self.admin)
        try:
            for key, value in (("publicationStatus", "LOADING"), ("storageBackend", "sqlite")):
                with self.subTest(key=key):
                    with connection.cursor() as cursor:
                        cursor.execute("UPDATE __metadata SET `value`=%s WHERE `key`=%s", (json.dumps(value), key))
                    connection.commit()
                    try:
                        response = self.mysql_client.get("/api/v1/health")
                        self.assertEqual(response.status_code, 503)
                        self.assertEqual(response.json()["code"], "DATA_NOT_READY")
                        self.assertIsNone(response.json()["meta"]["publishedBatchId"])
                    finally:
                        restored = "PUBLISHED" if key == "publicationStatus" else "mysql"
                        with connection.cursor() as cursor:
                            cursor.execute("UPDATE __metadata SET `value`=%s WHERE `key`=%s", (json.dumps(restored), key))
                        connection.commit()
        finally:
            connection.close()

    def test_connection_errors_are_safe_and_do_not_fall_back(self):
        settings = replace(self.reader_settings, password="private-incorrect-password")
        with TestClient(create_app(mysql_settings=settings)) as client:
            response = client.get("/api/v1/health")
        self.assertEqual(response.status_code, 503)
        self.assertEqual(response.json()["code"], "DATA_UNAVAILABLE")
        for private in (settings.password, settings.user, settings.database, settings.host):
            self.assertNotIn(private, response.text)

    def test_request_transaction_sees_one_consistent_batch(self):
        connection = self.connect(self.admin)
        try:
            with open_snapshot(mysql_settings=self.reader_settings) as snapshot:
                self.assertEqual(snapshot.metadata["publishedBatchId"], "batch-1")
                with connection.cursor() as cursor:
                    cursor.execute("UPDATE __metadata SET `value`=%s WHERE `key`='publishedBatchId'", (json.dumps("batch-next"),))
                connection.commit()
                value = snapshot.one("SELECT `value` FROM __metadata WHERE `key` = ?", ("publishedBatchId",))["value"]
                self.assertEqual(json.loads(value), "batch-1")
        finally:
            with connection.cursor() as cursor:
                cursor.execute("UPDATE __metadata SET `value`=%s WHERE `key`='publishedBatchId'", (json.dumps("batch-1"),))
            connection.commit()
            connection.close()


if __name__ == "__main__":
    unittest.main()
