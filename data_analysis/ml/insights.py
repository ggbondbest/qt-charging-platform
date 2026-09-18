"""Delivery adapter for the two independent administrator ML capabilities.

No training on HTTP requests. The trusted local bundle is verified before
unpickling and bound to the exact CLEAN source and a frozen evaluation report.
Only synthetic identifiers / observed aggregate facts are returned to the UI.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import tempfile
import time

import joblib
import numpy as np
import pandas as pd
import sklearn

from .anomaly import delivery as anomaly
from .churn import common as churn_common
from .churn import delivery as churn

ROOT = Path(__file__).resolve().parents[1]
DATASET = ROOT / "datasets" / "analytics_full_180d_v1"
OUTPUT = ROOT / "outputs" / "ml_insights_delivery"
TABLES = ("users", "vehicles", "charging_attempts", "charging_sessions", "queue_entries", "battery_samples", "anomaly_labels")
ARTIFACT = "insights.joblib"
METADATA = "metadata.json"
REPORT = "evaluation_report.json"


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_binding(dataset):
    dataset = Path(dataset)
    manifest = json.loads((dataset / "serving_manifest.json").read_text(encoding="utf-8"))
    files = {}
    for table in TABLES:
        parts = sorted((dataset / "clean" / table).glob("*.parquet"))
        if not parts:
            raise FileNotFoundError(f"Missing CLEAN table: {table}")
        for part in parts:
            if not part.resolve().is_relative_to(dataset.resolve()):
                raise ValueError("CLEAN source must stay inside dataset directory")
            files[part.relative_to(dataset).as_posix()] = sha256(part)
    return {"datasetId": manifest["datasetId"], "publishedBatchId": manifest["publishedBatchId"],
            "sourceManifestSha256": manifest["sourceManifestSha256"],
            "servingManifestSha256": sha256(dataset / "serving_manifest.json"),
            "cleanFiles": files, "dataKind": "SIMULATED"}


def read_tables(dataset):
    return {name: pd.concat([pd.read_parquet(p) for p in sorted((Path(dataset) / "clean" / name).glob("*.parquet"))],
                            ignore_index=True) for name in TABLES}


def _write_json(path, payload):
    Path(path).write_text(json.dumps(payload, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def train(output_dir=OUTPUT, dataset_dir=DATASET):
    """Train on CPU. Existing frozen delivery directories are never overwritten."""
    started = time.monotonic()
    output = Path(output_dir)
    if (output / METADATA).exists():
        service = InsightsService(output, dataset_dir=dataset_dir)
        return service.report()
    output.mkdir(parents=True, exist_ok=True)
    # The lock only protects this output directory; independent experiment
    # directories are allowed. Its removal is confined to the file we created.
    lock_path = output / ".training.lock"
    fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)
    try:
        provenance = source_binding(dataset_dir)
        manifest = json.loads((Path(dataset_dir) / "serving_manifest.json").read_text(encoding="utf-8"))
        label_end = churn_common.OBSERVE_END + pd.Timedelta(days=14)
        if pd.Timestamp(manifest["periodEndExclusive"]).tz_convert("UTC").tz_localize(None) < label_end:
            raise ValueError("Dataset does not cover the complete 14-day churn label window")
        tables = read_tables(dataset_dir)
        print("Building point-in-time churn features ...", flush=True)
        users = churn_common.build_user_table(tables=tables)
        churn_bundle, churn_metrics = churn.fit(users)
        users["modelScore"] = churn.score(churn_bundle, users)
        print("Building completed-session anomaly features ...", flush=True)
        sessions = tables["charging_sessions"].copy()
        sessions["started_at"] = pd.to_datetime(sessions.started_at)
        sessions["ended_at"] = pd.to_datetime(sessions.ended_at)
        points = anomaly.prepare_points(tables["battery_samples"], sessions)
        train_ids = sessions.loc[anomaly.split_sessions(sessions) == "TRAIN", "session_id"]
        context = anomaly.fit_context(points, train_ids)
        features = anomaly.build_features(points, sessions, context)
        anomaly_bundle, anomaly_metrics = anomaly.fit(features, tables["anomaly_labels"], context)
        # Product default exposes TEST examples only; it cannot accidentally
        # demonstrate training-set accuracy as held-out model performance.
        test_sessions = features[features.split == "TEST"].copy()
        test_sessions["modelScore"] = anomaly.score(anomaly_bundle, test_sessions)
        report = {"schemaVersion": "1.0.0", "dataKind": "SIMULATED", "source": provenance,
                  "churn": {"modelId": churn.MODEL_ID, **churn_metrics},
                  "anomaly": {"modelId": anomaly.MODEL_ID, **anomaly_metrics},
                  "notes": ["使用项目合成数据，不是 ACN 实测结果。",
                            "流失预测是固定历史观察日的14天未回访风险排序，不代表实时运营或发券收益。",
                            "异常检测是已结束会话的辅助复核，不是设备实时保护或故障诊断。",
                            "旧 PR67 TEST 曾用于研究版本比较；本报告是修正协议后的回顾性独立用户/时间留出评估，不宣称全新未见现场验证。"],
                  "trainingSeconds": round(time.monotonic() - started, 2)}
        bundle = {"schemaVersion": "1.0.0", "source": provenance,
                  "churn": churn_bundle, "anomaly": anomaly_bundle,
                  "users": users, "sessions": test_sessions}
        # Fail if another process changed the source while training.
        if source_binding(dataset_dir) != provenance:
            raise ValueError("CLEAN source changed during training; refusing publication")
        with tempfile.TemporaryDirectory(prefix=".insights-build-", dir=output) as temp:
            temp = Path(temp)
            joblib.dump(bundle, temp / ARTIFACT, compress=3)
            _write_json(temp / REPORT, report)
            metadata = {"schemaVersion": "1.0.0", "source": provenance,
                        "modelIds": {"churn": churn.MODEL_ID, "anomaly": anomaly.MODEL_ID},
                        "artifactFile": ARTIFACT, "artifactSha256": sha256(temp / ARTIFACT),
                        "reportFile": REPORT, "reportSha256": sha256(temp / REPORT),
                        "dependencies": {"python": platform.python_version(), "scikit-learn": sklearn.__version__,
                                         "numpy": np.__version__, "pandas": pd.__version__, "joblib": joblib.__version__}}
            _write_json(temp / METADATA, metadata)
            for name in (ARTIFACT, REPORT, METADATA):
                os.replace(temp / name, output / name)
        print(f"Bound delivery artifacts -> {output} ({report['trainingSeconds']}s)", flush=True)
        return report
    finally:
        lock_path.unlink(missing_ok=True)


class InsightsService:
    def __init__(self, output_dir=OUTPUT, *, dataset_dir=DATASET):
        self.output_dir = Path(output_dir)
        self.bundle = None
        self.metadata = {}
        self._report = {}
        if not (self.output_dir / METADATA).is_file():
            return
        metadata = json.loads((self.output_dir / METADATA).read_text(encoding="utf-8"))
        if metadata.get("schemaVersion") != "1.0.0":
            raise ValueError("Unsupported insights metadata schema")
        if metadata.get("artifactFile") != ARTIFACT or metadata.get("reportFile") != REPORT:
            raise ValueError("Unexpected insights artifact/report path")
        if metadata.get("source") != source_binding(dataset_dir):
            raise ValueError("Insights source provenance mismatch; retrain for this dataset")
        if metadata.get("modelIds") != {"churn": churn.MODEL_ID, "anomaly": anomaly.MODEL_ID}:
            raise ValueError("Insights model version mismatch")
        expected_versions = {"scikit-learn": sklearn.__version__, "numpy": np.__version__,
                             "pandas": pd.__version__, "joblib": joblib.__version__}
        if any(metadata.get("dependencies", {}).get(k) != v for k, v in expected_versions.items()):
            raise ValueError("Insights dependency versions differ; retrain with installed requirements")
        if (sha256(self.output_dir / ARTIFACT) != metadata.get("artifactSha256") or
                sha256(self.output_dir / REPORT) != metadata.get("reportSha256")):
            raise ValueError("Insights artifact or evaluation digest mismatch")
        report = json.loads((self.output_dir / REPORT).read_text(encoding="utf-8"))
        if report.get("source") != metadata["source"]:
            raise ValueError("Insights evaluation/source mismatch")
        # Hashes establish integrity, not authenticity: only load the locally
        # trained bundle shipped by the project, never a user-uploaded pickle.
        bundle = joblib.load(self.output_dir / ARTIFACT)
        if bundle.get("source") != metadata["source"] or bundle.get("schemaVersion") != "1.0.0":
            raise ValueError("Insights bundle/source mismatch")
        self.bundle, self.metadata, self._report = bundle, metadata, report
        self.users = bundle["users"].set_index("user_id", drop=False).rename_axis(None)
        self.sessions = bundle["sessions"].set_index("session_id", drop=False).rename_axis(None)

    def status(self):
        return {"status": "READY" if self.bundle is not None else "NOT_READY", "dataKind": "SIMULATED",
                "modelIds": {"churn": churn.MODEL_ID, "anomaly": anomaly.MODEL_ID},
                "publishedBatchId": self.metadata.get("source", {}).get("publishedBatchId"),
                "message": "历史留出样本推理已就绪" if self.bundle is not None else "先运行 python -m data_analysis.ml.insights train"}

    def report(self):
        return {**self.status(), **self._report}

    def _ready(self):
        if self.bundle is None:
            raise RuntimeError("INSIGHTS_NOT_READY: train the delivery models first")

    @staticmethod
    def _limit(limit):
        if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= 100:
            raise ValueError("limit must be an integer from 1 to 100")
        return limit

    def predict_user(self, user_id):
        self._ready()
        if user_id not in self.users.index:
            raise KeyError(f"Unknown synthetic user: {user_id}")
        return {**churn.describe(self.bundle["churn"], self.users.loc[user_id]),
                "publishedBatchId": self.metadata["source"]["publishedBatchId"]}

    def inspect_session(self, session_id):
        self._ready()
        if session_id not in self.sessions.index:
            raise KeyError(f"Unknown TEST session: {session_id}")
        return {**anomaly.describe(self.bundle["anomaly"], self.sessions.loc[session_id]),
                "publishedBatchId": self.metadata["source"]["publishedBatchId"]}

    def list_churn(self, limit=20):
        self._ready()
        rows = self.users[self.users.split == "TEST"].sort_values(["modelScore", "user_id"], ascending=[False, True])
        return {"items": [self.predict_user(uid) for uid in rows.head(self._limit(limit)).index],
                "total": len(rows), "modelId": churn.MODEL_ID, "dataKind": "SIMULATED", "evaluationSplit": "TEST",
                "referenceTime": churn_common.OBSERVE_END.strftime("%Y-%m-%dT%H:%M:%SZ"),
                "publishedBatchId": self.metadata["source"]["publishedBatchId"]}

    def list_anomalies(self, limit=20):
        self._ready()
        rows = self.sessions.sort_values(["modelScore", "session_id"], ascending=[False, True])
        flagged = rows[rows.modelScore > self.bundle["anomaly"]["threshold"]]
        return {"items": [self.inspect_session(sid) for sid in flagged.head(self._limit(limit)).index],
                "total": len(flagged), "inspectedSessions": len(rows), "modelId": anomaly.MODEL_ID,
                "dataKind": "SIMULATED", "evaluationSplit": "TEST", "referenceTime": "2026-05-29T16:00:00Z",
                "publishedBatchId": self.metadata["source"]["publishedBatchId"]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["train", "report"])
    parser.add_argument("--output-dir", type=Path, default=OUTPUT)
    parser.add_argument("--dataset-dir", type=Path, default=DATASET)
    args = parser.parse_args(argv)
    result = train(args.output_dir, args.dataset_dir) if args.action == "train" else InsightsService(args.output_dir, dataset_dir=args.dataset_dir).report()
    print(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
