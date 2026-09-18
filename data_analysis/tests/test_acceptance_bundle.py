"""Small byte-copy/lineage fixtures; these do NOT assert Spark/HDFS execution."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from data_analysis.charging_data.schema import TABLES
from data_analysis.publishing.acceptance_bundle import bundle_acceptance, inspect_acceptance_bundle
from data_analysis.publishing.publish import publish_dataset
from data_analysis.scripts.run_acceptance import validate_evidence
from data_analysis.tests.test_publishing import ExportFixture, sha


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


class AcceptanceBundleTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="acceptance-bundle-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source, self.output = self.root / "run", self.root / "bundle"
        self.source.mkdir()
        fixture = self.fixture = ExportFixture(self.source / "export")
        self.quality = fixture.quality
        self.quality.update(input_rows={name: 2 if name == "charging_sessions" else 1 for name in TABLES},
            clean_session_rows=1, rejected_session_rows=1, rejection_reasons={"DUPLICATE_SESSION_ID": 1})
        rules = [{"id": "fixture-rule", "action": "UNIT_TEST_ONLY"}]
        rule_hash = hashlib.sha256(json.dumps(rules, ensure_ascii=False, sort_keys=True,
            separators=(",", ":")).encode("utf-8")).hexdigest()
        self.audit = {"lineage": {key: self.quality[key] for key in
            ("dataset_id", "pipeline_run_id", "source_manifest_sha256")},
            "rule_version": "unit-test", "rules_sha256": rule_hash, "row_conservation": {"passed": True},
            "tables": {name: {"after": {"row_count": 1}, "row_conservation": {
                "input": count, "retained": 1, "quarantined": 0, "duplicate_copies": count - 1,
                "passed": True}} for name, count in self.quality["input_rows"].items()}}
        reports = self.source / "processed/reports"
        write(reports / "cleaning_rules.json", dict(rules=rules, rule_version="unit-test", rules_sha256=rule_hash))
        write(reports / "cleaning_audit.json", self.audit)
        self.quality["cleaning_audit_summary"] = dict(audit_sha256=sha(reports / "cleaning_audit.json"),
            rules_file_sha256=sha(reports / "cleaning_rules.json"))
        self.processed_quality = dict(self.quality, input="file:/unit-test/raw", output="file:/unit-test/processed")
        write(reports / "quality_report/part-00000.json", self.processed_quality)
        fixture.write()
        for folder in ["processed/clean/" + name for name in TABLES] + ["processed/rejected/charging_sessions"]:
            path = self.source / folder
            path.mkdir(parents=True)
            # Only tests immutable copying. Actual Parquet semantic checks are
            # covered by Spark suites and independent delivery verification.
            (path / "part-00000.snappy.parquet").write_bytes(b"PAR1-unit-test-byte-copy-only-PAR1")
            (path / "_SUCCESS").touch()
            (path / ".part-00000.snappy.parquet.crc").write_bytes(b"omit")
        analyses = {**{f"d{i}": {"dimensions": [f"dimension{i}"]} for i in range(8)},
            "c1": {"dimensions": ["dimension0", "dimension1"]},
            "c2": {"dimensions": ["dimension2", "dimension3"]}}
        for name, metadata in analyses.items():
            relative = "csv/" + name + "/part-00000.csv.gz"
            path = self.source / "analysis" / relative
            path.parent.mkdir(parents=True)
            path.write_bytes(b"compressed-fixture-not-read-as-csv")
            (path.parent / "_SUCCESS").touch()
            metadata.update(rows=1, files=[dict(path=relative, bytes=path.stat().st_size, sha256=sha(path))])
        self.analysis = dict(datasetId=fixture.manifest["datasetId"], pipelineRunId=fixture.manifest["pipelineRunId"],
            sourceManifestSha256=fixture.manifest["sourceManifestSha256"], semanticDimensionCount=8,
            comparisonCount=2, analyses=analyses)
        write(self.source / "analysis/analysis_manifest.json", self.analysis)
        write(self.source / "analysis/preview.json", {name: [] for name in analyses})
        for folder in (self.source, self.source / "processed", self.source / "analysis"):
            (folder / "_SUCCESS").touch()
        self.report = {**{key: fixture.manifest[key] for key in
            ("datasetId", "pipelineRunId", "publishedBatchId", "sourceManifestSha256")},
            "status": "LOCAL_DATA_READY", "analysis": self.analysis,
            "evidence": validate_evidence(self.source, self.quality, self.analysis),
            "cleaning": {"tableCount": len(TABLES), "inputRows": sum(self.quality["input_rows"].values()),
                "cleanSessions": 1, "rejectedSessions": 1, "rejectionReasons": self.quality["rejection_reasons"],
                "rowsConserved": True}}
        write(self.source / "acceptance_report.json", self.report)
        (self.source / "acceptance_report.md").write_text("obsolete processed/export local paths", encoding="utf-8")
        (self.source / "analytics.sqlite3").write_bytes(b"do not copy")
        (self.source / "processed/clean/.DS_Store").write_bytes(b"omit")

    def bundle(self):
        return bundle_acceptance(self.source, self.output)

    def test_complete_bundle_preserves_bytes_batch_and_original_publisher(self):
        before = {path: sha(path) for path in self.source.rglob("*") if path.is_file()}
        manifest = self.bundle()
        result = inspect_acceptance_bundle(self.output)
        self.assertEqual(result["manifest"], manifest)
        self.assertEqual(len(manifest["cleanTables"]), 23)
        self.assertEqual(manifest["evidence"]["semanticDimensions"], 8)
        self.assertEqual(manifest["hdfs"]["status"], "NOT_VERIFIED")
        self.assertEqual(manifest["publishedBatchId"], self.fixture.manifest["publishedBatchId"])
        for item in manifest["files"]:
            relative = item["path"]
            original = self.source / ("processed/" + relative if relative.startswith(("clean/", "rejected/", "reports/")) else relative)
            self.assertEqual(sha(original), sha(self.output / relative))
        self.assertEqual(before, {path: sha(path) for path in self.source.rglob("*") if path.is_file()})
        self.assertFalse(any(path.name.endswith(".crc") or path.name == ".DS_Store" for path in self.output.rglob("*")))
        for name in ("analytics.sqlite3", "parquet", "raw", "acceptance_report.md", "_RUNNING"):
            self.assertFalse((self.output / name).exists(), name)
        publication = publish_dataset(self.output, self.root / "query.sqlite3")
        self.assertFalse(publication["targetLabelsImported"])

    def test_existing_and_nested_destinations_are_untouched(self):
        self.output.mkdir()
        (self.output / "keep.txt").write_text("user data", encoding="utf-8")
        with self.assertRaises(FileExistsError):
            self.bundle()
        self.assertEqual((self.output / "keep.txt").read_text(), "user data")
        with self.assertRaises(ValueError):
            bundle_acceptance(self.source, self.source / "nested")
        with self.assertRaises(ValueError):
            bundle_acceptance("hdfs://nn/path", self.root / "other")

    def test_incomplete_run_is_refused_before_staging(self):
        for relative in ("_SUCCESS", "processed/_SUCCESS", "analysis/_SUCCESS", "export/_SUCCESS",
                         "processed/clean/cities/_SUCCESS"):
            marker = self.source / relative
            marker.unlink()
            with self.subTest(marker=relative), self.assertRaises(ValueError):
                self.bundle()
            self.assertFalse(self.output.exists())
            marker.touch()

    def test_mixed_report_or_quality_batch_is_rejected(self):
        self.report["publishedBatchId"] = "another-batch"
        write(self.source / "acceptance_report.json", self.report)
        with self.assertRaisesRegex(ValueError, "batch"):
            self.bundle()
        self.report["publishedBatchId"] = self.fixture.manifest["publishedBatchId"]
        write(self.source / "acceptance_report.json", self.report)
        self.processed_quality["clean_session_rows"] = 999
        write(self.source / "processed/reports/quality_report/part-00000.json", self.processed_quality)
        with self.assertRaisesRegex(ValueError, "quality"):
            self.bundle()

    def test_analysis_tampering_and_path_traversal_are_rejected(self):
        item = self.analysis["analyses"]["d0"]["files"][0]
        path = self.source / "analysis" / item["path"]
        path.write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "checksum/size"):
            self.bundle()
        item["path"] = "../outside.csv.gz"
        write(self.source / "analysis/analysis_manifest.json", self.analysis)
        with self.assertRaisesRegex(ValueError, "relative"):
            self.bundle()

    def test_symlink_data_and_output_parent_are_rejected(self):
        link = self.root / "link"
        try:
            link.symlink_to(self.source, target_is_directory=True)
        except OSError:
            self.skipTest("Unprivileged symlinks are unavailable")
        with self.assertRaisesRegex(ValueError, "symlink"):
            bundle_acceptance(link, self.output)
        with self.assertRaisesRegex(ValueError, "symlink"):
            bundle_acceptance(self.source, link / "new-output")
        path = self.source / "processed/clean/cities/part-00000.snappy.parquet"
        path.unlink()
        path.symlink_to(self.source / "analytics.sqlite3")
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.bundle()

    def test_attachment_damage_or_unlisted_file_fails_read_only_verification(self):
        self.bundle()
        path = self.output / "clean/cities/part-00000.snappy.parquet"
        original = path.read_bytes()
        path.write_bytes(b"damage")
        with self.assertRaisesRegex(ValueError, "checksum/size"):
            inspect_acceptance_bundle(self.output)
        path.write_bytes(original)
        (self.output / "clean/cities/extra.bin").write_bytes(b"unlisted")
        with self.assertRaisesRegex(ValueError, "inventory"):
            inspect_acceptance_bundle(self.output)

    def test_manifest_cannot_claim_another_batch_or_count(self):
        manifest = self.bundle()
        path = self.output / "acceptance_manifest.json"
        manifest["cleanTables"]["cities"]["rowCount"] = 999
        write(path, manifest)
        with self.assertRaisesRegex(ValueError, "declarations"):
            inspect_acceptance_bundle(self.output)
        manifest["pipelineRunId"] = "other"
        write(path, manifest)
        with self.assertRaisesRegex(ValueError, "batch"):
            inspect_acceptance_bundle(self.output)

    def test_copy_failure_preserves_hidden_diagnostics_without_public_success(self):
        real_copy = shutil.copyfile
        def corrupt(source, destination):
            result = real_copy(source, destination)
            if str(destination).endswith(".parquet"):
                Path(destination).write_bytes(b"broken")
            return result
        with patch("data_analysis.publishing.acceptance_bundle.shutil.copyfile", side_effect=corrupt):
            with self.assertRaisesRegex(ValueError, "copy verification"):
                self.bundle()
        self.assertFalse(self.output.exists())
        staging = next(self.root.glob(".bundle.staging-*"))
        self.assertTrue((staging / "failure_report.json").is_file())
        self.assertTrue((staging / "bundle/_RUNNING").is_file())
        self.assertFalse((staging / "bundle/_SUCCESS").exists())

    def test_cli_verifies_without_spark(self):
        self.bundle()
        result = subprocess.run([sys.executable, "-S", "-m", "data_analysis.publishing.acceptance_bundle",
            "--verify", str(self.output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "VERIFIED")
        invalid = subprocess.run([sys.executable, "-m", "data_analysis.publishing.acceptance_bundle",
            "--verify", str(self.output), "--output", str(self.root / "unused")], capture_output=True, text=True)
        self.assertNotEqual(invalid.returncode, 0)

    def test_audit_file_tampering_is_rejected_before_output(self):
        path = self.source / "processed/reports/cleaning_audit.json"
        path.write_text(path.read_text(encoding="utf-8") + " ", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "hashes"):
            self.bundle()
        self.assertFalse(self.output.exists())

    def test_source_change_during_copy_never_publishes_a_mixed_snapshot(self):
        real_copy = shutil.copyfile
        changed = False
        def change_source(source, destination):
            nonlocal changed
            result = real_copy(source, destination)
            if not changed and str(destination).endswith(".parquet"):
                path = self.source / "acceptance_report.json"
                path.write_text(path.read_text(encoding="utf-8") + " ", encoding="utf-8")
                changed = True
            return result
        with patch("data_analysis.publishing.acceptance_bundle.shutil.copyfile", side_effect=change_source):
            with self.assertRaisesRegex(ValueError, "Source batch changed"):
                self.bundle()
        self.assertFalse(self.output.exists())
        staging = next(self.root.glob(".bundle.staging-*"))
        self.assertTrue((staging / "failure_report.json").is_file())
        self.assertFalse((staging / "bundle/_SUCCESS").exists())


if __name__ == "__main__":
    unittest.main()
