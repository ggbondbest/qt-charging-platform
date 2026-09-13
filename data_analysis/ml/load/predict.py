"""Contract-compatible online predictor for the load task.

Implements ``Predictor`` from data_analysis.contracts.model:
    predict(history: list[dict], context: PredictionContext) -> dict

``history`` holds the 24 raw station hours (station_hourly_metrics fields)
strictly before ``context.reference_time``; every model input is rebuilt here
from that window, so the offline/online feature transformation is one and the
same code path (audited by prepare_data and by the smoke test below).

Smoke test (repo root, after training):
    python -m data_analysis.ml.load.predict
"""

from __future__ import annotations

import json
import sys
from datetime import timedelta

import joblib
import numpy as np
import pandas as pd

from data_analysis.contracts.model import (
    PredictionContext,
    validate_prediction,
)

from . import common


class LoadForecastPredictor:
    """Loads one trusted bundle; never evaluates arbitrary pickle."""

    def __init__(self, bundle_path=None):
        bundle_path = bundle_path or common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load" / f"{common.MODEL_ID}.joblib"
        bundle = joblib.load(bundle_path)
        self.models = bundle["models"]
        self.feature_columns = bundle["feature_columns"]
        self.calendar = bundle["calendar"]
        self.model_id = bundle["model_id"]
        self.model_version = bundle["model_version"]
        self.metadata = bundle["metadata"]
        self._levels = bundle["category_levels"]

    def _matrix(self, feature_row: dict) -> pd.DataFrame:
        matrix = pd.DataFrame([feature_row], columns=self.feature_columns)
        for column in common.CATEGORICAL_FEATURES:
            matrix[column] = pd.Categorical(
                matrix[column], categories=self._levels[column]
            )
        return matrix

    def predict(self, history: list[dict], context: PredictionContext) -> dict:
        row = common.build_feature_row(history, context.reference_time, self.calendar)
        matrix = self._matrix(row)
        ref = common.parse_utc(context.reference_time)
        capacity_limit = row["rated_capacity_kw"]
        points = []
        for offset in range(context.horizon_hours):
            horizon = offset + 1
            value = float(self.models[horizon].predict(matrix)[0])
            points.append(
                {
                    "timestamp": common.format_utc(ref + timedelta(hours=offset)),
                    "value": float(np.clip(value, 0.0, capacity_limit)),
                }
            )
        result = {
            "schemaVersion": self.metadata.get("schemaVersion", "1.0.0"),
            "featureVersion": self.metadata["featureVersion"],
            "modelId": self.model_id,
            "modelVersion": self.model_version,
            "unit": "kW",
            "points": points,
        }
        return validate_prediction(result, context, capacity_limit, task="load")


def _smoke_test() -> int:
    """Serve 3 random VALIDATION windows through the predictor and compare
    against the offline table-feature path, then validate the contract end to
    end. TEST rows are never touched."""
    out = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
    frame = pd.read_pickle(out / "joined_usable.pkl")
    pool = frame[frame["split_24h"] == "VALIDATION"].reset_index(drop=True)
    hourly = common.load_hourly_metrics()
    hourly_by_station = {k: g for k, g in hourly.groupby("station_id")}
    predictor = LoadForecastPredictor()

    sample = pool.sample(n=3, random_state=11)
    max_diff = 0.0
    for row in sample.itertuples(index=False):
        station_hours = hourly_by_station[row.station_id]
        window = station_hours[
            (station_hours["recorded_at"] >= row.reference_dt - pd.Timedelta(hours=24))
            & (station_hours["recorded_at"] < row.reference_dt)
        ]
        if len(window) != 24:
            print(f"skip {row.station_id} {row.reference_time}: history window incomplete")
            continue
        history = [
            {
                "station_id": rec.station_id,
                "city_id": rec.city_id,
                "recorded_at": common.format_utc(rec.recorded_at.to_pydatetime()),
                "mean_power_kw": float(rec.mean_power_kw),
                "capacity": int(rec.capacity),
                "rated_capacity_kw": float(row.rated_capacity_kw),
                "end_available_count": int(rec.end_available_count),
            }
            for rec in window.itertuples(index=False)
        ]
        context = PredictionContext(
            dataset_id=predictor.metadata["datasetId"],
            published_batch_id=predictor.metadata["trainingPublishedBatchId"],
            station_id=row.station_id,
            reference_time=row.reference_time,
            horizon_hours=24,
            model_id=predictor.model_id,
        )
        result = predictor.predict(history, context)

        built = common.build_feature_row(history, row.reference_time, predictor.calendar)
        for column in common.NUMERIC_FEATURES:
            diff = abs(float(built[column]) - float(getattr(row, column)))
            if diff > 1e-9:
                print(f"FEATURE MISMATCH {row.station_id} {row.reference_time} {column}: {diff}", file=sys.stderr)
                return 1
        offline = predictor._matrix(
            {column: getattr(row, column) for column in common.FEATURE_COLUMNS}
        )
        offline_cap = float(getattr(row, "rated_capacity_kw"))
        for offset in range(24):
            # online points are clipped to [0, rated_capacity_kw]; compare like for like
            offline_value = float(
                np.clip(predictor.models[offset + 1].predict(offline)[0], 0.0, offline_cap)
            )
            max_diff = max(max_diff, abs(offline_value - result["points"][offset]["value"]))
        print(f"OK {row.station_id} {row.reference_time} horizon24 last={result['points'][-1]['value']:.2f} kW")

    if max_diff > 1e-9:
        print(f"OFFLINE/ONLINE MISMATCH: max abs diff {max_diff}", file=sys.stderr)
        return 1
    print(f"contract + offline/online parity passed (max diff {max_diff:.2e})")
    return 0


if __name__ == "__main__":
    raise SystemExit(_smoke_test())
