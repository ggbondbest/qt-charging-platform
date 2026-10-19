"""Ownership and pipe protocol regressions using temporary data and fake MySQL."""
import asyncio
from contextlib import closing
import importlib.util
import io
import json
import os
from pathlib import Path
import queue
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import MagicMock, patch
from types import SimpleNamespace
from urllib.request import urlopen

from scripts import managed_web_service as managed

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/managed_web_service.py"
HAS_API = all(importlib.util.find_spec(name) for name in ("fastapi", "uvicorn"))
HAS_CP_API = HAS_API and importlib.util.find_spec("httpx") is not None
HAS_FRONTEND = (ROOT / "data_analysis/frontend/dist/index.html").is_file()


def wait_line(stream, timeout=15):
    output = queue.Queue()
    threading.Thread(target=lambda: output.put(stream.readline()), daemon=True).start()
    try:
        return output.get(timeout=timeout)
    except queue.Empty:
        raise AssertionError("managed service did not report readiness") from None


class ManagedConfigTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.database = self.base / "business.sqlite3"
        self.database.touch()
        self.config = self.base / "service.json"

    def write(self, config):
        self.config.write_text(json.dumps(config), encoding="utf-8")
        return self.config

    def test_default_is_live_only_and_database_must_exist_absolute(self):
        config = managed.load_config(self.database)
        self.assertEqual(config.mode, "qt-live-only")
        self.assertEqual(config.database, self.database.resolve())
        self.assertIsNone(config.mysql)
        for path in (Path("relative.sqlite3"), self.base / "missing.sqlite3"):
            with self.assertRaises(managed.ServiceError):
                managed.load_config(path)
        self.assertFalse((self.base / "missing.sqlite3").exists())

    def test_private_environment_relative_to_config_and_never_echoed(self):
        env = self.base / "secret.json"
        env.write_text(json.dumps({"TEST_PASSWORD": "private-test-value"}), encoding="utf-8")
        config = managed.load_config(self.database, self.write({"mode": "full", "environmentFile": "secret.json",
                                                               "pythonExecutable": str(sys.executable)}))
        self.assertEqual(config.environment, {"TEST_PASSWORD": "private-test-value"})
        env.write_text('{"password": "private-test-value", "bad": 42}', encoding="utf-8")
        with self.assertRaises(managed.ServiceError) as failure:
            managed.load_config(self.database, self.config)
        self.assertNotIn("private-test-value", str(failure.exception))
        self.assertNotIn(str(env), str(failure.exception))

    def test_rejects_unknown_mode_unknown_keys_and_unprepared_mysql(self):
        for config in ({"mode": "production"}, {"mod": "full"}, {"mysql": {}}, {"mode": "full", "mysql": {}}):
            with self.assertRaises(managed.ServiceError):
                managed.load_config(self.database, self.write(config))
        mysql = {"executable": sys.executable, "baseDir": str(self.base), "dataDir": "unprepared", "port": 33717}
        with self.assertRaises(managed.ServiceError) as failure:
            managed.load_config(self.database, self.write({"mode": "full", "mysql": mysql}))
        self.assertEqual(failure.exception.code, "MYSQL_NOT_PREPARED")
        self.assertFalse((self.base / "unprepared").exists())
        mysql["port"] = True
        with self.assertRaises(managed.ServiceError):
            managed.load_config(self.database, self.write({"mode": "full", "mysql": mysql}))

    def test_parent_watch_current_process_alive(self):
        watch = managed.ParentWatch(os.getpid())
        try:
            self.assertTrue(watch.alive())
        finally:
            watch.close()

    def test_unavailable_database_subprocess_has_only_safe_error(self):
        path = self.base / "private-customer-missing.sqlite3"
        result = subprocess.run([sys.executable, str(SCRIPT), "--database", str(path)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertTrue(result.stderr.startswith(managed.ERROR_PREFIX))
        error = json.loads(result.stderr.removeprefix(managed.ERROR_PREFIX))
        self.assertEqual(error["code"], "QT_DATABASE_UNAVAILABLE")
        self.assertNotIn(str(path), result.stderr)
        self.assertFalse(path.exists())

    def test_invalid_shared_keys_report_safe_error_before_service_start(self):
        keys = self.base / "services.local.json"
        keys.write_text('{"version":1,"ai":{"apiKey":"private-key-must-not-echo",'
                        '"unknown":"private-other-value"}}', encoding="utf-8")
        result = subprocess.run([sys.executable, str(SCRIPT), "--database", str(self.database)],
                                env=dict(os.environ, CHARGING_SERVICES_CONFIG=str(keys)),
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        error = json.loads(result.stderr.removeprefix(managed.ERROR_PREFIX))
        self.assertEqual(error["code"], "INVALID_SERVICES_CONFIG")
        for private in ("private-key-must-not-echo", "private-other-value", str(keys)):
            self.assertNotIn(private, result.stderr)

    def test_managed_server_ignores_invalid_client_maps_and_never_injects_their_keys(self):
        keys = self.base / "services.local.json"
        for maps in ({"webServiceKey": "private-client-map-value", "secretKey": "private-client-secret"},
                     {"webServiceKey": ["invalid-private-client-value"]}):
            with self.subTest(valid=isinstance(maps["webServiceKey"], str)):
                keys.write_text(json.dumps({"version": 1, "ai": {"apiKey": "private-server-ai-key"},
                                            "tencentMap": maps}), encoding="utf-8")
                observed = {}

                async def fake_service(config, parent_pid, protocol_stdout):
                    observed.update(os.environ)

                with patch.dict(os.environ, {"CHARGING_SERVICES_CONFIG": str(keys)}, clear=True), \
                     patch.object(managed, "run_service", side_effect=fake_service), \
                     patch("sys.stdout", new_callable=io.StringIO) as stdout, \
                     patch("sys.stderr", new_callable=io.StringIO) as stderr:
                    self.assertEqual(managed.main(["--database", str(self.database)]), 0)
                    self.assertEqual(stdout.getvalue(), "")
                    self.assertEqual(stderr.getvalue(), "")
                self.assertFalse(any(name.startswith(("TENCENT_MAP_", "CHARGING_TENCENT_MAP_")) for name in observed))
                self.assertNotIn("private-client-map-value", observed.values())


class ManagedDependencyTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.config = managed.MysqlConfig(Path(sys.executable), base, base, 33333,
                                          runtime_dir=base / "logs")

    async def test_reused_listener_is_never_stopped_or_spawned(self):
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        config = managed.MysqlConfig(self.config.executable, self.config.base_dir, self.config.data_dir,
                                    listener.getsockname()[1])
        dependency = managed.OwnedMysql(config)
        with patch.object(managed.subprocess, "Popen") as start:
            await dependency.start(asyncio.Event())
            await dependency.close()
            start.assert_not_called()
        self.assertTrue(managed.tcp_ready(config.port))
        self.assertIsNone(dependency.process)

    async def test_owned_process_is_reaped_and_args_never_initialize(self):
        real_popen = subprocess.Popen
        captured = []

        def fake_mysql(args, **kwargs):
            captured.extend(args)
            return real_popen([sys.executable, "-c", "import time; time.sleep(120)"], **kwargs)

        dependency = managed.OwnedMysql(self.config)
        try:
            with patch.object(managed.subprocess, "Popen", side_effect=fake_mysql), \
                 patch.object(managed, "tcp_ready", side_effect=[False, True]):
                await dependency.start(asyncio.Event())
            self.assertIsNone(dependency.process.poll())
        finally:
            await dependency.close()
        self.assertIsNotNone(dependency.process.poll())
        self.assertIn("--no-defaults", captured)
        self.assertIn("--bind-address=127.0.0.1", captured)
        self.assertFalse(any("initialize" in argument for argument in captured))
        logs = list(self.config.runtime_dir.glob("web-*/process.log"))
        self.assertEqual(len(logs), 1)

    async def test_existing_socket_is_not_replaced(self):
        if os.name == "nt":
            self.skipTest("Unix sockets only")
        path = self.config.data_dir / "existing.sock"
        path.write_text("owned elsewhere", encoding="utf-8")
        config = managed.MysqlConfig(self.config.executable, self.config.base_dir, self.config.data_dir,
                                    self.config.port, path)
        dependency = managed.OwnedMysql(config)
        with patch.object(managed, "tcp_ready", return_value=False), \
             patch.object(managed.subprocess, "Popen") as start:
            with self.assertRaises(managed.ServiceError) as failure:
                await dependency.start(asyncio.Event())
            self.assertEqual(failure.exception.code, "MYSQL_SOCKET_IN_USE")
            start.assert_not_called()
        await dependency.close()
        self.assertEqual(path.read_text(), "owned elsewhere")

    @unittest.skipUnless(HAS_API, "Install API dependencies for startup lifecycle test")
    async def test_stop_during_application_load_still_cleans_owned_dependency(self):
        real_popen = subprocess.Popen
        processes = []

        def fake_mysql(args, **kwargs):
            process = real_popen([sys.executable, "-c", "import time; time.sleep(120)"], **kwargs)
            processes.append(process)
            return process

        def slow_factory(config):
            time.sleep(0.3)
            return object()

        output = io.StringIO()
        config = managed.ServiceConfig(self.config.data_dir / "unused.sqlite3", "full", {}, self.config)
        # Trigger the already-installed stop handler while the factory is in its thread.
        loop = asyncio.get_running_loop()
        timer = loop.call_later(0.1, lambda: signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None))
        try:
            with patch.object(managed.subprocess, "Popen", side_effect=fake_mysql), \
                 patch.object(managed, "tcp_ready", side_effect=[False, True]), \
                 patch.object(managed, "create_application", side_effect=slow_factory):
                await managed.run_service(config, None, output)
            self.assertEqual(output.getvalue(), "")
            self.assertEqual(len(processes), 1)
            self.assertIsNotNone(processes[0].poll())
        finally:
            timer.cancel()
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)

    @unittest.skipUnless(HAS_API, "Install API dependencies for startup lifecycle test")
    async def test_application_failure_does_not_leave_owned_dependency(self):
        real_popen = subprocess.Popen
        processes = []

        def fake_mysql(args, **kwargs):
            process = real_popen([sys.executable, "-c", "import time; time.sleep(120)"], **kwargs)
            processes.append(process)
            return process

        output = io.StringIO()
        config = managed.ServiceConfig(self.config.data_dir / "unused.sqlite3", "full", {}, self.config)
        try:
            with patch.object(managed.subprocess, "Popen", side_effect=fake_mysql), \
                 patch.object(managed, "tcp_ready", side_effect=[False, True]), \
                 patch.object(managed, "create_application", side_effect=ValueError("private-test-secret")):
                with self.assertRaises(ValueError):
                    await managed.run_service(config, None, output)
            self.assertEqual(output.getvalue(), "")
            self.assertEqual(len(processes), 1)
            self.assertIsNotNone(processes[0].poll())
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)


