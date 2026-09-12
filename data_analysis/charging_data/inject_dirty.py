"""Create a reproducible dirty-data exercise without changing its source batch.

Example (run from the repository root)::

    python -m data_analysis.charging_data.inject_dirty \
        --input data_analysis/datasets/charging_sample_7d_v2 \
        --output data_analysis/outputs/dirty_demo_1 --seed 42 --rate 0.05

Only charging_sessions is modified. Existing corruption is retained; rate is the
probability per canonical session not already mentioned in corruption_log, not
the percentage of all raw rows. Each newly selected session receives one of the
existing five operations. Valid canonical business records remain recoverable.
The source manifest, plan, old/new counts, hashes, and a fresh independent
validation report are retained. Interrupted outputs must not be reused.
"""

from __future__ import annotations

import argparse
from collections import Counter
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path
import random
import re
import shutil

from .io import DatasetWriter, write_json
from .schema import SCHEMA_VERSION, SUMMARY_TABLES, TABLES
from .validate import validate_dataset


INJECTOR_VERSION = "1.0.0"
OPERATIONS = ("DUPLICATE", "NEGATIVE_FEE", "UNKNOWN_STATION", "MISSING_ID", "STATUS_FORMAT")
ACTIONS = {"DUPLICATE": "REMOVE_DUPLICATE", "STATUS_FORMAT": "NORMALIZE",
           "NEGATIVE_FEE": "QUARANTINE", "UNKNOWN_STATION": "QUARANTINE",
           "MISSING_ID": "QUARANTINE"}


