#!/usr/bin/env python3
"""Start the local Qt/Vue demo with one SQLite path and owned child processes.

Use the delivery virtualenv's Python. Ctrl+C stops only processes started here.
MySQL publication/training remain explicit preparation steps for --full.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
from urllib.error import URLError
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
# Direct `python scripts/launch_demo.py` execution starts with scripts/ on the
# import path; the shared configuration parser lives in the project package.
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def _configuration_environment(config_file: Path | None = None):
    """Keep an explicitly selected file stable across child working directories."""
    inherited = dict(os.environ)
    if config_file is not None:
        inherited["CHARGING_SERVICES_CONFIG"] = str(config_file.expanduser().resolve())
    if "CHARGING_SERVICES_CONFIG" in inherited:
        from data_analysis.runtime_config import ConfigError
        if not inherited["CHARGING_SERVICES_CONFIG"].strip():
            raise ConfigError("服务配置 CHARGING_SERVICES_CONFIG 须指定配置文件路径")
        inherited["CHARGING_SERVICES_CONFIG"] = str(
            Path(inherited["CHARGING_SERVICES_CONFIG"]).expanduser().resolve())
    return inherited


def configured_server_environment(config_file: Path | None = None):
    """The management server and its API consume AI configuration only."""
    from data_analysis.runtime_config import load_ai_environment

    inherited = _configuration_environment(config_file)
    environment = load_ai_environment(environ=inherited)
    environment.update({key: value for key, value in inherited.items()
                        if not key.startswith(("TENCENT_MAP_", "CHARGING_TENCENT_MAP_"))})
    return environment


def configured_client_environment(config_file: Path | None = None):
    """The optional launcher only shares map configuration with the client."""
    from data_analysis.runtime_config import client_map_environment

    inherited = _configuration_environment(config_file)
    environment = {key: value for key, value in inherited.items()
                   if not key.startswith("ML_ADVISOR_")}
    environment.update(client_map_environment(environ=inherited))
    return environment


def configured_environments(config_file: Path | None = None, *, include_client: bool = True):
    return (configured_server_environment(config_file),
            configured_client_environment(config_file) if include_client else {})


def executable(build_dir: Path, component: str) -> Path:
    name = "charging-" + component
    candidates = [build_dir / component / (name + (".exe" if os.name == "nt" else "")),
                  build_dir / component / "Release" / (name + ".exe"),
                  build_dir / component / (name + ".app") / "Contents/MacOS" / name]
    for path in candidates:
        if path.is_file():
            return path.resolve()
    raise RuntimeError(f"Missing {name}; build the project first: {build_dir}")


def require_free_port(port: int):
    with socket.socket() as probe:
        try:
            probe.bind(("127.0.0.1", port))
        except OSError:
            raise RuntimeError(f"Port {port} is already occupied; choose another port.") from None


def http_json(url: str):
    with urlopen(url, timeout=2) as response:
        return json.load(response)


def wait_ready(process: subprocess.Popen, probe, description: str, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"{description} exited with code {process.returncode}; inspect its log.")
        try:
            if probe():
                return
        except (OSError, URLError, ValueError):
            pass
        time.sleep(0.2)
    raise RuntimeError(f"Timed out waiting for {description}; inspect its log.")


def tcp_ready(port: int):
    with socket.create_connection(("127.0.0.1", port), timeout=1):
        return True


@contextmanager
def children(log_dir: Path):
    processes = []
    logs = []
    log_dir.mkdir(parents=True, exist_ok=True)

    def start(name, argv, env):
        log = (log_dir / (name + ".log")).open("w", encoding="utf-8")
        logs.append(log)
        process = subprocess.Popen([str(arg) for arg in argv], cwd=ROOT, env=env,
                                   stdout=log, stderr=subprocess.STDOUT)
        processes.append(process)
        return process

    try:
        yield start
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
        for process in reversed(processes):
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        for log in logs:
            log.close()


def port_number(value):
    port = int(value)
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return port


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-delivery")
    parser.add_argument("--services-config", type=Path, help="local AI/map configuration JSON")
    parser.add_argument("--database", type=Path, default=ROOT / "runtime/competition.sqlite3")
    parser.add_argument("--demo-seed", action="store_true", help="initialize simulated stations/users (demo DB only)")
    parser.add_argument("--full", action="store_true", help="require prepared historical MySQL and all trained models")
    parser.add_argument("--no-client", action="store_true", help="launch management server and Vue only")
    parser.add_argument("--headless", action="store_true", help="offscreen server and no QML client")
    parser.add_argument("--tcp-port", type=port_number, default=9527)
    parser.add_argument("--http-port", type=port_number, default=8000)
    parser.add_argument("--log-dir", type=Path, default=ROOT / "runtime/logs")
    args = parser.parse_args()
    database = args.database.expanduser().resolve()
    server = executable(args.build_dir, "server")
    client = executable(args.build_dir, "client") if not (args.no_client or args.headless) else None
    if not (ROOT / "data_analysis/frontend/dist/index.html").is_file():
        parser.error("Build Vue first: cd data_analysis/frontend && npm ci && npm run build")
    if args.tcp_port == args.http_port:
        parser.error("TCP and HTTP ports must differ")
    require_free_port(args.tcp_port)
    require_free_port(args.http_port)
    database.parent.mkdir(parents=True, exist_ok=True)
    env, client_env = configured_environments(args.services_config, include_client=client is not None)
    env.update(QT_SQLITE_PATH=str(database), PYTHONUNBUFFERED="1")
    url = f"http://127.0.0.1:{args.http_port}"
    env["CHARGING_DASHBOARD_URL"] = url + "/?embedded=operator"
    qt_env = dict(env)
    if args.headless:
        qt_env["QT_QPA_PLATFORM"] = "offscreen"
    with children(args.log_dir.resolve()) as start:
        # This launcher owns the API child and its shutdown; Qt must not start a second one.
        server_args = [server, "--address", "127.0.0.1", "--port", args.tcp_port,
                       "--database", database, "--no-dashboard-autostart"]
        if args.demo_seed:
            server_args.append("--demo-seed")
        qt = start("qt-server", server_args, qt_env)
        wait_ready(qt, lambda: database.is_file() and tcp_ready(args.tcp_port), "Qt server")
        mode = [] if args.full else ["--qt-live-only"]
        subprocess.run([sys.executable, "-m", "data_analysis.delivery.cli", "check", *mode],
                       cwd=ROOT, env=env, check=True)
        api = start("web-api", [sys.executable, "-m", "data_analysis.delivery.cli", "serve",
                                "--host", "127.0.0.1", "--port", args.http_port, *mode], env)
        wait_ready(api, lambda: http_json(url + "/api/v1/qt-live/health").get("code") == "OK", "Qt live API")
        active = [qt, api]
        if client:
            active.append(start("qt-client", [client, "--host", "127.0.0.1", "--port", args.tcp_port], client_env))
        print(f"Ready: {url}/?embedded=operator\nSQLite: {database}\nLogs: {args.log_dir.resolve()}", flush=True)
        print("Mode: " + ("complete historical ML + current Qt business" if args.full else
                          "current Qt business only; historical ML disabled"), flush=True)
        print("Press Ctrl+C to stop this demo.", flush=True)
        while all(process.poll() is None for process in active):
            time.sleep(0.5)
        if any(process.returncode not in (None, 0) for process in active):
            raise RuntimeError("A demo component failed; inspect the logs.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Demo stopped.")
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"Demo startup failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from None