@unittest.skipUnless(HAS_CP_API, "Install API dependencies for operational preparation checks")
class ExistingOperationalStoreTests(unittest.TestCase):
    def setUp(self):
        from data_analysis.chargepilot import app
        from data_analysis.chargepilot.settings import Settings
        self.module = app
        self.settings = Settings()
        self.predictor = SimpleNamespace(catalog=[{"stationId": "station-1", "cityId": "city-1", "capacity": 3}],
                                         cities=[{"cityId": "city-1"}], bounds={}, metadata={}, snapshot=MagicMock(return_value=[]))
        self.store = MagicMock()
        self.store.admin.return_value = {"baselineInitialized": True, "clock": {"time": "2026-05-05T00:00:00Z"}}
        self.routing = MagicMock()
        self.load = SimpleNamespace(report={"status": "READY"})
        self.connection = MagicMock()
        self.cursor = self.connection.cursor.return_value.__enter__.return_value
        self.state = dict(time=1, wall=1, speed=10, paused=False, config={}, baselineInitialized=True, queueSequence=0)
        self.cursor.fetchone.side_effect = lambda: {"payload": json.dumps(self.state)}
        self.cursor.fetchall.side_effect = [[{"payload": json.dumps(item)} for item in self.predictor.cities],
                                           [{"payload": json.dumps(item)} for item in self.predictor.catalog]]

    def app(self, **kwargs):
        return self.module.create_app(self.settings, predictor=self.predictor, store=self.store,
                                      routing=self.routing, load_adapter=self.load, **kwargs)

    def test_default_preserves_explicit_preparation_flow(self):
        self.store.admin.return_value["baselineInitialized"] = False
        with patch.object(self.module, "connect") as connect:
            self.app()
        self.store.initialize.assert_called_once_with(self.predictor.catalog, self.predictor.cities)
        self.store.set_baseline.assert_called_once_with([])
        connect.assert_not_called()

    def test_existing_mode_checks_schema_catalog_and_baseline_without_preparing(self):
        with patch.object(self.module, "connect", return_value=self.connection):
            self.app(initialize_store=False)
        self.store.initialize.assert_not_called()
        self.store.set_baseline.assert_not_called()
        statements = [call.args[0] for call in self.cursor.execute.call_args_list]
        self.assertEqual(statements[:2], ["SET TRANSACTION READ ONLY", "START TRANSACTION WITH CONSISTENT SNAPSHOT"])
        self.assertTrue(all(statement.startswith("SELECT ") for statement in statements[2:]))
        self.assertTrue(any("expires_wall" in statement for statement in statements))
        self.connection.rollback.assert_called_once()
        self.connection.close.assert_called_once()
        self.connection.commit.assert_not_called()

    def test_missing_baseline_rejected_before_any_operational_mutation(self):
        self.state["baselineInitialized"] = False
        with patch.object(self.module, "connect", return_value=self.connection):
            with self.assertRaises(self.module.DomainError) as failure:
                self.app(initialize_store=False)
        self.assertEqual(failure.exception.code, "OPERATIONAL_STORE_NOT_PREPARED")
        self.store.initialize.assert_not_called()
        self.store.admin.assert_not_called()
        self.store.set_baseline.assert_not_called()
        self.connection.close.assert_called_once()

    def test_missing_schema_and_mismatched_catalog_rejected_without_migration(self):
        self.cursor.execute.side_effect = RuntimeError("private schema-path password")
        with patch.object(self.module, "connect", return_value=self.connection):
            with self.assertRaises(self.module.DomainError) as failure:
                self.app(initialize_store=False)
        self.assertNotIn("private", failure.exception.message)
        self.store.initialize.assert_not_called()
        self.store.admin.assert_not_called()
        self.cursor.execute.side_effect = None
        self.cursor.fetchall.side_effect = [[], []]
        with patch.object(self.module, "connect", return_value=self.connection):
            with self.assertRaises(self.module.DomainError):
                self.app(initialize_store=False)
        self.store.initialize.assert_not_called()
        self.store.admin.assert_not_called()

    def test_managed_full_factory_requires_existing_store(self):
        from data_analysis.delivery import app
        from data_analysis.delivery.qt_live_adapter import QtLiveAdapter
        config = managed.ServiceConfig(Path("/temporary/current.sqlite3"), "full", {})
        with patch.object(QtLiveAdapter, "health", return_value={}), \
             patch.object(Path, "is_file", return_value=True), \
             patch.object(managed, "validate_full_application") as validate, \
             patch.object(app, "create_app", return_value="application") as factory:
            self.assertEqual(managed.create_application(config), "application")
        factory.assert_called_once_with(qt_database_path=config.database, initialize_operational_store=False)
        validate.assert_called_once_with("application")

    def test_full_readiness_rejects_missing_model_family_and_batch_mismatch(self):
        from data_analysis.backend import database
        from data_analysis.backend.errors import ApiError
        model = MagicMock()
        model.registry.return_value = {name: {"status": "READY"} for name in ("load", "availability", "insights", "arrival")}
        application = SimpleNamespace(state=SimpleNamespace(database_path=None, mysql_settings=object(), models=model))
        snapshot = MagicMock()
        snapshot.__enter__.return_value.metadata = {"publishedBatchId": "test-only"}
        with patch.object(database, "open_snapshot", return_value=snapshot):
            managed.validate_full_application(application)
            model.registry.return_value["availability"]["status"] = "NOT_READY"
            with self.assertRaises(managed.ServiceError) as failure:
                managed.validate_full_application(application)
            self.assertEqual(failure.exception.code, "MODELS_NOT_READY")
            model.registry.side_effect = ApiError(409, "BATCH_MISMATCH", "模型与发布批次不同")
            with self.assertRaises(managed.ServiceError) as failure:
                managed.validate_full_application(application)
            self.assertEqual(failure.exception.code, "BATCH_MISMATCH")


