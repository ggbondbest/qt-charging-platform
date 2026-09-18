"""Validate the optional, immutable SQL copy of the seven Spark aggregates.

This module deliberately does not import ``advanced``: that module may use this
reader without introducing a cycle. Only a completely absent SQL extension may
fall back to a disk bundle. Every present extension is verified in full inside
the caller's read-only, batch-bound snapshot.
"""

from datetime import date
import hashlib
import json
from pathlib import PurePosixPath
import re

from .errors import ApiError


TABLES = ("station_day", "station_hour", "attempt_flow", "session_segments", "retention", "user_behavior", "service_hour")
PUBLICATION_TABLE = "adv_publication"
SQL_TABLES = tuple("adv_" + name for name in TABLES)
BINDINGS = {"datasetId": "dataset_id", "publishedBatchId": "published_batch_id",
            "pipelineRunId": "pipeline_run_id", "sourceManifestSha256": "source_manifest_sha256"}
STORE_VERSION = "1.0.0"
MAX_ROWS = 250_000
MAX_EXPANDED_BYTES = 300_000_000
MAX_MANIFEST_BYTES = 4_100_000
_HASH = re.compile(r"[0-9a-f]{64}\Z")
_RAW_IDS = {"user_id", "userid", "session_id", "sessionid", "attempt_id", "attemptid", "vehicle_id", "vehicleid"}


def _unavailable():
    return ApiError(503, "DATA_NOT_READY", "数据库中的多维统计尚未完整发布或校验失败，请核对高级统计发布清单")


def _mismatch():
    return ApiError(409, "BATCH_MISMATCH", "数据库中的多维统计与当前发布批次不一致，请发布匹配批次的分析成果")


def canonical_json(value):
    """A single, lossless UTF-8 representation used by both publisher and reader."""
    result = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)
    result.encode("utf-8")  # Reject isolated surrogate code points before SQL I/O.
    return result


