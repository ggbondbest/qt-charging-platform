"""Process ownership and fail-fast checks for the cross-platform demo launcher."""
import importlib.util
from contextlib import chdir, redirect_stdout
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

from scripts import launch_demo
from scripts.launch_demo import ROOT, children, executable, require_free_port, wait_ready


class DemoLauncherTests(unittest.TestCase):
    def test_launcher_owns_one_api_in_live_full_and_client_modes(self):
        for options in (["--headless"], ["--no-client", "--full"], ["--full"]):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                dist = root / "data_analysis/frontend/dist/index.html"
                dist.parent.mkdir(parents=True)
                dist.touch()
                database = (root / "business.sqlite3").resolve()
                start = MagicMock(side_effect=lambda *args: MagicMock(
                    poll=MagicMock(return_value=0), returncode=0))
                context = MagicMock()
                context.__enter__.return_value = start
                with patch.object(launch_demo, "ROOT", root), \
                     patch.object(launch_demo, "executable", side_effect=lambda _, name: root / name), \
                     patch.object(launch_demo, "configured_environments", return_value=(
                         {"ML_ADVISOR_API_KEY": "file-ai"},
                         {"TENCENT_MAP_API_KEY": "map"})), \
                     patch.object(launch_demo, "require_free_port"), \
                     patch.object(launch_demo, "children", return_value=context), \
                     patch.object(launch_demo, "wait_ready"), \
                     patch.object(launch_demo.subprocess, "run") as prepare, \
                     patch.object(sys, "argv", ["launch_demo.py", "--database", str(database),
                                               "--demo-seed", *options]), redirect_stdout(io.StringIO()):
                    launch_demo.main()

                launched = {call.args[0]: call.args[1:] for call in start.call_args_list}
                has_client = not any(option in options for option in ("--no-client", "--headless"))
                self.assertEqual(len(start.call_args_list), 3 if has_client else 2)
                qt_args, qt_env = launched["qt-server"]
                api_args, api_env = launched["web-api"]
                self.assertIn("--no-dashboard-autostart", qt_args)
                self.assertIn("--demo-seed", qt_args)
                self.assertEqual(qt_args[qt_args.index("--database") + 1], database)
                self.assertEqual(qt_env["QT_SQLITE_PATH"], str(database))
                self.assertEqual(api_env["QT_SQLITE_PATH"], str(database))
                self.assertEqual(api_env["ML_ADVISOR_API_KEY"], "file-ai")
                self.assertEqual(qt_env["ML_ADVISOR_API_KEY"], "file-ai")
                self.assertNotIn("TENCENT_MAP_API_KEY", qt_env)
                self.assertNotIn("TENCENT_MAP_API_KEY", api_env)
                self.assertEqual(qt_env["CHARGING_DASHBOARD_URL"],
                                 "http://127.0.0.1:8000/?embedded=operator")
                self.assertEqual(api_args.count("serve"), 1)
                self.assertEqual("--qt-live-only" in api_args, "--full" not in options)
                self.assertEqual("--qt-live-only" in prepare.call_args.args[0], "--full" not in options)
                if "--headless" in options:
                    self.assertEqual(qt_env["QT_QPA_PLATFORM"], "offscreen")
                if "qt-client" in launched:
                    self.assertEqual(launched["qt-client"][0][-2:], ["--port", 9527])
                    self.assertNotIn("ML_ADVISOR_API_KEY", launched["qt-client"][1])
                    self.assertEqual(launched["qt-client"][1]["TENCENT_MAP_API_KEY"], "map")

    def test_configuration_is_split_by_role_with_process_precedence(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "services.local.json"
            path.write_text(json.dumps({"version": 1, "ai": {"apiKey": "file-ai"},
                                        "tencentMap": {"webServiceKey": "file-map"}}))
            with patch.dict(os.environ, {"ML_ADVISOR_API_KEY": "process-ai", "PATH": "kept",
                                        "TENCENT_MAP_API_KEY": "process-map",
                                        "CHARGING_TENCENT_MAP_SECRET": "process-secret"}, clear=True):
                api, native = launch_demo.configured_environments(path)
            self.assertEqual(api["ML_ADVISOR_API_KEY"], "process-ai")
            self.assertFalse(any(key.startswith("ML_ADVISOR_") for key in native))
            self.assertFalse(any(key.startswith(("TENCENT_MAP_", "CHARGING_TENCENT_MAP_")) for key in api))
            self.assertEqual(native["TENCENT_MAP_API_KEY"], "process-map")
            self.assertEqual(native["TENCENT_MAP_SECRET_KEY"], "process-secret")
            self.assertEqual(api["PATH"], native["PATH"])
            self.assertEqual(api["CHARGING_SERVICES_CONFIG"], native["CHARGING_SERVICES_CONFIG"])
            self.assertEqual(api["CHARGING_SERVICES_CONFIG"], str(path.resolve()))

    def test_server_only_does_not_load_invalid_map_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "services.local.json"
            path.write_text(json.dumps({"version": 1, "ai": {"apiKey": "file-ai"},
                                        "tencentMap": {"webServiceKey": ["invalid-map-field"]}}))
            with patch.dict(os.environ, {}, clear=True):
                server, client = launch_demo.configured_environments(path, include_client=False)
            self.assertEqual(server["ML_ADVISOR_API_KEY"], "file-ai")
            self.assertEqual(client, {})

    def test_empty_selected_path_does_not_fall_back_to_another_file(self):
        with patch.dict(os.environ, {"CHARGING_SERVICES_CONFIG": "  "}, clear=True):
            with self.assertRaisesRegex(RuntimeError, "须指定配置文件路径"):
                launch_demo.configured_client_environment()

    def test_environment_config_keeps_same_file_after_child_changes_directory(self):
        from data_analysis.runtime_config import load_ai_environment, load_map_environment
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "selected-services.json"
            path.write_text(json.dumps({"version": 1, "ai": {"apiKey": "test-ai"},
                                        "tencentMap": {"webServiceKey": "test-map"}}))
            with chdir(directory), patch.dict(os.environ, {"CHARGING_SERVICES_CONFIG": path.name}, clear=True):
                api, native = launch_demo.configured_environments()
            with chdir(ROOT):
                self.assertEqual(load_ai_environment(environ=api)["ML_ADVISOR_API_KEY"], "test-ai")
                self.assertEqual(load_map_environment(environ=native)["TENCENT_MAP_API_KEY"], "test-map")
            self.assertEqual(api["CHARGING_SERVICES_CONFIG"], str(path.resolve()))
            self.assertEqual(api["CHARGING_SERVICES_CONFIG"], native["CHARGING_SERVICES_CONFIG"])

    def test_acceptance_owns_api_instead_of_autostarting_it_from_qt(self):
        spec = importlib.util.spec_from_file_location("verify_qt_live_test", ROOT / "scripts/verify_qt_live.py")
        verify = importlib.util.module_from_spec(spec)
        # The acceptance script supports direct CLI execution from scripts/.
        with patch.dict(sys.modules, {"launch_demo": launch_demo}):
            spec.loader.exec_module(verify)
        start = MagicMock()
        context = MagicMock()
        context.__enter__.return_value = start
        with patch.object(verify, "executable", return_value=Path("charging-server")), \
             patch.object(verify, "children", return_value=context), \
             patch.object(verify, "wait_ready", side_effect=[None, RuntimeError("stop after API launch")]):
            with self.assertRaisesRegex(RuntimeError, "stop after API launch"):
                verify.run(ROOT / "unused-build", ROOT / "unused-logs")
        self.assertEqual(len(start.call_args_list), 2)
        qt, api = start.call_args_list
        self.assertEqual(qt.args[0], "verify-qt")
        self.assertIn("--no-dashboard-autostart", qt.args[1])
        self.assertEqual(api.args[0], "verify-api")
        self.assertIn("--qt-live-only", api.args[1])
        self.assertEqual(qt.args[2]["QT_SQLITE_PATH"], api.args[2]["QT_SQLITE_PATH"])

    def test_acceptance_cannot_claim_pass_with_assertions_disabled(self):
        result = subprocess.run([sys.executable, "-O", "scripts/verify_qt_live.py"], cwd=ROOT,
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("assertions must remain enabled", result.stderr)
        self.assertNotIn('"status": "PASS"', result.stdout)

    def test_cli_rejects_invalid_http_port_before_startup(self):
        for port in ("0", "65536", "-1"):
            with self.subTest(port=port):
                result = subprocess.run([sys.executable, "-m", "data_analysis.delivery.cli", "serve",
                    "--qt-live-only", "--port", port], cwd=ROOT, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2)
                self.assertIn("port must be between", result.stderr)

    def test_occupied_port_is_rejected_without_reusing_listener(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            with self.assertRaisesRegex(RuntimeError, "already occupied"):
                require_free_port(listener.getsockname()[1])
            self.assertGreaterEqual(listener.fileno(), 0)

    def test_exception_stops_only_owned_children(self):
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        try:
            with tempfile.TemporaryDirectory() as directory:
                with self.assertRaisesRegex(RuntimeError, "probe failed"):
                    with children(Path(directory)) as start:
                        owned = start("child", [sys.executable, "-c", "import time; time.sleep(60)"], os.environ)
                        raise RuntimeError("probe failed")
                self.assertIsNotNone(owned.poll())
                self.assertIsNone(unrelated.poll())
        finally:
            unrelated.terminate()
            unrelated.wait(timeout=5)

    def test_early_exit_is_not_reported_as_ready(self):
        process = subprocess.Popen([sys.executable, "-c", "raise SystemExit(7)"])
        process.wait(timeout=5)
        with self.assertRaisesRegex(RuntimeError, "exited with code 7"):
            wait_ready(process, lambda: True, "test child", timeout=1)

    def test_mac_bundle_executable_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            binary = build / "server/charging-server.app/Contents/MacOS/charging-server"
            binary.parent.mkdir(parents=True)
            binary.touch()
            self.assertEqual(executable(build, "server"), binary.resolve())
            with self.assertRaisesRegex(RuntimeError, "Missing charging-client"):
                executable(build, "client")

    @unittest.skipUnless(importlib.util.find_spec("fastapi"), "Install requirements-api.txt")
    def test_cli_qt_only_does_not_require_mysql_or_create_missing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "must-not-create.sqlite3"
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(("CHARGEPILOT_", "ANALYTICS_MYSQL_", "QT_SQLITE_"))}
            result = subprocess.run([sys.executable, "-m", "data_analysis.delivery.cli", "check",
                                     "--qt-live-only", "--qt-database", str(path)],
                                    cwd=ROOT, env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertEqual(json.loads(result.stdout)["status"], "UNAVAILABLE")
            self.assertFalse(path.exists())
            self.assertNotIn(str(path), result.stdout)
            self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
