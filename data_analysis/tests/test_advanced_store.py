"""Independent SQL publication fixtures; no server, credentials or real data.

The reader runs against SQLite, while MySQL publication is exercised through
a DB-API adapter over a separate in-memory SQLite connection.
"""

from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import tempfile
import unittest
from unittest.mock import patch

from data_analysis.backend import advanced_store as store
from data_analysis.backend.database import Snapshot
from data_analysis.backend.errors import ApiError
from data_analysis.tests.test_advanced_api import write_bundle


TABLE_NAMES = ("station_day", "station_hour", "attempt_flow", "session_segments",
               "retention", "user_behavior", "service_hour")
DATA_COLUMNS = ("row_number", "payload", "row_sha256", "station_id", "city_id", "business_date")
MARKER_COLUMNS = ("publication_id", "status", "dataset_id", "published_batch_id", "pipeline_run_id",
                  "source_manifest_sha256", "fingerprint", "payload")
BINDINGS = {"datasetId": "dataset_id", "publishedBatchId": "published_batch_id",
            "pipelineRunId": "pipeline_run_id", "sourceManifestSha256": "source_manifest_sha256"}


def canonical(value):
    """Compute expected values independently of the production serializer."""
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)


def digest(value):
    return hashlib.sha256(canonical(value).encode("utf-8")).hexdigest()


def make_record(manifest, tables):
    return {"storeVersion": "1.0.0", "manifest": deepcopy(manifest),
            "tables": {name: {"rows": len(tables[name]), "sha256": digest(tables[name])}
                       for name in TABLE_NAMES}}


def create_schema(connection, name):
    if name == "adv_publication":
        columns = MARKER_COLUMNS
    else:
        columns = DATA_COLUMNS
    connection.execute("CREATE TABLE " + name + " (" + ", ".join(
        column + (" INTEGER" if column in {"publication_id", "row_number"} else " TEXT")
        for column in columns) + ")")


def independent_rows(name, rows, manifest):
    stations = {row["station_id"]: row for row in manifest["stations"]}
    for index, row in enumerate(rows, 1):
        station, city, day = row.get("station_id"), row.get("city_id"), row.get("business_date")
        if name == "retention":
            station = row["scope_id"] if row["scope_type"] == "STATION" else None
            city = (row["scope_id"] if row["scope_type"] == "CITY"
                    else stations[station]["city_id"] if station else None)
            day = row["cohort_month"]
        yield index, canonical(row), digest(row), station, city, day


def install_publication(connection, metadata, manifest, tables):
    """Populate independently, without calling any production writer helper."""
    record = make_record(manifest, tables)
    for name in TABLE_NAMES:
        create_schema(connection, "adv_" + name)
        connection.executemany("INSERT INTO adv_" + name + " VALUES (?,?,?,?,?,?)",
                               independent_rows(name, tables[name], manifest))
    create_schema(connection, "adv_publication")
    connection.execute("INSERT INTO adv_publication VALUES (?,?,?,?,?,?,?,?)",
                       (1, "READY", *(metadata[key] for key in BINDINGS), digest(record), canonical(record)))
    connection.commit()
    return record


class AdvancedStoreFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="advanced-store-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.metadata = dict(schemaVersion="1.0.0", datasetId="synthetic-fixture", publishedBatchId="fake-batch",
                             pipelineRunId="fake-run", sourceManifestSha256="f" * 64,
                             generatedAt="2026-03-01T00:00:00Z", startDate="2025-12-01", endDate="2026-03-01",
                             source="SIMULATED", tables={}, qualityReport={})
        self.manifest, self.tables = write_bundle(self.root, self.metadata)
        self.connection = sqlite3.connect(":memory:")
        self.connection.row_factory = sqlite3.Row
        self.addCleanup(self.connection.close)
        self.connection.execute("CREATE TABLE __metadata (`key` TEXT PRIMARY KEY, value TEXT)")
        self.connection.executemany("INSERT INTO __metadata VALUES (?,?)",
                                   [(key, canonical(value)) for key, value in self.metadata.items()])
        self.connection.commit()

    def install(self, *, manifest=None, tables=None):
        return install_publication(self.connection, self.metadata, manifest or self.manifest,
                                   self.tables if tables is None else tables)

    def load(self):
        return store.load_from_snapshot(Snapshot(self.connection))

    def assert_rejected(self, statements):
        self.connection.execute("SAVEPOINT corruption")
        try:
            for sql, parameters in statements:
                self.connection.execute(sql, parameters)
            with self.assertRaises(ApiError) as caught:
                self.load()
            self.assertIn(caught.exception.status, (409, 503))
            self.assertNotIn("SELECT", caught.exception.message)
        finally:
            self.connection.execute("ROLLBACK TO corruption")
            self.connection.execute("RELEASE corruption")


