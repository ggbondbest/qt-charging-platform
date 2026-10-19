"""Current Qt snapshots tested against the actual Qt schema, never user files."""
from contextlib import closing
from datetime import datetime
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

HAS_API = all(importlib.util.find_spec(name) for name in ("fastapi", "httpx"))
if HAS_API:
    from fastapi import FastAPI
    from fastapi.testclient import TestClient

    from data_analysis.delivery.app import create_qt_live_app
    from data_analysis.delivery.qt_live_adapter import CHARGER_STATES, ORDER_STATES, QtLiveAdapter, QtLiveError
    from data_analysis.delivery import qt_live_adapter as qt_module


ROOT = Path(__file__).resolve().parents[2]
TIME = "2026-10-17T08:15:30.125+08:00"


def seed_database(path):
    with closing(sqlite3.connect(path)) as connection, connection:
        connection.executescript((ROOT / "database/schema.sql").read_text(encoding="utf8"))
        for identity in range(1, 6):
            connection.execute("INSERT INTO users (id, phone, nickname) VALUES (?, ?, ?)",
                               (identity, f"1380000000{identity}", f"private-nickname-{identity}"))
        connection.execute("""INSERT INTO stations
            (id,code,name,address,city,district,contact_name,contact_phone,latitude,longitude,price_cents_per_kwh)
            VALUES (1,'STA-LIVE-001','当前模拟站','演示地址','北京','东城','private-contact','13912345678',39.9,116.4,125)""")
        connection.execute("""INSERT INTO stations
            (id,code,name,address,latitude,longitude,price_cents_per_kwh,status)
            VALUES (2,'STA-LIVE-002','暂停站','演示地址',39.9,116.4,100,'INACTIVE')""")
        for identity, status in enumerate(CHARGER_STATES, 1):
            connection.execute("""INSERT INTO chargers
                (id,station_id,code,type,power_watts,status) VALUES (?,1,?,'FAST',60000,?)""",
                (identity, f"CH-{identity}", status))
        for identity, status in enumerate(ORDER_STATES, 1):
            connection.execute("""INSERT INTO orders
                (id,order_no,user_id,charger_id,status,unit_price_cents_per_kwh,energy_wh,amount_cents,
                 created_at,started_at,stopped_at,paid_at)
                VALUES (?,?,?,?,?,125,?,?,?,?,?,?)""",
                (identity, f"private-order-no-{identity}", identity, identity, status,
                 4500 if status == "COMPLETED" else identity * 1000,
                 1234 if status == "COMPLETED" else identity * 100, TIME,
                 TIME if status in ("CHARGING", "WAITING_PAYMENT", "COMPLETED") else None,
                 TIME if status in ("WAITING_PAYMENT", "COMPLETED") else None,
                 TIME if status == "COMPLETED" else None))
        connection.execute("""INSERT INTO recharge_records
            (transaction_no,user_id,amount_cents,balance_after_cents,status)
            VALUES ('PRIVATE-TOPUP',1,100000,100000,'SUCCESS')""")


