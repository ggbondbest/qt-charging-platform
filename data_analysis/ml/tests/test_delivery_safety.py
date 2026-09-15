"""Delivery regressions independent of another developer's ignored model directories."""
from __future__ import annotations
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

STACK = all(importlib.util.find_spec(name) for name in ("numpy", "pandas", "sklearn", "joblib", "pyarrow"))
if STACK:
    import numpy as np
    import pandas as pd
    from data_analysis.ml.common import artifacts, data_io, features, forecaster
    from data_analysis.ml.availability.predict import AvailabilityForecaster, PredictionError
    from data_analysis.contracts.model import PredictionContext


def metadata():
    return artifacts.build_metadata(model_id="avail-test", model_version="test-v1", target="availability",
        feature_version="history24-v1", dataset_id="test", source_manifest_sha256="a" * 64,
        training_published_batch_id="test-batch", splits={"trainEnd": "2026-03-01", "validationEnd": "2026-04-01", "end": "2026-05-01"},
        feature_columns=["hour_of_day"], artifact_file="model.joblib", artifact_sha256="0" * 64,
        supported_horizons=[1], metrics={"mae": 0.5, "rmse": 0.7, "testSamples": 10, "unit": "chargers"})


@unittest.skipUnless(STACK, "availability scientific stack unavailable")
class ArtifactSafetyTests(unittest.TestCase):
    def make_bundle(self, directory):
        artifacts.save_bundle(directory, {"model": "test-fixture"}, metadata(), {"pooledTest": {"mae": 0.5}})

    def test_hash_is_checked_before_any_deserialization(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.make_bundle(directory)
            (directory / "model.joblib").write_bytes(b"damaged")
            with mock.patch.object(artifacts.joblib, "load") as loader:
                with self.assertRaises(artifacts.MetadataError):
                    artifacts.load_bundle(directory)
                loader.assert_not_called()

    def test_path_and_version_are_checked_before_deserialization(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.make_bundle(directory)
            source = json.loads((directory / artifacts.METADATA_NAME).read_text())
            variants = [dict(source, artifactFile="../model.joblib"),
                        dict(source, dependencies={**source["dependencies"], "scikit-learn": "0.0"}),
                        dict(source, dependencies={**source["dependencies"], "python": "2.7.0"}),
                        dict(source, featureColumns=["label_available_count_h01"])]
            for variant in variants:
                (directory / artifacts.METADATA_NAME).write_text(json.dumps(variant))
                with mock.patch.object(artifacts.joblib, "load") as loader:
                    with self.assertRaises(artifacts.MetadataError):
                        artifacts.load_bundle(directory)
                    loader.assert_not_called()

    def test_trusted_bundle_and_report_share_actual_hash(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.make_bundle(directory)
            bundle, description = artifacts.load_bundle(directory)
            self.assertEqual(bundle["model"], "test-fixture")
            report = json.loads((directory / artifacts.REPORT_NAME).read_text())
            self.assertEqual(report["artifactSha256"], description["artifactSha256"])

    def test_foreign_export_cannot_be_used_to_relabel_a_model(self):
        descriptor = metadata()
        frame = SimpleNamespace(export=SimpleNamespace(dataset_id="test", published_batch_id="another-batch",
            source_manifest_sha256="a" * 64, feature_version="history24-v1"), feature_columns=["hour_of_day"])
        with self.assertRaisesRegex(ValueError, "verified export batch"):
            forecaster.validate_bundle_frame({}, descriptor, frame)

    def test_target_feature_version_is_checked_before_joined_features(self):
        features_frame = pd.DataFrame({"station_id": ["S"], "reference_time": ["2026-01-01T00:00:00Z"],
                                       "feature_version": ["history24-v1"]})
        targets = features_frame.assign(feature_version="stale-target-contract")
        export = SimpleNamespace(feature_version="history24-v1",
            table=mock.Mock(side_effect=[features_frame, targets, pd.DataFrame()]))
        with mock.patch.object(forecaster, "open_export", return_value=export):
            with self.assertRaisesRegex(ValueError, "feature_version"):
                forecaster.build_frame("unused")


@unittest.skipUnless(STACK, "availability scientific stack unavailable")
class CacheSafetyTests(unittest.TestCase):
    def prepare(self, directory):
        shard = directory / "part.csv"
        shard.write_text("value\n1\n")
        manifest = {"publishedBatchId": "test", "tables": {"small": {"rows": 1,
            "columns": [{"name": "value", "type": "integer", "nullable": False}],
            "files": [{"path": "part.csv", "bytes": shard.stat().st_size,
                       "sha256": hashlib.sha256(shard.read_bytes()).hexdigest()}]}}}
        (directory / "serving_manifest.json").write_text(json.dumps(manifest))
        return data_io.Export(directory, manifest, "a" * 64), shard

    def test_cache_hit_still_checks_source_shard_hash(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            export, shard = self.prepare(directory)
            with mock.patch.object(data_io, "CACHE_ROOT", directory / "cache"):
                self.assertEqual(export.table("small").value.tolist(), [1])
                shard.write_text("value\n2\n")  # same size, different digest
                with self.assertRaisesRegex(data_io.ExportError, "sha256"):
                    export.table("small")

    def test_modified_parquet_is_rebuilt_from_verified_source(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            export, _ = self.prepare(directory)
            cache_dir = directory / "cache"
            with mock.patch.object(data_io, "CACHE_ROOT", cache_dir):
                export.table("small")
                cache = next(cache_dir.glob("*.parquet"))
                pd.DataFrame({"value": [99]}).to_parquet(cache)
                self.assertEqual(export.table("small").value.tolist(), [1])

    def test_no_cache_mode_does_not_create_cache(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            export, _ = self.prepare(directory)
            with mock.patch.object(data_io, "CACHE_ROOT", directory / "cache"):
                export.table("small", use_cache=False)
                self.assertFalse((directory / "cache").exists())

    def test_shard_cannot_escape_export(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            export, _ = self.prepare(directory)
            export.manifest["tables"]["small"]["files"][0]["path"] = "../outside.csv"
            with self.assertRaisesRegex(data_io.ExportError, "escapes"):
                export.table("small")


@unittest.skipUnless(STACK, "availability scientific stack unavailable")
class ServingSafetyTests(unittest.TestCase):
    def setUp(self):
        self.reference = "2026-05-01T00:00:00Z"
        self.history = [{"recorded_at": stamp, "station_id": "S", "capacity": 2, "is_complete": True,
                         "mean_power_kw": 1.0, "end_available_count": 1}
                        for stamp in pd.date_range("2026-04-30", periods=24, freq="h", tz="UTC")]
        self.profile = SimpleNamespace(stations={"S": {"capacity": 2, "city_id": "DL", "rated_capacity_kw": 10.0}},
            feature_columns=["hour_of_day"], calendar={}, cities=["DL"])

    def test_cross_station_history_is_rejected(self):
        history = [dict(row, station_id="another-station") for row in self.history]
        with self.assertRaisesRegex(features.HistoryError, "different station"):
            forecaster.online_feature_row(self.profile, self.reference, "S", history)

    def test_cross_capacity_history_is_rejected(self):
        history = [dict(row, capacity=3) for row in self.history]
        with self.assertRaisesRegex(features.HistoryError, "capacity"):
            forecaster.online_feature_row(self.profile, self.reference, "S", history)

    def test_fractional_inventory_is_invalid(self):
        self.history[0]["end_available_count"] = 0.5
        with self.assertRaises(features.HistoryError):
            forecaster.online_feature_row(self.profile, self.reference, "S", self.history)

    def predictor(self):
        description = metadata()
        from data_analysis.ml.availability.model import OrdinalForecastModel
        model = OrdinalForecastModel(classes=np.arange(4), steps={1: None}, feature_columns=["hour_of_day"],
                                     capacity=3, interval_levels={1: 0.8})
        model.distribution_for = mock.Mock(return_value=np.array([[0.1, 0.2, 0.3, 0.4]]))
        bundle = {"model": model, "target": "availability", "modelId": "avail-test", "modelVersion": "test-v1",
                  "featureVersion": "history24-v1", "featureColumns": ["hour_of_day"], "horizonHours": 1,
                  "datasetId": "test", "publishedBatchId": "test-batch", "stations": self.profile.stations,
                  "cities": ["DL"], "calendar": {}}
        return AvailabilityForecaster(bundle, description)

    def context(self, **changes):
        values = dict(dataset_id="test", published_batch_id="test-batch", station_id="S", reference_time=self.reference,
                      horizon_hours=1, model_id="avail-test")
        return PredictionContext(**{**values, **changes})

    def test_risk_rejects_wrong_horizon_and_model(self):
        predictor = self.predictor()
        for context in (self.context(horizon_hours=6), self.context(model_id="another-model")):
            with self.assertRaises(PredictionError):
                predictor.risk(self.history, context)

    def test_risk_and_point_respect_station_capacity(self):
        predictor = self.predictor()
        result = predictor.predict(self.history, self.context())
        hour = predictor.risk(self.history, self.context())["hours"][0]
        self.assertLessEqual(result["points"][0]["value"], 2)
        self.assertEqual(hour["distribution"]["3"], 0)
        self.assertLessEqual(hour["intervalHigh"], 2)
        self.assertAlmostEqual(sum(hour["distribution"].values()), 1)

    def test_incomplete_debug_bundle_cannot_serve(self):
        predictor = self.predictor()
        predictor.bundle["model"].steps[2] = None
        with self.assertRaisesRegex(PredictionError, "partial debug"):
            AvailabilityForecaster(predictor.bundle, predictor.metadata)

    def test_missing_zero_class_or_invalid_calibration_cannot_serve(self):
        for change in ("classes", "interval_levels"):
            predictor = self.predictor()
            setattr(predictor.model, change, np.array([1, 2, 3]) if change == "classes" else {1: float("nan")})
            with self.assertRaises(PredictionError):
                AvailabilityForecaster(predictor.bundle, predictor.metadata)


if __name__ == "__main__":
    unittest.main()