class AdvancedStoreReaderTests(AdvancedStoreFixture):
    def test_known_sql_contract_and_independent_canonical_fingerprint(self):
        self.assertEqual(set(store.TABLES), set(TABLE_NAMES))
        self.assertEqual(store.PUBLICATION_TABLE, "adv_publication")
        self.assertEqual(set(store.SQL_TABLES), {"adv_" + name for name in TABLE_NAMES})
        self.assertEqual(store.canonical_json({"中": [True, None, 0], "a": 1}), '{"a":1,"中":[true,null,0]}')
        expected = make_record(self.manifest, self.tables)
        actual = store.prepare_publication(self.metadata, self.manifest, self.tables)
        self.assertEqual(actual, expected)
        self.assertEqual(store.publication_fingerprint(actual), digest(expected))
        self.assertEqual(store.publication_fingerprint(dict(reversed(list(actual.items())))), digest(expected))

    def test_absent_extension_is_the_only_fallback(self):
        self.assertIsNone(self.load())
        self.connection.execute("CREATE TABLE unrelated (value TEXT)")
        self.assertIsNone(self.load())

    def test_valid_publication_preserves_original_manifest_rows_and_json_types(self):
        self.install()
        actual_manifest, actual_tables = self.load()
        self.assertEqual(actual_manifest, self.manifest)
        self.assertEqual(actual_tables, self.tables)
        self.assertIs(actual_tables["station_hour"][0]["is_complete"], True)
        self.assertIsNone(actual_tables["station_hour"][2]["energy_wh"])
        self.assertEqual(actual_tables["station_day"][0]["net_paid_cents"], 499)
        self.assertEqual(actual_manifest["stations"][0]["station_name"], "A站")

    def test_empty_tables_are_valid_publications(self):
        tables = {name: [] for name in TABLE_NAMES}
        manifest = deepcopy(self.manifest)
        for item in manifest["tables"].values():
            item["rows"] = 0
        self.install(manifest=manifest, tables=tables)
        self.assertEqual(self.load(), (manifest, tables))

    def test_partial_extra_and_missing_tables_fail_closed(self):
        create_schema(self.connection, "adv_station_day")
        with self.assertRaises(ApiError):
            self.load()
        self.connection.execute("DROP TABLE adv_station_day")
        self.install()
        for name in ("adv_station_hour", "adv_publication"):
            with self.subTest(missing=name):
                self.assert_rejected([("DROP TABLE " + name, ())])
        self.assert_rejected([("CREATE TABLE adv_unexpected (payload TEXT)", ())])

    def test_views_are_not_accepted_as_publication_tables(self):
        self.install()
        for name in ("adv_station_day", "adv_publication"):
            with self.subTest(view=name):
                self.assert_rejected([("ALTER TABLE " + name + " RENAME TO fixture_backup", ()),
                                      ("CREATE VIEW " + name + " AS SELECT * FROM fixture_backup", ())])

    def test_marker_must_be_unique_ready_and_have_id_one(self):
        self.install()
        for sql, parameters in [
            ("DELETE FROM adv_publication", ()),
            ("UPDATE adv_publication SET status=?", ("BUILDING",)),
            ("UPDATE adv_publication SET status=?", ("ready",)),
            ("UPDATE adv_publication SET publication_id=2", ()),
            ("INSERT INTO adv_publication SELECT * FROM adv_publication", ())]:
            with self.subTest(sql=sql, parameters=parameters):
                self.assert_rejected([(sql, parameters)])

    def test_required_columns_are_checked_for_marker_and_rows(self):
        self.install()
        for table, column in [("adv_publication", "fingerprint"), ("adv_publication", "source_manifest_sha256"),
                              ("adv_station_day", "row_sha256"), ("adv_station_hour", "business_date")]:
            with self.subTest(table=table, column=column):
                self.assert_rejected([("ALTER TABLE " + table + " RENAME COLUMN " + column + " TO missing_field", ())])

    def test_each_binding_is_checked_in_marker_manifest_and_snapshot(self):
        self.install()
        for key, column in BINDINGS.items():
            with self.subTest(binding=key, location="marker"):
                self.assert_rejected([("UPDATE adv_publication SET " + column + "=?", ("wrong-binding",))])
            with self.subTest(binding=key, location="snapshot"):
                self.assert_rejected([("UPDATE __metadata SET value=? WHERE `key`=?",
                                       (canonical("e" * 64 if key == "sourceManifestSha256" else "different"), key))])
            with self.subTest(binding=key, location="manifest"):
                record = make_record(self.manifest, self.tables)
                record["manifest"][key] = "e" * 64 if key == "sourceManifestSha256" else "different"
                self.assert_rejected([("UPDATE adv_publication SET payload=?, fingerprint=?",
                                       (canonical(record), digest(record)))])

    def test_marker_payload_hash_and_schema_fail_closed(self):
        self.install()
        self.assert_rejected([("UPDATE adv_publication SET fingerprint=?", ("0" * 64,))])
        for payload in ("{", "[]", "null", '{"storeVersion":"1.0.0"}'):
            with self.subTest(payload=payload):
                self.assert_rejected([("UPDATE adv_publication SET payload=?", (payload,))])
        for change in (lambda record: record.update(storeVersion="999.0.0"),
                       lambda record: record["tables"].pop("retention"),
                       lambda record: record["tables"].update(unexpected={"rows": 0, "sha256": digest([])})):
            record = make_record(self.manifest, self.tables)
            change(record)
            self.assert_rejected([("UPDATE adv_publication SET payload=?, fingerprint=?",
                                   (canonical(record), digest(record)))])

    def test_row_count_hash_sequence_and_payload_corruption_fail_closed(self):
        self.install()
        changes = [
            ("DELETE FROM adv_station_hour WHERE row_number=2", ()),
            ("UPDATE adv_station_hour SET row_number=8 WHERE row_number=2", ()),
            ("UPDATE adv_station_hour SET row_number=1 WHERE row_number=2", ()),
            ("UPDATE adv_station_hour SET row_number=0 WHERE row_number=1", ()),
            ("UPDATE adv_station_hour SET row_sha256=? WHERE row_number=1", ("0" * 64,)),
            ("UPDATE adv_station_day SET payload=?", ("{",)),
            ("UPDATE adv_station_day SET payload=?", ("[]",)),
            ("UPDATE adv_station_day SET payload=?", ("null",))]
        for sql, parameters in changes:
            with self.subTest(sql=sql, parameters=parameters):
                self.assert_rejected([(sql, parameters)])
        changed_row = dict(self.tables["station_day"][0], net_paid_cents=500)
        self.assert_rejected([("UPDATE adv_station_day SET payload=?, row_sha256=?",
                               (canonical(changed_row), digest(changed_row)))])

    def test_record_counts_and_table_hash_are_checked_independently(self):
        self.install()
        for key, value in (("rows", 99), ("sha256", "0" * 64)):
            record = make_record(self.manifest, self.tables)
            record["tables"]["station_day"][key] = value
            with self.subTest(key=key):
                self.assert_rejected([("UPDATE adv_publication SET payload=?, fingerprint=?",
                                       (canonical(record), digest(record)))])

    def test_same_snapshot_reuses_success_without_repeating_row_queries(self):
        self.install()
        snapshot = Snapshot(self.connection)
        with patch.object(snapshot, "rows", wraps=snapshot.rows) as query:
            self.assertEqual(store.load_from_snapshot(snapshot), (self.manifest, self.tables))
            first_read_count = query.call_count
            self.assertGreater(first_read_count, len(TABLE_NAMES))
            self.assertEqual(store.load_from_snapshot(snapshot), (self.manifest, self.tables))
            self.assertEqual(query.call_count, first_read_count)

    def test_new_snapshot_rechecks_integrity_after_a_successful_read(self):
        self.install()
        first = Snapshot(self.connection)
        self.assertEqual(store.load_from_snapshot(first), (self.manifest, self.tables))
        self.connection.execute("UPDATE adv_station_day SET row_sha256=?", ("0" * 64,))
        second = Snapshot(self.connection)
        with patch.object(second, "rows", wraps=second.rows) as query, self.assertRaises(ApiError):
            store.load_from_snapshot(second)
        self.assertGreater(query.call_count, 0)

    def test_snapshot_binding_changes_do_not_reuse_verified_results(self):
        self.install()
        for key in BINDINGS:
            with self.subTest(binding=key):
                snapshot = Snapshot(self.connection)
                with patch.object(snapshot, "rows", wraps=snapshot.rows) as query:
                    self.assertEqual(store.load_from_snapshot(snapshot), (self.manifest, self.tables))
                    first_read_count = query.call_count
                    snapshot.metadata[key] = "e" * 64 if key == "sourceManifestSha256" else "changed-binding"
                    with self.assertRaises(ApiError) as caught:
                        store.load_from_snapshot(snapshot)
                    self.assertEqual(caught.exception.status, 409)
                    self.assertGreater(query.call_count, first_read_count)

    def test_failed_verification_is_not_cached_on_the_snapshot(self):
        self.install()
        self.connection.execute("UPDATE adv_service_hour SET row_sha256=?", ("0" * 64,))
        snapshot = Snapshot(self.connection)
        with patch.object(snapshot, "rows", wraps=snapshot.rows) as query:
            with self.assertRaises(ApiError):
                store.load_from_snapshot(snapshot)
            failed_read_count = query.call_count
            self.connection.execute("UPDATE adv_service_hour SET row_sha256=?", (digest(self.tables["service_hour"][0]),))
            self.assertEqual(store.load_from_snapshot(snapshot), (self.manifest, self.tables))
            self.assertGreater(query.call_count, failed_read_count)

    def test_absent_extension_result_is_not_cached_on_the_snapshot(self):
        snapshot = Snapshot(self.connection)
        with patch.object(snapshot, "rows", wraps=snapshot.rows) as query:
            self.assertIsNone(store.load_from_snapshot(snapshot))
            absent_read_count = query.call_count
            self.install()
            self.assertEqual(store.load_from_snapshot(snapshot), (self.manifest, self.tables))
            self.assertGreater(query.call_count, absent_read_count)

    def test_noncanonical_payloads_are_rejected_even_with_matching_hash(self):
        self.install()
        row = self.tables["station_day"][0]
        payload = json.dumps(row, ensure_ascii=False, indent=2)
        self.assert_rejected([("UPDATE adv_station_day SET payload=?, row_sha256=?",
                               (payload, hashlib.sha256(payload.encode("utf-8")).hexdigest()))])
        payload = '{"station_id":"S1","station_id":"S2"}'
        self.assert_rejected([("UPDATE adv_station_day SET payload=?, row_sha256=?",
                               (payload, hashlib.sha256(payload.encode("utf-8")).hexdigest()))])

    def test_derived_scope_columns_are_verified_against_payload(self):
        self.install()
        for name, column, value in [("station_day", "station_id", "S2"), ("station_day", "city_id", "C2"),
                                    ("station_hour", "business_date", "2025-12-02"),
                                    ("retention", "station_id", "S1"), ("retention", "city_id", "C1"),
                                    ("retention", "business_date", "2026-02-01")]:
            with self.subTest(table=name, column=column):
                self.assert_rejected([("UPDATE adv_" + name + " SET " + column + "=? WHERE row_number=1", (value,))])

    def test_retention_scope_index_includes_station_city_and_cohort_date(self):
        rows = [dict(scope_type="STATION", scope_id="S1", cohort_month="2025-12-01", month_offset=0, n=1, cohort_size=1),
                dict(scope_type="CITY", scope_id="C1", cohort_month="2025-12-01", month_offset=0, n=1, cohort_size=1),
                dict(scope_type="ALL", scope_id="ALL", cohort_month="2025-12-01", month_offset=0, n=1, cohort_size=1)]
        self.assertEqual(list(store.storage_rows("retention", rows, self.manifest)),
                         list(independent_rows("retention", rows, self.manifest)))
        for name in TABLE_NAMES:
            self.assertEqual(list(store.storage_rows(name, self.tables[name], self.manifest)),
                             list(independent_rows(name, self.tables[name], self.manifest)))

    def test_fully_rehashed_raw_identity_or_label_payloads_are_rejected(self):
        for field in ("user_id", "userId", "future_energy_wh", "target_energy_wh", "label_power_kw_h01"):
            with self.subTest(field=field):
                tables = deepcopy(self.tables)
                tables["station_day"][0][field] = "synthetic-private-value"
                try:
                    self.install(tables=tables)
                    with self.assertRaises(ApiError):
                        self.load()
                finally:
                    for name in ("adv_publication", *("adv_" + name for name in TABLE_NAMES)):
                        self.connection.execute("DROP TABLE IF EXISTS " + name)