def _hash(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def publication_fingerprint(record):
    return _hash(canonical_json(record))


def _identifier(value):
    if not isinstance(value, str) or not value or len(value) > 160 or "\x00" in value:
        raise ValueError("Invalid aggregate dimension")
    return value


def _business_date(value):
    if not isinstance(value, str) or len(value) != 10 or date.fromisoformat(value).isoformat() != value:
        raise ValueError("Invalid aggregate date")
    return value


def _stations(manifest):
    stations = manifest.get("stations")
    if not isinstance(stations, list) or len(stations) > MAX_ROWS:
        raise ValueError("Invalid station inventory")
    result = {}
    for station in stations:
        if not isinstance(station, dict):
            raise ValueError("Invalid station inventory")
        station_id, city_id = _identifier(station.get("station_id")), _identifier(station.get("city_id"))
        if station_id in result:
            raise ValueError("Duplicate station inventory")
        result[station_id] = city_id
    return result


def _check_manifest(metadata, manifest):
    if not isinstance(manifest, dict):
        raise ValueError("Invalid aggregate manifest")
    for key in BINDINGS:
        if not isinstance(manifest.get(key), str) or not manifest[key] or manifest[key] != metadata.get(key):
            raise _mismatch()
        if key != "sourceManifestSha256":
            _identifier(manifest[key])
    if not _HASH.fullmatch(manifest["sourceManifestSha256"]):
        raise ValueError("Invalid source hash")
    if (manifest.get("schemaVersion") != "1.0.0" or manifest.get("analysisVersion") != "2.0.0"
            or not isinstance(manifest.get("tables"), dict) or set(manifest["tables"]) != set(TABLES)):
        raise ValueError("Unsupported aggregate schema")
    invariants = manifest.get("invariants")
    if (manifest.get("engine") != "PySpark" or manifest.get("rawFactsCollected") is not False
            or manifest.get("referenceAggregatesUsedAsInput") is not False
            or manifest.get("authenticatedCleanInventoryVerified") is not True
            or not isinstance(invariants, dict) or not invariants or any(value is not True for value in invariants.values())):
        raise ValueError("Missing aggregate provenance")
    for item in manifest["tables"].values():
        if (not isinstance(item, dict) or type(item.get("rows")) is not int or not 0 <= item["rows"] <= MAX_ROWS
                or not isinstance(item.get("sha256"), str) or not _HASH.fullmatch(item["sha256"])):
            raise ValueError("Invalid aggregate declaration")
        filename = item.get("file")
        if (not isinstance(filename, str) or "\\" in filename or PurePosixPath(filename).name != filename
                or not filename.endswith(".gz")):
            raise ValueError("Invalid aggregate filename")
    _stations(manifest)
    if len(canonical_json(manifest).encode("utf-8")) > MAX_MANIFEST_BYTES:
        raise ValueError("Aggregate manifest is too large")


def _aggregate_only(value):
    """The SQL extension is not a raw-user or future-ML-label export channel."""
    if isinstance(value, dict):
        for key, nested in value.items():
            if not isinstance(key, str):
                raise ValueError("Invalid aggregate field")
            normalized = key.casefold()
            if normalized in _RAW_IDS or normalized.startswith(("future_", "target_", "label_")):
                raise ValueError("Raw facts and future labels are not aggregate data")
            _aggregate_only(nested)
    elif isinstance(value, list):
        for nested in value:
            _aggregate_only(nested)


def _indexes(name, row, stations):
    if name == "retention":
        scope = row.get("scope_type")
        scope_id = _identifier(row.get("scope_id"))
        business_date = _business_date(row.get("cohort_month"))
        if scope == "ALL" and scope_id == "ALL":
            return None, None, business_date
        if scope == "CITY" and scope_id in stations.values():
            return None, scope_id, business_date
        if scope == "STATION" and scope_id in stations:
            return scope_id, stations[scope_id], business_date
        raise ValueError("Invalid retention scope")
    station_id = _identifier(row.get("station_id"))
    city_id = _identifier(row.get("city_id"))
    if stations.get(station_id) != city_id:
        raise ValueError("Aggregate dimension does not match station inventory")
    return station_id, city_id, _business_date(row.get("business_date"))


def storage_rows(name, rows, manifest):
    """Yield positional SQL rows without rounding numbers or dropping fields."""
    if name not in TABLES or not isinstance(rows, list) or len(rows) > MAX_ROWS:
        raise ValueError("Invalid aggregate table")
    stations = _stations(manifest)
    total_bytes = 2
    for index, row in enumerate(rows, 1):
        if not isinstance(row, dict):
            raise ValueError("Invalid aggregate row")
        _aggregate_only(row)
        payload = canonical_json(row)
        total_bytes += len(payload.encode("utf-8")) + (index > 1)
        if total_bytes > MAX_EXPANDED_BYTES:
            raise ValueError("Aggregate exceeds serving bounds")
        yield index, payload, _hash(payload), *_indexes(name, row, stations)


def prepare_publication(metadata, manifest, tables):
    """Validate an already authenticated disk bundle and build its SQL manifest."""
    _check_manifest(metadata, manifest)
    if not isinstance(tables, dict) or set(tables) != set(TABLES):
        raise ValueError("Invalid aggregate tables")
    summaries = {}
    for name in TABLES:
        digest = hashlib.sha256(b"[")
        count = 0
        for count, payload, *_ in storage_rows(name, tables[name], manifest):
            if count > 1:
                digest.update(b",")
            digest.update(payload.encode("utf-8"))
        digest.update(b"]")
        if count != manifest["tables"][name]["rows"]:
            raise ValueError("Aggregate row count does not match source manifest")
        summaries[name] = {"rows": count, "sha256": digest.hexdigest()}
    return {"storeVersion": STORE_VERSION, "manifest": manifest, "tables": summaries}


def _publication_present(snapshot):
    if snapshot.backend == "mysql":
        inventory = snapshot.rows("SELECT TABLE_NAME AS name, TABLE_TYPE AS type FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()")
        table_type = "BASE TABLE"
    elif snapshot.backend == "sqlite":
        inventory = snapshot.rows("SELECT name, type FROM sqlite_master WHERE type IN ('table', 'view')")
        table_type = "table"
    else:
        raise ValueError("Unsupported snapshot backend")
    advanced = [row for row in inventory if isinstance(row.get("name"), str) and row["name"].casefold().startswith("adv_")]
    if not advanced:
        return False
    if (len(advanced) != len(TABLES) + 1 or {row["name"] for row in advanced} != {*SQL_TABLES, PUBLICATION_TABLE}
            or any(row.get("type") != table_type for row in advanced)):
        raise ValueError("Incomplete or conflicting aggregate publication")
    return True


def _json_payload(payload, *, maximum):
    if not isinstance(payload, str) or len(payload.encode("utf-8")) > maximum:
        raise ValueError("Invalid stored payload")
    value = json.loads(payload)
    if canonical_json(value) != payload:
        raise ValueError("Stored payload is not canonical JSON")
    return value


def load_from_snapshot(snapshot):
    """Return verified (original manifest, all rows), or None only if absent.

    All hashes are checked before caching a successful result on this Snapshot
    instance. The caller owns and must retain its read-only SQL transaction and
    treat the returned aggregate rows as read-only. A new request/Snapshot never
    reuses this cache; absent and invalid publications are never cached.
    """
    try:
        connection = getattr(snapshot, "connection", None)
        binding = tuple(snapshot.metadata.get(key) for key in BINDINGS)
        cached = getattr(snapshot, "_advanced_store_verified", None)
        if (connection is not None and cached is not None and cached[0] is connection
                and cached[1] == snapshot.backend and cached[2] == binding):
            return cached[3]
        if not _publication_present(snapshot):
            return None
        markers = snapshot.rows("SELECT publication_id, status, dataset_id, published_batch_id, pipeline_run_id, source_manifest_sha256, fingerprint, payload FROM adv_publication LIMIT 2")
        if len(markers) != 1 or markers[0]["publication_id"] != 1 or markers[0]["status"] != "READY":
            raise ValueError("Aggregate publication is not ready")
        marker = markers[0]
        record = _json_payload(marker["payload"], maximum=MAX_MANIFEST_BYTES)
        if (not isinstance(record, dict) or set(record) != {"storeVersion", "manifest", "tables"}
                or record["storeVersion"] != STORE_VERSION
                or not isinstance(marker["fingerprint"], str) or not _HASH.fullmatch(marker["fingerprint"])
                or publication_fingerprint(record) != marker["fingerprint"]):
            raise ValueError("Invalid aggregate publication fingerprint")
        manifest = record["manifest"]
        _check_manifest(snapshot.metadata, manifest)
        for key, column in BINDINGS.items():
            if marker[column] != manifest[key]:
                raise _mismatch()
        declarations = record["tables"]
        if not isinstance(declarations, dict) or set(declarations) != set(TABLES):
            raise ValueError("Incomplete aggregate table declarations")
        tables = {}
        for name in TABLES:
            declaration = declarations[name]
            if (not isinstance(declaration, dict) or set(declaration) != {"rows", "sha256"}
                    or type(declaration["rows"]) is not int or declaration["rows"] != manifest["tables"][name]["rows"]
                    or not isinstance(declaration["sha256"], str) or not _HASH.fullmatch(declaration["sha256"])):
                raise ValueError("Invalid aggregate table declaration")
            # Only module-owned, allow-listed table names enter SQL text.
            stored = snapshot.rows("SELECT `row_number`, payload, row_sha256, station_id, city_id, business_date FROM adv_" + name
                                   + " ORDER BY `row_number` LIMIT " + str(MAX_ROWS + 1))
            if len(stored) != declaration["rows"]:
                raise ValueError("Stored aggregate row count mismatch")
            rows = [_json_payload(row["payload"], maximum=MAX_EXPANDED_BYTES) for row in stored]
            digest = hashlib.sha256(b"[")
            for expected, actual in zip(storage_rows(name, rows, manifest), stored):
                index, payload, row_hash, station, city, day = expected
                actual_day = actual["business_date"]
                if type(actual_day) is date:  # PyMySQL DATE converter; SQLite uses text.
                    actual_day = actual_day.isoformat()
                if (type(actual["row_number"]) is not int or actual["row_number"] != index
                        or actual["row_sha256"] != row_hash or actual["station_id"] != station
                        or actual["city_id"] != city or actual_day != day):
                    raise ValueError("Stored aggregate row or query index mismatch")
                if index > 1:
                    digest.update(b",")
                digest.update(payload.encode("utf-8"))
            digest.update(b"]")
            if digest.hexdigest() != declaration["sha256"]:
                raise ValueError("Stored aggregate table hash mismatch")
            tables[name] = rows
        result = manifest, tables
        if connection is not None:
            snapshot._advanced_store_verified = connection, snapshot.backend, binding, result
        return result
    except ApiError:
        raise
    except Exception:
        # SQL errors, malformed JSON and driver internals must not disclose
        # schema paths, credential strings or SQL in API responses.
        raise _unavailable() from None
