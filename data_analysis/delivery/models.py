"""Serve trusted local models against the exact published analytics batch.

Hourly resource planning and minute-level arrival prediction are intentionally
different targets. A missing model never becomes a fabricated forecast.
"""
from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import threading

from data_analysis.backend.errors import ApiError
from data_analysis.contracts.model import PredictionContext, validate_prediction
from data_analysis.chargepilot.load_adapter import LoadAdapter
from data_analysis.chargepilot.store import DomainError

ROOT = Path(__file__).resolve().parents[1]


def verify_arrival_source(output_dir, dataset, manifest):
    """Bind a trusted minute model to this deployment BEFORE deserializing it."""
    from data_analysis.chargepilot.ml.data import SOURCE_TABLES
    metadata = json.loads((Path(output_dir) / "arrival.metadata.json").read_text(encoding="utf8"))
    source = metadata.get("source", {})
    for key in ("datasetId", "publishedBatchId", "pipelineRunId", "sourceManifestSha256"):
        if source.get(key) != manifest.get(key):
            raise ValueError("Arrival model and published analytics batch differ; retrain for the current CLEAN dataset")
    dataset = Path(dataset)
    paths = [dataset / "serving_manifest.json"]
    for table in SOURCE_TABLES:
        parts = sorted((dataset / "clean" / table).glob("*.parquet"))
        if not parts:
            raise ValueError("Arrival CLEAN source table is missing")
        paths.extend(parts)
    actual = {}
    for path in paths:
        if not path.resolve().is_relative_to(dataset.resolve()):
            raise ValueError("Arrival CLEAN source escapes dataset")
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        actual[path.relative_to(dataset).as_posix()] = digest.hexdigest()
    if actual != source.get("sourceFilesSha256"):
        raise ValueError("Arrival CLEAN source fingerprint differs; do not mix replay with changed analytics")


