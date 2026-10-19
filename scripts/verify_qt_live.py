#!/usr/bin/env python3
"""Exercise real Qt TCP -> SQLite -> HTTP using a NEW temporary demo database.

Never connects to an existing Qt server or changes the user's business database.
Requires a built server and the API dependencies; no MySQL or GPU is needed.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import socket
import sqlite3
import struct
import sys
import tempfile
import time
from urllib.error import HTTPError

from launch_demo import ROOT, children, executable, http_json, tcp_ready, wait_ready


def unused_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def receive(sock, length):
    chunks = bytearray()
    while len(chunks) < length:
        part = sock.recv(length - len(chunks))
        if not part:
            raise AssertionError("Qt closed the TCP connection unexpectedly")
        chunks.extend(part)
    return bytes(chunks)


def exchange(sock, operation, data):
    request_id = str(time.monotonic_ns())
    payload = json.dumps({"protocolVersion": 1, "kind": "REQUEST", "type": operation,
                          "requestId": request_id, "data": data}).encode()
    sock.sendall(struct.pack("!I", len(payload)) + payload)
    for _ in range(50):
        length = struct.unpack("!I", receive(sock, 4))[0]
        assert 0 < length <= 1024 * 1024, "Invalid response frame length"
        response = json.loads(receive(sock, length))
        if response.get("kind") == "EVENT":
            continue
        assert response["requestId"] == request_id, "Wrong response identity"
        assert response["success"], f"{operation}: {response.get('error')}"
        return response["data"]
    raise AssertionError("No matching Qt response")


def run(build_dir: Path, log_dir: Path):
    if not __debug__:
        raise RuntimeError("Run verification without Python -O; acceptance assertions must remain enabled.")
    server = executable(build_dir, "server")
    with tempfile.TemporaryDirectory(prefix="charging-qt-live-") as temporary:
        database = Path(temporary) / "fresh-demo.sqlite3"
        tcp_port, http_port = unused_port(), unused_port()
        while http_port == tcp_port:
            http_port = unused_port()
        env = dict(os.environ, QT_SQLITE_PATH=str(database), QT_QPA_PLATFORM="offscreen",
                   PYTHONUNBUFFERED="1")
        url = f"http://127.0.0.1:{http_port}"
        env["CHARGING_DASHBOARD_URL"] = url + "/?embedded=operator"
        with children(log_dir) as start:
            qt = start("verify-qt", [server, "--address", "127.0.0.1", "--port", tcp_port,
                                      "--database", database, "--demo-seed",
                                      "--no-dashboard-autostart"], env)
            wait_ready(qt, lambda: database.is_file() and tcp_ready(tcp_port), "Qt server")
            api = start("verify-api", [sys.executable, "-m", "data_analysis.delivery.cli", "serve",
                                       "--qt-live-only", "--port", http_port], env)
            wait_ready(api, lambda: http_json(url + "/api/v1/qt-live/health")["code"] == "OK", "HTTP API")

            def snapshot():
                reply = http_json(url + "/api/v1/qt-live/snapshot")
                assert reply["code"] == "OK"
                assert reply["meta"]["mergedWithHistory"] is False
                assert reply["meta"]["scope"] == "CURRENT_QT_BUSINESS"
                assert reply["meta"]["simulated"] is True
                assert "13800138000" not in json.dumps(reply), "PII leaked through public snapshot"
                return reply["data"]

            before = snapshot()
            assert before["summary"]["totalStations"] > 0
            assert len({station["city"] for station in before["stations"] if station["city"]}) == 5, "Demo city metadata missing"
            with socket.create_connection(("127.0.0.1", tcp_port), timeout=5) as sock:
                exchange(sock, "USER_LOGIN", {"phone": "13800138000"})
                # Query only for choosing an available charger; no SQL writes.
                con = sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)
                try:
                    charger = con.execute("SELECT c.id FROM chargers c JOIN stations s ON s.id=c.station_id "
                                          "WHERE c.status='AVAILABLE' AND s.status='ACTIVE' ORDER BY c.id LIMIT 1").fetchone()
                finally:
                    con.close()
                assert charger, "No available charger in demo seed"
                reserved = exchange(sock, "RESERVE_CHARGER", {"chargerId": str(charger[0])})
                assert snapshot()["summary"]["chargerStatusCounts"].get("RESERVED", 0) >= 1
                started = exchange(sock, "START_CHARGING", {"reservationId": reserved["reservation"]["id"]})
                order_id = started["order"]["id"]
                during = snapshot()
                assert during["summary"]["chargingOrders"] == before["summary"]["chargingOrders"] + 1
                deadline = time.monotonic() + 12
                while True:
                    current = exchange(sock, "GET_CHARGING_STATUS", {"orderId": order_id})["order"]
                    if current["energyWh"] > 0 and current["amountCents"] > 0:
                        break
                    assert time.monotonic() < deadline, "Simulated charging did not advance"
                    time.sleep(0.5)
                stopped = exchange(sock, "STOP_CHARGING", {"orderId": order_id})["order"]
                pending = snapshot()
                assert pending["summary"]["completedOrders"] == before["summary"]["completedOrders"]
                assert pending["summary"]["paidAmountCents"] == before["summary"]["paidAmountCents"]
                balance_before = exchange(sock, "GET_USER_INFO", {})["user"]["balanceCents"]
                exchange(sock, "PAY_ORDER", {"orderId": order_id})
                paid = snapshot()
                for key, increment in (("completedOrders", 1), ("paidAmountCents", stopped["amountCents"]),
                                       ("completedEnergyWh", stopped["energyWh"])):
                    assert paid["summary"][key] == before["summary"][key] + increment, key
                balance_after = exchange(sock, "GET_USER_INFO", {})["user"]["balanceCents"]
                exchange(sock, "PAY_ORDER", {"orderId": order_id})
                repeated = snapshot()
                assert repeated["summary"] == paid["summary"], "Duplicate payment altered aggregate"
                assert exchange(sock, "GET_USER_INFO", {})["user"]["balanceCents"] == balance_after, "Duplicate payment changed user balance"
                assert balance_before - balance_after == stopped["amountCents"], "Payment debit differs from order amount"
            # An explicitly disabled history cannot masquerade as current data.
            try:
                http_json(url + "/api/v1/intelligence/models")
                raise AssertionError("Qt-only mode silently served history")
            except HTTPError as exc:
                assert exc.code == 503
                assert json.load(exc)["code"] == "HISTORICAL_SERVICES_DISABLED"
            return {"status": "PASS", "verifiedAt": datetime.now(timezone.utc).isoformat(),
                    "transport": "real Qt TCP + read-only SQLite + real HTTP",
                    "checks": ["reservation state", "charging state", "unpaid exclusion", "paid amount/energy",
                               "duplicate payment idempotency", "no PII", "historical boundary"],
                    "before": before["summary"], "after": paid["summary"],
                    "paidOrder": {"amountCents": stopped["amountCents"], "energyWh": stopped["energyWh"]}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-delivery")
    parser.add_argument("--log-dir", type=Path, default=ROOT / "runtime/verification")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    result = run(args.build_dir.resolve(), args.log_dir.resolve())
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(rendered, encoding="utf-8")
    print(rendered)


if __name__ == "__main__":
    main()