class MemoryMySQLCursor:
    """Translate the publication's bounded SQL subset, rejecting surprises."""

    def __init__(self, connection):
        self.connection = connection
        self.result = []

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    def execute(self, statement, parameters=()):
        parameters = tuple(parameters or ())
        self.connection.events.append((statement, parameters))
        upper = " ".join(statement.upper().split())
        if self.connection.fail_on and self.connection.fail_on in upper:
            raise OSError("synthetic publication failure")
        if "GET_LOCK(" in upper or "RELEASE_LOCK(" in upper:
            match = re.search(r"\bAS\s+`?(\w+)`?", statement, re.IGNORECASE)
            alias = match.group(1) if match else statement[7:].strip()
            value = self.connection.lock_result if "GET_LOCK(" in upper else 1
            self.result = [{alias: value}]
        elif "INFORMATION_SCHEMA.TABLES" in upper:
            self.result = [{"name": row["name"], "type": "BASE TABLE" if row["type"] == "table" else "VIEW"}
                           for row in self.connection.database.execute(
                               "SELECT name, type FROM sqlite_master WHERE type IN ('table','view')")]
        elif upper.startswith("SET "):
            self.result = []
        elif upper.startswith("CREATE TABLE "):
            match = re.search(r"CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?`?(\w+)`?", statement, re.IGNORECASE)
            if not match or match.group(1) not in {"adv_publication", *("adv_" + name for name in TABLE_NAMES)}:
                raise AssertionError("Unexpected schema mutation")
            create_schema(self.connection.database, match.group(1))
            self.result = []
        else:
            cursor = self.connection.database.execute(statement.replace("%s", "?").replace("%%", "%"), parameters)
            self.result = [dict(row) for row in cursor.fetchall()] if cursor.description else []
            if self.connection.corrupt_after_ready and re.match(r"INSERT INTO `?ADV_PUBLICATION`?\b", upper):
                self.connection.database.execute("UPDATE adv_station_day SET row_sha256=?", ("0" * 64,))
        return len(self.result)

    def executemany(self, statement, parameters):
        for row in parameters:
            self.execute(statement, row)

    def fetchall(self):
        return list(self.result)

    def fetchone(self):
        return self.result[0] if self.result else None


