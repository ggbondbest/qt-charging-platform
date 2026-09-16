"""Install an authenticated advanced-aggregate bundle into its existing batch.

This publisher never creates/replaces a database or changes existing tables.
MySQL DDL is intentionally completed before the single data/READY transaction.
A failed initial installation may leave empty tables; those are deliberately
rejected on retry, rather than silently deleting or repairing user data.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys

from data_analysis.backend.advanced import load_bundle
from data_analysis.backend.advanced_store import (
    BINDINGS, PUBLICATION_TABLE, SQL_TABLES, TABLES, canonical_json,
    load_from_snapshot, prepare_publication, publication_fingerprint, storage_rows,
)
from data_analysis.backend.database import Snapshot
from data_analysis.backend.errors import ApiError
from data_analysis.mysql_support import MySQLSettings, connect


CONFIG_KEYS = frozenset("ANALYTICS_MYSQL_" + name for name in ("HOST", "PORT", "USER", "PASSWORD", "DATABASE", "SSL_CA"))
REQUIRED_CONFIG_KEYS = frozenset("ANALYTICS_MYSQL_" + name for name in ("HOST", "PORT", "USER", "DATABASE"))
_IDENTIFIER = re.compile(r"[a-z][a-z0-9_]{0,63}\Z")
_CHUNK_ROWS = 1_000


def settings_from_config(path):
    """Read six literal settings, never source/eval a shell or interpolate vars.

    Whole-line comments and optional enclosing quotes are supported. The text
    inside quotes is literal (including dollar signs and backslashes). Explicit
    environment settings override the corresponding file settings; an explicit
    empty password is valid for an isolated local development instance.
    """
    try:
        config = Path(path).expanduser()
        if not config.is_file() or config.is_symlink() or config.stat().st_size > 16_384:
            raise ValueError("Invalid MySQL import configuration")
        values = {}
        for line in config.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            key, separator, raw = stripped.partition("=")
            key, raw = key.strip(), raw.strip()
            if not separator or key not in CONFIG_KEYS or key in values:
                raise ValueError("Invalid MySQL import configuration")
            if raw.startswith(("'", '"')):
                if len(raw) < 2 or raw[-1] != raw[0]:
                    raise ValueError("Invalid MySQL import configuration")
                raw = raw[1:-1]
            if "\x00" in raw or "\r" in raw or "\n" in raw:
                raise ValueError("Invalid MySQL import configuration")
            values[key] = raw
        values.update({key: os.environ[key] for key in CONFIG_KEYS if key in os.environ})
        if not REQUIRED_CONFIG_KEYS <= values.keys():
            raise ValueError("Missing explicit MySQL import configuration")
        return MySQLSettings(host=values["ANALYTICS_MYSQL_HOST"], port=int(values["ANALYTICS_MYSQL_PORT"]),
            user=values["ANALYTICS_MYSQL_USER"], password=values.get("ANALYTICS_MYSQL_PASSWORD", ""),
            database=values["ANALYTICS_MYSQL_DATABASE"], ssl_ca=values.get("ANALYTICS_MYSQL_SSL_CA") or None)
    except (OSError, ValueError, TypeError, UnicodeError):
        raise ValueError("MySQL 导入配置无效；请检查六项允许配置及明确的主机、端口、用户和数据库") from None


def _quote(name):
    if not isinstance(name, str) or not _IDENTIFIER.fullmatch(name):
        raise ValueError("Invalid SQL identifier")
    return "`" + name + "`"


def _execute(connection, sql, parameters=()):
    with connection.cursor() as cursor:
        cursor.execute(sql, tuple(parameters))


def _one(connection, sql, parameters=()):
    with connection.cursor() as cursor:
        cursor.execute(sql, tuple(parameters))
        return cursor.fetchone()


def _insert_many(connection, sql, rows):
    chunk = []
    with connection.cursor() as cursor:
        for row in rows:
            chunk.append(row)
            if len(chunk) >= _CHUNK_ROWS:
                cursor.executemany(sql, chunk)
                chunk.clear()
        if chunk:
            cursor.executemany(sql, chunk)


def _create_tables(connection):
    engine = " ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin"
    for name in SQL_TABLES:
        _execute(connection, "CREATE TABLE " + _quote(name) + " ("
            "`row_number` BIGINT UNSIGNED NOT NULL PRIMARY KEY, "
            "payload LONGTEXT NOT NULL, row_sha256 CHAR(64) NOT NULL, "
            "station_id VARCHAR(160) NULL, city_id VARCHAR(160) NULL, business_date DATE NULL, "
            "CHECK (`row_number` >= 1), CHECK (JSON_VALID(payload)), "
            "KEY station_date (station_id, business_date), KEY city_date (city_id, business_date), "
            "KEY business_date_index (business_date))" + engine)
    _execute(connection, "CREATE TABLE " + _quote(PUBLICATION_TABLE) + " ("
        "publication_id TINYINT UNSIGNED NOT NULL PRIMARY KEY, status VARCHAR(16) NOT NULL, "
        "dataset_id VARCHAR(160) NOT NULL, published_batch_id VARCHAR(160) NOT NULL, "
        "pipeline_run_id VARCHAR(160) NOT NULL, source_manifest_sha256 CHAR(64) NOT NULL, "
        "fingerprint CHAR(64) NOT NULL, payload LONGTEXT NOT NULL, "
        "CHECK (publication_id = 1), CHECK (status = 'READY'), CHECK (JSON_VALID(payload)))" + engine)


def _report(status, record):
    return {"status": status, "fingerprint": publication_fingerprint(record),
            "datasetId": record["manifest"]["datasetId"], "publishedBatchId": record["manifest"]["publishedBatchId"],
            "pipelineRunId": record["manifest"]["pipelineRunId"], "sourceManifestSha256": record["manifest"]["sourceManifestSha256"],
            "importedTables": {name: record["tables"][name]["rows"] for name in TABLES}}


def publish_advanced_mysql(input_root, settings=None):
    """Publish once, or verify an exactly matching publication and do nothing.

    ``connect`` is injectable for offline DB-API tests. Real callers must have
    append privileges on the explicitly selected, already published schema.
    """
    settings = settings or MySQLSettings.from_env()
    connection, locked = None, False
    lock_name = "adv:" + hashlib.sha256(settings.database.encode("utf-8")).hexdigest()[:60]
    try:
        connection = connect(settings, dict_rows=True)
        acquired = _one(connection, "SELECT GET_LOCK(%s, %s) AS acquired", (lock_name, 5))
        if not acquired or acquired.get("acquired") != 1:
            raise ValueError("高级统计发布锁暂不可用，请稍后重试")
        locked = True
        snapshot = Snapshot(connection, "mysql")
        manifest, tables = load_bundle(snapshot.metadata, input_root)
        record = prepare_publication(snapshot.metadata, manifest, tables)
        fingerprint = publication_fingerprint(record)
        existing = load_from_snapshot(snapshot)
        if existing is not None:
            existing_record = prepare_publication(snapshot.metadata, *existing)
            if publication_fingerprint(existing_record) != fingerprint:
                raise ValueError("已有高级统计与输入指纹不一致，禁止覆盖；请使用新的匹配发布批次")
            return _report("ALREADY_PUBLISHED", record)

        # MySQL DDL implicitly commits. It must never be interleaved with rows
        # or the ready marker; CREATE (without IF NOT EXISTS) fails on races.
        connection.rollback()
        connection.autocommit(True)
        _create_tables(connection)
        connection.autocommit(False)
        connection.begin()
        current = Snapshot(connection, "mysql")
        if any(current.metadata.get(key) != snapshot.metadata.get(key) for key in BINDINGS):
            raise ApiError(409, "BATCH_MISMATCH", "统计库批次在发布期间发生变化，已取消高级统计写入")
        for name in TABLES:
            _insert_many(connection, "INSERT INTO " + _quote("adv_" + name)
                + " (`row_number`, payload, row_sha256, station_id, city_id, business_date) VALUES (%s, %s, %s, %s, %s, %s)",
                storage_rows(name, tables[name], manifest))
        _execute(connection, "INSERT INTO " + _quote(PUBLICATION_TABLE)
            + " (publication_id, status, dataset_id, published_batch_id, pipeline_run_id, source_manifest_sha256, fingerprint, payload)"
            + " VALUES (%s, %s, %s, %s, %s, %s, %s, %s)",
            (1, "READY", *(manifest[key] for key in BINDINGS), fingerprint, canonical_json(record)))
        verified = load_from_snapshot(current)
        if verified is None or publication_fingerprint(prepare_publication(current.metadata, *verified)) != fingerprint:
            raise ValueError("高级统计事务内读回校验失败，已取消写入")
        connection.commit()
        return _report("PUBLISHED", record)
    finally:
        if connection is not None:
            # Also ends read transactions for no-op and preflight failures.
            # Never attempt a compensating DROP, DELETE or UPDATE.
            try:
                connection.rollback()
            except Exception:
                pass
            if locked:
                try:
                    _one(connection, "SELECT RELEASE_LOCK(%s) AS released", (lock_name,))
                except Exception:
                    pass
            connection.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description="向现有匹配 MySQL 统计批次追加不可覆盖的七表高级统计副本")
    parser.add_argument("--input", required=True, type=Path, help="含 advanced_manifest.json 与 _SUCCESS 的已验证统计包目录")
    parser.add_argument("--config", type=Path, help="仅含六项 ANALYTICS_MYSQL_* 字面量配置的本地文件")
    args = parser.parse_args(argv)
    try:
        settings = settings_from_config(args.config) if args.config else MySQLSettings.from_env()
        report = publish_advanced_mysql(args.input, settings)
    except Exception:
        # Authentication and driver exceptions can contain usernames, hosts or
        # connection details. Do not serialize exceptions or settings here.
        print("高级统计发布失败：请检查本地导入配置、匹配批次和完整发布状态；未覆盖任何已有表。", file=sys.stderr)
        return 1
    print(json.dumps(report, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