@unittest.skipUnless(HAS_API, "Install requirements-api.txt for Qt snapshot HTTP tests")
class QtLiveAdapterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        # SQLite URI escaping must preserve spaces, # and ? in real file names.
        self.path = Path(self.temp.name) / ("Qt 业务 #1.sqlite3" if os.name == "nt" else "Qt 业务 #1?.sqlite3")
        seed_database(self.path)
        self.adapter = QtLiveAdapter(self.path)

    def test_completed_totals_and_all_charger_states_keep_exact_units(self):
        result = self.adapter.snapshot()
        summary = result["summary"]
        self.assertEqual((summary["totalStations"], summary["activeStations"], summary["totalChargers"]), (2, 1, 5))
        self.assertEqual(summary["chargerStatusCounts"], dict.fromkeys(CHARGER_STATES, 1))
        self.assertEqual(summary["orderStatusCounts"], dict.fromkeys(ORDER_STATES, 1))
        self.assertEqual((summary["totalOrders"], summary["chargingOrders"], summary["completedOrders"]), (5, 1, 1))
        self.assertEqual(summary["paidAmountCents"], 1234)
        self.assertEqual(summary["completedEnergyWh"], 4500)
        self.assertEqual(result["stations"][1]["chargerStatusCounts"], dict.fromkeys(CHARGER_STATES, 0))
        self.assertEqual(result["stations"][1]["totalChargers"], 0)
        self.assertIsNone(result["stations"][1]["city"])
        self.assertTrue(result["simulated"])
        self.assertEqual(summary["totalUsers"], 5)
        self.assertEqual(summary["activeOrders"], 3)
        self.assertEqual(summary["onlineChargers"], 4)  # FAULT is online in the native dashboard.
        self.assertEqual(summary["onlineRatio"], 0.8)

    def test_beijing_day_month_paid_boundaries_and_created_order_counts(self):
        fixed = datetime.fromisoformat("2026-09-30T16:30:00+00:00")

        class FrozenDateTime(datetime):
            @classmethod
            def now(cls, tz=None):
                return fixed.astimezone(tz)

        with closing(sqlite3.connect(self.path)) as connection, connection:
            connection.execute("DELETE FROM orders")
            values = [("2026-09-30T15:59:59Z", "2026-09-30T15:59:59Z", 100, 1000),
                      ("2026-09-30T16:00:00Z", "2026-09-30T15:00:00Z", 200, 2000),
                      ("2026-10-01T00:20:00+08:00", "2026-10-01T00:10:00+08:00", 300, 3000)]
            for index, (paid, created, cents, energy) in enumerate(values):
                connection.execute("""INSERT INTO orders (order_no,user_id,charger_id,status,unit_price_cents_per_kwh,
                    amount_cents,energy_wh,created_at,started_at,stopped_at,paid_at)
                    VALUES (?,1,1,'COMPLETED',125,?,?,?,?,?,?)""", (f"boundary-{index}", cents, energy, created, created, paid, paid))
            connection.execute("""INSERT INTO orders (order_no,user_id,charger_id,status,unit_price_cents_per_kwh,
                amount_cents,energy_wh,created_at,started_at,stopped_at)
                VALUES ('unpaid-boundary',1,1,'WAITING_PAYMENT',125,9000,90000,?,?,?)""",
                ("2026-10-01T00:05:00+08:00",) * 3)
        with patch.object(qt_module, "datetime", FrozenDateTime):
            result = self.adapter.snapshot()
            monthly = self.adapter.snapshot(trend_days=30)
        summary, overview = result["summary"], result["overview"]
        self.assertEqual((summary["paidAmountCents"], summary["completedEnergyWh"]), (600, 6000))
        self.assertEqual((summary["todayRevenueCents"], summary["monthRevenueCents"]), (500, 500))
        self.assertEqual((summary["todayOrders"], summary["todayCompletedOrders"], summary["todayCompletedEnergyWh"]), (2, 2, 5000))
        self.assertEqual((overview["timeZone"], overview["today"], overview["trendDays"]), ("Asia/Shanghai", "2026-10-01", 7))
        self.assertEqual(len(overview["revenueTrend"]), 7)
        self.assertEqual(overview["revenueTrend"][-2]["revenueCents"], 100)
        self.assertEqual(overview["revenueTrend"][-1], dict(date="2026-10-01", revenueCents=500, completedOrderCount=2, completedEnergyWh=5000))
        self.assertEqual(overview["revenueTrend"][0]["revenueCents"], 0)
        self.assertEqual(len(monthly["overview"]["revenueTrend"]), 30)
        self.assertEqual(monthly["overview"]["revenueTrend"][-1], overview["revenueTrend"][-1])

    def test_abnormal_chargers_match_native_filter_without_private_operator_text(self):
        with closing(sqlite3.connect(self.path)) as connection, connection:
            connection.execute("UPDATE chargers SET updated_at='2027-01-01T00:00:00Z' WHERE id=1")
            for charger, status, when in ((1, "ACTIVE", TIME), (1, "RECOVERED", "2026-10-18T00:00:00Z"),
                                           (2, "RECOVERED", TIME)):
                connection.execute("""INSERT INTO charger_exceptions
                    (charger_id,code,severity,safe_summary,status,occurred_at,updated_at)
                    VALUES (?,'SIMULATED_FAULT','WARNING','private-operator-phone-13800000001',?,?,?)""", (charger, status, when, when))
        result = self.adapter.snapshot()["overview"]["abnormalChargers"]
        self.assertEqual(result["total"], 3)  # Two abnormal statuses and one AVAILABLE with open event.
        self.assertEqual({row["qtChargerId"] for row in result["items"]}, {1, 4, 5})
        event = result["items"][0]["activeException"]
        self.assertEqual((event["status"], event["safeSummary"]), ("ACTIVE", "模拟电桩故障"))
        self.assertTrue(any(row["activeException"] is None for row in result["items"]))
        self.assertNotIn("private-", json.dumps(result))
        self.assertNotIn("13800000001", json.dumps(result))
        with closing(sqlite3.connect(self.path)) as connection, connection:
            for index in range(6, 10):
                connection.execute("INSERT INTO chargers (station_id,code,type,power_watts,status) VALUES (1,?,'FAST',60000,'FAULT')", (f"extra-{index}",))
        capped = self.adapter.snapshot()["overview"]["abnormalChargers"]
        self.assertEqual((capped["total"], len(capped["items"])), (7, 5))

    def test_no_private_rows_or_contact_fields_and_utc_normalization(self):
        result = self.adapter.snapshot()
        serialized = json.dumps(result)
        for prohibited in ("userId", "user_id", "phone", "nickname", "contact", "balance", "order_no", "private-", "13800000001", "13912345678", str(self.path)):
            self.assertNotIn(prohibited, serialized)
        self.assertTrue(result["asOf"].endswith("Z"))
        self.assertIsNotNone(datetime.fromisoformat(result["asOf"].replace("Z", "+00:00")).tzinfo)
        completed = next(item for item in result["orders"] if item["status"] == "COMPLETED")
        self.assertEqual(completed["paidAt"], "2026-10-17T00:15:30.125Z")
        self.assertEqual(completed["qtStationCode"], "STA-LIVE-001")

    def test_queries_are_read_only_and_connections_close(self):
        with self.adapter._read() as (connection, _):
            self.assertEqual(connection.execute("PRAGMA query_only").fetchone()[0], 1)
            with self.assertRaises(sqlite3.OperationalError):
                connection.execute("DELETE FROM orders")
            connection.execute("PRAGMA query_only = OFF")
            with self.assertRaises(sqlite3.OperationalError):
                connection.execute("DELETE FROM orders")  # mode=ro is an independent guard.
        with self.assertRaises(sqlite3.ProgrammingError):
            connection.execute("SELECT 1")
        self.assertEqual(self.adapter.summary()["summary"]["totalOrders"], 5)

    def test_snapshot_is_consistent_during_concurrent_qt_commit_and_next_poll_updates(self):
        original = self.adapter._summary

        def read_then_write(connection, as_of, paid_days):
            summary = original(connection, as_of, paid_days)
            with closing(sqlite3.connect(self.path)) as writer, writer:
                writer.execute("UPDATE chargers SET status = 'OFFLINE' WHERE id = 1")
                writer.execute("UPDATE orders SET amount_cents = 4321 WHERE status = 'COMPLETED'")
            return summary

        with patch.object(self.adapter, "_summary", side_effect=read_then_write):
            result = self.adapter.snapshot()
        self.assertEqual(result["summary"]["chargerStatusCounts"]["AVAILABLE"], 1)
        self.assertEqual(result["stations"][0]["chargerStatusCounts"]["AVAILABLE"], 1)
        completed = next(item for item in result["orders"] if item["status"] == "COMPLETED")
        self.assertEqual(completed["amountCents"], result["summary"]["paidAmountCents"])
        updated = self.adapter.snapshot()
        self.assertEqual(updated["summary"]["paidAmountCents"], 4321)
        self.assertEqual(updated["stations"][0]["chargerStatusCounts"]["AVAILABLE"], 0)
        self.assertEqual(result["overview"]["abnormalChargers"]["total"], 2)
        self.assertEqual(updated["overview"]["abnormalChargers"]["total"], 3)

    def test_paging_filter_and_truncation(self):
        result = self.adapter.snapshot(station_limit=1, order_limit=2)
        self.assertEqual(result["truncated"], {"stations": True, "orders": True})
        self.assertEqual([item["qtOrderId"] for item in result["orders"]], [5, 4])
        self.assertEqual(self.adapter.stations(limit=1, offset=1)["stations"][0]["qtStationId"], 2)
        orders = self.adapter.orders(status="COMPLETED")
        self.assertEqual(orders["total"], 1)
        self.assertEqual(orders["orders"][0]["qtOrderId"], 4)
        for args in ({"limit": 0}, {"limit": 101}, {"offset": -1}, {"status": "COMPLETED' OR 1=1"}):
            with self.subTest(args=args), self.assertRaises(QtLiveError) as error:
                self.adapter.orders(**args)
            self.assertEqual(error.exception.code, "INVALID_ARGUMENT")

    def test_missing_configuration_file_invalid_schema_and_corruption_are_explicit(self):
        missing = Path(self.temp.name) / "missing.sqlite3"
        empty = Path(self.temp.name) / "empty.sqlite3"
        sqlite3.connect(empty).close()
        corrupt = Path(self.temp.name) / "private-corrupted.sqlite3"
        corrupt.write_text("not SQLite", encoding="utf8")
        with patch.dict(os.environ, {}, clear=True):
            candidates = [(QtLiveAdapter(), "QT_DATABASE_NOT_CONFIGURED"),
                          (QtLiveAdapter(missing), "QT_DATABASE_UNAVAILABLE"),
                          (QtLiveAdapter(empty), "QT_SCHEMA_MISMATCH"),
                          (QtLiveAdapter(corrupt), "QT_DATABASE_UNAVAILABLE"),
                          (QtLiveAdapter("relative.sqlite3"), "QT_DATABASE_UNAVAILABLE")]
        for adapter, expected in candidates:
            with self.subTest(expected=expected), self.assertRaises(QtLiveError) as error:
                adapter.health()
            self.assertEqual(error.exception.code, expected)
            self.assertNotIn(self.temp.name, str(error.exception))
        self.assertFalse(missing.exists(), "Readonly opening must never create a database")

    def test_busy_database_is_not_reported_as_zero_data(self):
        locked = Path(self.temp.name) / "locked.sqlite3"
        seed_database(locked)
        writer = sqlite3.connect(locked)
        self.addCleanup(writer.close)
        writer.execute("PRAGMA journal_mode = DELETE")
        writer.execute("BEGIN EXCLUSIVE")
        try:
            with self.assertRaises(QtLiveError) as error:
                QtLiveAdapter(locked, timeout_seconds=0.01).snapshot()
            self.assertEqual(error.exception.code, "QT_DATABASE_BUSY")
        finally:
            writer.rollback()

    def test_unsafe_aggregate_and_naive_timestamp_are_rejected(self):
        with closing(sqlite3.connect(self.path)) as connection, connection:
            connection.execute("UPDATE orders SET amount_cents = 9007199254740991 WHERE id = 4")
            connection.execute("""INSERT INTO orders
                (order_no,user_id,charger_id,status,unit_price_cents_per_kwh,amount_cents,started_at,stopped_at,paid_at)
                VALUES ('SECOND-COMPLETED',4,4,'COMPLETED',125,1,?,?,?)""", (TIME, TIME, TIME))
        with self.assertRaises(QtLiveError) as error:
            self.adapter.summary()
        self.assertEqual(error.exception.code, "QT_DATA_INVALID")
        with closing(sqlite3.connect(self.path)) as connection, connection:
            connection.execute("UPDATE orders SET amount_cents = 0, created_at = '2026-10-17T08:00:00'")
        with self.assertRaises(QtLiveError) as error:
            self.adapter.orders()
        self.assertEqual(error.exception.code, "QT_DATA_INVALID")

    def test_empty_database_reports_genuine_zeroes(self):
        empty = Path(self.temp.name) / "empty-valid.sqlite3"
        with closing(sqlite3.connect(empty)) as connection, connection:
            connection.executescript((ROOT / "database/schema.sql").read_text(encoding="utf8"))
        result = QtLiveAdapter(empty).snapshot()
        self.assertEqual(result["summary"]["paidAmountCents"], 0)
        self.assertEqual(result["stations"], [])
        self.assertEqual(result["orders"], [])


