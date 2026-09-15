"""Reuse PR66's exact feature builder and predictor, with explicit batch guards."""
from __future__ import annotations
import hashlib
import json
from datetime import timedelta
from pathlib import Path

from data_analysis.contracts.model import PredictionContext


class LoadAdapter:
    @classmethod
    def unavailable(cls, message):
        adapter = object.__new__(cls)
        adapter.predictor = None
        adapter.report = {"status": "NOT_READY", "message": message, "modelId": "hgb-q50-history24-v1"}
        return adapter

    def __init__(self, output_dir):
        from data_analysis.ml.load import common
        self.output_dir = Path(output_dir)
        self.predictor = None
        self.report = {"status": "NOT_READY", "modelId": common.MODEL_ID,
                       "message": "先运行 PR66 prepare_data / train / evaluate，未用假数据替代模型"}
        bundle = self.output_dir / f"{common.MODEL_ID}.joblib"
        if not bundle.is_file():
            return
        metadata = json.loads((self.output_dir / f"{common.MODEL_ID}.metadata.json").read_text(encoding="utf8"))
        metrics = json.loads((self.output_dir / "train_metrics.json").read_text(encoding="utf8"))
        manifest = common.read_manifest()
        expected = {"datasetId": manifest["datasetId"], "trainingPublishedBatchId": manifest["publishedBatchId"],
                    "featureVersion": manifest["featureVersion"], "sourceManifestSha256": manifest["sourceManifestSha256"]}
        if any(metadata.get(k) != v for k, v in expected.items()):
            raise ValueError("PR66 load artifact batch/feature provenance mismatch")
        # Integrity is not an authenticity signature: only load locally trained trusted files.
        if hashlib.sha256(bundle.read_bytes()).hexdigest() != metrics.get("artifactFileSha256"):
            raise ValueError("PR66 load artifact file digest mismatch")
        import sklearn
        if metadata.get("dependencies", {}).get("scikit-learn") != sklearn.__version__:
            raise ValueError("PR66 artifact sklearn version mismatch; retrain with installed version")
        from data_analysis.ml.load.predict import LoadForecastPredictor
        self.predictor = LoadForecastPredictor(bundle)
        if self.predictor.metadata != metadata or self.predictor.feature_columns != common.FEATURE_COLUMNS:
            raise ValueError("PR66 bundle/metadata feature mismatch")
        self.manifest = manifest
        hourly = common.load_hourly_metrics()
        self.history = {station_id: group.sort_values("recorded_at") for station_id, group in hourly.groupby("station_id")}
        self.report = {"status": "READY", "modelId": self.predictor.model_id,
                       "metadata": metadata, "metrics": {"validation": metrics["validation"]},
                       "message": "复用 PR66：过去24个完整小时预测未来1/6/24小时负荷；不替代分钟级空桩预测"}
        binding_path = self.output_dir/"chargepilot_evaluation_binding.json"
        binding = json.loads(binding_path.read_text(encoding="utf8")) if binding_path.exists() else {}
        for file in self.output_dir.glob("test_metrics_*.json"):
            report = json.loads(file.read_text(encoding="utf8"))
            if (report.get("modelId") == self.predictor.model_id and
                    report.get("trainingPublishedBatchId") == manifest["publishedBatchId"] and
                    binding.get("artifactSha256") == metrics.get("artifactFileSha256") and
                    binding.get("reportSha256") == hashlib.sha256(file.read_bytes()).hexdigest()):
                self.report["metrics"]["test"] = report.get("contractHorizons", {})
                break

    def predict(self, station_id, reference_time, horizon):
        from .store import DomainError
        from data_analysis.ml.load import common
        if self.predictor is None:
            raise DomainError("MODEL_NOT_READY", self.report["message"], 503)
        if horizon not in (1, 6, 24):
            raise DomainError("UNSUPPORTED_HORIZON", "负荷预测只支持1/6/24小时", 422)
        if station_id not in self.history:
            raise DomainError("STATION_NOT_FOUND", "电站不存在", 404)
        # Ref is floored, never rounded forward into an incomplete hour.
        ref = common.parse_utc(reference_time).replace(minute=0, second=0, microsecond=0)
        history = self.history[station_id]
        window = history[(history.recorded_at >= ref-timedelta(hours=24)) & (history.recorded_at < ref)]
        if len(window) != 24 or window.recorded_at.nunique() != 24:
            raise DomainError("HISTORY_TOO_SHORT", "没有连续24小时历史", 409)
        metadata = self.predictor.metadata
        # PR66 capacity needs station rated kW, not the hourly-average signal.
        from data_analysis.ml.load.common import load_table
        if not hasattr(self, "_rated"):
            snapshots = load_table("station_snapshot")
            if "rated_capacity_kw" in snapshots:
                self._rated = snapshots.set_index("station_id")["rated_capacity_kw"].to_dict()
            else:
                import pandas as pd
                chargers = pd.read_parquet(common.DATASET_DIR / "clean" / "chargers")
                self._rated = chargers.groupby("station_id").rated_power_kw.sum().to_dict()
        records = [{"station_id": station_id, "city_id": row.city_id,
            "recorded_at": common.format_utc(row.recorded_at.to_pydatetime()),
            "mean_power_kw": float(row.mean_power_kw), "capacity": int(row.capacity),
            "rated_capacity_kw": float(self._rated[station_id]), "end_available_count": int(row.end_available_count)}
            for row in window.itertuples(index=False)]
        context = PredictionContext(metadata["datasetId"], metadata["trainingPublishedBatchId"],
                                    station_id, common.format_utc(ref), horizon, self.predictor.model_id)
        result = self.predictor.predict(records, context)
        return {**result, "referenceTime": common.format_utc(ref), "stationId": station_id,
                "dataSource": "SIMULATED", "historyHours": 24}
