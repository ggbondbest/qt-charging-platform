"""Fail CI if HTTP schemas/types drift; stdlib checks require no HTTP runtime."""

import ast
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from data_analysis.contracts import CONTRACT_VERSION, FEATURE_VERSION
from data_analysis.contracts.model import ERROR_CODES, SUPPORTED_HORIZONS

ROOT = Path(__file__).resolve().parents[2]
CONTRACTS = ROOT / "data_analysis" / "contracts"
HAS_API = importlib.util.find_spec("fastapi") is not None


class GeneratedTypesTests(unittest.TestCase):
    def command(self, output):
        return [sys.executable, "-m", "data_analysis.contracts.generate_types", "--input",
                str(CONTRACTS / "openapi.json"), "--output", str(output), "--check"]

    def test_committed_types_match_generator_cli_check(self):
        output = CONTRACTS / "types.ts"
        self.assertTrue(output.is_file(), "Generate and commit contracts/types.ts")
        before = output.read_bytes()
        result = subprocess.run(self.command(output), cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(output.read_bytes(), before, "--check must not rewrite generated files")

    def test_types_check_rejects_missing_or_stale_output_without_writing(self):
        with tempfile.TemporaryDirectory(prefix="contract-drift-") as directory:
            output = Path(directory) / "types.ts"
            missing = subprocess.run(self.command(output), cwd=ROOT, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(missing.returncode, 0)
            self.assertFalse(output.exists(), "--check must not create a missing output")
            output.write_text("// intentionally stale fixture\n", encoding="utf-8")
            before = output.read_bytes()
            stale = subprocess.run(self.command(output), cwd=ROOT, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(stale.returncode, 0)
            self.assertEqual(output.read_bytes(), before, "--check must not fix stale output silently")

    def test_api_error_literals_are_in_shared_catalog(self):
        observed = set()
        for path in (ROOT / "data_analysis" / "backend").glob("*.py"):
            for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
                if not isinstance(node, ast.Call):
                    continue
                if isinstance(node.func, ast.Name) and node.func.id == "ApiError" and len(node.args) >= 2:
                    value = node.args[1]
                    if isinstance(value, ast.Constant) and isinstance(value.value, str):
                        observed.add(value.value)
                for keyword in node.keywords:
                    if keyword.arg == "code" and isinstance(keyword.value, ast.Constant):
                        observed.add(keyword.value.value)
        observed.discard("OK")
        self.assertTrue(observed)
        self.assertFalse(observed - ERROR_CODES, "Add API error codes to the shared catalog: " + repr(observed - ERROR_CODES))


@unittest.skipUnless(HAS_API, "Install optional requirements-api.txt to compare live OpenAPI")
class OpenApiDriftTests(unittest.TestCase):
    def test_live_schema_equals_committed_openapi(self):
        from data_analysis.backend.app import create_app
        from data_analysis.backend.database import SCHEMA_VERSION
        committed = json.loads((CONTRACTS / "openapi.json").read_text(encoding="utf-8"))
        actual = create_app().openapi()  # no database, HTTP requests or Spark needed
        self.assertEqual(SCHEMA_VERSION, CONTRACT_VERSION)
        self.assertEqual(actual["info"]["version"], CONTRACT_VERSION)
        self.assertEqual(actual, committed, "Regenerate contracts/openapi.json and types.ts from the current API")

    def test_prediction_request_and_model_identity_share_public_names(self):
        from data_analysis.backend.app import create_app
        components = create_app().openapi()["components"]["schemas"]
        request = components["PredictionRequest"]
        self.assertFalse(request["additionalProperties"])
        self.assertTrue({"stationId", "referenceTime", "modelId"}.issubset(request["required"]))
        self.assertNotIn("forecastStart", request["properties"])
        self.assertEqual(set(request["properties"]["horizonHours"]["enum"]), set(SUPPORTED_HORIZONS))
        published = components["PublishedModel"]
        metadata = json.loads((CONTRACTS / "model_metadata.schema.json").read_text(encoding="utf-8"))
        self.assertIn("modelVersion", published["required"])
        self.assertIn("modelVersion", metadata["required"])
        self.assertNotIn("version", published["properties"])
        self.assertEqual(set(published["properties"]["target"]["enum"]),
                         set(metadata["properties"]["target"]["enum"]))

    def test_future_success_payload_matches_public_result_schema(self):
        from data_analysis.backend.app import create_app
        from data_analysis.backend.models import PredictionResult
        components = create_app().openapi()["components"]["schemas"]
        public = json.loads((CONTRACTS / "prediction_result.schema.json").read_text(encoding="utf-8"))

        def normalize(schema):
            if isinstance(schema, list):
                return [normalize(item) for item in schema]
            if not isinstance(schema, dict):
                return schema
            if "$ref" in schema:
                return normalize(components[schema["$ref"].rsplit("/", 1)[-1]])
            result = {key: normalize(value) for key, value in schema.items()
                      if key not in {"title", "description", "$schema", "$id"}}
            # Pydantic adds a redundant primitive type next to const/enum.
            if "const" in result or "enum" in result:
                result.pop("type", None)
            for key in ("required", "enum"):
                if key in result:
                    result[key] = sorted(result[key])
            return result

        self.assertEqual(normalize(components["PredictionResult"]), normalize(public))
        valid = dict(schemaVersion=CONTRACT_VERSION, featureVersion=FEATURE_VERSION, modelId="model",
                     modelVersion="test", unit="kW", points=[dict(timestamp="2026-01-01T00:00:00Z", value=2)])
        PredictionResult.model_validate(valid)
        for value in (True, "2", -1, float("nan"), float("inf")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                PredictionResult.model_validate(dict(valid, points=[dict(timestamp="2026-01-01T00:00:00Z", value=value)]))


if __name__ == "__main__":
    unittest.main()