class MemoryMySQLConnection:
    def __init__(self, database):
        self.database = database
        self.events = []
        self.fail_on = None
        self.fail_commit = False
        self.lose_commit_ack = False
        self.lock_result = 1
        self.corrupt_after_ready = False

    def cursor(self):
        return MemoryMySQLCursor(self)

    def autocommit(self, enabled):
        self.events.append(("autocommit", (enabled,)))
        self.database.isolation_level = None if enabled else ""

    def begin(self):
        self.events.append(("begin", ()))
        self.database.execute("BEGIN")

    def commit(self):
        self.events.append(("commit", ()))
        if self.fail_commit:
            raise OSError("synthetic lost commit acknowledgement")
        self.database.commit()
        if self.lose_commit_ack:
            raise OSError("synthetic acknowledgement lost after server commit")

    def rollback(self):
        self.events.append(("rollback", ()))
        self.database.rollback()

    def close(self):
        self.events.append(("close", ()))


class AdvancedMySQLPublisherTests(AdvancedStoreFixture):
    def setUp(self):
        super().setUp()
        from data_analysis.mysql_support import MySQLSettings
        from data_analysis.publishing import advanced_mysql
        self.publisher = advanced_mysql
        self.settings = MySQLSettings(host="example.invalid", port=3306, user="fake_publisher",
                                      password="synthetic-password-never-a-credential", database="fake_advanced_test")
        for key, value in {"storageBackend": "mysql", "publicationStatus": "PUBLISHED"}.items():
            self.metadata[key] = value
            self.connection.execute("INSERT INTO __metadata VALUES (?,?)", (key, canonical(value)))
        self.connection.commit()
        self.mysql = MemoryMySQLConnection(self.connection)
        self.connect_patch = patch.object(self.publisher, "connect", return_value=self.mysql)
        self.connect = self.connect_patch.start()
        self.addCleanup(self.connect_patch.stop)

    def publish(self):
        return self.publisher.publish_advanced_mysql(self.root, self.settings)

    def event_names(self):
        return [" ".join(statement.upper().split()) for statement, _ in self.mysql.events]

    def assert_no_mutation(self):
        self.assertFalse(any(re.match(r"(?:CREATE|INSERT|UPDATE|DELETE|DROP|ALTER|REPLACE|TRUNCATE)\b", event)
                             for event in self.event_names()))
        self.assertNotIn("COMMIT", self.event_names())

    def test_new_publication_real_validation_ddl_before_transaction_and_commit_last(self):
        report = self.publish()
        self.connect.assert_called_once_with(self.settings, dict_rows=True)
        self.assertEqual(report["status"], "PUBLISHED")
        self.assertEqual(report["fingerprint"], digest(make_record(self.manifest, self.tables)))
        self.assertEqual(report["importedTables"], {name: len(rows) for name, rows in self.tables.items()})
        self.assertEqual(store.load_from_snapshot(Snapshot(self.connection, "sqlite")), (self.manifest, self.tables))
        events = self.event_names()
        ddl = [i for i, event in enumerate(events) if event.startswith("CREATE TABLE ")]
        inserts = [i for i, event in enumerate(events) if event.startswith("INSERT INTO ")]
        locks = [i for i, event in enumerate(events) if "GET_LOCK(" in event]
        self.assertEqual(len(ddl), 8)
        self.assertEqual(len(locks), 1)
        self.assertLess(locks[0], min(ddl))
        self.assertLess(max(ddl), events.index("BEGIN"))
        self.assertLess(events.index("BEGIN"), min(inserts))
        self.assertLess(max(inserts), events.index("COMMIT"))
        self.assertIn("ADV_PUBLICATION", events[max(inserts)])
        verification = events[max(inserts) + 1:events.index("COMMIT")]
        for name in TABLE_NAMES:
            self.assertTrue(any(" FROM ADV_" + name.upper() + " " in event for event in verification), name)
        self.assertTrue(any("RELEASE_LOCK(" in event for event in events))
        self.assertEqual(events[-1], "CLOSE")
        self.assertFalse(any(event.startswith(("DROP ", "UPDATE ", "DELETE ")) for event in events))
        serialized = json.dumps(report)
        for private in (self.settings.password, self.settings.user, str(self.root)):
            self.assertNotIn(private, serialized)

    def test_verified_identical_publication_is_noop_without_ddl_dml_or_commit(self):
        self.install()
        before = list(self.connection.iterdump())
        report = self.publish()
        self.assertEqual(report["status"], "ALREADY_PUBLISHED")
        self.assertEqual(report["fingerprint"], digest(make_record(self.manifest, self.tables)))
        self.assertEqual(list(self.connection.iterdump()), before)
        self.assert_no_mutation()
        self.assertEqual(self.event_names()[-1], "CLOSE")

    def test_partial_schema_is_not_repaired(self):
        create_schema(self.connection, "adv_station_day")
        self.connection.commit()
        with self.assertRaises((ValueError, ApiError)):
            self.publish()
        self.assert_no_mutation()
        self.assertEqual({row["name"] for row in self.connection.execute(
            "SELECT name FROM sqlite_master WHERE name LIKE 'adv_%'")}, {"adv_station_day"})

    def test_extra_schema_table_is_not_accepted_as_published(self):
        self.install()
        self.connection.execute("CREATE TABLE adv_unexpected (payload TEXT)")
        with self.assertRaises((ValueError, ApiError)):
            self.publish()
        self.assert_no_mutation()

    def test_same_marker_fingerprint_with_corrupt_rows_is_not_noop(self):
        self.install()
        self.connection.execute("UPDATE adv_station_day SET row_sha256=?", ("0" * 64,))
        self.connection.commit()
        with self.assertRaises((ValueError, ApiError)):
            self.publish()
        self.assert_no_mutation()

    def test_different_verified_publication_is_not_overwritten(self):
        tables = deepcopy(self.tables)
        tables["station_day"][0]["energy_wh"] += 1
        self.install(tables=tables)
        before = list(self.connection.iterdump())
        with self.assertRaises((ValueError, ApiError)):
            self.publish()
        self.assert_no_mutation()
        self.assertEqual(list(self.connection.iterdump()), before)

    def test_batch_mismatch_prevents_any_ddl_or_dml(self):
        self.connection.execute("UPDATE __metadata SET value=? WHERE `key`='publishedBatchId'", (canonical("other-batch"),))
        self.connection.commit()
        with self.assertRaises((ValueError, ApiError)):
            self.publish()
        self.assert_no_mutation()

    def test_incomplete_disk_bundle_prevents_ddl_or_dml(self):
        (self.root / "_SUCCESS").unlink()
        with self.assertRaises((ValueError, ApiError)):
            self.publish()
        self.assert_no_mutation()

    def test_failed_named_lock_prevents_mutation_and_closes_connection(self):
        self.mysql.lock_result = 0
        with self.assertRaises((ValueError, ApiError)):
            self.publish()
        self.assert_no_mutation()
        self.assertEqual(self.event_names()[-1], "CLOSE")

    def test_insert_failure_rolls_back_rows_and_never_drops_or_repairs_schema(self):
        self.mysql.fail_on = "INSERT INTO `ADV_RETENTION`"
        with self.assertRaises((OSError, ValueError, ApiError)):
            self.publish()
        self.assertIn("ROLLBACK", self.event_names())
        self.assertNotIn("COMMIT", self.event_names())
        self.assertFalse(any(event.startswith(("DROP ", "UPDATE ", "DELETE ")) for event in self.event_names()))
        for name in ("adv_publication", *("adv_" + name for name in TABLE_NAMES)):
            self.assertEqual(self.connection.execute("SELECT COUNT(*) FROM " + name).fetchone()[0], 0)
        self.assertEqual(self.event_names()[-1], "CLOSE")

    def test_lost_commit_acknowledgement_never_reports_success_and_attempts_rollback(self):
        self.mysql.fail_commit = True
        with self.assertRaises((OSError, ValueError, ApiError)):
            self.publish()
        self.assertIn("ROLLBACK", self.event_names())
        self.assertFalse(any(event.startswith(("DROP ", "UPDATE ", "DELETE ")) for event in self.event_names()))
        self.assertEqual(self.event_names()[-1], "CLOSE")

    def test_server_committed_before_lost_ack_is_verified_as_published_on_retry(self):
        self.mysql.lose_commit_ack = True
        with self.assertRaises(OSError):
            self.publish()
        self.assertIn("COMMIT", self.event_names())
        self.assertIn("ROLLBACK", self.event_names())
        # The rollback after the lost response cannot undo an actual commit.
        self.assertEqual(store.load_from_snapshot(Snapshot(self.connection)), (self.manifest, self.tables))
        before = list(self.connection.iterdump())

        # A fresh client session sees the retained server state, not a cache.
        self.mysql = MemoryMySQLConnection(self.connection)
        self.connect.return_value = self.mysql
        report = self.publish()
        self.assertEqual(report["status"], "ALREADY_PUBLISHED")
        self.assertEqual(report["fingerprint"], digest(make_record(self.manifest, self.tables)))
        self.assertEqual(list(self.connection.iterdump()), before)
        self.assert_no_mutation()
        events = self.event_names()
        for name in TABLE_NAMES:
            self.assertTrue(any(" FROM ADV_" + name.upper() + " " in event for event in events), name)
        self.assertEqual(events[-1], "CLOSE")

    def test_verification_after_ready_rejects_corruption_and_rolls_back_before_commit(self):
        self.mysql.corrupt_after_ready = True
        with self.assertRaises((ValueError, ApiError)):
            self.publish()
        self.assertIn("ROLLBACK", self.event_names())
        self.assertNotIn("COMMIT", self.event_names())
        for name in ("adv_publication", *("adv_" + name for name in TABLE_NAMES)):
            self.assertEqual(self.connection.execute("SELECT COUNT(*) FROM " + name).fetchone()[0], 0)


