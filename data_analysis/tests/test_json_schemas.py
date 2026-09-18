"""Real Draft 2020-12 checks; this is schema validation, not content hash validation."""

import copy
import importlib.util
import json
from pathlib import Path
import unittest

from data_analysis.contracts import CONTRACT_VERSION, FEATURE_VERSION

ROOT = Path(__file__).resolve().parents[1]
HAS_JSONSCHEMA = importlib.util.find_spec("jsonschema") is not None
if HAS_JSONSCHEMA:
    from jsonschema import Draft202012Validator, FormatChecker


@unittest.skipUnless(HAS_JSONSCHEMA, "Install optional requirements-contract-test.txt for Draft 2020-12 checks")
class JsonSchemaTests(unittest.TestCase):
    def validator(self, filename):
        schema = json.loads((ROOT / "contracts" / filename).read_text(encoding="utf-8"))
        self.assertEqual(schema["$schema"], "https://json-schema.org/draft/2020-12/schema")
        Draft202012Validator.check_schema(schema)
        checker = FormatChecker()
        self.assertIn("date-time", checker.checkers, "Install rfc3339-validator; otherwise date-time is silently ignored")
        return Draft202012Validator(schema, format_checker=checker)

    def manifest(self):
        return json.loads((ROOT / "datasets/analytics_full_180d_v1/serving_manifest.json").read_text(encoding="utf-8"))

    def test_full_committed_manifest_matches_real_schema(self):
        manifest = self.manifest()
        self.validator("serving_manifest.schema.json").validate(manifest)
        self.assertEqual(len(manifest["tables"]), 10)
        self.assertEqual(manifest["tables"]["station_snapshot"]["rows"], 25)
        self.assertEqual(manifest["tables"]["station_hourly_metrics"]["rows"], 108000)

    def test_manifest_required_dates_types_paths_and_units(self):
        validator = self.validator("serving_manifest.schema.json")
        for key in validator.schema["required"]:
            value = self.manifest()
            value.pop(key)
            with self.subTest(missing=key):
                self.assertFalse(validator.is_valid(value))
        for path, bad in [(("generatedAt",), "not-a-date"), (("endDate",), "2026-02-30"),
                          (("businessTimezone",), "UTC"), (("periodStart",), "2025-12-01T00:00:00+08:00"),
                          (("tables", "cities", "rows"), True), (("tables", "cities", "rows"), -1),
                          (("tables", "cities", "files", 0, "path"), "../secret.csv.gz"),
                          (("tables", "cities", "files", 0, "sha256"), "not-a-hash"),
                          (("tables", "cities", "columns", 0, "unit"), 123)]:
            value = self.manifest()
            target = value
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = bad
            with self.subTest(path=path, value=bad):
                self.assertFalse(validator.is_valid(value))

    def model(self):
        return dict(schemaVersion=CONTRACT_VERSION, featureVersion=FEATURE_VERSION, modelId="fixture-model",
            modelVersion="fixture-only", target="load", datasetId="fixture-data", sourceManifestSha256="a"*64,
            trainingPublishedBatchId="fixture-batch", trainEndExclusive="2026-03-29T16:00:00Z",
            validationEndExclusive="2026-04-28T16:00:00Z", testEndExclusive="2026-05-28T16:00:00Z",
            historyHours=24, supportedHorizons=[1,6,24], featureColumns=["lag_power_kw_h01"],
            artifactFile="fixture-model.joblib", artifactSha256="b"*64,
            metrics=dict(mae=1.0, rmse=2.0, testSamples=10, unit="kW"), dependencies={"python":"3.11"})

    def test_model_metadata_valid_load_and_availability(self):
        validator = self.validator("model_metadata.schema.json")
        load = self.model()
        validator.validate(load)
        available = copy.deepcopy(load)
        available["target"] = "availability"
        available["metrics"]["unit"] = "chargers"
        validator.validate(available)

    def test_model_metadata_invalid_fixtures_are_rejected(self):
        validator = self.validator("model_metadata.schema.json")
        for changes in [{"modelVersion":""}, {"historyHours":23}, {"supportedHorizons":[True]},
                        {"supportedHorizons":[1,1]}, {"featureColumns":["label_power_kw_h01"]},
                        {"artifactFile":"../model.joblib"}, {"trainEndExclusive":"not-a-time"},
                        {"unexpected":1}, {"target":"availability"}]:
            with self.subTest(changes=changes):
                self.assertFalse(validator.is_valid(dict(self.model(), **changes)))
        for key in validator.schema["required"]:
            model = self.model()
            model.pop(key)
            self.assertFalse(validator.is_valid(model), key)

    def prediction(self):
        return dict(schemaVersion=CONTRACT_VERSION, featureVersion=FEATURE_VERSION, modelId="fixture-model",
            modelVersion="fixture-only", unit="kW", points=[dict(timestamp="2026-05-01T00:00:00Z", value=12.3)])

    def test_prediction_valid_and_invalid_fixtures(self):
        validator = self.validator("prediction_result.schema.json")
        validator.validate(self.prediction())
        for changes in [{"modelVersion":""}, {"schemaVersion":"0.0.0"}, {"unit":"Wh"},
                        {"points":[]}, {"points":self.prediction()["points"]*25}, {"unexpected":1}]:
            self.assertFalse(validator.is_valid(dict(self.prediction(), **changes)))
        for point in [dict(timestamp="not-a-time",value=1), dict(timestamp="2026-05-01T00:00:00Z",value=True),
                      dict(timestamp="2026-05-01T00:00:00Z",value="12"), dict(timestamp="2026-05-01T00:00:00Z",value=-1),
                      dict(timestamp="2026-05-01T00:00:00Z",value=1,extra=2)]:
            self.assertFalse(validator.is_valid(dict(self.prediction(), points=[point])))
        for key in validator.schema["required"]:
            value = self.prediction()
            value.pop(key)
            self.assertFalse(validator.is_valid(value), key)


if __name__ == "__main__":
    unittest.main()
