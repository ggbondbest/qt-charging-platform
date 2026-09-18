"""Contract inference for the availability model.

    python -m data_analysis.ml.availability.predict \
        --bundle data_analysis/outputs/ml_avail_run6/h06 \
        --station ST-BJ-01 --reference-time 2026-05-20T00:00:00Z --horizon 6

    python -m data_analysis.ml.availability.predict --self-check \
        --run-dir data_analysis/outputs/ml_avail_run6

``AvailabilityForecaster.predict`` is the ``Predictor`` the backend adapter is supposed to call:
it receives the 24 complete station hours strictly before ``reference_time``, rebuilds the
features from those rows exactly as the Spark job did, and returns one point per requested hour
in ``contracts/model.py`` shape.  ``risk`` is the side channel the dashboard can use for the
"预计无桩可用" signal without changing the public point-prediction contract.

``--self-check`` is the 「最小推理测试」 the handoff asks for: it loads every bundle of a run,
serves one deterministic TEST row through the public contract, and exits non-zero unless each
answer is exactly ``horizon`` consecutive hourly whole charger counts inside ``[0, capacity]``.
"""

from __future__ import annotations

import argparse
import json
from datetime import timedelta
from pathlib import Path

import numpy as np
import pandas as pd

from data_analysis.contracts.model import PredictionContext, validate_prediction
from data_analysis.ml.availability.model import point_value
from data_analysis.ml.availability.prior import site_key_of
from data_analysis.ml.common import artifacts, forecaster
from data_analysis.ml.common.data_io import DEFAULT_EXPORT

CAPACITY_TASK = "availability"


