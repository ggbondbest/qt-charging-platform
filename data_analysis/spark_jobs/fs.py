"""Small metadata and checksums through Hadoop FileSystem, local or real HDFS."""

import json
from pathlib import PurePosixPath

from data_analysis.spark_jobs.pipeline import _filesystem, _qualified


def exists(spark, path):
    fs, uri = _filesystem(spark, path)
    return fs.exists(uri)


def is_file(spark, path):
    fs, uri = _filesystem(spark, path)
    return fs.isFile(uri)


def require_success(spark, path):
    if not is_file(spark, path.rstrip("/") + "/_SUCCESS") or exists(spark, path.rstrip("/") + "/_RUNNING"):
        raise ValueError("Input batch is incomplete")


def require_raw_complete(spark, path, manifest):
    if exists(spark, path.rstrip("/") + "/_INJECTION_RUNNING"):
        raise ValueError("Raw injection batch is incomplete")
    if "dirty_injection" in manifest and not is_file(spark, path.rstrip("/") + "/_INJECTION_SUCCESS"):
        raise ValueError("Raw injection batch has no completion marker")


def independent_paths(spark, *paths):
    qualified = [_qualified(spark, path) for path in paths]
    for i, left in enumerate(qualified):
        for right in qualified[i + 1:]:
            if left == right or left.startswith(right + "/") or right.startswith(left + "/"):
                raise ValueError("Input/output roots must be independent, non-nested paths")


def read_text(spark, path, max_bytes=16 * 1024 * 1024):
    fs, uri = _filesystem(spark, path)
    if fs.getFileStatus(uri).getLen() > max_bytes:
        raise ValueError("Metadata file exceeds allowed size")
    stream = fs.open(uri)
    try:
        return spark._jvm.org.apache.commons.io.IOUtils.toString(stream, "UTF-8")
    finally:
        stream.close()


def read_json(spark, path):
    return json.loads(read_text(spark, path))


def write_text(spark, path, text):
    fs, uri = _filesystem(spark, path)
    if fs.exists(uri):
        raise FileExistsError("Refusing to overwrite metadata")
    if uri.getParent() is not None:
        fs.mkdirs(uri.getParent())
    stream = fs.create(uri, False)
    try:
        stream.write(bytearray(text.encode("utf-8")))
    finally:
        stream.close()


def write_json(spark, path, value):
    write_text(spark, path, json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + "\n")


def checksum(spark, path):
    fs, uri = _filesystem(spark, path)
    stream = fs.open(uri)
    try:
        return spark._jvm.org.apache.commons.codec.digest.DigestUtils.sha256Hex(stream)
    finally:
        stream.close()


def file_size(spark, path):
    fs, uri = _filesystem(spark, path)
    return fs.getFileStatus(uri).getLen()


def glob(spark, path):
    fs, uri = _filesystem(spark, path)
    matches = fs.globStatus(uri)
    return sorted(str(item.getPath().toString()) for item in (matches or []))


def safe_relative(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError("Manifest paths must be portable relative paths")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or str(path) != value:
        raise ValueError("Manifest path escapes its dataset")
    return value


def verify_raw_files(spark, root, manifest):
    """Return False only for legacy in-memory test manifests lacking file lists."""
    declarations = manifest["tables"]
    declared_with_files = [name for name, meta in declarations.items() if "files" in meta]
    if not declared_with_files:
        return False
    if len(declared_with_files) != len(declarations):
        raise ValueError("Every manifest table must declare files")
    for table, metadata in declarations.items():
        declared = set()
        count = 0
        for item in metadata["files"]:
            relative = safe_relative(item["path"])
            if not relative.startswith("raw/" + table + "/part-") or not relative.endswith(".csv.gz"):
                raise ValueError("Unexpected raw shard path")
            full = root.rstrip("/") + "/" + relative
            qualified = _qualified(spark, full)
            if qualified in declared:
                raise ValueError("Duplicate manifest shard")
            declared.add(qualified)
            if file_size(spark, full) != item["bytes"] or checksum(spark, full) != item["sha256"]:
                raise ValueError("Raw shard checksum/size mismatch: " + relative)
            if type(item.get("rows")) is not int or item["rows"] < 0:
                raise ValueError("Invalid shard row count")
            count += item["rows"]
        actual = {_qualified(spark, path) for path in glob(spark, root.rstrip("/") + "/raw/" + table + "/part-*.csv.gz")}
        if declared != actual or count != metadata["rows"]:
            raise ValueError("Raw shard inventory/count mismatch: " + table)
    return True