class AdvancedMySQLConfigTests(unittest.TestCase):
    def setUp(self):
        from data_analysis.publishing import advanced_mysql
        self.publisher = advanced_mysql
        self.temporary = tempfile.TemporaryDirectory(prefix="advanced-config-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.config = self.root / "fake_config.env"
        self.config.write_text(
            'ANALYTICS_MYSQL_HOST = "db.example.invalid"\n'
            'ANALYTICS_MYSQL_PORT = 3307\n'
            'ANALYTICS_MYSQL_USER = "fixture_user"\n'
            'ANALYTICS_MYSQL_PASSWORD = "only-a-synthetic-password"\n'
            'ANALYTICS_MYSQL_DATABASE = "fake_advanced_test"\n'
            'ANALYTICS_MYSQL_SSL_CA = "/synthetic/test-ca.pem"\n', encoding="utf-8")

    def test_reads_only_literal_mysql_settings_without_executing_config(self):
        with patch.dict(os.environ, {}, clear=True), patch("os.system") as system:
            settings = self.publisher.settings_from_config(self.config)
        system.assert_not_called()
        self.assertEqual((settings.host, settings.port, settings.user, settings.password, settings.database, settings.ssl_ca),
                         ("db.example.invalid", 3307, "fixture_user", "only-a-synthetic-password",
                          "fake_advanced_test", "/synthetic/test-ca.pem"))
        self.assertNotIn("only-a-synthetic-password", repr(settings))

    def test_environment_overrides_literal_file_settings(self):
        environment = {"ANALYTICS_MYSQL_HOST": "env.example.invalid", "ANALYTICS_MYSQL_PORT": "3308",
                       "ANALYTICS_MYSQL_USER": "env_fixture", "ANALYTICS_MYSQL_PASSWORD": "env-fake-password",
                       "ANALYTICS_MYSQL_DATABASE": "fake_env_test", "ANALYTICS_MYSQL_SSL_CA": "/fake/env-ca.pem"}
        with patch.dict(os.environ, environment, clear=True):
            settings = self.publisher.settings_from_config(self.config)
        self.assertEqual((settings.host, settings.port, settings.user, settings.password, settings.database, settings.ssl_ca),
                         ("env.example.invalid", 3308, "env_fixture", "env-fake-password", "fake_env_test", "/fake/env-ca.pem"))

    def test_unknown_keys_and_command_lines_are_rejected_without_evaluation(self):
        original = self.config.read_text(encoding="utf-8")
        for extra in ('UNRELATED_API_KEY="not-used-by-publisher"', 'import os',
                      'os.system("never execute configuration")', 'source /synthetic/unread.env'):
            self.config.write_text(original + extra + "\n", encoding="utf-8")
            with self.subTest(extra=extra), patch.dict(os.environ, {}, clear=True), \
                 patch("os.system") as system, self.assertRaises(ValueError):
                self.publisher.settings_from_config(self.config)
            system.assert_not_called()

    def test_shell_expansion_syntax_in_password_remains_literal(self):
        password = "$HOME/$(printf never-executed)\\literal"
        self.config.write_text(self.config.read_text(encoding="utf-8").replace(
            'ANALYTICS_MYSQL_PASSWORD = "only-a-synthetic-password"',
            'ANALYTICS_MYSQL_PASSWORD = "' + password + '"'), encoding="utf-8")
        with patch.dict(os.environ, {}, clear=True), patch("os.system") as system:
            self.assertEqual(self.publisher.settings_from_config(self.config).password, password)
        system.assert_not_called()

    def test_invalid_port_is_rejected_without_secret_values_in_error(self):
        with patch.dict(os.environ, {"ANALYTICS_MYSQL_PORT": "not-a-port"}, clear=True), \
             self.assertRaises(ValueError) as caught:
            self.publisher.settings_from_config(self.config)
        self.assertNotIn("only-a-synthetic-password", str(caught.exception))

    def test_host_port_user_and_database_must_be_explicit(self):
        original = self.config.read_text(encoding="utf-8")
        for suffix in ("HOST", "PORT", "USER", "DATABASE"):
            key = "ANALYTICS_MYSQL_" + suffix
            self.config.write_text("\n".join(line for line in original.splitlines() if not line.startswith(key + " =")),
                                   encoding="utf-8")
            with self.subTest(missing=key), patch.dict(os.environ, {}, clear=True), self.assertRaises(ValueError):
                self.publisher.settings_from_config(self.config)

    def test_explicit_empty_password_is_allowed(self):
        self.config.write_text(self.config.read_text(encoding="utf-8").replace(
            'ANALYTICS_MYSQL_PASSWORD = "only-a-synthetic-password"', 'ANALYTICS_MYSQL_PASSWORD = ""'),
            encoding="utf-8")
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(self.publisher.settings_from_config(self.config).password, "")


if __name__ == "__main__":
    unittest.main()
