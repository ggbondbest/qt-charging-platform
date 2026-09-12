"""Pure-Python model handoff contract; no ML library or fake inference required."""

from dataclasses import dataclass
from datetime import timedelta
from math import isfinite
from typing import Protocol

from data_analysis.contracts import CONTRACT_VERSION, FEATURE_VERSION
from data_analysis.contracts.serving import parse_utc

SUPPORTED_HORIZONS = (1, 6, 24)
HISTORY_HOURS = 24
ERROR_CODES = {
    "INVALID_REQUEST", "DATASET_NOT_FOUND", "BATCH_MISMATCH", "STATION_NOT_FOUND",
    "DATA_NOT_READY", "HISTORY_TOO_SHORT", "MODEL_NOT_READY", "MODEL_INCOMPATIBLE",
    "UNSUPPORTED_HORIZON", "INTERNAL_ERROR",
    "INVALID_ARGUMENT", "FILTER_MISMATCH", "CITY_NOT_FOUND", "DATE_OUT_OF_RANGE",
    "INVALID_DATE_RANGE", "INVALID_SORT", "DATA_UNAVAILABLE", "NOT_FOUND",
    "METHOD_NOT_ALLOWED", "HTTP_ERROR",
}


@dataclass(frozen=True)
class PredictionContext:
    dataset_id: str
    published_batch_id: str
    station_id: str
    reference_time: str
    horizon_hours: int
    model_id: str

    def __post_init__(self):
        for value in (self.dataset_id, self.published_batch_id, self.station_id, self.model_id):
            if not isinstance(value, str) or not value.strip() or len(value) > 128:
                raise ValueError("Prediction identifiers must contain 1-128 characters")
        stamp = parse_utc(self.reference_time)
        if stamp.minute or stamp.second or stamp.microsecond:
            raise ValueError("referenceTime must be an exact hour boundary")
        if type(self.horizon_hours) is not int or self.horizon_hours not in SUPPORTED_HORIZONS:
            raise ValueError("horizonHours must be 1, 6 or 24")


class Predictor(Protocol):
    def predict(self, history: list[dict], context: PredictionContext) -> dict:
        """Return all consecutive hourly points, not just the last lead time.

        history contains 24 complete station hours strictly before reference_time.
        The first predicted interval is [reference_time, reference_time + 1h).
        Model-specific transformations are loaded with the trusted model artifact.
        """
        ...


def validate_prediction(result, context, capacity_limit, task="load"):
    """capacity_limit is station kW for load, charger COUNT for availability."""
    if task not in {"load", "availability"}:
        raise ValueError("Unsupported prediction task")
    required = {"schemaVersion", "featureVersion", "modelId", "modelVersion", "unit", "points"}
    if not isinstance(result, dict) or set(result) != required:
        raise ValueError("Model result fields do not match the contract")
    if result["modelId"] != context.model_id or not isinstance(result["modelVersion"], str) or not result["modelVersion"].strip():
        raise ValueError("Model result identity/version mismatch")
    if type(capacity_limit) not in {int, float} or not isfinite(capacity_limit) or capacity_limit <= 0:
        raise ValueError("Station capacity must be finite and positive")
    if result.get("schemaVersion") != CONTRACT_VERSION:
        raise ValueError("Model result contract mismatch")
    if result.get("featureVersion") != FEATURE_VERSION:
        raise ValueError("Feature contract mismatch")
    expected_unit = "kW" if task == "load" else "chargers"
    if result.get("unit") != expected_unit:
        raise ValueError("Prediction unit mismatch")
    points = result.get("points", [])
    if len(points) != context.horizon_hours:
        raise ValueError("One prediction point is required for each requested hour")
    start = parse_utc(context.reference_time)
    for index, point in enumerate(points):
        if not isinstance(point, dict) or set(point) != {"timestamp", "value"}:
            raise ValueError("Unexpected prediction point fields")
        if parse_utc(point["timestamp"]) != start + timedelta(hours=index):
            raise ValueError("Prediction timestamps are not consecutive interval starts")
        value = point["value"]
        if type(value) not in {int, float} or not isfinite(value) or not 0 <= value <= capacity_limit:
            raise ValueError("Prediction is not finite or exceeds the configured capacity")
    return result
