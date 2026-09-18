"""MySQL configuration and actual export/API reconciliation (opt-in server)."""

from dataclasses import replace
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import uuid

from data_analysis.mysql_support import MySQLSettings, connect
from data_analysis.scripts.verify_serving import main, verify, require_mysql_readonly_account


class MySQLConfigurationTests(unittest.TestCase):
    def test_missing_configuration_does_not_select_a_default_database_or_root(self):
        with patch.dict(os.environ, {}, clear=True), self.assertRaises(ValueError):
            MySQLSettings.from_env()

    def test_port_and_schema_are_strict_and_system_databases_forbidden(self):
        for database in ("mysql", "sys", "information_schema", "performance_schema", "Bad", "a`b", "1db", "a" * 65):
            with self.subTest(database=database), self.assertRaises(ValueError):
                MySQLSettings(user="reader", database=database)
        for port in (0, 65536, True, "3306"):
            with self.subTest(port=port), self.assertRaises(ValueError):
                MySQLSettings(user="reader", database="charging1", port=port)
        for host in ("mysql://reader:secret@server", "user@host", "server/path", "a b"):
            with self.subTest(host=host), self.assertRaises(ValueError):
                MySQLSettings(user="reader", database="charging1", host=host)

    def test_password_is_excluded_from_repr_and_never_embedded_in_a_url(self):
        settings = MySQLSettings(user="reader", database="charging1", password="unit-test-secret")
        self.assertNotIn("unit-test-secret", repr(settings))
        self.assertNotIn("mysql://", repr(settings))

    def test_env_prefix_is_explicit_and_empty_password_is_not_silently_replaced(self):
        with patch.dict(os.environ, {"SAMPLE_USER": "reader", "SAMPLE_DATABASE": "charging1", "SAMPLE_PORT": "33316", "SAMPLE_PASSWORD": ""}, clear=True):
            settings = MySQLSettings.from_env("SAMPLE_")
        self.assertEqual((settings.user, settings.database, settings.port, settings.password), ("reader", "charging1", 33316, ""))

    def test_cli_cannot_mix_mysql_and_offline_file_and_never_overwrites_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / "report.json"
            report.write_text("existing", encoding="utf-8")
            cases = [
                ["--database-backend", "mysql", "--database", "offline.sqlite3"],
                ["--database-backend", "sqlite"],
                ["--database-backend", "mysql"],
            ]
            for extra in cases:
                with self.subTest(extra=extra), patch("data_analysis.scripts.verify_serving.verify") as check, self.assertRaises(SystemExit):
                    main(["--bundle", temporary, "--output", str(report), *extra])
                check.assert_not_called()
            self.assertEqual(report.read_text(encoding="utf-8"), "existing")


HAS_RUNTIME = all(importlib.util.find_spec(name) is not None for name in ("pymysql", "fastapi", "httpx"))


@unittest.skipUnless(os.environ.get("RUN_MYSQL_TESTS") == "1" and HAS_RUNTIME, "Requires explicit RUN_MYSQL_TESTS=1 and MySQL/API runtime")
class MySQLVerificationIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from data_analysis.publishing import mysql_publish
        cls.schema = "test_verify_" + uuid.uuid4().hex
        cls.user = "verify" + uuid.uuid4().hex[:20]
        cls.created_schema = cls.created_user = False
        with patch.dict(os.environ, {"MYSQL_TEST_DATABASE": cls.schema}):
            cls.settings = MySQLSettings.from_env(prefix="MYSQL_TEST_")
        cls.bundle = Path(__file__).resolve().parents[1] / "datasets/analytics_sample_7d_v1"
        cls.addClassCleanup(cls.cleanup)
        original = mysql_publish._execute
        def tracked(connection, statement, parameters=None):
            result = original(connection, statement, parameters)
            if statement.startswith("CREATE DATABASE `" + cls.schema + "` "):
                cls.created_schema = True
            return result
        with patch.object(mysql_publish, "_execute", side_effect=tracked):
            mysql_publish.publish_mysql(cls.bundle, cls.settings)
        connection = connect(cls.settings, database="")
        try:
            with connection.cursor() as cursor:
                cursor.execute("CREATE USER %s@'%%' IDENTIFIED BY %s", (cls.user, "ephemeral-test-reader"))
                cls.created_user = True
                # Escape GRANT database wildcards even though this name is UUID-scoped.
                grant_schema = cls.schema.replace("_", "\\_")
                cursor.execute("GRANT SELECT ON `" + grant_schema + "`.* TO %s@'%%'", (cls.user,))
        finally:
            connection.close()
        cls.reader = replace(cls.settings, user=cls.user, password="ephemeral-test-reader")

    @classmethod
    def cleanup(cls):
        connection = connect(cls.settings, database="")
        try:
            with connection.cursor() as cursor:
                if cls.created_user:
                    cursor.execute("DROP USER %s@'%%'", (cls.user,))
                if cls.created_schema:
                    if not cls.schema.startswith("test_verify_") or len(cls.schema) != 44:
                        raise AssertionError("Unexpected owned test schema")
                    cursor.execute("DROP DATABASE `" + cls.schema + "`")
        finally:
            connection.close()

    def test_complete_sample_verification_with_real_select_only_account(self):
        report = verify(None, self.bundle, mysql_settings=self.reader)
        self.assertEqual(report["storageBackend"], "mysql")
        self.assertEqual(report["checkedApiRequests"], 59)
        self.assertEqual((report["cities"], report["stations"]), (5, 25))
        self.assertTrue(report["readonlyAccountVerified"])
        self.assertTrue(report["sqlApiTotalsMatch"])
        self.assertTrue(report["readOnlyDatabaseUnchanged"])
        self.assertFalse(report["targetLabelsImported"])
        self.assertNotIn("databaseSha256", report)
        self.assertEqual(len(report["logicalContentSha256"]), 64)

    def test_verifier_rejects_privileged_api_credentials(self):
        with self.assertRaisesRegex(ValueError, "SELECT-only"):
            require_mysql_readonly_account(self.settings)

    def test_delete_permission_is_rejected_even_when_update_is_denied(self):
        connection = connect(self.settings, database="")
        schema = self.schema.replace("_", "\\_")
        try:
            with connection.cursor() as cursor:
                cursor.execute("GRANT DELETE ON `" + schema + "`.* TO %s@'%%'", (self.user,))
            with self.assertRaisesRegex(ValueError, "SELECT-only"):
                require_mysql_readonly_account(self.reader)
        finally:
            with connection.cursor() as cursor:
                cursor.execute("REVOKE DELETE ON `" + schema + "`.* FROM %s@'%%'", (self.user,))
            connection.close()


if __name__ == "__main__":
    unittest.main()
