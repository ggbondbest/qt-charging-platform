"""MySQL publication guards; real-server tests require RUN_MYSQL_TESTS=1.

Integration tests create UUID-named schemas and drop only schemas whose CREATE
DATABASE succeeded in that test. They never reuse a user-provided database.
"""

from contextlib import contextmanager
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import uuid

from data_analysis.contracts.serving import SERVING_TABLES, TABLE_KEYS
from data_analysis.mysql_support import MySQLSettings, connect
from data_analysis.publishing import mysql_publish as publishing
from data_analysis.tests.test_publishing import ExportFixture, column, sha


class MySQLPublishingGuardTests(unittest.TestCase):
    def test_mysql_types_are_exact_bounded_and_no_pad(self):
        self.assertEqual(publishing._column_type(column("money_cents", "integer"), []), "BIGINT")
        self.assertEqual(publishing._column_type(column("energy_wh", "integer"), []), "BIGINT")
        self.assertEqual(publishing._column_type(column("power_kw", "number"), []), "DOUBLE")
        self.assertIn("VARCHAR(160)", publishing._column_type(column("station_id"), ["station_id"]))
        self.assertIn("VARCHAR(10)", publishing._column_type(column("business_date", "date"), []))
        self.assertIn("VARCHAR(32)", publishing._column_type(column("reference_time", "timestamp"), []))
        self.assertIn("TEXT", publishing._column_type(column("station_name"), []))
        self.assertIn("utf8mb4_0900_bin", publishing._column_type(column("city_id"), []))

    def test_identifiers_and_string_lengths_are_rejected_not_truncated(self):
        for name in ["x`; DROP DATABASE x", "a" * 65, "", None, "1invalid"]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                publishing._quote(name)
        for name, value in [("city_id", "中" * 161), ("description", "中" * 4097)]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                publishing._bounded_row_values({name: value}, [column(name)], [])
        self.assertEqual(publishing._bounded_row_values({"city_id": "C1 "}, [column("city_id")], []), ["C1 "])

    def test_relationship_sql_uses_mysql_null_safe_comparison_and_antijoins(self):
        fixture = ExportFixture(Path("unused"))
        # Only table definitions are needed, no filesystem or server access.
        fixture.manifest["tables"] = {name: {"columns": columns} for name, columns in fixture.columns.items()}
        with patch.object(publishing, "_one", return_value=None) as query:
            publishing._relationships(object(), fixture.manifest)
        statements = "\n".join(call.args[1] for call in query.call_args_list)
        self.assertIn("<=>", statements)
        self.assertIn("LEFT JOIN `__validation_ml_target_keys`", statements)
        self.assertNotIn(" EXCEPT ", statements)
        self.assertNotIn(" IS NOT d.", statements)

    def test_ddl_finishes_before_begin_and_commit_is_last_publication_action(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = ExportFixture(Path(directory) / "export")
            fixture.write()
            connection = Mock()
            events = []
            connection.autocommit.side_effect = lambda enabled: events.append("autocommit")
            connection.begin.side_effect = lambda: events.append("begin")
            connection.commit.side_effect = lambda: events.append("commit")
            connection.close.side_effect = lambda: events.append("close")
            with patch.object(publishing, "connect", return_value=connection), \
                 patch.object(publishing, "_prepare_schema", side_effect=lambda *args: events.append("ddl")), \
                 patch.object(publishing, "_import_table", side_effect=lambda *args: events.append("rows") or 1), \
                 patch.object(publishing, "_relationships", side_effect=lambda *args: events.append("validation")), \
                 patch.object(publishing, "_many", side_effect=lambda *args: events.append("metadata")):
                report = publishing.publish_mysql(fixture.root, SimpleNamespace(database="test_new_batch"))
            self.assertEqual(events[:3], ["autocommit", "ddl", "begin"])
            self.assertEqual(events[-4:], ["validation", "metadata", "commit", "close"])
            self.assertEqual(report["status"], "PUBLISHED")
            self.assertFalse(report["apiConfigurationChanged"])
            self.assertNotIn("password", json.dumps(report).lower())

    def test_uncertain_commit_never_returns_published_or_drops_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = ExportFixture(Path(directory) / "export")
            fixture.write()
            connection = Mock()
            connection.commit.side_effect = OSError("lost commit acknowledgement")
            with patch.object(publishing, "connect", return_value=connection), \
                 patch.object(publishing, "_prepare_schema"), \
                 patch.object(publishing, "_import_table", return_value=1), \
                 patch.object(publishing, "_relationships"), patch.object(publishing, "_many"), \
                 self.assertRaises(OSError):
                publishing.publish_mysql(fixture.root, SimpleNamespace(database="test_new_batch"))
            connection.rollback.assert_called_once()
            connection.close.assert_called_once()

    def test_invalid_export_is_rejected_before_connect_and_reserved_metadata_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = ExportFixture(Path(directory) / "export")
            fixture.write()
            fixture.manifest["storageBackend"] = "mysql"
            fixture.save_metadata()
            with patch.object(publishing, "connect") as connection, self.assertRaises(ValueError):
                publishing.publish_mysql(fixture.root, SimpleNamespace(database="test_new_batch"))
            connection.assert_not_called()

    def test_cli_existing_report_never_publishes_or_overwrites(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.json"
            report.write_text("old report", encoding="utf-8")
            with patch.object(publishing, "publish_mysql") as publish, self.assertRaises(SystemExit):
                publishing.main(["--input", directory, "--report", str(report)])
            publish.assert_not_called()
            self.assertEqual(report.read_text(encoding="utf-8"), "old report")


@unittest.skipUnless(os.environ.get("RUN_MYSQL_TESTS") == "1", "Set RUN_MYSQL_TESTS=1 for real MySQL publication checks")
class MySQLPublishingIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fixture = ExportFixture(self.root / "export")
        self.fixture.write()
        self.schema = "test_publish_" + uuid.uuid4().hex
        with patch.dict(os.environ, {"MYSQL_TEST_DATABASE": self.schema}):
            self.settings = MySQLSettings.from_env(prefix="MYSQL_TEST_")
        self.owned = False
        real_execute = publishing._execute
        def tracked_execute(connection, statement, parameters=None):
            result = real_execute(connection, statement, parameters)
            if statement.startswith("CREATE DATABASE " + publishing._quote(self.schema) + " "):
                self.owned = True
            return result
        self.execute_patch = patch.object(publishing, "_execute", side_effect=tracked_execute)
        self.execute_patch.start()
        self.addCleanup(self.execute_patch.stop)
        self.addCleanup(self.cleanup_schema)

    @contextmanager
    def database(self, *, no_schema=False):
        connection = connect(self.settings, database="" if no_schema else self.schema)
        try:
            yield connection
        finally:
            connection.close()

    def cleanup_schema(self):
        if not self.owned:
            return
        # Only the exact UUID-named schema created by this test is in scope.
        self.assertRegex(self.schema, r"\Atest_publish_[0-9a-f]{32}\Z")
        with self.database(no_schema=True) as connection:
            publishing._execute(connection, "DROP DATABASE " + publishing._quote(self.schema))

    def publish(self):
        return publishing.publish_mysql(self.fixture.root, self.settings)

    def assert_unpublished_empty(self):
        self.assertTrue(self.owned)
        with self.database() as connection:
            for table in SERVING_TABLES | {"__metadata"}:
                self.assertEqual(publishing._one(connection, "SELECT COUNT(*) FROM " + publishing._quote(table))[0], 0)

    def test_publishes_nine_tables_and_metadata_without_future_labels(self):
        before = {path: sha(path) for path in self.fixture.root.rglob("*") if path.is_file()}
        report = self.publish()
        self.assertEqual(report["status"], "PUBLISHED")
        self.assertEqual(report["databaseSchema"], self.schema)
        self.assertEqual(set(report["importedTables"]), SERVING_TABLES)
        self.assertFalse(report["targetLabelsImported"])
        with self.database() as connection:
            with connection.cursor() as cursor:
                cursor.execute("SHOW TABLES")
                self.assertEqual({row[0] for row in cursor.fetchall()}, SERVING_TABLES | {"__metadata"})
                cursor.execute("SELECT `key`,`value` FROM __metadata")
                metadata = {key: json.loads(value) for key, value in cursor.fetchall()}
            self.assertEqual(metadata, dict(self.fixture.manifest, qualityReport=self.fixture.quality,
                                            **publishing.PUBLICATION_METADATA))
            self.assertEqual(publishing._one(connection, "SELECT net_paid_cents FROM city_daily")[0], -50)
            self.assertEqual(publishing._one(connection, "SELECT reference_time FROM ml_features_hourly")[0],
                             "2025-12-01T01:00:00Z")
        self.assertEqual(before, {path: sha(path) for path in self.fixture.root.rglob("*") if path.is_file()})

    def test_existing_schema_is_never_overwritten(self):
        self.publish()
        with self.assertRaises(Exception):
            self.publish()
        with self.database() as connection:
            self.assertEqual(publishing._one(connection, "SELECT COUNT(*) FROM cities")[0], 1)
            self.assertEqual(json.loads(publishing._one(connection,
                "SELECT value FROM __metadata WHERE `key`=%s", ("publicationStatus",))[0]), "PUBLISHED")

    def test_rows_and_ready_marker_are_invisible_until_commit(self):
        original = publishing._relationships
        def inspect_before_commit(connection, manifest):
            original(connection, manifest)
            self.assert_unpublished_empty()
        with patch.object(publishing, "_relationships", side_effect=inspect_before_commit):
            self.publish()
        with self.database() as connection:
            self.assertEqual(publishing._one(connection, "SELECT COUNT(*) FROM cities")[0], 1)

    def test_duplicate_primary_key_rolls_back_every_table(self):
        self.fixture.rows["ml_targets_hourly"] *= 2
        self.fixture.write()
        with self.assertRaises(Exception):
            self.publish()
        self.assert_unpublished_empty()

    def test_mismatched_city_totals_roll_back_every_table(self):
        self.fixture.rows["city_daily"][0]["energy_wh"] += 1
        self.fixture.write()
        with self.assertRaisesRegex(ValueError, "station-day sums"):
            self.publish()
        self.assert_unpublished_empty()

    def test_missing_target_key_rolls_back_every_table(self):
        self.fixture.rows["ml_targets_hourly"][0]["reference_time"] = "2025-12-01T02:00:00Z"
        self.fixture.write()
        with self.assertRaisesRegex(ValueError, "key sets differ"):
            self.publish()
        self.assert_unpublished_empty()

    def test_mutated_export_before_commit_rolls_back_every_table(self):
        original = publishing._relationships
        def mutate(connection, manifest):
            original(connection, manifest)
            path = self.fixture.root / "serving_manifest.json"
            path.write_text(path.read_text(encoding="utf-8") + " ", encoding="utf-8")
        with patch.object(publishing, "_relationships", side_effect=mutate), self.assertRaisesRegex(ValueError, "changed"):
            self.publish()
        self.assert_unpublished_empty()

    def test_case_and_trailing_spaces_are_distinct_ids(self):
        self.fixture.rows["cities"] += [dict(city_id="c1", latitude=38.9, longitude=121.6),
                                       dict(city_id="C1 ", latitude=38.9, longitude=121.6)]
        self.fixture.write()
        report = self.publish()
        self.assertEqual(report["importedTables"]["cities"], 3)
        with self.database() as connection:
            for city in ["C1", "c1", "C1 "]:
                self.assertEqual(publishing._one(connection, "SELECT COUNT(*) FROM cities WHERE city_id=%s", (city,))[0], 1)

    def test_foreign_city_ownership_is_case_sensitive(self):
        self.fixture.rows["station_snapshot"][0]["city_id"] = "c1"
        self.fixture.write()
        with self.assertRaisesRegex(ValueError, "Unknown/null city|city ownership"):
            self.publish()
        self.assert_unpublished_empty()

    def test_overlong_identifier_is_not_silently_truncated(self):
        self.fixture.rows["cities"][0]["city_id"] = "C" * 161
        self.fixture.write()
        with self.assertRaisesRegex(ValueError, "storage contract"):
            self.publish()
        self.assert_unpublished_empty()

    def test_targets_are_type_checked_even_though_never_persisted(self):
        self.fixture.rows["ml_targets_hourly"][0]["label_power_kw_h01"] = "NaN"
        self.fixture.write()
        with self.assertRaisesRegex(ValueError, "Non-finite"):
            self.publish()
        self.assert_unpublished_empty()

    def test_batch_streaming_and_bigint_exactness(self):
        self.fixture.rows["user_activity_daily"] = [dict(station_id="S1", city_id="C1",
            business_date="2025-12-01", user_id="U" + str(index)) for index in range(1005)]
        for table in ["station_metrics_daily", "city_daily"]:
            self.fixture.rows[table][0]["energy_wh"] = 2**53 + 1
        self.fixture.write()
        report = self.publish()
        self.assertEqual(report["importedTables"]["user_activity_daily"], 1005)
        with self.database() as connection:
            self.assertEqual(publishing._one(connection, "SELECT energy_wh FROM city_daily")[0], 2**53 + 1)

    def test_empty_typed_tables_are_supported(self):
        self.fixture.rows = {table: [] for table in TABLE_KEYS}
        self.fixture.write()
        self.assertEqual(sum(self.publish()["importedTables"].values()), 0)


if __name__ == "__main__":
    unittest.main()