class PredictionError(ValueError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


class AvailabilityForecaster:
    def __init__(self, bundle: dict, metadata: dict):
        self.bundle = bundle
        self.metadata = metadata
        self.model = bundle["model"]
        fields = {"modelId": "modelId", "modelVersion": "modelVersion", "target": "target",
                  "featureVersion": "featureVersion", "featureColumns": "featureColumns",
                  "datasetId": "datasetId", "publishedBatchId": "trainingPublishedBatchId"}
        if any(bundle.get(key) != metadata.get(value) for key, value in fields.items()):
            raise PredictionError("MODEL_INCOMPATIBLE", "bundle identity differs from metadata")
        horizon = bundle.get("horizonHours")
        if (type(horizon) is not int or horizon not in (1, 6, 24) or
                metadata.get("supportedHorizons") != [horizon] or
                set(self.model.steps) != set(range(1, horizon + 1))):
            raise PredictionError("MODEL_INCOMPATIBLE", "bundle must contain every requested hour, no partial debug models")
        if set(self.model.interval_levels) != set(range(1, horizon + 1)):
            raise PredictionError("MODEL_INCOMPATIBLE", "bundle has not calibrated every prediction hour")
        levels = list(self.model.interval_levels.values())
        if (not np.isfinite(levels).all() or any(not 0 < value <= 1 for value in levels) or
                not 0 < self.model.nominal_coverage < 1):
            raise PredictionError("MODEL_INCOMPATIBLE", "invalid calibrated interval levels")
        classes = np.asarray(self.model.classes)
        if (classes.ndim != 1 or not len(classes) or not np.isfinite(classes).all() or
                np.any(classes < 0) or np.any(classes % 1 != 0) or classes[0] != 0 or
                np.any(np.diff(classes) <= 0)):
            raise PredictionError("MODEL_INCOMPATIBLE", "charger classes must be unique sorted nonnegative integers")

    @classmethod
    def load(cls, directory) -> "AvailabilityForecaster":
        bundle, metadata = artifacts.load_bundle(directory)
        if bundle.get("target") != "availability":
            raise PredictionError("MODEL_INCOMPATIBLE", f"{directory} is not an availability model")
        return cls(bundle, metadata)

    @property
    def horizon_hours(self) -> int:
        return int(self.bundle["horizonHours"])

    def bundle_profile(self) -> "_Profile":
        """The frozen station/calendar/city profile, for callers that build features themselves."""
        return _Profile(
            feature_columns=self.bundle["featureColumns"],
            cities=self.bundle["cities"],
            calendar=self.bundle["calendar"],
            stations=self.bundle["stations"],
        )

    def _features(self, history: list[dict], context: PredictionContext) -> pd.DataFrame:
        if context.horizon_hours != self.horizon_hours:
            raise PredictionError("UNSUPPORTED_HORIZON", "request horizon differs from this model")
        if context.model_id != self.metadata["modelId"]:
            raise PredictionError("MODEL_INCOMPATIBLE", "request model identity mismatch")
        station = self.bundle["stations"].get(context.station_id)
        if station is None:
            raise PredictionError("STATION_NOT_FOUND", f"station {context.station_id} is not in this batch")
        if context.dataset_id != self.bundle["datasetId"] or context.published_batch_id != self.bundle["publishedBatchId"]:
            raise PredictionError("BATCH_MISMATCH", "the served batch differs from the trained batch")
        row = forecaster.online_feature_row(
            self.bundle_profile(),  # lightweight stand-in so the shared builder is used verbatim
            context.reference_time,
            context.station_id,
            history,
        )
        return pd.DataFrame([row], columns=self.bundle["featureColumns"])

    def _distribution(self, x, step: int, keys, capacity: int) -> np.ndarray:
        probability = np.array(self.model.distribution_for(x, step, keys), dtype=float, copy=True)
        classes = np.asarray(self.model.classes)
        if (probability.shape != (1, len(classes)) or not np.isfinite(probability).all() or
                np.any(probability < 0) or np.any(classes < 0) or np.any(classes % 1 != 0)):
            raise PredictionError("MODEL_INCOMPATIBLE", "invalid charger probability distribution")
        # Different station capacities must constrain the risk channel as well as the point value.
        probability[:, classes > capacity] = 0
        total = probability.sum(axis=1, keepdims=True)
        if np.any(total <= 0):
            raise PredictionError("MODEL_INCOMPATIBLE", "model has no probability on legal charger counts")
        return probability / total

    def _keys(self, x: pd.DataFrame, context: PredictionContext) -> "pd.DataFrame | None":
        """Lookup keys for the hierarchical prior: the prior is keyed on the *reference* hour and
        shifts it per step internally, so one row serves every step of a request."""
        if not getattr(self.model, "hierarchical", False):
            return None
        station = self.bundle["stations"][context.station_id]
        # A 0.3.0 bundle records the site label it trained with; older ones only have the one-hots,
        # from which site_key_of() recovers the same string.
        site_key = self.bundle.get("stationSiteKeys", {}).get(context.station_id) or site_key_of(station)
        return pd.DataFrame([{"station_id": context.station_id,
                              "hour_of_day": int(x["hour_of_day"].iloc[0]),
                              "site_key": site_key,
                              "city_id": station["city_id"]}])

    def predict(self, history: list[dict], context: PredictionContext) -> dict:
        """Return the contracted point forecast: a whole number of free chargers."""
        if context.horizon_hours != self.horizon_hours:
            raise PredictionError(
                "UNSUPPORTED_HORIZON",
                f"{self.metadata['modelId']} serves {self.horizon_hours}h, not {context.horizon_hours}h",
            )
        x = self._features(history, context)
        keys = self._keys(x, context)
        start = pd.Timestamp(context.reference_time)
        capacity = int(self.bundle["stations"][context.station_id]["capacity"])
        points = []
        for step in range(1, self.horizon_hours + 1):
            # Free chargers are whole units: serve the distribution's point value as an int, clamped
            # into the contracted range rather than trusting the model to stay inside 0..capacity.
            probability = self._distribution(x, step, keys, capacity)
            value = int(point_value(self.model.classes, probability, getattr(self.model, "point_rule", "mode"))[0])
            points.append({"timestamp": (start + timedelta(hours=step - 1)).strftime("%Y-%m-%dT%H:%M:%SZ"),
                           "value": max(0, min(capacity, value))})
        result = {
            "schemaVersion": self.metadata["schemaVersion"],
            "featureVersion": self.metadata["featureVersion"],
            "modelId": self.metadata["modelId"],
            "modelVersion": self.metadata["modelVersion"],
            "unit": "chargers",
            "points": points,
        }
        return validate_prediction(result, context, float(capacity), task=CAPACITY_TASK)

    def risk(self, history: list[dict], context: PredictionContext) -> dict:
        """Per-hour depletion probability and the calibrated shortest central interval."""
        x = self._features(history, context)
        keys = self._keys(x, context)
        rule = getattr(self.model, "point_rule", "mode")
        zero = int(np.where(self.model.classes == 0)[0][0])
        start = pd.Timestamp(context.reference_time)
        capacity = int(self.bundle["stations"][context.station_id]["capacity"])
        rows = []
        for step in range(1, self.horizon_hours + 1):
            # One pmf per hour, everything else derived from it, so the displayed median, interval
            # and distribution can never disagree with each other.
            probability = self._distribution(x, step, keys, capacity)
            low, high = self.model.interval_for(probability, self.model.interval_levels[step])
            rows.append({
                "timestamp": (start + timedelta(hours=step - 1)).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "expectedChargers": round(float(probability[0] @ self.model.classes), 3),
                "medianChargers": int(point_value(self.model.classes, probability, rule)[0]),
                "probabilityDepleted": round(float(probability[0][zero]), 4),
                "intervalLevel": self.model.interval_levels[step],
                "intervalLow": int(low[0]),
                "intervalHigh": int(high[0]),
                "distribution": _rounded_distribution(self.model.classes, probability[0]),
            })
        return {
            "modelId": self.metadata["modelId"],
            "modelVersion": self.metadata["modelVersion"],
            "stationId": context.station_id,
            "referenceTime": context.reference_time,
            "unit": "chargers",
            "pointRule": rule,
            "definition": "空闲桩数为整数；expectedChargers 是分布期望，页面必须标注为预计值，medianChargers 才是可展示的整数桩数",
            "hours": rows,
        }


def _rounded_distribution(classes, masses) -> dict[str, float]:
    """Round per-class probabilities for display without breaking the sum-to-one invariant."""
    rounded = [round(float(mass), 4) for mass in masses]
    residual = round(1.0 - sum(rounded), 4)
    if residual:
        largest = max(range(len(rounded)), key=rounded.__getitem__)
        rounded[largest] = round(rounded[largest] + residual, 4)
    return {str(int(value)): mass for value, mass in zip(classes, rounded)}


class _Profile:
    """Attribute holder so :func:`online_feature_row` can be reused without a full frame."""

    def __init__(self, *, feature_columns, cities, calendar, stations):
        self.feature_columns = feature_columns
        self.cities = cities
        self.calendar = calendar
        self.stations = stations


def _window(hourly: pd.DataFrame, station_id: str, start: pd.Timestamp) -> list[dict]:
    """The 24 complete station hours strictly before ``start`` - all a request is allowed to see.

    Order is not normalised here because :func:`ml.common.features.derive_features` sorts and then
    proves contiguity itself, which is the check that refuses a history with a hole in it.
    """
    return hourly[(hourly["station_id"] == station_id)
                  & (hourly["recorded_at"] >= start - pd.Timedelta(hours=24))
                  & (hourly["recorded_at"] < start)].to_dict("records")


def _history_from_export(export_dir, station_id: str, reference_time: str, bundle: dict) -> list[dict]:
    del bundle  # the export is the authority on history; the bundle only states which features to build
    frame = forecaster.build_frame(export_dir)
    return _window(frame.hourly, station_id, pd.Timestamp(reference_time))


def _check_one(frame, directory: Path, *, sample_index: int) -> dict:
    """Serve one bundle once and report what the public contract made of it: PASS / FAIL / SKIPPED.

    The checks are the delivery claims, run against the *served* values rather than the stored
    metrics: the artefact loads and its hash matches its metadata, the batch being served is the
    batch it was trained on, and the answer is a whole number of free chargers per consecutive
    hour, clamped inside the station's own capacity.
    """
    record: dict = {"bundle": str(directory)}
    try:
        predictor = AvailabilityForecaster.load(directory)
    except Exception as error:  # a bundle that will not load is a failure of this test, not a crash in it
        return {**record, "status": "FAIL", "error": f"{type(error).__name__}: {error}"}
    bundle, metadata = predictor.bundle, predictor.metadata
    horizon = predictor.horizon_hours
    record.update({"modelId": metadata["modelId"], "horizonHours": horizon})
    if bundle.get("excludeCity"):
        # A cold-start bundle never saw this city; serving it there is not a supported path, so
        # reporting it as a failure would hide the failures that matter.
        return {**record, "status": "SKIPPED", "reason": f"holdout trained without {bundle['excludeCity']}"}

    rows = frame.data.loc[frame.data[frame.export.split_column(horizon)] == "TEST"]
    row = rows.iloc[min(sample_index, len(rows) - 1)]
    station_id = str(row["station_id"])
    start = pd.Timestamp(row["reference_time"])
    capacity = int(bundle["stations"][station_id]["capacity"])
    record.update({"stationId": station_id, "referenceTime": start.strftime("%Y-%m-%dT%H:%M:%SZ"),
                   "capacity": capacity})
    # The batch identifiers come from the export being served, not from the bundle: a model trained
    # on another published batch has to be refused here, which is the binding this test exists for.
    context = PredictionContext(
        dataset_id=frame.export.dataset_id,
        published_batch_id=frame.export.published_batch_id,
        station_id=station_id,
        reference_time=record["referenceTime"],
        horizon_hours=horizon,
        model_id=metadata["modelId"],
    )
    history = _window(frame.hourly, station_id, start)
    try:
        result = predictor.predict(history, context)
        risk = predictor.risk(history, context)
    except Exception as error:
        return {**record, "status": "FAIL", "error": f"{type(error).__name__}: {error}"}

    points = result["points"]
    expected = [(start + timedelta(hours=step)).strftime("%Y-%m-%dT%H:%M:%SZ") for step in range(horizon)]
    problems: list[str] = []
    if len(points) != horizon:
        problems.append(f"{len(points)} points for a {horizon}h horizon")
    if [point["timestamp"] for point in points] != expected:
        problems.append("timestamps are not the consecutive hours after reference_time")
    for point in points:
        if not isinstance(point["value"], int):
            problems.append(f"{point['value']!r} is not a whole number of chargers")
        elif not 0 <= point["value"] <= capacity:
            problems.append(f"{point['value']} is outside [0, {capacity}]")
    if len(risk["hours"]) != horizon:
        problems.append("risk side channel does not cover the horizon")
    for hour in risk["hours"]:
        if abs(sum(hour["distribution"].values()) - 1.0) > 1e-9:
            problems.append(f"{hour['timestamp']}: distribution does not sum to one")
        if not hour["intervalLow"] <= hour["medianChargers"] <= hour["intervalHigh"]:
            problems.append(f"{hour['timestamp']}: median outside its own interval")
    record.update({"values": [point["value"] for point in points],
                   "unit": result["unit"],
                   "expectedChargers": [hour["expectedChargers"] for hour in risk["hours"]],
                   "status": "PASS" if not problems else "FAIL"})
    if problems:
        record["problems"] = sorted(set(problems))
    return record


def _self_check(arguments) -> int:
    """:func:`_check_one` over one bundle or a whole run; non-zero exit if any bundle fails."""
    if arguments.run_dir:
        directories = artifacts.bundle_directories(Path(arguments.run_dir))
        if not directories:
            raise SystemExit(f"{arguments.run_dir} holds no {artifacts.METADATA_NAME}; "
                             f"run ml.availability.train first")
    else:
        directories = [Path(arguments.bundle)]
    export = Path(arguments.export or DEFAULT_EXPORT)
    frame = forecaster.build_frame(export)
    print(f"[self-check] {export.name} batch {frame.export.published_batch_id}; "
          f"{len(directories)} bundle(s); sample row {arguments.sample_index}", flush=True)
    records = [_check_one(frame, directory, sample_index=arguments.sample_index) for directory in directories]
    for record in records:
        name = record.get("modelId") or Path(record["bundle"]).name
        if record["status"] == "SKIPPED":
            print(f"[SKIPPED] {name}: {record['reason']}")
            continue
        print(f"[{record['status']}] {name} " + (
            f"{record['stationId']}@{record.get('referenceTime')} values={record.get('values')}"
            if record.get("values") is not None else ""))
        for key in ("error", "problems"):
            if record.get(key):
                detail = record[key] if isinstance(record[key], str) else "; ".join(record[key])
                print(f"          {key}: {detail}")
    counts = {status: sum(1 for record in records if record["status"] == status)
              for status in ("PASS", "FAIL", "SKIPPED")}
    print(f"[self-check] {counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIPPED']} skipped")
    return 1 if counts["FAIL"] else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--bundle", help="one saved bundle directory")
    target.add_argument("--run-dir", help="every bundle of a training run (with --self-check)")
    parser.add_argument("--station")
    parser.add_argument("--reference-time")
    parser.add_argument("--horizon", type=int, default=0, help="defaults to the horizon the bundle serves")
    parser.add_argument("--export", default=None, help="export bundle to pull 24h history from")
    parser.add_argument("--database", default=None, help="published SQLite file to pull history from")
    parser.add_argument("--self-check", action="store_true", help="minimal inference test, no station needed")
    parser.add_argument("--sample-index", type=int, default=3, help="which TEST row --self-check serves")
    arguments = parser.parse_args(argv)

    if arguments.self_check:
        return _self_check(arguments)
    if not arguments.station or not arguments.reference_time:
        raise SystemExit("--station and --reference-time are required unless --self-check is given")

    forecaster_model = AvailabilityForecaster.load(arguments.bundle)
    horizon = arguments.horizon or forecaster_model.horizon_hours
    if arguments.export:
        history = _history_from_export(arguments.export, arguments.station, arguments.reference_time, forecaster_model.bundle)
    elif arguments.database:
        history = _history_from_database(arguments.database, arguments.station, arguments.reference_time)
    else:
        raise SystemExit("provide --export or --database to read the 24h history from")
    context = PredictionContext(
        dataset_id=forecaster_model.bundle["datasetId"],
        published_batch_id=forecaster_model.bundle["publishedBatchId"],
        station_id=arguments.station,
        reference_time=arguments.reference_time,
        horizon_hours=horizon,
        model_id=forecaster_model.metadata["modelId"],
    )
    print(json.dumps(forecaster_model.predict(history, context), ensure_ascii=False, indent=2))
    print(json.dumps(forecaster_model.risk(history, context), ensure_ascii=False, indent=2))
    return 0


def _history_from_database(path: str, station_id: str, reference_time: str) -> list[dict]:
    import sqlite3

    connection = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row
    try:
        cursor = connection.execute(
            "SELECT * FROM station_hourly_metrics WHERE station_id = ? "
            "AND recorded_at >= datetime(?, '-24 hours') AND recorded_at < ? ORDER BY recorded_at",
            (station_id, reference_time, reference_time),
        )
        rows = [dict(row) for row in cursor]
    finally:
        connection.close()
    for row in rows:
        if isinstance(row.get("recorded_at"), str) and not row["recorded_at"].endswith("Z"):
            row["recorded_at"] = row["recorded_at"].replace(" ", "T") + "Z"
    return rows


if __name__ == "__main__":
    raise SystemExit(main())
