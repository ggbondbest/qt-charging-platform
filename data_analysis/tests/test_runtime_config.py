"""Unified service configuration using temporary files and no network clients."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from data_analysis import runtime_config as runtime
from data_analysis.chargepilot.settings import Settings
from data_analysis.ml.advisor import config as advisor


class RuntimeConfigTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="services-config-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.config = self.root / "services.local.json"
        self.legacy = self.root / "advisor.env"
        for patcher in (patch.object(runtime, "DEFAULT_CONFIG_FILE", self.config),
                        patch.dict(os.environ, {"ML_ADVISOR_ENV_FILE": str(self.legacy)}, clear=True)):
            patcher.start()
            self.addCleanup(patcher.stop)

    def write(self, value, path=None):
        path = path or self.config
        path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        return path

    def legacy_configuration(self):
        self.legacy.write_text(
            "ML_ADVISOR_ONLINE_ENABLED=1\nML_ADVISOR_API_KEY=legacy-test-key\n"
            "ML_ADVISOR_BASE_URL=https://old.example/v1\nML_ADVISOR_MODEL=old-model\n"
            "ML_ADVISOR_PROVIDER_LABEL=Old provider\n", encoding="utf-8")

    def test_default_file_is_optional_but_explicit_file_is_required(self):
        self.assertEqual(runtime.load_service_environment(), {})
        for kwargs in ({"config_file": self.config},
                       {"environ": {runtime.CONFIG_ENV: str(self.config)}}):
            with self.subTest(kwargs=kwargs), self.assertRaises(runtime.ConfigError) as error:
                runtime.load_service_environment(**kwargs)
            self.assertIn(runtime.CONFIG_ENV, str(error.exception))
            self.assertNotIn(str(self.config), str(error.exception))
        self.assertFalse(self.config.exists())

    def test_explicit_path_precedes_environment_and_default(self):
        self.write({"version": 1, "tencentMap": {"webServiceKey": "default-key"}})
        selected = self.write({"version": 1, "tencentMap": {"webServiceKey": "env-key"}}, self.root / "env.json")
        explicit = self.write({"version": 1, "tencentMap": {"webServiceKey": "cli-key"}}, self.root / "cli.json")
        environ = {runtime.CONFIG_ENV: str(selected)}
        self.assertEqual(runtime.load_service_environment(environ=environ)["TENCENT_MAP_API_KEY"], "env-key")
        self.assertEqual(runtime.load_service_environment(explicit, environ=environ)["TENCENT_MAP_API_KEY"], "cli-key")
        self.assertEqual(runtime.load_service_environment(environ={})["TENCENT_MAP_API_KEY"], "default-key")

    def test_empty_template_and_auto_without_key_preserve_entire_legacy_group(self):
        self.legacy_configuration()
        for ai in ({}, {"enabled": "auto", "apiKey": "  ", "model": "new-unused-model",
                        "baseUrl": "https://new-unused.example/v1", "providerLabel": "Unused"}):
            with self.subTest(ai=ai):
                self.write({"version": 1, "ai": ai,
                            "tencentMap": {"webServiceKey": "", "jsKey": "", "secretKey": ""}})
                self.assertEqual(runtime.load_service_environment(), {})
                current = advisor.online_settings()
                self.assertEqual(current.api_key, "legacy-test-key")
                self.assertEqual(current.model, "old-model")
                self.assertEqual(current.base_url, "https://old.example/v1")

    def test_key_auto_enables_ai_and_applies_complete_defaults(self):
        self.legacy_configuration()
        self.write({"version": 1, "ai": {"apiKey": "  file-test-key  "}})
        values = runtime.load_service_environment()
        self.assertEqual(values, {
            "ML_ADVISOR_ONLINE_ENABLED": "1", "ML_ADVISOR_API_KEY": "file-test-key",
            "ML_ADVISOR_BASE_URL": "https://aiping.cn/api/v1", "ML_ADVISOR_MODEL": "DeepSeek-V4.1-Flash",
            "ML_ADVISOR_PROVIDER_LABEL": "AIPing",
        })
        settings = advisor.online_settings()
        self.assertEqual(settings.api_key, "file-test-key")
        self.assertEqual(settings.model, "DeepSeek-V4.1-Flash")
        self.assertEqual(settings.provider, "AIPing")

    def test_explicit_enable_with_empty_key_cannot_reuse_legacy_key(self):
        self.legacy_configuration()
        self.write({"version": 1, "ai": {"enabled": True}})
        self.assertEqual(runtime.load_service_environment()["ML_ADVISOR_API_KEY"], "")
        self.assertIsNone(advisor.online_settings())

    def test_false_disables_ai_but_explicit_process_settings_still_win(self):
        self.legacy_configuration()
        self.write({"version": 1, "ai": {"enabled": False, "apiKey": "unused-file-key"}})
        self.assertEqual(runtime.load_service_environment(), {"ML_ADVISOR_ONLINE_ENABLED": "0"})
        self.assertIsNone(advisor.online_settings())
        with patch.dict(os.environ, {"ML_ADVISOR_ONLINE_ENABLED": "1", "ML_ADVISOR_API_KEY": "process-key"}):
            self.assertEqual(advisor.online_settings().api_key, "process-key")

    def test_process_overrides_file_per_field_without_mutating_environment(self):
        self.write({"version": 1, "ai": {"apiKey": "file-key", "model": "file-model"}})
        with patch.dict(os.environ, {"ML_ADVISOR_API_KEY": "process-key", "ML_ADVISOR_MODEL": "process-model"}):
            before = dict(os.environ)
            current = advisor.online_settings()
            self.assertEqual(current.api_key, "process-key")
            self.assertEqual(current.model, "process-model")
            self.assertEqual(dict(os.environ), before)
            self.assertEqual(runtime.load_service_environment()["ML_ADVISOR_API_KEY"], "file-key")
        with patch.dict(os.environ, {"ML_ADVISOR_API_KEY": ""}):
            self.assertIsNone(advisor.online_settings())

    def test_ai_https_and_existing_key_validation_remain_in_force(self):
        for base in ("http://model.example/v1", "https://user:password@model.example/v1",
                     "https://model.example/v1?token=private", "https://model.example/v1/chat/completions",
                     "https://model.example/v1/responses", "https://model.example:99999/v1"):
            with self.subTest(base=base):
                self.write({"version": 1, "ai": {"apiKey": "file-key", "baseUrl": base}})
                self.assertIsNone(advisor.online_settings())
        self.write({"version": 1, "ai": {"apiKey": "中文密钥"}})
        self.assertIsNone(advisor.online_settings())

    def test_values_are_trimmed_literals_and_config_is_read_only(self):
        literal = "$(touch should-not-exist) `literal` ${HOME}"
        self.write({"version": 1, "ai": {"apiKey": "file-key", "model": literal},
                    "tencentMap": {"webServiceKey": " api-key ", "jsKey": " js-key ", "secretKey": " secret-key "}})
        original = self.config.read_bytes()
        before = dict(os.environ)
        result = runtime.load_service_environment()
        self.assertEqual(result["ML_ADVISOR_MODEL"], literal)
        self.assertEqual(result["TENCENT_MAP_API_KEY"], "api-key")
        self.assertEqual(result["TENCENT_MAP_JS_KEY"], "js-key")
        self.assertEqual(result["TENCENT_MAP_SECRET_KEY"], "secret-key")
        self.assertEqual(self.config.read_bytes(), original)
        self.assertEqual(dict(os.environ), before)
        self.assertEqual(sorted(path.name for path in self.root.iterdir()), [self.config.name])

    def test_strict_schema_rejects_unknown_fields_types_and_versions(self):
        cases = [None, [], {}, {"version": True}, {"version": 1.0}, {"version": 2},
                 {"version": 1, "private-test-value": "private-test-value"},
                 {"version": 1, "ai": None}, {"version": 1, "tencentMap": []},
                 {"version": 1, "ai": {"private-test-value": "private-test-value"}},
                 {"version": 1, "tencentMap": {"private-test-value": "private-test-value"}},
                 {"version": 1, "ai": {"apiKey": 7}},
                 {"version": 1, "tencentMap": {"webServiceKey": False}}]
        cases.extend({"version": 1, "ai": {"enabled": value}} for value in (0, 1, None, "true", [], {}))
        for value in cases:
            with self.subTest(value=value):
                self.write(value)
                with self.assertRaises(runtime.ConfigError) as error:
                    runtime.load_service_environment()
                self.assertNotIn("private-test-value", str(error.exception))
                self.assertNotIn(str(self.config), str(error.exception))

    def test_rejects_duplicate_keys_at_every_level_and_non_json_numbers(self):
        for text in ('{"version":1,"version":1}',
                     '{"version":1,"ai":{"apiKey":"private-test-value","apiKey":"second"}}',
                     '{"version":1,"tencentMap":{"jsKey":"a","jsKey":"b"}}',
                     '{"version":NaN}', '{"version":Infinity}', '{"version":1,"ai":'):
            with self.subTest(text=text):
                self.config.write_text(text, encoding="utf-8")
                with self.assertRaises(runtime.ConfigError) as error:
                    runtime.load_service_environment()
                self.assertNotIn("private-test-value", str(error.exception))

    def test_size_limit_utf8_bom_and_invalid_text(self):
        valid = b'{"version":1}'
        self.config.write_bytes(valid + b" " * (runtime.MAX_CONFIG_BYTES - len(valid)))
        self.assertEqual(runtime.load_service_environment(), {})
        self.config.write_bytes(valid + b" " * (runtime.MAX_CONFIG_BYTES - len(valid) + 1))
        with self.assertRaises(runtime.ConfigError):
            runtime.load_service_environment()
        self.config.write_bytes(b"\xef\xbb\xbf" + valid)
        self.assertEqual(runtime.load_service_environment(), {})
        for content in (b"\xff", b'{"version":1,"ai":{"apiKey":"\\ud800"}}'):
            self.config.write_bytes(content)
            with self.assertRaises(runtime.ConfigError):
                runtime.load_service_environment()

    def test_field_limits_and_embedded_control_characters_have_safe_errors(self):
        cases = [({"ai": {"apiKey": "a" * 513}}, "ai.apiKey"),
                 ({"ai": {"baseUrl": "a" * 2049}}, "ai.baseUrl"),
                 ({"ai": {"model": "a" * 121}}, "ai.model"),
                 ({"ai": {"providerLabel": "a" * 81}}, "ai.providerLabel"),
                 ({"tencentMap": {"secretKey": "a" * 513}}, "tencentMap.secretKey"),
                 ({"ai": {"apiKey": "private\nvalue"}}, "ai.apiKey"),
                 ({"tencentMap": {"webServiceKey": "private\0value"}}, "tencentMap.webServiceKey")]
        for fields, expected in cases:
            with self.subTest(field=expected):
                self.write({"version": 1, **fields})
                with self.assertRaises(runtime.ConfigError) as error:
                    runtime.load_service_environment()
                self.assertIn(expected, str(error.exception))
                self.assertNotIn("private", str(error.exception))

    def test_bad_explicit_paths_do_not_disclose_input(self):
        for path in ("", "private\0value", self.root):
            with self.subTest(path=path), self.assertRaises(runtime.ConfigError) as error:
                runtime.load_service_environment(path)
            self.assertNotIn("private", str(error.exception))
            self.assertNotIn(str(self.root), str(error.exception))

    def test_client_map_preloaded_values_do_not_reopen_file_or_include_ai(self):
        services = {"ML_ADVISOR_API_KEY": "never-forward-ai", "TENCENT_MAP_API_KEY": "file-api",
                    "TENCENT_MAP_JS_KEY": "file-js", "TENCENT_MAP_SECRET_KEY": "file-secret"}
        before = dict(services)
        with patch.object(runtime, "load_map_environment", side_effect=AssertionError("read twice")):
            result = runtime.client_map_environment(environ={}, service_environment=services)
        self.assertEqual(result, {"TENCENT_MAP_API_KEY": "file-api", "TENCENT_MAP_JS_KEY": "file-js",
            "TENCENT_MAP_SECRET_KEY": "file-secret", "CHARGING_TENCENT_MAP_KEY": "file-api",
            "CHARGING_TENCENT_MAP_SECRET": "file-secret"})
        self.assertNotIn("never-forward-ai", result.values())
        self.assertEqual(services, before)

    def test_client_map_modern_environment_precedes_legacy_and_file(self):
        services = {"TENCENT_MAP_API_KEY": "file-api", "TENCENT_MAP_JS_KEY": "file-js",
                    "TENCENT_MAP_SECRET_KEY": "file-secret"}
        environ = {"TENCENT_MAP_API_KEY": " modern-api ", "CHARGING_TENCENT_MAP_KEY": "legacy-api",
                   "TENCENT_MAP_JS_KEY": " modern-js ", "TENCENT_MAP_SECRET_KEY": "modern-secret",
                   "CHARGING_TENCENT_MAP_SECRET": "legacy-secret", "ML_ADVISOR_API_KEY": "never-forward"}
        before = dict(environ)
        result = runtime.client_map_environment(environ=environ, service_environment=services)
        self.assertEqual(result["TENCENT_MAP_API_KEY"], "modern-api")
        self.assertEqual(result["TENCENT_MAP_JS_KEY"], "modern-js")
        self.assertEqual(result["TENCENT_MAP_SECRET_KEY"], "modern-secret")
        self.assertEqual(result["CHARGING_TENCENT_MAP_KEY"], "modern-api")
        self.assertEqual(environ, before)

    def test_client_map_legacy_precedence_and_js_fallback(self):
        services = {"TENCENT_MAP_API_KEY": "file-api", "TENCENT_MAP_SECRET_KEY": "file-secret"}
        result = runtime.client_map_environment(environ={"CHARGING_TENCENT_MAP_KEY": "old-api",
            "CHARGING_TENCENT_MAP_SECRET": "old-secret"}, service_environment=services)
        self.assertEqual(result["TENCENT_MAP_API_KEY"], "old-api")
        self.assertEqual(result["TENCENT_MAP_JS_KEY"], "old-api")
        self.assertEqual(result["TENCENT_MAP_SECRET_KEY"], "old-secret")
        result = runtime.client_map_environment(environ={"TENCENT_MAP_API_KEY": " "}, service_environment=services)
        self.assertEqual(result["TENCENT_MAP_API_KEY"], "file-api")
        self.assertEqual(result["CHARGING_TENCENT_MAP_KEY"], "file-api")
        self.assertEqual(set(runtime.client_map_environment(environ={}, service_environment={})), {
            "TENCENT_MAP_API_KEY", "TENCENT_MAP_JS_KEY", "TENCENT_MAP_SECRET_KEY",
            "CHARGING_TENCENT_MAP_KEY", "CHARGING_TENCENT_MAP_SECRET"})

    def test_client_helper_loads_selected_file_when_mapping_not_supplied(self):
        self.write({"version": 1, "tencentMap": {"webServiceKey": "file-api", "jsKey": "file-js"}})
        result = runtime.client_map_environment(self.config, environ={})
        self.assertEqual(result["TENCENT_MAP_API_KEY"], "file-api")
        self.assertEqual(result["TENCENT_MAP_JS_KEY"], "file-js")

    def test_server_and_client_read_only_their_own_validated_group(self):
        for invalid_map in (None, [], True, {"webServiceKey": 42}, {"secretKey": "bad\0value"},
                            {"unknown": "private-map-value"}):
            with self.subTest(invalid_map=invalid_map):
                self.write({"version": 1, "ai": {"apiKey": "server-ai-key"}, "tencentMap": invalid_map})
                values = runtime.load_ai_environment()
                self.assertEqual(values["ML_ADVISOR_API_KEY"], "server-ai-key")
                self.assertTrue(all(name.startswith("ML_ADVISOR_") for name in values))
                self.assertEqual(advisor.online_settings().api_key, "server-ai-key")
                with self.assertRaises(runtime.ConfigError):
                    runtime.load_map_environment()
                with self.assertRaises(runtime.ConfigError):
                    runtime.load_service_environment()
        for invalid_ai in (None, [], True, {"enabled": "invalid"}, {"apiKey": {}},
                           {"providerLabel": 42}, {"unknown": "private-ai-value"}):
            with self.subTest(invalid_ai=invalid_ai):
                self.write({"version": 1, "ai": invalid_ai,
                            "tencentMap": {"webServiceKey": "client-api-key", "jsKey": "client-js-key"}})
                values = runtime.load_map_environment()
                self.assertEqual(values, {"TENCENT_MAP_API_KEY": "client-api-key", "TENCENT_MAP_JS_KEY": "client-js-key"})
                self.assertEqual(runtime.client_map_environment()["CHARGING_TENCENT_MAP_KEY"], "client-api-key")
                with self.assertRaises(runtime.ConfigError):
                    runtime.load_ai_environment()
                with self.assertRaises(runtime.ConfigError):
                    runtime.load_service_environment()

    def test_consumer_readers_still_validate_shared_document_structure(self):
        for payload in ('{"version":1,"unknown":{}}', '{"version":2}', '{"version":true}',
                        'null', '{"version":1,', '{"version":1,"ai":{"model":"a","model":"b"}}'):
            self.config.write_text(payload, encoding="utf-8")
            for reader in (runtime.load_ai_environment, runtime.load_map_environment):
                with self.subTest(payload=payload, reader=reader.__name__), self.assertRaises(runtime.ConfigError):
                    reader()

    def test_chargepilot_settings_ignores_shared_map_but_preserves_explicit_environment_opt_in(self):
        self.write({"version": 1, "tencentMap": {"webServiceKey": "file-api", "secretKey": "file-secret"}})
        with patch.dict(os.environ, {"CHARGEPILOT_MYSQL_USER": "test-user", "CHARGEPILOT_MYSQL_PASSWORD": "test-password"}):
            current = Settings.from_env()
            self.assertEqual(current.tencent_key, "")
            self.assertEqual(current.tencent_secret, "")
            # Even a malformed unused map section cannot affect server settings.
            self.write({"version": 1, "tencentMap": {"webServiceKey": {"invalid": "private-client-value"}}})
            self.assertEqual(Settings.from_env().tencent_key, "")
            with patch.dict(os.environ, {"TENCENT_MAP_API_KEY": " process-api ", "TENCENT_MAP_SECRET_KEY": "process-secret"}):
                current = Settings.from_env()
                self.assertEqual(current.tencent_key, "process-api")
                self.assertEqual(current.tencent_secret, "process-secret")
            with patch.dict(os.environ, {"TENCENT_MAP_API_KEY": ""}):
                self.assertEqual(Settings.from_env().tencent_key, "")
        self.assertEqual(Settings(tencent_key="explicit-key").tencent_key, "explicit-key")


if __name__ == "__main__":
    unittest.main()
