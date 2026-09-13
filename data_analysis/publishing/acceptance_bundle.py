"""Package a completed local acceptance run without Spark or data recomputation.

The existing ten-table serving contract remains unchanged. Additional cleaned
Parquet, quarantine records and audit/analysis evidence have their own manifest.
Parquet row counts below are declarations from the Spark audit, not a recount
by this byte-copy tool; use Spark for an independent row/schema verification.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile

from data_analysis.charging_data.schema import TABLES
from data_analysis.publishing.bundle import bundle_export
from data_analysis.publishing.publish import _file, _hash, _json, inspect_export
from data_analysis.scripts.run_acceptance import validate_evidence


VERSION = "acceptance-bundle-1.0.0"
IDENTITY = ("datasetId", "pipelineRunId", "publishedBatchId", "sourceManifestSha256")
MANAGED = ("clean", "rejected", "reports", "analysis")


def _canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _write(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


def _root(value):
    if "://" in str(value):
        raise ValueError("Acceptance bundles use local directories")
    path = Path(os.path.abspath(value))
    if any(part.is_symlink() for part in (path, *path.parents)):
        raise ValueError("Root/parent symlinks are forbidden")
    return path.resolve()


def _complete(root):
    _file(root, "_SUCCESS")
    if (root / "_RUNNING").exists() or (root / "_RUNNING").is_symlink():
        raise ValueError("Incomplete acceptance output: _RUNNING is present")


def _ignored(path):
    return path.name == ".DS_Store" or path.name.endswith(".crc")


def _inventory(root, folder):
    base = root / folder
    if not base.is_dir() or base.is_symlink():
        raise ValueError("Missing/symlink attachment directory: " + folder)
    result = {}
    for path in sorted(base.rglob("*")):
        if _ignored(path):
            continue
        if path.is_symlink():
            raise ValueError("Attachment symlinks are forbidden")
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            result[relative] = _file(root, relative)
    return result


def _entry(relative, path):
    return {"path": relative, "bytes": path.stat().st_size, "sha256": _hash(path)}


def _checked_files(root, entries):
    if not isinstance(entries, list) or not entries:
        raise ValueError("Missing attachment file inventory")
    result = {}
    for item in entries:
        path = _file(root, item["path"])
        if item["path"] in result:
            raise ValueError("Duplicate attachment path")
        if (type(item.get("bytes")) is not int or item["bytes"] < 0 or
                path.stat().st_size != item["bytes"] or _hash(path) != item.get("sha256")):
            raise ValueError("Attachment checksum/size mismatch: " + item["path"])
        result[item["path"]] = path
    return result


def _analysis(root, quality):
    _complete(root)
    manifest = _json(_file(root, "analysis_manifest.json"))
    for key, source_key in (("datasetId", "dataset_id"), ("pipelineRunId", "pipeline_run_id"),
                            ("sourceManifestSha256", "source_manifest_sha256")):
        if manifest.get(key) != quality.get(source_key):
            raise ValueError("Analysis belongs to another cleaning batch")
    analyses = manifest.get("analyses", {})
    dimensions = {dim for item in analyses.values() for dim in item["dimensions"]}
    comparisons = {tuple(item["dimensions"]) for item in analyses.values() if len(set(item["dimensions"])) >= 2}
    if (len(dimensions) < 8 or len(comparisons) < 2 or
            manifest.get("semanticDimensionCount") != len(dimensions) or
            manifest.get("comparisonCount") != len(comparisons)):
        raise ValueError("Invalid analysis dimension/comparison declaration")
    files = {name: _file(root, name) for name in ("analysis_manifest.json", "preview.json", "_SUCCESS")}
    declared = {}
    for name, item in analyses.items():
        if type(item.get("rows")) is not int or item["rows"] < 0:
            raise ValueError("Invalid analysis row declaration")
        for relative, path in _checked_files(root, item["files"]).items():
            if (path.parent != root / "csv" / name or not path.name.startswith("part-") or
                    not path.name.endswith(".csv.gz") or relative in declared):
                raise ValueError("Analysis shard does not belong to declared table")
            declared[relative] = path
    actual = {name: path for name, path in _inventory(root, "csv").items() if path.name != "_SUCCESS"}
    if set(actual) != set(declared):
        raise ValueError("Analysis CSV inventory differs from manifest")
    preview = _json(files["preview.json"])
    if set(preview) != set(analyses) or any(not isinstance(rows, list) or len(rows) > 12 for rows in preview.values()):
        raise ValueError("Analysis preview differs from declared analyses")
    files.update(declared)
    return manifest, files


def _clean_tables(root, audit, quality, *, source_prefix=""):
    if set(audit.get("tables", {})) != set(TABLES) or set(quality.get("input_rows", {})) != set(TABLES):
        raise ValueError("Cleaning audit must declare all 23 tables")
    files, tables = {}, {}
    for name, columns in TABLES.items():
        folder = source_prefix + "clean/" + name
        inventory = _inventory(root, folder)
        _complete(root / folder)
        shards = {relative: path for relative, path in inventory.items() if path.name != "_SUCCESS"}
        if not shards or any(path.parent != root / folder or not path.name.startswith("part-") or
                not path.name.endswith(".parquet") for path in shards.values()):
            raise ValueError("Invalid cleaned Parquet inventory: " + name)
        item = audit["tables"][name]
        count = item["after"]["row_count"]
        conservation = item["row_conservation"]
        if (type(count) is not int or count < 0 or conservation.get("passed") is not True or
                conservation.get("retained") != count or conservation.get("input") != quality["input_rows"][name] or
                conservation["input"] != count + conservation["quarantined"] + conservation["duplicate_copies"]):
            raise ValueError("Invalid clean row conservation declaration: " + name)
        if name == "charging_sessions" and (count != quality["clean_session_rows"] or
                conservation["quarantined"] + conservation["duplicate_copies"] != quality["rejected_session_rows"]):
            raise ValueError("Session audit/quality counts differ")
        files.update(inventory)
        tables[name] = {"path": "clean/" + name, "rowCount": count, "columnNames": columns,
                        "countSource": "SPARK_CLEANING_AUDIT_NOT_RECOUNTED_BY_PACKAGER"}
    actual_clean = _inventory(root, source_prefix + "clean")
    if set(actual_clean) != set(files):
        raise ValueError("Unexpected clean table/file inventory")
    folder = source_prefix + "rejected/charging_sessions"
    rejected = _inventory(root, folder)
    _complete(root / folder)
    shards = [path for path in rejected.values() if path.name != "_SUCCESS"]
    if not shards or any(path.parent != root / folder or not path.name.startswith("part-") or
            not path.name.endswith(".parquet") for path in shards):
        raise ValueError("Invalid rejected Parquet inventory")
    files.update(rejected)
    return files, tables


def _rules(audit, rules, quality, audit_path, rules_path):
    rule_hash = hashlib.sha256(_canonical(rules["rules"]).encode("utf-8")).hexdigest()
    for key in ("dataset_id", "pipeline_run_id", "source_manifest_sha256"):
        if audit["lineage"].get(key) != quality.get(key):
            raise ValueError("Audit belongs to another batch")
    summary = quality.get("cleaning_audit_summary", {})
    if (audit.get("rules_sha256") != rule_hash or rules.get("rules_sha256") != rule_hash or
            audit.get("rule_version") != rules.get("rule_version") or
            summary.get("audit_sha256") != _hash(audit_path) or
            summary.get("rules_file_sha256") != _hash(rules_path) or
            audit.get("row_conservation", {}).get("passed") is not True):
        raise ValueError("Audit/rules evidence checksum mismatch")
    return {"ruleVersion": rules["rule_version"], "rulesSha256": rule_hash,
            "auditSha256": _hash(audit_path), "rulesFileSha256": _hash(rules_path)}


def _inspect_run(root):
    _complete(root)
    _complete(root / "processed")
    serving = inspect_export(root / "export")
    quality_files = list((root / "processed/reports/quality_report").glob("part-*.json"))
    if len(quality_files) != 1:
        raise ValueError("Expected one processed quality report shard")
    quality_path = _file(root, quality_files[0].relative_to(root).as_posix())
    quality = _json(quality_path)
    exported = {key: value for key, value in serving["quality"].items() if key != "rejection_samples"}
    if _canonical(exported) != _canonical({key: value for key, value in quality.items() if key not in {"input", "output"}}):
        raise ValueError("Export quality differs from completed cleaning batch")
    analysis, analysis_files = _analysis(root / "analysis", quality)
    evidence = validate_evidence(root, quality, analysis)
    report_path = _file(root, "acceptance_report.json")
    report = _json(report_path)
    if (report.get("status") != "LOCAL_DATA_READY" or
            any(report.get(key) != serving["manifest"][key] for key in IDENTITY) or
            _canonical(report.get("analysis")) != _canonical(analysis) or
            _canonical(report.get("evidence")) != _canonical(evidence)):
        raise ValueError("Acceptance report belongs to another or incomplete batch")
    expected_counts = {"tableCount": len(TABLES), "inputRows": sum(quality["input_rows"].values()),
        "cleanSessions": quality["clean_session_rows"], "rejectedSessions": quality["rejected_session_rows"],
        "rejectionReasons": quality["rejection_reasons"], "rowsConserved": True}
    if any(_canonical(report.get("cleaning", {}).get(key)) != _canonical(value) for key, value in expected_counts.items()):
        raise ValueError("Acceptance report cleaning counts differ")
    audit_path = _file(root, "processed/reports/cleaning_audit.json")
    rules_path = _file(root, "processed/reports/cleaning_rules.json")
    audit, rules = _json(audit_path), _json(rules_path)
    files, tables = _clean_tables(root, audit, quality, source_prefix="processed/")
    attachments = {relative.removeprefix("processed/"): path for relative, path in files.items()}
    attachments.update({"reports/cleaning_audit.json": audit_path, "reports/cleaning_rules.json": rules_path})
    attachments.update({"analysis/" + relative: path for relative, path in analysis_files.items()})
    # Include source-only metadata in the snapshot to detect an upstream change
    # during copying, but do not ship reports containing obsolete local paths.
    snapshot = {str(path.relative_to(root)): _hash(path) for path in attachments.values()}
    for path in [report_path, quality_path, *serving["files"].values()]:
        snapshot[str(path.relative_to(root))] = _hash(path)
    return serving, attachments, tables, evidence, snapshot


def _inspect_bundle(root, *, require_complete=True):
    serving = inspect_export(root, require_complete=require_complete)
    manifest = _json(_file(root, "acceptance_manifest.json"))
    if (manifest.get("bundleVersion") != VERSION or
            any(manifest.get(key) != serving["manifest"][key] for key in IDENTITY) or
            manifest.get("servingManifestSha256") != serving["manifestSha256"] or
            manifest.get("hdfs", {}).get("status") != "NOT_VERIFIED"):
        raise ValueError("Acceptance manifest batch/version mismatch")
    files = _checked_files(root, manifest["files"])
    actual = {}
    for folder in MANAGED:
        actual.update(_inventory(root, folder))
    if set(actual) != set(files):
        raise ValueError("Acceptance attachment inventory differs from manifest")
    audit_path, rules_path = _file(root, "reports/cleaning_audit.json"), _file(root, "reports/cleaning_rules.json")
    audit, rules = _json(audit_path), _json(rules_path)
    evidence = _rules(audit, rules, serving["quality"], audit_path, rules_path)
    analysis, _ = _analysis(root / "analysis", serving["quality"])
    evidence.update(semanticDimensions=analysis["semanticDimensionCount"], comparisons=analysis["comparisonCount"])
    _, tables = _clean_tables(root, audit, serving["quality"])
    if (_canonical(manifest.get("cleanTables")) != _canonical(tables) or
            _canonical(manifest.get("evidence")) != _canonical(evidence) or
            manifest.get("rejectedSessions") != serving["quality"]["rejected_session_rows"]):
        raise ValueError("Acceptance manifest declarations differ from attached evidence")
    return {"root": root, "manifest": manifest, "serving": serving, "files": files}


def inspect_acceptance_bundle(input_root):
    """Read-only attachment/hash/lineage verification; no Spark recount implied."""
    return _inspect_bundle(_root(input_root))


def bundle_acceptance(input_root, output_root):
    """Copy one verified completed run to a NEW independent portable directory."""
    source, destination = _root(input_root), _root(output_root)
    if destination.exists():
        raise FileExistsError("Bundle output already exists; it will not be overwritten")
    if destination.is_relative_to(source) or source.is_relative_to(destination):
        raise ValueError("Input/output must be independent, non-nested directories")
    serving, attachments, tables, evidence, snapshot = _inspect_run(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix="." + destination.name + ".staging-", dir=destination.parent))
    payload = staging / "bundle"
    try:
        bundle_export(source / "export", payload)
        # CSV bundling has its own completion boundary. The additional bundle
        # remains hidden in staging and unavailable at the requested output.
        (payload / "_SUCCESS").rename(payload / "_RUNNING")
        entries = []
        for relative, path in sorted(attachments.items()):
            target = payload / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            expected = _entry(relative, path)
            shutil.copyfile(path, target)
            if _entry(relative, target) != expected:
                raise ValueError("Attachment copy verification failed")
            entries.append(expected)
        evidence = dict(evidence, rulesFileSha256=_hash(payload / "reports/cleaning_rules.json"))
        manifest = {"bundleVersion": VERSION, "source": "SIMULATED",
            "generatedAt": datetime.now(timezone.utc).isoformat(),
            **{key: serving["manifest"][key] for key in IDENTITY},
            "servingManifestSha256": _hash(payload / "serving_manifest.json"),
            "cleanTables": tables, "rejectedSessions": serving["quality"]["rejected_session_rows"],
            "evidence": evidence, "files": entries,
            "hdfs": {"status": "NOT_VERIFIED", "reason": "Portable local copy; run verify_hdfs in target environment"}}
        _write(payload / "acceptance_manifest.json", manifest)
        _inspect_bundle(payload, require_complete=False)
        latest = _inspect_run(source)
        if latest[4] != snapshot:
            raise ValueError("Source batch changed while bundling")
        if destination.exists() or destination.is_symlink():
            raise FileExistsError("Output appeared while bundling; refusing overwrite")
        (payload / "_RUNNING").unlink()
        (payload / "_SUCCESS").touch(exist_ok=False)
        payload.rename(destination)
        staging.rmdir()
        return manifest
    except Exception as exc:
        if payload.is_dir():
            (payload / "_SUCCESS").unlink(missing_ok=True)
            (payload / "_RUNNING").touch(exist_ok=True)
        _write(staging / "failure_report.json", {"status": "FAILED", "errorType": type(exc).__name__,
            "message": str(exc), "note": "Staging retained for diagnosis; original data/output are not deleted"})
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--input", help="Completed run_acceptance output directory")
    group.add_argument("--verify", help="Read-only verification of an existing acceptance bundle")
    parser.add_argument("--output", help="New, independent bundle directory")
    args = parser.parse_args(argv)
    if (args.input and not args.output) or (args.verify and args.output):
        parser.error("Use --input RUN --output NEW_BUNDLE, or --verify BUNDLE")
    result = inspect_acceptance_bundle(args.verify)["manifest"] if args.verify else bundle_acceptance(args.input, args.output)
    print(json.dumps({"status": "VERIFIED", "datasetId": result["datasetId"],
        "publishedBatchId": result["publishedBatchId"], "cleanTableCount": len(result["cleanTables"]),
        "semanticDimensions": result["evidence"]["semanticDimensions"], "hdfs": result["hdfs"]["status"]}))


if __name__ == "__main__":
    main()
