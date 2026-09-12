"""Shared contracts are usable without Spark, FastAPI or an ML framework."""

import copy
import json
from pathlib import Path
import unittest

from data_analysis.contracts import CONTRACT_VERSION, FEATURE_VERSION
from data_analysis.contracts.generate_types import generate
from data_analysis.contracts.model import PredictionContext, validate_prediction
from data_analysis.contracts.serving import convert_csv_value, parse_utc, validate_table_contract


class ContractTests(unittest.TestCase):
    def test_typed_csv_nulls_dates_and_finite_values(self):
        col = {"name": "value", "type": "integer", "nullable": False}
        self.assertEqual(convert_csv_value("0", col), 0)
        for bad in ["", "1.5", "1e2", "NaN", "-0.5", str(2**63)]:
            with self.subTest(value=bad), self.assertRaises(ValueError):
                convert_csv_value(bad, col)
        self.assertIsNone(convert_csv_value("", dict(col, nullable=True)))
        for bad in ["NaN", "Infinity", "-inf"]:
            with self.assertRaises(ValueError):
                convert_csv_value(bad, dict(col, type="number"))
        with self.assertRaises(ValueError):
            convert_csv_value("2025-02-30", dict(col, type="date"))
        self.assertEqual(convert_csv_value("true", dict(col, type="boolean")), 1)

    def test_utc_contract_rejects_ambiguous_dates(self):
        self.assertEqual(parse_utc("2026-01-01T00:00:00Z").hour, 0)
        for value in ["2026-01-01Z", "2026-01-01 00:00:00Z", "2026-01-01T00:00:00+08:00", "bad"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                parse_utc(value)

    def context(self, **changes):
        fields = dict(dataset_id="data", published_batch_id="batch", station_id="ST-BJ-01",
                      reference_time="2026-05-01T00:00:00Z", horizon_hours=1, model_id="rf")
        return PredictionContext(**dict(fields, **changes))

    def test_request_requires_hour_and_integer_horizon(self):
        for changes in [{"horizon_hours": True}, {"horizon_hours": 1.0}, {"horizon_hours": 2},
                        {"reference_time": "2026-05-01T00:01:00Z"}]:
            with self.assertRaises(ValueError):
                self.context(**changes)

    def test_model_adapter_identity_units_and_time_are_checked(self):
        result = {"schemaVersion": CONTRACT_VERSION, "featureVersion": FEATURE_VERSION,
                  "modelId": "rf", "modelVersion": "test", "unit": "kW",
                  "points": [{"timestamp": "2026-05-01T00:00:00Z", "value": 20.0}]}
        self.assertIs(validate_prediction(result, self.context(), 120), result)
        for changes in [{"unit": "Wh"}, {"modelId": "lstm"}, {"modelVersion": ""}, {"extra": 1},
                        {"points": []}, {"points": [{"timestamp": "2026-05-01T01:00:00Z", "value": 1}]}]:
            with self.assertRaises(ValueError):
                validate_prediction(dict(result, **changes), self.context(), 120)
        for value in [True, -1, float("nan"), 121]:
            changed = copy.deepcopy(result)
            changed["points"][0]["value"] = value
            with self.assertRaises(ValueError):
                validate_prediction(changed, self.context(), 120)

    def test_table_contract_rejects_identifier_injection(self):
        table = {"primaryKey": ["city_id"], "columns": [
            {"name": "city_id", "type": "string", "nullable": False}], "rows": 1}
        validate_table_contract("cities", table)
        changed = copy.deepcopy(table)
        changed["columns"][0]["name"] = 'x";DROP TABLE cities;--'
        with self.assertRaises(ValueError):
            validate_table_contract("cities", changed)

    def test_openapi_types_and_request_do_not_drift(self):
        root = Path(__file__).parents[1] / "contracts"
        schema = json.loads((root / "openapi.json").read_text(encoding="utf-8"))
        request = schema["components"]["schemas"]["PredictionRequest"]
        self.assertIn("referenceTime", request["required"])
        self.assertIn("modelId", request["required"])
        self.assertNotIn("forecastStart", request["properties"])
        self.assertEqual(schema["info"]["version"], CONTRACT_VERSION)
        self.assertEqual((root / "types.ts").read_text(encoding="utf-8"), generate(schema))


if __name__ == "__main__":
    unittest.main()
