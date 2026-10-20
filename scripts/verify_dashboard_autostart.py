#!/usr/bin/env python3
"""Verify a real Qt process starts its own Vue/API and cleans it up on exit.

Uses a fresh temporary Qt database and Qt-only configuration. No MySQL or user
database is touched. Run with the API virtualenv's Python after building Qt/Vue.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit
from urllib.request import urlopen

from launch_demo import executable

ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def listening(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.3):
            return True
    except OSError:
        return False


def verify(build_dir):
    server = executable(build_dir, "server")
    with tempfile.TemporaryDirectory(prefix="charging-autostart-") as directory:
        temporary = Path(directory)
        database = temporary / "business.sqlite3"
        config = temporary / "service.json"
        config.write_text(json.dumps({"pythonExecutable": sys.executable,
                                      "mode": "qt-live-only"}), encoding="utf-8")
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            tcp_port = probe.getsockname()[1]
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
                   CHARGING_DASHBOARD_CONFIG=str(config), PYTHONUNBUFFERED="1")
        env.pop("CHARGING_DASHBOARD_URL", None)
        env.pop("CHARGING_WEB_PYTHON", None)
        process = subprocess.Popen([str(server), "--database", str(database), "--demo-seed",
                                    "--address", "127.0.0.1", "--port", str(tcp_port)],
                                   cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
        lines = queue.Queue()

        def collect():
            for line in process.stdout:
                lines.put(line.rstrip())
            lines.put(None)

        reader = threading.Thread(target=collect, daemon=True)
        reader.start()
        api_port = None
        try:
            deadline = time.monotonic() + 100
            url = None
            while time.monotonic() < deadline:
                try:
                    line = lines.get(timeout=0.5)
                except queue.Empty:
                    require(process.poll() is None, "Qt exited before its web service was ready")
                    continue
                require(line is not None, "Qt exited before its web service was ready")
                if line.startswith("CHARGING_DASHBOARD_READY="):
                    url = line.split("=", 1)[1]
                    break
            require(url is not None, "Qt did not announce web readiness within 100 seconds")
            parts = urlsplit(url)
            require(parts.scheme == "http" and parts.hostname == "127.0.0.1" and parts.port,
                    "Managed dashboard did not bind to a local port")
            api_port = parts.port
            base = f"http://127.0.0.1:{api_port}"
            with urlopen(base + "/api/v1/qt-live/snapshot", timeout=10) as response:
                snapshot = json.load(response)
            summary = snapshot["data"]["summary"]
            require(snapshot["code"] == "OK" and summary["totalStations"] == 25
                    and summary["totalChargers"] == 75 and summary["totalOrders"] == 0,
                    "Managed API did not read the fresh Qt database")
            with urlopen(base + "/", timeout=10) as response:
                require(response.status == 200 and b'<div id="app">' in response.read(),
                        "Managed service did not serve the built Vue application")
            # Termination deliberately bypasses Qt's normal window-close path:
            # the Python parent watcher must still remove the owned API.
            process.terminate()
            process.wait(timeout=10)
            deadline = time.monotonic() + 20
            while listening(api_port) and time.monotonic() < deadline:
                time.sleep(0.1)
            require(not listening(api_port), "Managed API survived its owning Qt process")
            return {"result": "PASS", "mode": "qt-live-only", "qtStartedApi": True,
                    "vueServed": True, "sameQtDatabase": True, "stations": 25,
                    "chargers": 75, "apiStoppedAfterQtExit": True}
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            reader.join(timeout=2)
            if not reader.is_alive():
                process.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-delivery")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    result = verify(args.build_dir.resolve())
    encoded = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(encoded, encoding="utf-8")
    print(encoded, end="")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"Dashboard auto-start verification failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from None
