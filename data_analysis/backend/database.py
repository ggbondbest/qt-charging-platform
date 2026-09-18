"""One read-only, batch-bound MySQL transaction per request.

An explicitly supplied SQLite path remains an offline verification adapter;
production configuration never silently falls back to an SQLite file.
"""

from contextlib import contextmanager
from datetime import date, datetime
from decimal import Decimal
import json
from pathlib import Path
import re
import sqlite3

from data_analysis.contracts import CONTRACT_VERSION

from .errors import ApiError

SCHEMA_VERSION = CONTRACT_VERSION
META_FIELDS = ("datasetId", "publishedBatchId", "pipelineRunId", "schemaVersion", "generatedAt")


def business_date(value):
    if not isinstance(value, str) or len(value) != 10:
        raise ValueError("Expected YYYY-MM-DD")
    parsed = date.fromisoformat(value)
    if parsed.isoformat() != value:
        raise ValueError("Expected YYYY-MM-DD")
    return parsed


def mysql_parameters(sql):
    """Translate internal qmark SQL, never interpolating caller values.

    Literal/question-mark and comment content is preserved. Percent escaping
    is required by the DB-API driver's format protocol, including literals.
    Query text is module-owned; identifiers are separately allow-listed.
    """
    result, quote, comment, index = [], None, None, 0
    while index < len(sql):
        char = sql[index]
        pair = sql[index:index + 2]
        if comment == "line":
            if char == "\n":
                comment = None
        elif comment == "block":
            if pair == "*/":
                result.append(pair)
                index += 2
                comment = None
                continue
        elif quote:
            if char == "\\" and index + 1 < len(sql):
                result.append(sql[index:index + 2].replace("%", "%%"))
                index += 2
                continue
            if char == quote:
                if index + 1 < len(sql) and sql[index + 1] == quote:
                    result.append(char * 2)
                    index += 2
                    continue
                quote = None
        elif pair == "/*":
            comment = "block"
        elif pair == "--" or char == "#":
            comment = "line"
        elif char in "'\"`":
            quote = char
        elif char == "?":
            result.append("%s")
            index += 1
            continue
        result.append("%%" if char == "%" else char)
        index += 1
    return "".join(result)


def _value(value):
    # SUM(BIGINT) is returned as scale-zero Decimal by MySQL. Avoid a float
    # round trip for cents, Wh and counts, even above the IEEE-754 safe range.
    if isinstance(value, Decimal):
        return int(value) if value.as_tuple().exponent >= 0 else float(value)
    return value


def _rows(connection, sql, parameters, backend):
    if backend == "mysql":
        with connection.cursor() as cursor:
            cursor.execute(mysql_parameters(sql), tuple(parameters))
            return [{key: _value(value) for key, value in row.items()} for row in cursor.fetchall()]
    return [dict(row) for row in connection.execute(sql, parameters)]


def read_metadata(connection, backend="sqlite"):
    try:
        result = {row["key"]: json.loads(row["value"])
                  for row in _rows(connection, "SELECT `key`, `value` FROM __metadata", (), backend)}
        if backend == "mysql" and (result.get("storageBackend") != "mysql"
                or result.get("publicationStatus") != "PUBLISHED"):
            raise ValueError("Publication has not completed")
        for name in META_FIELDS:
            if not isinstance(result[name], str) or not result[name] or len(result[name]) > 160:
                raise ValueError("Invalid publication metadata")
        if result["schemaVersion"] != SCHEMA_VERSION or result["source"] != "SIMULATED":
            raise ValueError("Unsupported data contract")
        if business_date(result["startDate"]) >= business_date(result["endDate"]):
            raise ValueError("Invalid dataset range")
        if not result["generatedAt"].endswith("Z"):
            raise ValueError("generatedAt must be UTC")
        datetime.fromisoformat(result["generatedAt"][:-1]+"+00:00")
        if not isinstance(result["qualityReport"], dict) or not isinstance(result["tables"], dict):
            raise ValueError("Invalid publication metadata")
        if not isinstance(result["sourceManifestSha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", result["sourceManifestSha256"]):
            raise ValueError("Invalid source manifest hash")
        return result
    except (sqlite3.Error, ValueError, KeyError, TypeError) as exc:
        raise ApiError(503, "DATA_NOT_READY", "统计数据尚未完成发布或版本不兼容") from exc


class Snapshot:
    def __init__(self, connection, backend="sqlite"):
        self.connection = connection
        self.backend = backend
        self.metadata = read_metadata(connection, backend)

    def rows(self, sql, parameters=()):
        return _rows(self.connection, sql, parameters, self.backend)

    def one(self, sql, parameters=()):
        rows = self.rows(sql, parameters)
        return rows[0] if rows else None


@contextmanager
def open_snapshot(database_path=None, *, mysql_settings=None):
    if database_path is not None and mysql_settings is not None:
        raise ValueError("Choose one snapshot backend")
    if database_path is None:
        with _open_mysql_snapshot(mysql_settings) as snapshot:
            yield snapshot
        return
    if not database_path:
        raise ApiError(503, "DATA_NOT_READY", "尚未配置已发布的统计数据")
    connection = None
    try:
        path = Path(database_path).expanduser().resolve()
        if not path.is_file():
            raise ApiError(503, "DATA_NOT_READY", "统计数据尚未完成发布")
        connection = sqlite3.connect(path.as_uri()+"?mode=ro", uri=True, timeout=2,
                                     check_same_thread=False)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA query_only = ON")
        connection.execute("PRAGMA trusted_schema = OFF")
        connection.execute("BEGIN")
        yield Snapshot(connection)
    except (OSError, sqlite3.Error) as exc:
        raise ApiError(503, "DATA_UNAVAILABLE", "统计数据暂时不可用，请稍后重试") from exc
    finally:
        if connection is not None:
            connection.close()


@contextmanager
def _open_mysql_snapshot(settings):
    if settings is None:
        raise ApiError(503, "DATA_NOT_READY", "尚未配置已发布的 MySQL 统计批次")
    try:
        import pymysql
        from data_analysis.mysql_support import connect
    except ImportError as exc:
        raise ApiError(503, "DATA_NOT_READY", "统计数据库组件尚未就绪") from exc
    connection = None
    try:
        # No USE, dynamic schema selector, or registry lookup: the selected
        # immutable publication schema stays fixed for this connection.
        try:
            connection = connect(settings, dict_rows=True)
        except ValueError as exc:
            raise ApiError(503, "DATA_NOT_READY", "统计数据库配置或版本尚未就绪") from exc
        with connection.cursor() as cursor:
            cursor.execute("SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ")
            cursor.execute("SET SESSION TRANSACTION READ ONLY")
            cursor.execute("START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY")
        yield Snapshot(connection, "mysql")
    except (OSError, pymysql.Error) as exc:
        # Deliberately exclude connection DSNs, credentials, SQL and driver
        # messages from the public envelope. The request ID is safe to share.
        raise ApiError(503, "DATA_UNAVAILABLE", "统计数据暂时不可用，请稍后重试") from exc
    finally:
        if connection is not None:
            # Always discard the read-only transaction; never commit a query.
            try:
                connection.rollback()
            except (OSError, pymysql.Error):
                pass
            connection.close()
