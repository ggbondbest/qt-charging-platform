"""Guard acceptance orchestration without needing Spark or a Hadoop server."""

import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from data_analysis.scripts.run_acceptance import local_roots, prepare_acceptance, report_markdown, validate_evidence
from data_analysis.scripts.verify_hdfs import require_hdfs_uri


class AcceptancePreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="acceptance-runner-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "manifest.json").write_text('{}', encoding="utf-8")
        self.output = self.root / "output"

    def test_paths_cannot_overwrite_or_overlap(self):
        for output in (self.source, self.source / "child", self.root):
            with self.subTest(output=output), self.assertRaises((ValueError, FileExistsError)):
                local_roots(self.source, output)
        with self.assertRaises(ValueError):
            local_roots("hdfs://nn/charging/raw", self.output)
        self.assertEqual(local_roots(self.source, self.output), (self.source.resolve(), self.output.resolve()))

    def test_invalid_injection_parameters_write_nothing(self):
        for rate in (-1, 2, float('nan'), float('inf'), True):
            with self.subTest(rate=rate), self.assertRaises(ValueError):
                prepare_acceptance(Mock(), self.source, self.output, inject_rate=rate)
            self.assertFalse(self.output.exists())

    def test_report_is_explicit_about_unverified_hdfs_and_models(self):
        report = {"datasetId": "test", "pipelineRunId": "run", "publishedBatchId": "batch", "cleaning": {
            "tableCount": 23, "inputRows": 15, "cleanSessions": 10, "rejectedSessions": 2,
            "rowsConserved": True, "rejectionReasons": {"DUPLICATE_SESSION_ID": 2}}}
        text = report_markdown(report)
        self.assertIn("FastAPI", text)
        self.assertIn("不是 HDFS", text)
        self.assertIn("未宣称模型训练", text)
        self.assertIn("DUPLICATE_SESSION_ID | 2", text)

    def test_failed_pipeline_keeps_failure_evidence_without_success(self):
        with patch("data_analysis.spark_jobs.pipeline.run_pipeline", side_effect=ValueError("test failure")):
            with self.assertRaisesRegex(ValueError, "test failure"):
                prepare_acceptance(Mock(), self.source, self.output)
        self.assertTrue((self.output / "_RUNNING").is_file())
        self.assertFalse((self.output / "_SUCCESS").exists())
        self.assertEqual(json.loads((self.output / "failure_report.json").read_text())["status"], "FAILED")
        self.assertEqual((self.source / "manifest.json").read_text(), '{}')

    def evidence_fixture(self):
        folder = self.output / "processed" / "reports"
        folder.mkdir(parents=True)
        (self.output / "processed" / "_SUCCESS").touch()
        (self.output / "analysis").mkdir()
        (self.output / "analysis" / "_SUCCESS").touch()
        rules = [{"id": "fixture", "action": "UNIT_TEST_ONLY"}]
        rule_hash = hashlib.sha256(json.dumps(rules, ensure_ascii=False, sort_keys=True,
            separators=(",", ":")).encode()).hexdigest()
        quality = dict(dataset_id="fixture", pipeline_run_id="unit-test", source_manifest_sha256="source")
        audit = dict(lineage=quality.copy(), rule_version="fixture", rules_sha256=rule_hash,
            row_conservation={"passed": True})
        rules_path, audit_path = folder / "cleaning_rules.json", folder / "cleaning_audit.json"
        rules_path.write_text(json.dumps(dict(rules=rules, rule_version="fixture", rules_sha256=rule_hash)), encoding="utf-8")
        audit_path.write_text(json.dumps(audit), encoding="utf-8")
        quality["cleaning_audit_summary"] = dict(audit_sha256=hashlib.sha256(audit_path.read_bytes()).hexdigest(),
            rules_file_sha256=hashlib.sha256(rules_path.read_bytes()).hexdigest())
        analysis = dict(datasetId="fixture", pipelineRunId="unit-test", sourceManifestSha256="source",
            semanticDimensionCount=8, comparisonCount=2, analyses={
                **{f"d{i}": {"dimensions": [f"d{i}"]} for i in range(8)},
                "c1": {"dimensions": ["d0", "d1"]}, "c2": {"dimensions": ["d2", "d3"]}})
        return quality, analysis

    def test_acceptance_requires_actual_audit_files_and_distinct_comparisons(self):
        quality, analysis = self.evidence_fixture()
        self.assertEqual(validate_evidence(self.output, quality, analysis)["semanticDimensions"], 8)
        analysis["analyses"]["c2"] = analysis["analyses"]["c1"]
        with self.assertRaisesRegex(ValueError, "dimensions"):
            validate_evidence(self.output, quality, analysis)

    def test_evidence_tampering_or_incomplete_marker_is_rejected(self):
        quality, analysis = self.evidence_fixture()
        marker = self.output / "analysis" / "_RUNNING"
        marker.touch()
        with self.assertRaisesRegex(ValueError, "incomplete"):
            validate_evidence(self.output, quality, analysis)
        marker.unlink()
        quality["cleaning_audit_summary"]["audit_sha256"] = "wrong"
        with self.assertRaisesRegex(ValueError, "hashes"):
            validate_evidence(self.output, quality, analysis)


class HdfsAcceptancePathTests(unittest.TestCase):
    def test_explicit_hdfs_and_ha_nameservice_uris(self):
        self.assertEqual(require_hdfs_uri("hdfs://nn:9000/charging/raw/"), "hdfs://nn:9000/charging/raw")
        self.assertEqual(require_hdfs_uri("hdfs://nameservice/charging/raw"), "hdfs://nameservice/charging/raw")

    def test_local_root_credentials_and_traversal_never_count_as_hdfs(self):
        for value in ("/charging/raw", "file:///tmp/charging", "hdfs:///charging/raw", "hdfs://nn/",
                "hdfs://user:password@nn/charging", "hdfs://nn/a/../b", "hdfs://nn/a/%2e%2e/b",
                "hdfs://nn/raw?x=1", "hdfs://nn/raw#x", "hdfs://nn/a\\b"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                require_hdfs_uri(value)


if __name__ == "__main__":
    unittest.main()
