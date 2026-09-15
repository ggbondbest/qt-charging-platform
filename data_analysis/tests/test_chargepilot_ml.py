"""Arrival ML: temporal isolation, nonzero censoring, distribution and serving parity."""
import json
from pathlib import Path
import tempfile
import unittest
from importlib.metadata import version
from unittest.mock import patch

try:
    import numpy as np
    import pandas as pd
    from data_analysis.chargepilot.ml.data import FEATURE_NAMES, Replay, epoch, iso, purged_mask, source_provenance
    from data_analysis.chargepilot.ml.predict import ArrivalPredictor
    from data_analysis.chargepilot.ml.train import wait_samples, availability_metrics
    HAS_DEPS = True
except ImportError:
    HAS_DEPS = False


def replay_fixture():
    start = epoch("2026-04-30T16:00:00Z")
    counts = np.zeros((1, 80, 6), dtype=np.uint8)
    counts[0, :, 0] = np.arange(80) % 4
    counts[0, :, 1] = 3 - counts[0, :, 0]
    cfg = {"startEpoch": start, "endEpoch": start + 80 * 300,
           "catalog": [{"stationId": "S", "cityId": "C", "capacity": 3, "powerKw": 90}],
           "cities": [{"cityId": "C"}], "cityCodes": [0], "siteCodes": [0],
           "calendar": {"C:2026-05-01": [1, 0]}, "prices": {"C:1": 1.5}}
    return Replay(cfg, counts, counts[:, :, 1].astype(np.float32) * 30,
                  np.zeros((1, 80), dtype=np.int16), np.ones((1, 80), dtype=np.int16))


@unittest.skipUnless(HAS_DEPS, "Arrival ML optional dependencies absent")
class HistoricalFeatures(unittest.TestCase):
    def test_same_tick_and_all_future_changes_do_not_change_features(self):
        replay = replay_fixture()
        origin = replay.start + 20 * 300
        before = replay.features([0], [origin], [30])
        # Arrival's own state and every later observation are deliberately corrupted.
        counts = replay.counts.copy()
        counts[:, 20:, :] = 0
        counts[:, 20:, 0] = 3
        power = replay.power.copy()
        power[:, 20:] = 9999
        waiting = replay.waiting.copy()
        waiting[:, 20:] = 999
        arrivals = replay.arrivals.copy()
        arrivals[:, 20:] = 999
        mutated = Replay(replay.config, counts, power, waiting, arrivals)
        np.testing.assert_array_equal(before, mutated.features([0], [origin], [30]))
        self.assertEqual(before[0, FEATURE_NAMES.index("last_available")], replay.counts[0, 19, 0])
        self.assertEqual(before[0, FEATURE_NAMES.index("arrivals_60min")], 12)

    def test_complete_interval_boundary_and_utc_microsecond_precision(self):
        replay = replay_fixture()
        origin = replay.start + 20 * 300
        left = replay.features([0], [origin - 1], [0])
        exact = replay.features([0], [origin], [0])
        self.assertEqual(left[0, FEATURE_NAMES.index("last_available")], replay.counts[0, 18, 0])
        self.assertEqual(exact[0, FEATURE_NAMES.index("last_available")], replay.counts[0, 19, 0])
        # Arrow Parquet is timestamp[us]; unit confusion would silently shift all dates.
        stamp = pd.Series(np.array(["2026-05-01T00:00:00"], dtype="datetime64[us]"))
        self.assertEqual(epoch(stamp)[0], epoch("2026-05-01T00:00:00Z"))

    def test_unknown_or_out_of_range_histories_rejected(self):
        replay = replay_fixture()
        for station, origin, horizon in [(0, replay.start, 0), (1, replay.start + 6000, 0),
                                         (0, replay.start + 6000, 61), (0, replay.start + 6000, float("nan"))]:
            with self.assertRaises(ValueError):
                replay.features([station], [origin], [horizon])

    def test_replay_persistence_retains_feature_parity(self):
        replay = replay_fixture()
        with tempfile.TemporaryDirectory() as folder:
            replay.save(folder)
            restored = Replay.load(folder)
        origin = replay.start + 20 * 300
        np.testing.assert_array_equal(replay.features([0], [origin], [15]), restored.features([0], [origin], [15]))

    def test_purge_origin_arrival_and_outcome_crossing_split(self):
        mask = purged_mask(np.array([100, 99, 100, 100, 100]), np.array([150, 150, 200, 150, 150]),
                           np.array([199, 199, 199, 200, 201]), 100, 200)
        self.assertEqual(mask.tolist(), [True, False, False, False, False])

    def test_unsuccessful_wait_remains_unknown_after_sampling(self):
        replay = replay_fixture()
        origin = replay.start + 40 * 300
        label = pd.DataFrame({"attempt_id": ["success", "failed"], "stationCode": [0, 0],
                              "arrivalEpoch": [origin, origin], "finalEpoch": [origin + 600, origin + 600],
                              "success": [1, 0], "waitMinutes": [10.0, np.nan]})
        sample = wait_samples(replay, label, (replay.start + 3600, replay.start + 80 * 300), 100,
                              np.random.default_rng(7))
        self.assertTrue(np.isnan(sample["wait"][sample["success"] == 0]).all())
        self.assertTrue((sample["wait"][sample["success"] == 1] == 10).all())

    def test_probability_baseline_normalized(self):
        result = availability_metrics(np.array([0, 3]), np.array([[1, 1e-8, 1e-8, 1e-8], [1e-8, 1e-8, 1e-8, 1]]))
        self.assertLess(result["countMae"], 1e-6)


