"""The optional wrapper configures only native client maps without shell commands."""
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest
from unittest.mock import MagicMock, patch

from scripts import launch_client, launch_demo


class ClientLauncherTests(unittest.TestCase):
    def test_main_forwards_arguments_literally_and_returns_client_exit_status(self):
        environment = {"TENCENT_MAP_API_KEY": "map-only", "PATH": "inherited"}
        binary = Path("/build/charging-client")
        with patch.object(launch_demo, "configured_client_environment", return_value=environment) as config, \
             patch.object(launch_client, "find_client", return_value=binary) as find, \
             patch.object(launch_client, "run_client", return_value=7) as run:
            code = launch_client.main(["--services-config", "private.json", "--build-dir", "build-here",
                                       "--host", "127.0.0.1", "--port", "9600", "--",
                                       "--mock", "literal;$(not-executed)"])
        self.assertEqual(code, 7)
        config.assert_called_once_with(Path("private.json"))
        find.assert_called_once_with(Path("build-here"))
        run.assert_called_once_with([str(binary), "--host", "127.0.0.1", "--port", "9600",
                                     "--mock", "literal;$(not-executed)"], environment)

    def test_shared_json_configures_maps_without_sending_ai_key_to_client(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "services.local.json"
            config.write_text(json.dumps({"version": 1, "ai": {"apiKey": "private-ai-key"},
                                          "tencentMap": {"webServiceKey": "map-api", "jsKey": "map-js",
                                                         "secretKey": "map-secret"}}), encoding="utf-8")
            with patch.dict(os.environ, {"ML_ADVISOR_API_KEY": "process-ai-key"}, clear=True), \
                 patch.object(launch_client, "find_client", return_value=Path("client")), \
                 patch.object(launch_client, "run_client", return_value=0) as run:
                self.assertEqual(launch_client.main(["--services-config", str(config)]), 0)
            environment = run.call_args.args[1]
            self.assertEqual(environment["TENCENT_MAP_API_KEY"], "map-api")
            self.assertEqual(environment["TENCENT_MAP_JS_KEY"], "map-js")
            self.assertEqual(environment["CHARGING_TENCENT_MAP_KEY"], "map-api")
            self.assertEqual(environment["TENCENT_MAP_SECRET_KEY"], "map-secret")
            self.assertEqual(environment["CHARGING_SERVICES_CONFIG"], str(config.resolve()))
            self.assertFalse(any(key.startswith("ML_ADVISOR_") for key in environment))
            self.assertNotIn("private-ai-key", environment.values())

    def test_js_only_configuration_does_not_become_a_webservice_key_through_legacy_alias(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "services.local.json"
            config.write_text(json.dumps({"version": 1, "tencentMap": {"jsKey": "js-product-only"}}))
            with patch.dict(os.environ, {}, clear=True):
                environment = launch_demo.configured_client_environment(config)
            self.assertEqual(environment["TENCENT_MAP_JS_KEY"], "js-product-only")
            self.assertEqual(environment["TENCENT_MAP_API_KEY"], "")
            self.assertEqual(environment["CHARGING_TENCENT_MAP_KEY"], "")

    def test_invalid_ai_fields_do_not_block_client_maps(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "services.local.json"
            config.write_text(json.dumps({"version": 1, "ai": {"enabled": ["invalid"]},
                                          "tencentMap": {"webServiceKey": "map-api"}}))
            with patch.dict(os.environ, {}, clear=True), \
                 patch.object(launch_client, "find_client", return_value=Path("client")), \
                 patch.object(launch_client, "run_client", return_value=0) as run:
                self.assertEqual(launch_client.main(["--services-config", str(config)]), 0)
            self.assertEqual(run.call_args.args[1]["TENCENT_MAP_API_KEY"], "map-api")

    def test_optional_host_and_port_preserve_original_client_environment(self):
        binary = Path("/build/client")
        with patch.object(launch_demo, "configured_client_environment", return_value={}), \
             patch.object(launch_client, "find_client", return_value=binary), \
             patch.object(launch_client, "run_client", return_value=0) as run:
            self.assertEqual(launch_client.main([]), 0)
        run.assert_called_once_with([str(binary)], {})

    def test_default_build_discovery_prefers_current_creator_then_delivery(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(launch_client, "ROOT", Path(directory)):
            root = Path(directory)
            creator = root / "build/Qt_6_12_0_for_macOS_Debug/client/charging-client.app/Contents/MacOS/charging-client"
            delivery = root / "build-delivery/client/charging-client.app/Contents/MacOS/charging-client"
            for binary in (creator, delivery):
                binary.parent.mkdir(parents=True)
                binary.touch()
            self.assertEqual(launch_client.find_client(), creator.resolve())
            creator.unlink()
            self.assertEqual(launch_client.find_client(), delivery.resolve())
            with self.assertRaisesRegex(RuntimeError, "Missing charging-client"):
                launch_client.find_client(root / "explicit-missing")

    def test_windows_release_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            binary = build / "client/Release/charging-client.exe"
            binary.parent.mkdir(parents=True)
            binary.touch()
            self.assertEqual(launch_client.find_client(build), binary.resolve())

    def test_normal_exit_does_not_terminate_client(self):
        child = MagicMock()
        child.wait.return_value = 0
        child.poll.return_value = 0
        previous = signal.getsignal(signal.SIGTERM)
        with patch.object(launch_client.subprocess, "Popen", return_value=child) as start:
            self.assertEqual(launch_client.run_client(["client", "literal;value"], {"MAP": "private"}), 0)
        start.assert_called_once_with(["client", "literal;value"], cwd=launch_client.ROOT, env={"MAP": "private"})
        child.terminate.assert_not_called()
        child.kill.assert_not_called()
        self.assertIs(signal.getsignal(signal.SIGTERM), previous)

    def test_keyboard_interrupt_stops_owned_process_with_bounded_kill_fallback(self):
        child = MagicMock()
        child.poll.return_value = None
        child.wait.side_effect = [KeyboardInterrupt(), subprocess.TimeoutExpired("owned", 8), 0]
        with patch.object(launch_client.subprocess, "Popen", return_value=child), redirect_stdout(io.StringIO()):
            self.assertEqual(launch_client.run_client(["client"], {}), 130)
        child.terminate.assert_called_once_with()
        child.kill.assert_called_once_with()
        self.assertEqual(child.wait.call_args_list[-1].kwargs, {"timeout": 5})

    def test_sigterm_also_cleans_up_owned_client(self):
        child = MagicMock()
        child.poll.return_value = None

        def wait(timeout=None):
            if timeout is None:
                signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)
            return 0

        child.wait.side_effect = wait
        previous = signal.getsignal(signal.SIGTERM)
        with patch.object(launch_client.subprocess, "Popen", return_value=child), redirect_stdout(io.StringIO()):
            self.assertEqual(launch_client.run_client(["client"], {}), 128 + signal.SIGTERM)
        child.terminate.assert_called_once_with()
        child.kill.assert_not_called()
        self.assertIs(signal.getsignal(signal.SIGTERM), previous)

    def test_configuration_error_never_launches_client(self):
        output = io.StringIO()
        with patch.object(launch_demo, "configured_client_environment", side_effect=RuntimeError("配置字段无效。")), \
             patch.object(launch_client, "run_client") as run, redirect_stderr(output):
            self.assertEqual(launch_client.main([]), 1)
        run.assert_not_called()
        self.assertIn("配置字段无效", output.getvalue())

    def test_os_errors_do_not_print_exception_contents(self):
        output = io.StringIO()
        with patch.object(launch_demo, "configured_client_environment", return_value={}), \
             patch.object(launch_client, "find_client", return_value=Path("client")), \
             patch.object(launch_client, "run_client", side_effect=OSError("untrusted-sensitive-value")), \
             redirect_stderr(output):
            self.assertEqual(launch_client.main([]), 1)
        self.assertNotIn("untrusted-sensitive-value", output.getvalue())
        self.assertIn("客户端启动失败", output.getvalue())


if __name__ == "__main__":
    unittest.main()
