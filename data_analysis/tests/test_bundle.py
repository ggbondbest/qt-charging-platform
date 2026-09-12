"""Portable handoff tests use small typed exports, not a Spark installation."""

import json
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from data_analysis.publishing.bundle import bundle_export
from data_analysis.publishing.publish import inspect_export, publish_dataset
from data_analysis.tests.test_publishing import ExportFixture, sha


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fixture = ExportFixture(self.root / "export")
        self.fixture.write()
        self.output = self.root / "portable"

    def bundle(self):
        return bundle_export(self.fixture.root, self.output)

    def test_portable_bundle_preserves_batch_csv_and_future_labels_but_not_parquet(self):
        parquet = self.fixture.root / "parquet" / "not_for_handoff.bin"
        parquet.parent.mkdir()
        parquet.write_bytes(b"not copied")
        before = {path: sha(path) for path in self.fixture.root.rglob("*") if path.is_file()}
        result = self.bundle()
        self.assertEqual(result["storageProfile"], "PORTABLE_CSV_GZIP")
        for key in ["datasetId", "publishedBatchId", "pipelineRunId", "sourceManifestSha256"]:
            self.assertEqual(result[key], self.fixture.manifest[key])
        self.assertTrue(all("parquetPath" not in item for item in result["tables"].values()))
        self.assertTrue((self.output / "csv" / "ml_targets_hourly" / "part-00000.csv.gz").is_file())
        self.assertFalse((self.output / "parquet").exists())
        self.assertFalse((self.output / "_RUNNING").exists())
        self.assertTrue((self.output / "_SUCCESS").is_file())
        inspected = inspect_export(self.output)
        self.assertEqual(inspected["manifest"], result)
        self.assertEqual(before, {path: sha(path) for path in self.fixture.root.rglob("*") if path.is_file()})
        database = self.root / "analytics.sqlite3"
        self.assertFalse(publish_dataset(self.output, database)["targetLabelsImported"])
        with sqlite3.connect(database.as_uri() + "?mode=ro", uri=True) as connection:
            self.assertIsNone(connection.execute("SELECT name FROM sqlite_master WHERE name='ml_targets_hourly'").fetchone())
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM cities").fetchone()[0], 1)

    def test_existing_and_nested_output_rejected_without_overwrite(self):
        self.output.mkdir()
        (self.output / "keep.txt").write_text("user data")
        with self.assertRaises(FileExistsError):
            self.bundle()
        self.assertEqual((self.output / "keep.txt").read_text(), "user data")
        with self.assertRaises(ValueError):
            bundle_export(self.fixture.root, self.fixture.root / "nested")
        with self.assertRaises(FileExistsError):
            bundle_export(self.fixture.root, self.fixture.root)
        self.assertFalse((self.fixture.root / "nested").exists())

    def test_incomplete_batch_and_bad_hash_are_refused_before_output_creation(self):
        for marker in ["_RUNNING", "missing_success"]:
            with self.subTest(marker=marker):
                if marker == "_RUNNING":
                    (self.fixture.root / marker).write_text("")
                else:
                    (self.fixture.root / "_SUCCESS").unlink()
                with self.assertRaises(ValueError):
                    self.bundle()
                self.assertFalse(self.output.exists())
                self.fixture.write()
                (self.fixture.root / "_RUNNING").unlink(missing_ok=True)
        self.fixture.manifest["tables"]["cities"]["files"][0]["sha256"] = "f" * 64
        self.fixture.save_metadata()
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.bundle()
        self.assertFalse(self.output.exists())

    def test_symlink_shard_cannot_copy_files_outside_source(self):
        path = self.fixture.root / self.fixture.manifest["tables"]["cities"]["files"][0]["path"]
        outside = self.root / "outside.csv.gz"
        outside.write_bytes(path.read_bytes())
        path.unlink()
        try:
            path.symlink_to(outside)
        except OSError:
            self.skipTest("Unprivileged symlinks are unavailable")
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.bundle()
        self.assertFalse(self.output.exists())

    def test_failed_copy_keeps_running_marker_and_cannot_be_published(self):
        real_copy = shutil.copyfile
        def broken_copy(source, target):
            result = real_copy(source, target)
            if str(target).endswith(".csv.gz"):
                Path(target).write_bytes(b"damaged copy")
            return result
        with patch("data_analysis.publishing.bundle.shutil.copyfile", side_effect=broken_copy):
            with self.assertRaisesRegex(ValueError, "copy verification"):
                self.bundle()
        self.assertTrue((self.output / "_RUNNING").exists())
        self.assertFalse((self.output / "_SUCCESS").exists())
        with self.assertRaises(ValueError):
            publish_dataset(self.output, self.root / "failed.sqlite3")
        with self.assertRaises(FileExistsError):
            self.bundle()

    def test_changed_source_after_copy_prevents_completion(self):
        real_copy = shutil.copyfile
        changed = False
        def changing_source(source, target):
            nonlocal changed
            result = real_copy(source, target)
            if not changed and str(source).endswith(".csv.gz"):
                path = self.fixture.root / "serving_manifest.json"
                path.write_text(path.read_text() + " ")
                changed = True
            return result
        with patch("data_analysis.publishing.bundle.shutil.copyfile", side_effect=changing_source):
            with self.assertRaisesRegex(ValueError, "Source batch changed"):
                self.bundle()
        self.assertTrue((self.output / "_RUNNING").is_file())
        self.assertFalse((self.output / "_SUCCESS").exists())

    def test_precommit_validation_failure_never_advertises_success(self):
        real_inspect = inspect_export
        def reject_staging(path, **kwargs):
            if kwargs.get("require_complete") is False:
                self.assertFalse((Path(path) / "_SUCCESS").exists())
                raise ValueError("injected final validation failure")
            return real_inspect(path, **kwargs)
        with patch("data_analysis.publishing.bundle.inspect_export", side_effect=reject_staging):
            with self.assertRaisesRegex(ValueError, "final validation failure"):
                self.bundle()
        self.assertTrue((self.output / "_RUNNING").is_file())
        self.assertFalse((self.output / "_SUCCESS").exists())

    def test_cli_runs_without_spark(self):
        result = subprocess.run([sys.executable, "-m", "data_analysis.publishing.bundle",
            "--input", str(self.fixture.root), "--output", str(self.output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["storageProfile"], "PORTABLE_CSV_GZIP")


if __name__ == "__main__":
    unittest.main()