@unittest.skipUnless(HAS_DEPS, "Arrival ML optional dependencies absent")
class ArrivalServing(unittest.TestCase):
    def test_artifact_hash_mismatch_rejected_before_pickle_load(self):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            (p / "arrival.metadata.json").write_text(json.dumps({"versions": {"sklearn": version("scikit-learn")}, "artifacts": {"arrival.joblib": {"sha256": "bad"}}}))
            (p / "arrival.joblib").write_bytes(b"not a pickle")
            with self.assertRaisesRegex(ValueError, "integrity mismatch"):
                ArrivalPredictor(folder)

    def test_sklearn_version_rejected_before_pickle_load(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "arrival.metadata.json").write_text(json.dumps({"versions": {"sklearn": "0.0.0"}}))
            with self.assertRaisesRegex(ValueError, "version mismatch"):
                ArrivalPredictor(folder)

    def test_source_binding_rejected_before_pickle_load(self):
        artifact = Path(__file__).resolve().parents[1] / "outputs" / "chargepilot"
        if not (artifact / "arrival.metadata.json").exists():
            self.skipTest("Run chargepilot.ml.train to exercise actual source binding")
        metadata = json.loads((artifact / "arrival.metadata.json").read_text())
        metadata["source"]["datasetId"] = "different-dataset"
        metadata["provenance"] = source_provenance(metadata["source"])
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            (p / "arrival.metadata.json").write_text(json.dumps(metadata))
            for name in ["arrival.joblib", "replay.npz", "replay.json"]:
                (p / name).symlink_to(artifact / name)
            with patch("data_analysis.chargepilot.ml.predict.joblib.load", side_effect=AssertionError("must not deserialize")):
                with self.assertRaisesRegex(ValueError, "provenance mismatch"):
                    ArrivalPredictor(folder)

    def test_trained_artifact_contract_and_no_ground_truth_dependency(self):
        artifact = Path(__file__).resolve().parents[1] / "outputs" / "chargepilot"
        if not (artifact / "arrival.metadata.json").exists():
            self.skipTest("Run chargepilot.ml.train to exercise actual artifact integration")
        predictor = ArrivalPredictor(artifact)
        station = predictor.catalog[0]["stationId"]
        for item in predictor.catalog:
            self.assertAlmostEqual(item["powerKw"] * item["capacity"], item["ratedCapacityKw"], delta=1e-5)
        with patch.object(predictor, "actual", side_effect=AssertionError("Future truth must not be read")):
            result = predictor.predict(station, "2026-05-05T00:00:00Z", 12.7)
        p = np.array(result["availabilityDistribution"])
        self.assertAlmostEqual(p.sum(), 1)
        self.assertAlmostEqual(result["expectedFree"], p @ np.arange(len(p)))
        self.assertAlmostEqual(result["availableProbability"], 1 - p[0])
        self.assertLessEqual(result["waitMinutes"], result["waitP90Minutes"])
        self.assertEqual(result["forecastTime"], "2026-05-05T00:10:00Z")
        self.assertEqual(result["arrivalTime"], "2026-05-05T00:12:42Z")
        self.assertEqual(result["featureAsOf"], "2026-05-04T23:55:00Z")
        for split, values in predictor.metadata["samples"].items():
            self.assertLess(epoch(values["maxLabelTime"]), epoch(predictor.metadata["splitBoundaries"][split]["endExclusive"]))


if __name__ == "__main__":
    unittest.main()