class ForecastService:
    def __init__(self, load, availability, insights, arrival, manifest):
        self.load, self.availability, self.insights, self.arrival = load, availability, insights, arrival
        self.manifest = manifest
        self._cache = {}
        self._lock = threading.RLock()
        self.catalog = {row["stationId"]: row for row in arrival.catalog}

    @classmethod
    def from_local(cls, settings):
        from data_analysis.chargepilot.ml.predict import ArrivalPredictor
        dataset = ROOT / "datasets/analytics_full_180d_v1"
        manifest = json.loads((dataset / "serving_manifest.json").read_text(encoding="utf8"))
        verify_arrival_source(settings.model_dir, dataset, manifest)
        arrival = ArrivalPredictor(settings.model_dir)
        try:
            load = LoadAdapter(settings.load_dir)
        except (OSError, ValueError, KeyError, TypeError):
            load = LoadAdapter.unavailable("负荷模型校验失败，请重新训练可信工件")
        import os
        availability, insights = None, None
        try:
            from data_analysis.chargepilot.availability_adapter import AvailabilityAdapter
            availability = AvailabilityAdapter(Path(os.getenv("AVAILABILITY_MODEL_DIR", ROOT / "outputs/ml_availability_delivery")))
        except (ImportError, OSError, ValueError, KeyError, TypeError, RuntimeError):
            pass
        try:
            from data_analysis.ml.insights import InsightsService
            insights = InsightsService(Path(os.getenv("INSIGHTS_MODEL_DIR", ROOT / "outputs/ml_insights_delivery")))
        except (ImportError, OSError, ValueError, KeyError, TypeError, RuntimeError):
            pass
        return cls(load, availability, insights, arrival, manifest)

    def check_batch(self, publication):
        if publication.get("datasetId") != self.manifest["datasetId"]:
            raise ApiError(409, "BATCH_MISMATCH", "统计数据与模型数据集不同，请选择一致批次")
        if publication.get("publishedBatchId") != self.manifest["publishedBatchId"]:
            raise ApiError(409, "BATCH_MISMATCH", "统计数据与模型发布批次不同，请重新发布或训练")
        source_hash = publication.get("sourceManifestSha256")
        if source_hash is not None and source_hash != self.manifest["sourceManifestSha256"]:
            raise ApiError(409, "BATCH_MISMATCH", "统计数据与模型来源指纹不同")

    def registry(self, publication):
        self.check_batch(publication)
        load = deepcopy(self.load.report)
        metadata = load.get("metadata", {})
        load["models"] = [dict(modelId=load["modelId"], modelVersion=metadata.get("modelVersion", "0.4.0"),
                               target="load", horizonHours=h) for h in (1, 6, 24)] if load["status"] == "READY" else []
        load["targetSemantics"] = "MEAN_POWER_DURING_HOUR"
        availability = deepcopy(self.availability.report) if self.availability else {
            "status": "NOT_READY", "models": [], "message": "先完成小时空闲桩模型训练"}
        insights = self.insights.report() if self.insights else {
            "status": "NOT_READY", "message": "先完成异常筛查和用户流失模型训练"}
        return {"load": load, "availability": availability, "insights": insights,
                "arrival": {"status": "READY", "metadata": self.arrival.metadata},
                "datasetId": self.manifest["datasetId"], "publishedBatchId": self.manifest["publishedBatchId"],
                "dataSource": "SIMULATED", "defaultReferenceTime": "2026-05-05T00:00:00Z",
                "scope": "负荷为小时均值，空闲桩为小时末采样；智能找桩另用分钟到站模型。"}

    def capabilities(self, publication):
        registry = self.registry(publication)
        models = []
        seen = set()
        for target in ("load", "availability"):
            for model in registry[target].get("models", []):
                if model["modelId"] not in seen:
                    models.append({key: model[key] for key in ("modelId", "modelVersion", "target")})
                    seen.add(model["modelId"])
        return dict(models=models, implementedPrediction=bool(models), supportedTargets=["load", "availability"],
                    status="READY" if len(models) else "MODEL_NOT_READY")

    def predict(self, target, body, publication, *, detailed=True):
        if target not in ("load", "availability"):
            raise ApiError(422, "INVALID_REQUEST", "预测目标必须是 load 或 availability")
        registry = self.registry(publication)
        adapter = self.load if target == "load" else self.availability
        if adapter is None or registry[target]["status"] != "READY":
            raise ApiError(503, "MODEL_NOT_READY", "此模型未训练或校验未通过；不会返回替代预测")
        selected = next((m for m in registry[target].get("models", [])
                         if m["modelId"] == body.modelId and m["horizonHours"] == body.horizonHours), None)
        if selected is None:
            raise ApiError(422, "MODEL_INCOMPATIBLE", "模型名称与预测目标或跨度不匹配，请刷新模型列表")
        station = self.catalog.get(body.stationId)
        if station is None:
            raise ApiError(404, "STATION_NOT_FOUND", "模型目录中不存在该电站")
        capacity = station["ratedCapacityKw"] if target == "load" else station["capacity"]
        context = PredictionContext(self.manifest["datasetId"], self.manifest["publishedBatchId"],
                                    body.stationId, body.referenceTime, body.horizonHours, body.modelId)

        def validated_base(result):
            # Do not silently "repair" a different unit/feature contract with constants.
            # The source adapter must actually have produced this model's declared output.
            try:
                if (result["modelVersion"] != selected["modelVersion"] or
                        result.get("referenceTime", body.referenceTime) != body.referenceTime):
                    raise ValueError("adapter version/reference mismatch")
                base = {key: result[key] for key in ("schemaVersion", "featureVersion", "modelId", "modelVersion", "unit")}
                base["points"] = [{"timestamp": point["timestamp"], "value": point["value"]}
                                  for point in result["points"]]
                validate_prediction(base, context, capacity, task=target)
                if target == "availability" and any(float(point["value"]) % 1 for point in base["points"]):
                    raise ValueError("free charger point values must be integers")
                return base
            except (ValueError, KeyError, TypeError, AttributeError) as exc:
                raise ApiError(503, "MODEL_INCOMPATIBLE", "模型输出违反版本、单位、容量或时间契约") from exc

        key = (target, body.modelId, body.stationId, body.referenceTime, body.horizonHours)
        try:
            with self._lock:
                if key not in self._cache:
                    result = adapter.predict(body.stationId, body.referenceTime, body.horizonHours)
                    validated_base(result)  # invalid output must never poison subsequent requests
                    self._cache[key] = deepcopy(result)
                    if len(self._cache) > 256:
                        self._cache.pop(next(iter(self._cache)))
                result = deepcopy(self._cache[key])
        except DomainError as exc:
            raise ApiError(exc.status, exc.code, exc.message) from exc
        except (ValueError, KeyError, RuntimeError) as exc:
            raise ApiError(409, "HISTORY_TOO_SHORT", "无法构造该站点连续、完整、同批次的24小时历史") from exc
        point_list = result.get("points", [])
        base = validated_base(result)
        if not detailed:
            return base
        return {**result, **base, "points": point_list, "referenceTime": body.referenceTime,
                "stationId": body.stationId, "target": target, "dataSource": "SIMULATED",
                "targetSemantics": "MEAN_POWER_DURING_HOUR" if target == "load" else "LAST_SAMPLE_IN_HOUR",
                "historyHours": 24}

    def insight(self, kind, publication, *, entity_id=None, limit=20):
        self.check_batch(publication)
        if self.insights is None:
            raise ApiError(503, "MODEL_NOT_READY", "分析模型尚未训练或校验失败")
        try:
            if kind == "churn":
                return self.insights.predict_user(entity_id) if entity_id else self.insights.list_churn(limit)
            return self.insights.inspect_session(entity_id) if entity_id else self.insights.list_anomalies(limit)
        except (KeyError, LookupError) as exc:
            raise ApiError(404, "NOT_FOUND", "所选模拟用户或会话不存在") from exc
        except (ValueError, RuntimeError) as exc:
            raise ApiError(503, "MODEL_NOT_READY", "分析数据或模型未就绪") from exc
