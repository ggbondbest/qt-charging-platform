"""Causal policy inputs and continuous capacity in the paired insertion simulator.

Small explicit fixtures; no trained-model evaluation or artifact writes. Changing
future background must not alter an earlier recommendation via oracle outcomes.
"""
from datetime import datetime, timezone
import unittest
from unittest.mock import patch

try:
    from data_analysis.chargepilot import experiment
    HAS_EXPERIMENT = True
except ImportError:
    HAS_EXPERIMENT = False


class CapacityFixture:
    catalog = [{"stationId": "S", "stationName": "Capacity fixture", "cityId": "C",
                "latitude": 31.2, "longitude": 121.5, "capacity": 1,
                "powerKw": 30.0, "ratedCapacityKw": 30.0, "pricePerKwh": 1.2}]
    metadata = {"modelId": "explicit-test-fixture", "artifacts": {"arrival.joblib": {"sha256": "test-fixture"}}}

    def __init__(self, future_cut=None, interrupted=False):
        self.future_cut = future_cut
        self.interrupted = interrupted
        self.latest_reference = None

    def predict(self, station, reference, eta):
        self.latest_reference = reference
        # Intentionally independent of ground truth, isolating the experiment's
        # commitment overlay as the only possible path for future information.
        return {"availabilityDistribution": [.5, .5], "availableProbability": .5,
                "expectedFree": .5, "waitMinutes": 10.0, "waitP90Minutes": 20.0,
                "loadRatio": .5, "currentFree": 1}

    def actual(self, station, timestamp):
        dt = datetime.fromisoformat(timestamp.replace("Z", "+00:00"))
        free = 1
        if self.interrupted:
            free = int((int(dt.timestamp()) // 300) % 11 < 8)
        if self.future_cut and dt >= self.future_cut:
            free = 0
        return {"currentFree": free, "availableCount": free}

    def price(self, station, timestamp):
        return 1.2


@unittest.skipUnless(HAS_EXPERIMENT, "Experiment's optional HTTP dependency absent")
class PairedExperimentCausality(unittest.TestCase):
    def _run_trace(self, future_cut):
        predictor = CapacityFixture(future_cut=future_cut)
        trace = []
        original_score = experiment.score_candidates

        def record(candidates, *args, **kwargs):
            # First policy populates the shared forecast cache. Second policy uses
            # those exact requests in the same order, without new model calls.
            index = len(trace)
            reference = predictor.latest_reference if index < 100 else trace[index - 100][0]
            row = [(c["stationId"], c["expectedFree"], c["availableProbability"],
                    c["waitMinutes"], c["loadRatio"], c["etaMinutes"], c["pricePerKwh"])
                   for c in candidates]
            trace.append((reference, row))
            return original_score(candidates, *args, **kwargs)

        with patch.object(experiment, "score_candidates", side_effect=record), \
                patch.object(experiment, "atomic_report", side_effect=AssertionError("No report writes allowed")):
            report = experiment.run_experiment(predictor, users=100, seed=42, output=None)
        self.assertEqual(len(trace), 200)
        return trace, report

    def test_future_background_cannot_change_prior_recommendations(self):
        cutoff = datetime(2026, 5, 6, 4, tzinfo=timezone.utc)
        ordinary, report_a = self._run_trace(None)
        changed, report_b = self._run_trace(cutoff)
        self.assertEqual(report_a["requestHash"], report_b["requestHash"])
        # The intervention affects actual outcomes, ensuring this is not vacuous.
        self.assertNotEqual(report_a["policies"][0]["served"], report_b["policies"][0]["served"])
        checked = 0
        for baseline, intervention in zip(ordinary, changed):
            if datetime.fromisoformat(baseline[0].replace("Z", "+00:00")) < cutoff:
                self.assertEqual(baseline, intervention)
                checked += 1
        self.assertGreater(checked, 20)

    def test_interrupted_background_off_grid_capacity_and_pairing(self):
        # Non-grid arrivals and ends repeatedly cross changes in background
        # capacity. The simulator's continuous-interval assertions must hold.
        first = experiment.run_experiment(CapacityFixture(interrupted=True), users=100, seed=42, output=None)
        second = experiment.run_experiment(CapacityFixture(interrupted=True), users=100, seed=42, output=None)
        self.assertEqual(first["requestHash"], second["requestHash"])
        self.assertEqual(first["policies"], second["policies"])
        nearest, ai = first["policies"]
        # With only one eligible station, an apparent policy lift would be a bug.
        for key in nearest:
            if key not in {"policy", "label", "rewardPoints"}:
                self.assertEqual(nearest[key], ai[key], key)
        self.assertLessEqual(nearest["maxUtilization"], 1)
        self.assertGreater(nearest["abandoned"], 0)


if __name__ == "__main__":
    unittest.main()
