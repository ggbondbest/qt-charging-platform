"""No live database/model/key is needed to validate the integrated API contract."""
import ast
import importlib.util
import json
from pathlib import Path
import unittest

from data_analysis.contracts.model import ERROR_CODES

ROOT = Path(__file__).resolve().parents[1]
HAS_API = all(importlib.util.find_spec(name) for name in ("fastapi", "httpx", "numpy"))


class DeliveryErrors(unittest.TestCase):
    def test_delivery_and_advisor_errors_are_public(self):
        found = set()
        for folder in (ROOT / "delivery", ROOT / "ml/advisor"):
            for path in folder.glob("*.py"):
                for node in ast.walk(ast.parse(path.read_text(encoding="utf8"))):
                    if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                            and node.func.id == "ApiError" and len(node.args) >= 2
                            and isinstance(node.args[1], ast.Constant)):
                        found.add(node.args[1].value)
        self.assertFalse(found - ERROR_CODES, repr(found - ERROR_CODES))


@unittest.skipUnless(HAS_API, "Install unified delivery dependencies")
class DeliveryContractTests(unittest.TestCase):
    def test_unified_schema_and_types_match_committed_files(self):
        from data_analysis.contracts.delivery_schema import artifacts
        for name, content in artifacts().items():
            with self.subTest(name=name):
                self.assertEqual((ROOT / "contracts" / name).read_text(encoding="utf8"), content)

    def test_advisor_is_typed_and_requires_batch_and_consent_contract(self):
        document = json.loads((ROOT / "contracts/delivery_openapi.json").read_text(encoding="utf8"))
        path = document["paths"]["/api/v1/intelligence/advisor"]
        request = path["post"]["requestBody"]["content"]["application/json"]["schema"]
        self.assertTrue({"question", "datasetId", "publishedBatchId"}.issubset(request["required"]))
        self.assertFalse(request["additionalProperties"])
        self.assertEqual(request["properties"]["question"]["maxLength"], 300)
        self.assertFalse(request["properties"]["consent"]["default"])
        self.assertEqual(request["properties"]["mode"]["default"], "offline")
        for verb in ("get", "post"):
            self.assertIn("$ref", path[verb]["responses"]["200"]["content"]["application/json"]["schema"])
        self.assertIn("AdvisorAnswer", document["components"]["schemas"])


if __name__ == "__main__":
    unittest.main()
