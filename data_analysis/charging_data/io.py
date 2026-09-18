"""Streaming, reproducible gzip CSV writing without third-party dependencies."""

import csv
import gzip
import hashlib
import io
import json
from pathlib import Path


class DatasetWriter:
    def __init__(self, root, schemas, prefix="raw"):
        self.root = Path(root)
        self.schemas = schemas
        self.prefix = prefix
        self.handles = {}
        self.files = {}
        self.samples = {table: [] for table in schemas}

    def write(self, table, row, partition="static"):
        if set(row) != set(self.schemas[table]):
            raise ValueError(f"Schema mismatch {table}: {set(row) ^ set(self.schemas[table])}")
        key = table, partition
        if key not in self.handles:
            path = self.root / self.prefix / table / f"part-{partition}.csv.gz"
            path.parent.mkdir(parents=True, exist_ok=True)
            binary = path.open("xb")
            compressed = gzip.GzipFile(filename="", mode="wb", fileobj=binary, mtime=0)
            stream = io.TextIOWrapper(compressed, encoding="utf-8", newline="")
            writer = csv.DictWriter(stream, self.schemas[table], lineterminator="\n")
            writer.writeheader()
            self.handles[key] = stream, binary, writer
            self.files[key] = {"path": path.relative_to(self.root).as_posix(), "rows": 0}
        self.handles[key][2].writerow(row)
        self.files[key]["rows"] += 1
        if len(self.samples[table]) < 12:
            self.samples[table].append(dict(row))

    def finish(self, preview=False):
        # Even tables with zero events have a header and a discoverable schema.
        for table, fields in self.schemas.items():
            if not any(key[0] == table for key in self.files):
                path = self.root / self.prefix / table / "part-empty.csv.gz"
                path.parent.mkdir(parents=True, exist_ok=True)
                with path.open("xb") as binary:
                    with gzip.GzipFile(filename="", mode="wb", fileobj=binary, mtime=0) as gz:
                        gz.write((",".join(fields) + "\n").encode())
                self.files[(table, "empty")] = {
                    "path": path.relative_to(self.root).as_posix(), "rows": 0}
        for stream, binary, _ in self.handles.values():
            stream.close()
            binary.close()
        result = {table: {"rows": 0, "files": []} for table in self.schemas}
        for (table, _), entry in sorted(self.files.items()):
            path = self.root / entry["path"]
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            entry.update(sha256=digest.hexdigest(), bytes=path.stat().st_size)
            result[table]["rows"] += entry["rows"]
            result[table]["files"].append(entry)
        if preview:
            for table, rows in self.samples.items():
                path = self.root / "preview" / f"{table}.csv"
                path.parent.mkdir(parents=True, exist_ok=True)
                with path.open("x", encoding="utf-8-sig", newline="") as stream:
                    writer = csv.DictWriter(stream, self.schemas[table], lineterminator="\n")
                    writer.writeheader()
                    writer.writerows(rows)
        return result


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


def read_table(root, table, prefix="raw"):
    for path in sorted((Path(root) / prefix / table).glob("*.csv.gz")):
        with gzip.open(path, "rt", encoding="utf-8-sig", newline="") as stream:
            yield from csv.DictReader(stream)
