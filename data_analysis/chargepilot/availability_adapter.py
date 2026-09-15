"""Serve PR70's hourly inventory distribution, never a minute-level arrival estimate.

No model imports happen at module import time. Missing trusted local artifacts are an
explicit NOT_READY capability, so the rest of the delivery application can still start.
"""
from __future__ import annotations

import json
from datetime import timedelta
from pathlib import Path
from threading import RLock

from data_analysis.contracts.model import PredictionContext
from data_analysis.contracts.serving import parse_utc


class AvailabilityAdapter:
    @classmethod
    def unavailable(cls, message):
        adapter = object.__new__(cls)
        adapter.predictors = {}
        adapter.report = {"status": "NOT_READY", "models": [], "message": str(message),
                          "target": "availability", "targetSemantics": "LAST_SAMPLE_IN_HOUR"}
        return adapter

    def __init__(self, output_dir, export_dir=None):
        self.output_dir = Path(output_dir)
        self.predictors = {}
        self.report = self.unavailable(
            "请先运行空闲桩模型 train 和 build_hierarchical；未使用固定值代替模型").report
        directories = {h: self.output_dir / f"h{h:02d}" for h in (1, 6, 24)}
        missing = [h for h, directory in directories.items()
                   if not (directory / "model_metadata.json").is_file() or not (directory / "model.joblib").is_file()]
        if missing:
            self.report["missingHorizons"] = missing
            return
        from data_analysis.ml.availability.predict import AvailabilityForecaster
        from data_analysis.ml.common import forecaster
        from data_analysis.ml.common.data_io import DEFAULT_EXPORT
        frame = forecaster.build_frame(export_dir or DEFAULT_EXPORT)
        metrics, models = {}, []
        for horizon, directory in directories.items():
            predictor = AvailabilityForecaster.load(directory)
            forecaster.validate_bundle_frame(predictor.bundle, predictor.metadata, frame)
            if predictor.horizon_hours != horizon or predictor.bundle.get("excludeCity"):
                raise ValueError("hourly availability serving requires complete non-holdout models")
            if predictor.bundle["stations"] != frame.stations:
                raise ValueError("hourly availability station capacity/profile differs from this export")
            metadata = predictor.metadata
            self.predictors[horizon] = predictor
            models.append({"modelId": metadata["modelId"], "modelVersion": metadata["modelVersion"],
                           "target": "availability", "horizonHours": horizon,
                           "supportedHorizons": [horizon]})
            # The headline metrics are bound to the verified artifact's metadata. Richer
            # training report metrics are shown only if its own artifact binding agrees.
            entry = {**metadata["metrics"], "pointRule": getattr(predictor.model, "point_rule", "mode"),
                     "rmseDefinition": "DISTRIBUTION_EXPECTATION", "split": "TEST"}
            report_path = directory / "training_report.json"
            if report_path.is_file():
                report = json.loads(report_path.read_text(encoding="utf-8"))
                if report.get("artifactSha256") == metadata["artifactSha256"]:
                    pooled = report.get("pooledTest", {})
                    for key in ("coverage", "intervalWidth", "baselinePersistenceMae", "depletionAUC", "depletionBaseRate"):
                        if key in pooled:
                            entry[key] = pooled[key]
            metrics[f"h{horizon:02d}"] = entry
        self.history = {station_id: group.sort_values("recorded_at")
                        for station_id, group in frame.hourly.groupby("station_id")}
        self.stations = frame.stations
        self.export = frame.export
        self._lock = RLock()
        metadata = self.predictors[1].metadata
        test = frame.data[frame.data[frame.export.split_column(24)] == "TEST"]
        self.report = {
            "status": "READY", "target": "availability", "models": models,
            "modelId": metadata["modelId"], "modelVersion": metadata["modelVersion"],
            "schemaVersion": metadata["schemaVersion"], "featureVersion": metadata["featureVersion"],
            "datasetId": frame.export.dataset_id, "publishedBatchId": frame.export.published_batch_id,
            "sourceManifestSha256": frame.export.source_manifest_sha256,
            "metrics": metrics, "unit": "chargers", "dataSource": "SIMULATED", "historyHours": 24,
            "targetSemantics": "LAST_SAMPLE_IN_HOUR", "sampleMinute": 55,
            "testReferenceStart": test.reference_time.min().strftime("%Y-%m-%dT%H:%M:%SZ"),
            "testReferenceEnd": test.reference_time.max().strftime("%Y-%m-%dT%H:%M:%SZ"),
            "message": "每点预测该小时最后一次采样（hh:55）的空闲桩数，不是此刻库存或分钟级到站可用性",
        }

    def predict(self, station_id, reference_time, horizon_hours):
        from .store import DomainError
        if not self.predictors:
            raise DomainError("MODEL_NOT_READY", self.report["message"], 503)
        if type(horizon_hours) is not int or horizon_hours not in (1, 6, 24):
            raise DomainError("UNSUPPORTED_HORIZON", "空闲桩预测只支持 1/6/24 小时", 422)
        if station_id not in self.history:
            raise DomainError("STATION_NOT_FOUND", "电站不存在", 404)
        try:
            reference = parse_utc(reference_time).replace(minute=0, second=0, microsecond=0)
        except (TypeError, ValueError) as error:
            raise DomainError("INVALID_REQUEST", "referenceTime 必须是 UTC Z 时间", 422) from error
        hourly = self.history[station_id]
        window = hourly[(hourly.recorded_at >= reference - timedelta(hours=24)) & (hourly.recorded_at < reference)]
        if len(window) != 24 or window.recorded_at.nunique() != 24:
            raise DomainError("HISTORY_TOO_SHORT", "缺少连续 24 个完整小时历史，无法预测", 409)
        predictor = self.predictors[horizon_hours]
        context = PredictionContext(self.export.dataset_id, self.export.published_batch_id, station_id,
                                    reference.strftime("%Y-%m-%dT%H:%M:%SZ"), horizon_hours,
                                    predictor.metadata["modelId"])
        records = window.to_dict("records")
        from data_analysis.ml.common.features import HistoryError
        from data_analysis.ml.availability.predict import PredictionError
        try:
            with self._lock:
                forecast = predictor.predict(records, context)
                risk = predictor.risk(records, context)
        except (HistoryError, PredictionError) as error:
            raise DomainError(error.code, str(error), 409) from error
        points = [{**point, "expectedChargers": hour["expectedChargers"],
                   "lower": hour["intervalLow"], "upper": hour["intervalHigh"],
                   "probabilityNoFree": hour["probabilityDepleted"],
                   "intervalLevel": hour["intervalLevel"], "distribution": hour["distribution"],
                   "sampleTimestamp": (parse_utc(point["timestamp"]) + timedelta(minutes=55)).strftime("%Y-%m-%dT%H:%M:%SZ")}
                  for point, hour in zip(forecast["points"], risk["hours"])]
        return {**forecast, "points": points, "referenceTime": context.reference_time,
                "stationId": station_id, "horizonHours": horizon_hours, "historyHours": 24,
                "datasetId": self.export.dataset_id, "publishedBatchId": self.export.published_batch_id,
                "targetSemantics": "LAST_SAMPLE_IN_HOUR", "dataSource": "SIMULATED",
                "pointRule": risk["pointRule"], "nominalIntervalCoverage": predictor.model.nominal_coverage,
                "history": [{"timestamp": row.recorded_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
                             "availableChargers": int(row.end_available_count), "meanPowerKw": float(row.mean_power_kw)}
                            for row in window.itertuples(index=False)],
                "message": self.report["message"]}
