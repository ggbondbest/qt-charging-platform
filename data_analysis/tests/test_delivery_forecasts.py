"""Independent contract and recommendation-boundary checks for the composed application."""
from __future__ import annotations
from copy import deepcopy
from datetime import datetime, timedelta, timezone
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
import tempfile
from unittest import mock

STACK = all(importlib.util.find_spec(name) for name in ("pandas", "numpy", "sklearn", "joblib", "pyarrow", "pydantic"))
if STACK:
    from data_analysis.backend.errors import ApiError
    from data_analysis.backend.models import PredictionRequest
    from data_analysis.chargepilot.store import DomainError
    from data_analysis.delivery.models import ForecastService, verify_arrival_source
    from data_analysis.chargepilot.ranking import RecommendationService, score_candidates

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = {"datasetId": "test-data", "publishedBatchId": "test-batch", "sourceManifestSha256": "a" * 64}


class FakeAdapter:
    def __init__(self, target="load"):
        self.target = target
        self.calls = []
        self.extra = {}
        self.report = {"status": "READY", "modelId": "load-test" if target == "load" else "availability-1",
            "metadata": {"modelVersion": "1.0", "datasetId": MANIFEST["datasetId"],
                         "trainingPublishedBatchId": MANIFEST["publishedBatchId"],
                         "sourceManifestSha256": MANIFEST["sourceManifestSha256"]},
            "datasetId": MANIFEST["datasetId"], "publishedBatchId": MANIFEST["publishedBatchId"],
            "models": [{"modelId": f"availability-{h}", "modelVersion": "1.0", "target": "availability", "horizonHours": h}
                       for h in (1, 6, 24)]}

    def predict(self, station_id, reference_time, horizon):
        self.calls.append((station_id, reference_time, horizon))
        start = datetime.fromisoformat(reference_time.replace("Z", "+00:00"))
        points = [{"timestamp": (start + timedelta(hours=step)).isoformat().replace("+00:00", "Z"),
                   "value": 60.0 if self.target == "load" else 2, "lower": 0, "upper": 120 if self.target == "load" else 3,
                   "probabilityNoFree": 0.1} for step in range(horizon)]
        return {"schemaVersion": "1.0.0", "featureVersion": "history24-v1", "modelVersion": "1.0",
                "modelId": "load-test" if self.target == "load" else f"availability-{horizon}",
                "unit": "kW" if self.target == "load" else "chargers", "points": points,
                "referenceTime": reference_time, "historyHours": 24, **self.extra}


@unittest.skipUnless(STACK, "composed application dependencies unavailable")
class ArrivalPublicationBindingTests(unittest.TestCase):
    def test_current_clean_and_source_identity_are_verified_before_loading(self):
        metadata_path = ROOT / "outputs/chargepilot/arrival.metadata.json"
        if not metadata_path.exists():
            self.skipTest("Train arrival model for source-binding integration")
        dataset = ROOT / "datasets/analytics_full_180d_v1"
        manifest = json.loads((dataset / "serving_manifest.json").read_text())
        metadata = json.loads(metadata_path.read_text())
        verify_arrival_source(metadata_path.parent, dataset, manifest)
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "arrival.metadata.json"
            for key in ("datasetId", "publishedBatchId", "pipelineRunId", "sourceManifestSha256"):
                changed = deepcopy(metadata)
                changed["source"][key] = "old-batch"
                file.write_text(json.dumps(changed))
                with self.assertRaisesRegex(ValueError, "batch differ"):
                    verify_arrival_source(directory, dataset, manifest)
            changed = deepcopy(metadata)
            changed["source"]["sourceFilesSha256"]["serving_manifest.json"] = "old-bytes"
            file.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, "fingerprint differs"):
                verify_arrival_source(directory, dataset, manifest)