@unittest.skipUnless(HAS_API and HAS_FRONTEND, "Install API dependencies and build Vue for managed HTTP subprocess tests")
class ManagedHttpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.database = Path(self.temp.name) / "current business.sqlite3"
        self.service_config = Path(self.temp.name) / "services.json"
        self.service_config.write_text('{"version":1}', encoding="utf-8")
        with closing(sqlite3.connect(self.database)) as connection:
            connection.executescript((ROOT / "database/schema.sql").read_text(encoding="utf-8"))

    def start_service(self, parent_pid=None):
        process = subprocess.Popen([sys.executable, str(SCRIPT), "--database", str(self.database),
                                    "--parent-pid", str(parent_pid or os.getpid())],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, cwd=ROOT,
                                   env=dict(os.environ, CHARGING_SERVICES_CONFIG=str(self.service_config)))

        def cleanup():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream and not stream.closed:
                    stream.close()
        self.addCleanup(cleanup)
        line = wait_line(process.stdout)
        self.assertTrue(line.startswith(managed.READY_PREFIX), line)
        ready = json.loads(line.removeprefix(managed.READY_PREFIX))
        self.assertEqual(ready["database"], str(self.database.resolve()))
        self.assertEqual(ready["mode"], "qt-live-only")
        self.assertTrue(ready["url"].endswith("/?embedded=operator"))
        return process, ready["url"].split("/?")[0]

    def assert_stopped(self, process, url):
        self.assertEqual(process.wait(timeout=10), 0, process.stderr.read())
        self.assertEqual(process.stdout.read(), "")
        port = int(url.rsplit(":", 1)[1])
        self.assertFalse(managed.tcp_ready(port))

    def test_ready_means_http_available_and_stop_command_cleans_listener(self):
        process, url = self.start_service()
        with urlopen(url + "/api/v1/qt-live/health", timeout=3) as response:
            body = json.load(response)
        self.assertEqual(body["code"], "OK")
        self.assertTrue(body["data"]["readOnly"])
        process.stdin.write("CHARGING_WEB_STOP\n")
        process.stdin.flush()
        self.assert_stopped(process, url)

    def test_stdin_eof_and_two_dynamic_ports(self):
        first, first_url = self.start_service()
        second, second_url = self.start_service()
        self.assertNotEqual(first_url, second_url)
        first.stdin.close()
        second.stdin.close()
        self.assert_stopped(first, first_url)
        self.assert_stopped(second, second_url)

    def test_parent_death_stops_even_when_stdin_stays_open(self):
        parent = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"])
        self.addCleanup(lambda: parent.poll() is None and parent.kill())
        process, url = self.start_service(parent.pid)
        parent.terminate()
        parent.wait(timeout=5)
        self.assert_stopped(process, url)

    @unittest.skipIf(os.name == "nt", "Windows Qt uses pipe shutdown, not terminate")
    def test_sigterm_is_graceful(self):
        process, url = self.start_service()
        process.send_signal(signal.SIGTERM)
        self.assert_stopped(process, url)
