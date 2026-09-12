"""One read-only SQLite snapshot connection per request, never raw-data scans."""

from contextlib import contextmanager
from datetime import date, datetime
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


def read_metadata(connection):
    try:
        result = {row["key"]: json.loads(row["value"])
                  for row in connection.execute("SELECT key, value FROM __metadata")}
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
    def __init__(self, connection):
        self.connection = connection
        self.metadata = read_metadata(connection)

    def rows(self, sql, parameters=()):
        return [dict(row) for row in self.connection.execute(sql, parameters)]

    def one(self, sql, parameters=()):
        row = self.connection.execute(sql, parameters).fetchone()
        return dict(row) if row is not None else None


@contextmanager
def open_snapshot(database_path):
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