@unittest.skipUnless(STACK, "composed application dependencies unavailable")
class ForecastContractTests(unittest.TestCase):
    def setUp(self):
        self.load, self.availability = FakeAdapter(), FakeAdapter("availability")
        self.arrival = SimpleNamespace(catalog=[{"stationId": "S", "capacity": 3, "ratedCapacityKw": 120.0}], metadata={})
        self.service = ForecastService(self.load, self.availability, None, self.arrival, MANIFEST)

    def body(self, target="load", **changes):
        values = {"stationId": "S", "referenceTime": "2026-05-05T00:00:00Z", "horizonHours": 1,
                  "modelId": "load-test" if target == "load" else "availability-1"}
        return PredictionRequest(**{**values, **changes})

    def test_batch_dataset_and_source_are_checked(self):
        for key in MANIFEST:
            with self.assertRaises(ApiError) as raised:
                self.service.registry({**MANIFEST, key: "foreign"})
            self.assertEqual(raised.exception.code, "BATCH_MISMATCH")
        self.assertEqual(self.load.calls, [])

    def test_registry_uses_real_model_ids_and_deduplicates_load(self):
        capabilities = self.service.capabilities(MANIFEST)
        self.assertEqual(len(capabilities["models"]), 4)
        self.assertEqual({entry["modelId"] for entry in capabilities["models"]},
                         {"load-test", "availability-1", "availability-6", "availability-24"})

    def test_incompatible_model_and_horizon_do_not_call_adapter(self):
        with self.assertRaises(ApiError) as raised:
            self.service.predict("availability", self.body("availability", horizonHours=6), MANIFEST)
        self.assertEqual(raised.exception.code, "MODEL_INCOMPATIBLE")
        with self.assertRaises(ApiError):
            self.service.predict("load", self.body(modelId="availability-1"), MANIFEST)
        self.assertEqual(self.load.calls + self.availability.calls, [])

    def test_legacy_shape_is_strict_and_detailed_shape_retains_risk(self):
        for target in ("load", "availability"):
            base = self.service.predict(target, self.body(target), MANIFEST, detailed=False)
            self.assertEqual(set(base), {"schemaVersion", "featureVersion", "modelId", "modelVersion", "unit", "points"})
            self.assertEqual(set(base["points"][0]), {"timestamp", "value"})
            detailed = self.service.predict(target, self.body(target), MANIFEST)
            self.assertIn("probabilityNoFree", detailed["points"][0])
            self.assertEqual(detailed["historyHours"], 24)
        self.assertEqual(len(self.load.calls), 1)
        self.assertEqual(len(self.availability.calls), 1)

    def test_callers_cannot_mutate_cached_predictions(self):
        first = self.service.predict("load", self.body(), MANIFEST)
        first["points"][0]["value"] = -900
        first["points"].append({"value": -1})
        second = self.service.predict("load", self.body(), MANIFEST)
        self.assertEqual(second["points"][0]["value"], 60)
        self.assertEqual(len(second["points"]), 1)

    def test_missing_model_is_explicit_not_ready(self):
        self.load.report["status"] = "NOT_READY"
        with self.assertRaises(ApiError) as raised:
            self.service.predict("load", self.body(), MANIFEST)
        self.assertEqual(raised.exception.code, "MODEL_NOT_READY")
        self.assertEqual(self.load.calls, [])

    def test_invalid_adapter_unit_or_feature_version_is_not_hidden(self):
        for extra in ({"unit": "chargers"}, {"featureVersion": "wrong-features"}, {"schemaVersion": "9.0.0"},
                      {"modelId": "wrong-model"}, {"modelVersion": "wrong-version"},
                      {"referenceTime": "2026-05-05T01:00:00Z"}):
            self.setUp()
            self.load.extra = extra
            with self.assertRaises(ApiError) as raised:
                self.service.predict("load", self.body(), MANIFEST)
            self.assertEqual(raised.exception.code, "MODEL_INCOMPATIBLE")

    def test_invalid_output_is_not_cached(self):
        self.load.extra = {"unit": "chargers"}
        with self.assertRaises(ApiError):
            self.service.predict("load", self.body(), MANIFEST)
        self.assertEqual(self.service._cache, {})
        self.load.extra = {}
        self.assertEqual(self.service.predict("load", self.body(), MANIFEST)["unit"], "kW")
        self.assertEqual(len(self.load.calls), 2)

    def test_bad_values_and_timestamps_are_rejected(self):
        for point in ({"timestamp": "2026-05-05T00:00:00Z", "value": 121},
                      {"timestamp": "2026-05-05T00:00:00Z", "value": float("nan")},
                      {"timestamp": "2026-05-05T01:00:00Z", "value": 1}):
            self.setUp()
            self.load.extra = {"points": [point]}
            with self.assertRaises(ApiError) as raised:
                self.service.predict("load", self.body(), MANIFEST)
            self.assertEqual(raised.exception.code, "MODEL_INCOMPATIBLE")


