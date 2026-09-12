"""Portable table contract. SQL identifiers never come from an HTTP request."""

import math
import re
from datetime import date, datetime, timezone

from data_analysis.contracts import CONTRACT_VERSION

TABLE_KEYS = {
    "cities": ["city_id"],
    "station_snapshot": ["station_id"],
    "station_hourly_metrics": ["station_id", "recorded_at"],
    "station_metrics_daily": ["station_id", "business_date"],
    "city_daily": ["city_id", "business_date"],
    "user_activity_daily": ["station_id", "business_date", "user_id"],
    "station_cohorts_daily": ["station_id", "business_date"],
    "station_service_daily": ["station_id", "business_date"],
    "ml_features_hourly": ["station_id", "reference_time"],
    "ml_targets_hourly": ["station_id", "reference_time"],
}
SERVING_TABLES = set(TABLE_KEYS) - {"ml_targets_hourly"}
SQL_TYPES = {"string": "TEXT", "date": "TEXT", "timestamp": "TEXT",
             "integer": "INTEGER", "number": "REAL", "boolean": "INTEGER"}
SAFE_IDENTIFIER = re.compile(r"[a-z][a-z0-9_]*\Z")


def validate_identifier(value):
    if not isinstance(value, str) or not SAFE_IDENTIFIER.fullmatch(value):
        raise ValueError("Invalid table or field identifier")
    return value


def parse_utc(value):
    if not isinstance(value, str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{1,6})?Z", value):
        raise ValueError("Timestamp must be ISO 8601 UTC with trailing Z")
    parsed = datetime.fromisoformat(value[:-1] + "+00:00")
    if parsed.tzinfo is None or parsed.utcoffset().total_seconds() != 0:
        raise ValueError("Timestamp must be UTC")
    return parsed.astimezone(timezone.utc)


def convert_csv_value(value, column):
    """Validate native values; NULL is an empty CSV cell, never a zero."""
    kind = column["type"]
    if kind not in SQL_TYPES:
        raise ValueError("Unsupported column type")
    if value == "":
        if not column.get("nullable", False):
            raise ValueError("Missing required field: " + column["name"])
        return None
    if kind == "integer":
        if not re.fullmatch(r"-?(0|[1-9][0-9]*)", value):
            raise ValueError("Invalid integer: " + column["name"])
        result = int(value)
        if not -(2**63) <= result < 2**63:
            raise ValueError("Integer exceeds SQLite range")
        return result
    if kind == "number":
        result = float(value)
        if not math.isfinite(result):
            raise ValueError("Non-finite number: " + column["name"])
        return result
    if kind == "boolean":
        if value not in {"true", "false"}:
            raise ValueError("Boolean must be true or false")
        return int(value == "true")
    if kind == "date":
        if date.fromisoformat(value).isoformat() != value:
            raise ValueError("Date must be YYYY-MM-DD")
    elif kind == "timestamp":
        parse_utc(value)
    return value


def validate_table_contract(name, metadata):
    if name not in TABLE_KEYS or metadata.get("primaryKey") != TABLE_KEYS[name]:
        raise ValueError("Unexpected table or primary key: " + name)
    columns = metadata.get("columns", [])
    names = [item.get("name") for item in columns]
    if not names or len(set(names)) != len(names):
        raise ValueError("Missing or duplicate column names")
    for col in columns:
        validate_identifier(col["name"])
        if col.get("type") not in SQL_TYPES or type(col.get("nullable")) is not bool:
            raise ValueError("Invalid column type/nullability")
    if not set(TABLE_KEYS[name]).issubset(names):
        raise ValueError("Primary key column is absent")
    if type(metadata.get("rows")) is not int or metadata["rows"] < 0:
        raise ValueError("Invalid table row count")
