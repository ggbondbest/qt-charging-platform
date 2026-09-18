"""Hourly delivery adapter tests; trained-model checks use locally reproduced artifacts."""
from __future__ import annotations
import importlib.util
from pathlib import Path
import tempfile
import unittest

from data_analysis.chargepilot.availability_adapter import AvailabilityAdapter

ROOT = Path(__file__).resolve().parents[1]
MODELS = ROOT / "outputs" / "ml_availability_delivery"
STACK = all(importlib.util.find_spec(name) for name in ("numpy", "pandas", "sklearn", "joblib", "pyarrow"))
READY = STACK and all((MODELS / f"h{h:02d}" / "model_metadata.json").exists() for h in (1, 6, 24))


class MissingAvailabilityTests(unittest.TestCase):
    def test_absent_artifacts_are_explicit_not_ready(self):
        with tempfile.TemporaryDirectory() as name:
            adapter = AvailabilityAdapter(Path(name))
            self.assertEqual(adapter.report["status"], "NOT_READY")
            self.assertEqual(adapter.report["models"], [])
            self.assertEqual(adapter.report["missingHorizons"], [1, 6, 24])


@unittest.skipUnless(READY, "train local ml_availability_delivery artifacts for actual inference checks")
class TrainedAvailabilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.adapter = AvailabilityAdapter(MODELS)

    def test_registry_and_bound_test_metrics(self):
        report = self.adapter.report
        self.assertEqual(report["status"], "READY")
        self.assertEqual([model["horizonHours"] for model in report["models"]], [1, 6, 24])
        self.assertEqual(report["targetSemantics"], "LAST_SAMPLE_IN_HOUR")
        for model in report["models"]:
            self.assertTrue(model["modelId"])
        for entry in report["metrics"].values():
            self.assertGreater(entry["testSamples"], 0)
            self.assertGreaterEqual(entry["coverage"], 0)
            self.assertLessEqual(entry["coverage"], 1)

    def test_actual_points_have_hourly_sampling_semantics(self):
        for horizon in (1, 6, 24):
            response = self.adapter.predict("ST-DL-01", "2026-05-05T00:37:00Z", horizon)
            self.assertEqual(response["referenceTime"], "2026-05-05T00:00:00Z")
            self.assertEqual(len(response["points"]), horizon)
            self.assertEqual(len(response["history"]), 24)
            self.assertEqual(response["points"][0]["sampleTimestamp"], "2026-05-05T00:55:00Z")
            for point in response["points"]:
                self.assertIsInstance(point["value"], int)
                self.assertTrue(0 <= point["value"] <= 3)
                self.assertTrue(0 <= point["probabilityNoFree"] <= 1)
                self.assertTrue(0 <= point["lower"] <= point["upper"] <= 3)
                self.assertAlmostEqual(sum(point["distribution"].values()), 1, places=6)
            self.assertLess(response["history"][-1]["timestamp"], response["referenceTime"])

    def test_request_guardrails(self):
        from data_analysis.chargepilot.store import DomainError
        for station, stamp, horizon in (("ST-DL-01", "2026-05-05T00:00:00Z", True),
                ("missing", "2026-05-05T00:00:00Z", 1),
                ("ST-DL-01", "2026-05-05T00:00:00", 1),
                ("ST-DL-01", "2027-05-05T00:00:00Z", 1)):
            with self.assertRaises(DomainError):
                self.adapter.predict(station, stamp, horizon)


if __name__ == "__main__":
    unittest.main()