@unittest.skipUnless(STACK, "composed application dependencies unavailable")
class LoadRecommendationBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.adapter = FakeAdapter()
        self.service = RecommendationService(None, None, None, self.adapter)
        self.station = {"stationId": "S", "ratedCapacityKw": 120.0}

    def test_completed_hour_floor_arrival_hour_selection_and_cache(self):
        first = self.service.load_context(self.station, "2026-05-05T00:37:00Z", "2026-05-05T01:05:00Z", .4)
        second = self.service.load_context(self.station, "2026-05-05T00:59:00Z", "2026-05-05T01:17:00Z", .4)
        self.assertEqual(self.adapter.calls, [("S", "2026-05-05T00:00:00Z", 6)])
        self.assertEqual(first["loadForecast"]["timestamp"], "2026-05-05T01:00:00Z")
        self.assertEqual(first, second)
        self.assertEqual(first["balancePressure"], .45)
        self.assertEqual(first["loadForecast"]["targetSemantics"], "MEAN_POWER_DURING_HOUR")

    def test_load_only_changes_default_five_percent_balance_dimension(self):
        candidate = {"stationId": "S", "availableProbability": .75, "waitMinutes": 6,
                     "etaMinutes": 15, "pricePerKwh": 1.2, "powerKw": 60, "loadRatio": .4,
                     "expectedFree": 1.5, "serviceProbability": .9, "waitP90Minutes": 15}
        context = self.service.load_context(self.station, "2026-05-05T00:37:00Z", "2026-05-05T01:05:00Z", .4)
        plain = score_candidates([candidate])[0]
        enriched = score_candidates([{**candidate, **context}])[0]
        for key in ("availability", "wait", "travel", "price", "power"):
            self.assertEqual(plain["scoreBreakdown"][key], enriched["scoreBreakdown"][key])
        self.assertLessEqual(abs(plain["score"] - enriched["score"]), 5)
        for key in ("expectedFree", "availableProbability", "serviceProbability", "waitMinutes", "waitP90Minutes"):
            self.assertEqual(plain[key], enriched[key])
        self.assertEqual(plain["rewardMultiplier"], enriched["rewardMultiplier"])

    def test_missing_or_failed_load_model_does_not_invent_estimate(self):
        self.adapter.report["status"] = "NOT_READY"
        self.assertEqual(self.service.load_context(self.station, "2026-05-05T00:00:00Z", "2026-05-05T00:10:00Z", .4), {})
        self.assertEqual(self.adapter.calls, [])
        self.adapter.report["status"] = "READY"
        with mock.patch.object(self.adapter, "predict", side_effect=DomainError("MODEL_NOT_READY", "not ready", 503)):
            self.assertEqual(self.service.load_context(self.station, "2026-05-05T00:00:00Z", "2026-05-05T00:10:00Z", .4), {})

    def test_forecast_outside_arrival_horizon_does_not_extrapolate(self):
        result = self.service.load_context(self.station, "2026-05-05T00:00:00Z", "2026-05-05T08:00:00Z", .4)
        self.assertEqual(result, {})

    def test_wrong_load_unit_is_not_applied_as_balance_pressure(self):
        self.adapter.extra = {"unit": "chargers"}
        self.assertEqual(self.service.load_context(self.station, "2026-05-05T00:00:00Z", "2026-05-05T00:10:00Z", .4), {})
        self.assertEqual(self.service._load_cache, {})

    def test_misaligned_hour_is_not_used_or_cached(self):
        result = self.adapter.predict("S", "2026-05-05T00:00:00Z", 6)
        result["points"][1]["timestamp"] = "2026-05-05T02:00:00Z"
        with mock.patch.object(self.adapter, "predict", return_value=result):
            self.assertEqual(self.service.load_context(self.station, "2026-05-05T00:00:00Z", "2026-05-05T01:10:00Z", .4), {})
        self.assertEqual(self.service._load_cache, {})


READY = STACK and all(path.is_file() for path in (
    ROOT / "outputs/chargepilot/arrival.joblib", ROOT / "outputs/ml_load/hgb-q50-history24-v1.joblib",
    ROOT / "outputs/ml_availability_delivery/h24/model.joblib"))


@unittest.skipUnless(READY, "train local arrival, PR66 load and hourly availability delivery models")
class ActualForecastIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from data_analysis.chargepilot.load_adapter import LoadAdapter
        from data_analysis.chargepilot.availability_adapter import AvailabilityAdapter
        from data_analysis.chargepilot.ml.predict import ArrivalPredictor
        manifest = json.loads((ROOT / "datasets/analytics_full_180d_v1/serving_manifest.json").read_text())
        cls.publication = manifest
        cls.service = ForecastService(LoadAdapter(ROOT / "outputs/ml_load"),
            AvailabilityAdapter(ROOT / "outputs/ml_availability_delivery"), None,
            ArrivalPredictor(ROOT / "outputs/chargepilot"), manifest)

    def test_all_hourly_models_serve_through_unified_contract(self):
        registry = self.service.registry(self.publication)
        for target in ("load", "availability"):
            self.assertEqual(registry[target]["status"], "READY")
            for model in registry[target]["models"]:
                body = PredictionRequest(stationId="ST-DL-01", modelId=model["modelId"],
                    horizonHours=model["horizonHours"], referenceTime="2026-05-05T00:00:00Z")
                result = self.service.predict(target, body, self.publication)
                self.assertEqual(len(result["points"]), body.horizonHours)
                self.assertEqual(result["modelId"], model["modelId"])
                self.assertEqual(result["unit"], "kW" if target == "load" else "chargers")
                self.assertEqual(result["targetSemantics"], "MEAN_POWER_DURING_HOUR" if target == "load" else "LAST_SAMPLE_IN_HOUR")
                legacy = self.service.predict(target, body, self.publication, detailed=False)
                self.assertEqual(set(legacy["points"][0]), {"timestamp", "value"})

    def test_real_load_enrichment_retains_minute_arrival_targets(self):
        station = next(row for row in self.service.arrival.catalog if row["stationId"] == "ST-DL-01")
        arrival = self.service.arrival.predict("ST-DL-01", "2026-05-05T00:37:00Z", 15)
        saved = deepcopy(arrival)
        service = RecommendationService(self.service.arrival, None, None, self.service.load)
        context = service.load_context(station, "2026-05-05T00:37:00Z", arrival["arrivalTime"], .3)
        self.assertTrue(context["loadForecast"]["meanPowerKw"] >= 0)
        self.assertEqual(arrival, saved)
        self.assertNotIn("expectedFree", context)
        self.assertNotIn("availableProbability", context)


if __name__ == "__main__":
    unittest.main()