def _digest(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _copy_file(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        shutil.copyfileobj(incoming, outgoing)


def _records(root, table):
    for part in sorted((root / "raw" / table).glob("part-*.csv.gz")):
        with gzip.open(part, "rt", encoding="utf-8", newline="") as stream:
            for number, row in enumerate(csv.DictReader(stream), 1):
                yield part.relative_to(root).as_posix(), number, row


def _verify_inventory(root, manifest):
    """Check declared path ownership and bytes before trusting any source rows."""
    if (not isinstance(manifest, dict) or manifest.get("schema_version") != SCHEMA_VERSION or
            not isinstance(manifest.get("tables"), dict) or set(manifest["tables"]) != set(TABLES)):
        raise ValueError("Dirty injection requires the current complete schema and all raw tables")
    if (not isinstance(manifest.get("reference_aggregates"), dict) or
            set(manifest["reference_aggregates"]) != set(SUMMARY_TABLES)):
        raise ValueError("Dirty injection requires both independent reference aggregate tables")
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError("Dataset symlinks are not accepted")
    if (root / "_INJECTION_RUNNING").exists():
        raise ValueError("Input is an incomplete dirty-injection batch")
    if "dirty_injection" in manifest:
        if not (root / "_INJECTION_SUCCESS").is_file():
            raise ValueError("Derived input has no injection success marker")
        metadata = manifest["dirty_injection"]["plan"]
        plan_path = root / "injection_plan.json"
        if (metadata["path"] != "injection_plan.json" or not plan_path.is_file() or
                _digest(plan_path) != metadata["sha256"] or plan_path.stat().st_size != metadata["bytes"]):
            raise ValueError("Source injection plan checksum/size mismatch")
        parent_path = root / "provenance" / "parent_manifest.json"
        if not parent_path.is_file() or _digest(parent_path) != manifest["parent_manifest_sha256"]:
            raise ValueError("Source parent-manifest checksum mismatch")
    for section, prefix in [("tables", "raw"), ("reference_aggregates", "reference_aggregates")]:
        declared = set()
        for table, metadata in manifest[section].items():
            if not isinstance(metadata, dict) or not isinstance(metadata.get("files"), list) or not metadata["files"]:
                raise ValueError("Manifest contains missing file metadata: " + table)
            for entry in metadata["files"]:
                if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
                    raise ValueError("Manifest contains invalid source file metadata")
                relative = entry["path"]
                path = root / relative
                if (path.parent != root / prefix / table or
                        not path.name.startswith("part-") or not path.name.endswith(".csv.gz") or
                        not path.resolve().is_relative_to(root) or relative in declared):
                    raise ValueError("Manifest contains an invalid or repeated source path")
                declared.add(relative)
                if not path.is_file() or _digest(path) != entry["sha256"] or path.stat().st_size != entry["bytes"]:
                    raise ValueError("Source file checksum/size mismatch: " + relative)
        actual = {path.relative_to(root).as_posix() for path in (root / prefix).rglob("*.csv.gz")}
        if actual != declared:
            raise ValueError("Source file inventory does not match the manifest: " + prefix)


def _write_preview(root, table):
    path = root / "preview" / (table + ".csv")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=TABLES[table], lineterminator="\n")
        writer.writeheader()
        for index, (_, _, row) in enumerate(_records(root, table)):
            if index == 12:
                break
            writer.writerow(row)


def inject_dataset(input_root, output_root, seed, rate, dataset_id=None):
    """Derive a new verified batch; identical input/parameters reproduce bytes.

    ``rate`` is sampled independently over previously untouched canonical
    sessions. The result includes old and new corruption counts. Existing output
    directories (including empty ones), symlinks, and overlapping trees fail
    before writing. The source is only read, never cleaned or rewritten in place.
    """
    if type(seed) is not int or not 0 <= seed <= 2**32 - 1:
        raise ValueError("seed must be an integer in [0, 2**32-1]")
    if type(rate) not in (int, float) or not math.isfinite(rate) or not 0 <= rate <= 1:
        raise ValueError("rate must be a finite number in [0, 1]")
    rate = float(rate)
    source_arg, destination_arg = Path(input_root), Path(output_root)
    if source_arg.is_symlink() or destination_arg.is_symlink():
        raise ValueError("Input/output roots must not be symlinks")
    source, destination = source_arg.resolve(), destination_arg.resolve()
    if not source.is_dir():
        raise ValueError("Input must be an existing dataset directory")
    if source == destination or source.is_relative_to(destination) or destination.is_relative_to(source):
        raise ValueError("Input and output must be independent, non-nested directories")
    if destination.exists():
        raise FileExistsError("Output already exists; choose a new dataset directory")
    source_manifest_path = source / "manifest.json"
    source_manifest_bytes = source_manifest_path.read_bytes()
    parent_digest = hashlib.sha256(source_manifest_bytes).hexdigest()
    manifest = json.loads(source_manifest_bytes)
    _verify_inventory(source, manifest)
    input_report = validate_dataset(source)
    if not input_report["valid"]:
        raise ValueError("Input dataset failed independent validation: " + "; ".join(input_report["errors"][:3]))
    parameters = {"injector_version": INJECTOR_VERSION, "parent_manifest_sha256": parent_digest,
                  "seed": seed, "rate": rate}
    parameter_digest = hashlib.sha256(json.dumps(parameters, sort_keys=True).encode("utf-8")).hexdigest()
    parent_id = manifest["dataset_id"]
    if dataset_id is None:
        dataset_id = f"{parent_id[:40]}_dirty_{seed}_{parameter_digest[:8]}"
    if not isinstance(dataset_id, str) or not re.fullmatch(r"[a-zA-Z0-9_-]{1,80}", dataset_id):
        raise ValueError("dataset_id must be a safe ASCII identifier of at most 80 characters")
    if dataset_id == parent_id:
        raise ValueError("A derived batch must have a different dataset_id from its parent")

    prior_log = [row for _, _, row in _records(source, "corruption_log")]
    touched = {row["record_id"] for row in prior_log}
    existing_corruption_ids = {row["corruption_id"] for row in prior_log}
    station_ids = {row["station_id"] for _, _, row in _records(source, "stations")}
    canonical_ids = set()
    operations = []
    eligible = 0
    rng = random.Random(seed)
    for relative, number, row in _records(source, "charging_sessions"):
        identifier = row["session_id"]
        if not identifier or row["station_id"] not in station_ids or int(row["total_fee_cents"]) < 0:
            continue
        if identifier in canonical_ids:
            continue
        canonical_ids.add(identifier)
        if identifier in touched:
            continue
        eligible += 1
        if rng.random() >= rate:
            continue
        kind = rng.choice(OPERATIONS)
        counter = len(operations) + 1
        corruption_id = f"COR-INJECT-{parameter_digest[:12]}-{counter:08d}"
        if corruption_id in existing_corruption_ids:
            raise ValueError("Injection corruption_id unexpectedly collides with its input")
        operations.append({"corruption_id": corruption_id, "table_name": "charging_sessions",
            "record_id": identifier, "corruption_type": kind, "expected_action": ACTIONS[kind],
            "source_path": relative, "source_data_row": number})
    if len(canonical_ids) != manifest["canonical_session_count"]:
        raise ValueError("Canonical selection differs from validated source session count")
    additions = Counter(operation["corruption_type"] for operation in operations)
    previous = Counter(manifest["corruptions"])
    totals = previous + additions
    plan = dict(parameters, dataset_id=dataset_id, parent_dataset_id=parent_id,
        rate_denominator="canonical sessions not already referenced by input corruption_log",
        canonical_sessions=len(canonical_ids), previously_touched_sessions=len(touched),
        eligible_sessions=eligible, selected_sessions=len(operations),
        input_corruptions=dict(previous), added_corruptions=dict(additions),
        total_corruptions=dict(totals), operations=operations)
    selected = {(operation["source_path"], operation["source_data_row"]): operation for operation in operations}

    destination.mkdir(parents=True, exist_ok=False)
    marker = destination / "_INJECTION_RUNNING"
    marker.touch(exist_ok=False)
    # Copy only the declared dataset, never arbitrary source outputs or stale
    # validation/behavior reports carrying the parent's dataset identity.
    updated = {"charging_sessions", "corruption_log"}
    for section in ["tables", "reference_aggregates"]:
        for table, metadata in manifest[section].items():
            if section == "tables" and table in updated:
                continue
            for entry in metadata["files"]:
                _copy_file(source / entry["path"], destination / entry["path"])
    writer = DatasetWriter(destination, {table: TABLES[table] for table in sorted(updated)})
    try:
        for relative, number, row in _records(source, "charging_sessions"):
            partition = Path(relative).name[len("part-"):-len(".csv.gz")]
            operation = selected.get((relative, number))
            original = row
            if operation and operation["corruption_type"] == "STATUS_FORMAT":
                row = dict(row, status=" " + row["status"].strip().lower() + " ")
            writer.write("charging_sessions", row, partition)
            if operation and operation["corruption_type"] != "STATUS_FORMAT":
                extra = dict(original)
                kind = operation["corruption_type"]
                if kind != "DUPLICATE":
                    extra["session_id"] = "BAD-" + operation["corruption_id"]
                if kind == "NEGATIVE_FEE":
                    extra["total_fee_cents"] = -100
                elif kind == "MISSING_ID":
                    extra["session_id"] = ""
                elif kind == "UNKNOWN_STATION":
                    extra["station_id"] = "ST-INJECT-UNKNOWN"
                    if extra["station_id"] in station_ids:
                        raise ValueError("Unknown-station sentinel collides with a real station")
                writer.write("charging_sessions", extra, partition)
        for row in prior_log:
            writer.write("corruption_log", row)
        for operation in operations:
            writer.write("corruption_log", {key: operation[key] for key in TABLES["corruption_log"]})
        manifest["tables"].update(writer.finish(preview=True))
    finally:
        for stream, binary, _ in writer.handles.values():
            stream.close()
            binary.close()
    for table in TABLES:
        if table not in updated:
            _write_preview(destination, table)
    write_json(destination / "schema.json", {"raw": TABLES, "reference_aggregates": SUMMARY_TABLES})
    parent_copy = destination / "provenance" / "parent_manifest.json"
    parent_copy.parent.mkdir(parents=True)
    with parent_copy.open("xb") as stream:
        stream.write(source_manifest_bytes)
    plan_path = destination / "injection_plan.json"
    write_json(plan_path, plan)
    manifest.update(dataset_id=dataset_id, parent_dataset_id=parent_id,
        parent_manifest_sha256=parent_digest, corruptions=dict(totals),
        dirty_injection={key: value for key, value in plan.items() if key != "operations"})
    manifest["config"] = dict(manifest["config"], dataset_id=dataset_id)
    manifest["dirty_injection"]["plan"] = {"path": "injection_plan.json", "sha256": _digest(plan_path),
                                          "bytes": plan_path.stat().st_size}
    manifest.setdefault("definitions", {})["dirty_injection"] = (
        "Derived batch; config.dirty_rate describes the original generator only. "
        "dirty_injection.rate is applied once to previously untouched canonical sessions. "
        "Old corruption remains; reference aggregates retain unchanged canonical business results.")
    write_json(destination / "manifest.json", manifest)
    output_report = validate_dataset(destination)
    write_json(destination / "validation_report.json", output_report)
    if not output_report["valid"]:
        raise ValueError("Derived dataset failed independent validation: " + "; ".join(output_report["errors"][:3]))
    # The original manifest and all copied source files were checked before use;
    # callers can additionally compare the unchanged source using its manifest.
    (destination / "_INJECTION_SUCCESS").touch(exist_ok=False)
    marker.unlink()
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help="Existing complete, independently valid dataset")
    parser.add_argument("--output", required=True, type=Path, help="New independent output directory")
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--rate", required=True, type=float, help="Probability per previously untouched canonical session, [0,1]")
    parser.add_argument("--dataset-id", help="Optional distinct batch ID; deterministic ID is generated by default")
    args = parser.parse_args(argv)
    try:
        manifest = inject_dataset(args.input, args.output, args.seed, args.rate, args.dataset_id)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(1, "Dirty injection failed: " + str(exc) + "\n")
    print(json.dumps({"dataset_id": manifest["dataset_id"],
        "canonical_session_count": manifest["canonical_session_count"],
        "dirty_injection": manifest["dirty_injection"]}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