@unittest.skipUnless(HAS_API, "Install requirements-api.txt for Qt snapshot HTTP tests")
class QtLiveApiTests(unittest.TestCase):
    def setUp(self):
        QtLiveAdapterTests.setUp(self)
        self.dist = Path(self.temp.name) / "dist"
        self.dist.mkdir()
        (self.dist / "index.html").write_text("<html>Qt live demo</html>", encoding="utf8")
        self.client = TestClient(create_qt_live_app(qt_database_path=self.path, frontend_dist=self.dist))
        self.addCleanup(self.client.close)

    def test_api_envelope_and_same_origin_static_page(self):
        response = self.client.get("/api/v1/qt-live/snapshot?orderLimit=2&stationLimit=1")
        self.assertEqual(response.status_code, 200, response.text)
        body = response.json()
        self.assertEqual(body["code"], "OK")
        self.assertEqual(body["meta"]["scope"], "CURRENT_QT_BUSINESS")
        self.assertFalse(body["meta"]["mergedWithHistory"])
        self.assertTrue(body["meta"]["simulated"])
        self.assertNotIn("publishedBatchId", body["meta"])
        self.assertEqual(response.headers["x-request-id"], body["meta"]["requestId"])
        self.assertEqual(response.headers["cache-control"], "no-store")
        self.assertEqual(self.client.get("/").status_code, 200)
        trend = self.client.get("/api/v1/qt-live/snapshot?trendDays=30")
        self.assertEqual(trend.status_code, 200, trend.text)
        self.assertEqual(len(trend.json()["data"]["overview"]["revenueTrend"]), 30)
        for endpoint in ("health", "summary", "stations", "orders"):
            self.assertEqual(self.client.get("/api/v1/qt-live/" + endpoint).status_code, 200)

    def test_validation_failure_offline_and_history_failure_remain_separate(self):
        for suffix in ("snapshot?orderLimit=0", "snapshot?stationLimit=501", "snapshot?trendDays=8", "snapshot?trendDays=x", "orders?status=INVALID", "stations?offset=-1"):
            response = self.client.get("/api/v1/qt-live/" + suffix)
            self.assertEqual(response.status_code, 422, response.text)
            self.assertEqual(response.json()["code"], "INVALID_ARGUMENT")
            self.assertIsNone(response.json()["data"])
        historical = self.client.get("/api/v1/datasets")
        self.assertEqual(historical.status_code, 503)
        self.assertEqual(historical.json()["code"], "HISTORICAL_SERVICES_DISABLED")
        self.assertEqual(historical.headers["x-request-id"], historical.json()["meta"]["requestId"])
        self.assertEqual(self.client.get("/api/v1/qt-live/health").status_code, 200)
        self.path.unlink()
        response = self.client.get("/api/v1/qt-live/snapshot")
        self.assertEqual(response.status_code, 503, response.text)
        self.assertIsNone(response.json()["data"])
        self.assertNotIn(self.temp.name, response.text)

    def test_readonly_methods_and_unknown_api_do_not_serve_html(self):
        response = self.client.post("/api/v1/qt-live/orders", json={"amountCents": 0})
        self.assertEqual(response.status_code, 405)
        self.assertEqual(self.client.get("/api/v1/qt-live/unknown").status_code, 404)
        self.assertEqual(self.adapter.summary()["summary"]["totalOrders"], 5)

    def test_missing_frontend_exposes_safe_traceable_error(self):
        with TestClient(create_qt_live_app(qt_database_path=self.path, frontend_dist=self.dist / "missing")) as client:
            response = client.get("/")
        self.assertEqual(response.status_code, 503)
        self.assertEqual(response.json()["code"], "FRONTEND_NOT_BUILT")
        self.assertEqual(response.headers["x-request-id"], response.json()["meta"]["requestId"])
        self.assertNotIn(str(self.dist), response.text)

    def test_live_factory_does_not_import_historical_or_ml_modules(self):
        code = """import sys
from data_analysis.delivery.app import create_qt_live_app
create_qt_live_app()
assert not any(name.startswith(('data_analysis.ml', 'data_analysis.chargepilot',
    'data_analysis.backend', 'numpy', 'sklearn', 'pymysql')) for name in sys.modules)
"""
        result = subprocess.run([sys.executable, "-c", code], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unified_routes_win_over_root_mount_and_do_not_open_history(self):
        from data_analysis.chargepilot.settings import Settings
        from data_analysis.delivery.app import create_app
        application = create_app(settings=Settings(), database_path=":memory:",
            provider=SimpleNamespace(arrival=None, load=None), operational_app=FastAPI(),
            qt_database_path=self.path)
        self.addCleanup(application.state.advisor_runner.close)
        with TestClient(application) as client:
            result = client.get("/api/v1/qt-live/snapshot")
            self.assertEqual(result.status_code, 200, result.text)
            self.assertEqual(result.json()["data"]["summary"]["paidAmountCents"], 1234)
            self.assertEqual(result.headers["x-request-id"], result.json()["meta"]["requestId"])
            self.assertNotIn("publishedBatchId", result.json()["meta"])
            bad = client.get("/api/v1/qt-live/orders?limit=-1")
            self.assertEqual(bad.json()["meta"]["scope"], "CURRENT_QT_BUSINESS")
            history = client.get("/api/v1/dashboard/overview")
            self.assertEqual(history.status_code, 503)
            self.assertEqual(client.get("/api/v1/qt-live/health").status_code, 200)


if __name__ == "__main__":
    unittest.main()
