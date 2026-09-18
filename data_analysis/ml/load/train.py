"""按预测时距 (h01..h24) 各训一个 HistGradientBoostingRegressor,
配方 = v0.4 "hgb-q50": loss="quantile", quantile=0.5, max_iter=600, lr=0.03,
max_leaf_nodes=63, min_samples_leaf=20, l2=0.5, early_stopping=False, seed=42。
quantile=0.5 转正自 L2: 中位数损失直接优化 MAE (合同对外汇报指标),
VALIDATION 上赢了 v0.2 L2 竞赛冠军 "hgb-deep" (EVALUATION.md §9/§10)。
特征为共享 ml_features_hourly 列, city_id/station_id 走原生 categorical。
24 个模型都只在 split_24h==TRAIN 上拟合 (24h 远期标签完整, 训练面一致),
打分统一在 split_24h==VALIDATION 且标签非空行; 预测裁剪到
[0, rated_capacity_kw], 与在线预测器同口径; persistence / 上周同时段
基线与每个模型一并记录。
schema 只允许一个 metrics 对象, 放 h01/h06/h24 的 VALIDATION 汇总, TEST 从未打分;
testSamples 为 schema 遗留字段名, 实为 VALIDATION 打分对数 (行 x 时距), 与 TEST 无关;
逐时距明细在 train_metrics.json。artifactSha256 是 payload 摘要
(common.canonical_payload_digest), 文件不能内嵌自身字节哈希; bundle 先做
pickle 往返稳定化再以 plain protocol-5 落盘, 字节稳定, 消费方可从交付文件
重算同一摘要 (joblib.load 透明可读); 整文件哈希见 artifactFileSha256。
metadata 另存 <model_id>.metadata.json (合同 README §6)。

用法(仓库根目录):
    python -m data_analysis.ml.load.train
"""

from __future__ import annotations

import hashlib
import json
import pickle
import platform
import time
from datetime import datetime, timezone

import joblib
import numpy as np
import pandas as pd
import sklearn
from sklearn.ensemble import HistGradientBoostingRegressor

from . import common

SEED = 42
MODEL_ID = common.MODEL_ID
MODEL_VERSION = common.MODEL_VERSION
PARAMS = {
    "loss": "quantile",
    "quantile": 0.5,
    "max_iter": 600,
    "learning_rate": 0.03,
    "max_leaf_nodes": 63,
    "min_samples_leaf": 20,
    "l2_regularization": 0.5,
    "early_stopping": False,
    "random_state": SEED,
}
OUT_DIR = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
BUNDLE_PATH = OUT_DIR / f"{MODEL_ID}.joblib"
METADATA_PATH = OUT_DIR / f"{MODEL_ID}.metadata.json"
SCHEMA_PATH = common.DATA_ANALYSIS_ROOT / "contracts" / "model_metadata.schema.json"
CONTRACT_HORIZONS = (1, 6, 24)


def regression_metrics(y_true: np.ndarray, y_pred: np.ndarray) -> dict[str, float]:
    error = y_pred - y_true
    return {
        "mae": float(np.abs(error).mean()),
        "rmse": float(np.sqrt((error**2).mean())),
        "n": int(len(y_true)),
    }


def contract_metrics(metrics: dict[str, dict], horizon_counts: dict[int, int]) -> dict:
    """三个合同时距 VALIDATION 数字汇总进 schema 唯一的 metrics 对象; RMSE 按平方均值开根合并, 不能对 RMSE 取算术平均。"""
    maes = [metrics[f"h{h:02d}"]["gbdt"]["mae"] for h in CONTRACT_HORIZONS]
    rmses = [metrics[f"h{h:02d}"]["gbdt"]["rmse"] for h in CONTRACT_HORIZONS]
    samples = sum(horizon_counts[h] for h in CONTRACT_HORIZONS)
    return {
        "mae": round(float(np.mean(maes)), 3),
        "rmse": round(float(np.sqrt(np.mean(np.square(rmses)))), 3),
        "testSamples": int(samples),
        "unit": "kW",
    }


def validate_metadata(metadata: dict) -> None:
    """校验 metadata 符合仓库分发的 JSON schema; 宁可在此报错, 不交出不合规产物。"""
    with open(SCHEMA_PATH, encoding="utf-8") as handle:
        schema = json.load(handle)
    required = [key for key in schema["required"] if key not in metadata]
    extra = [key for key in metadata if key not in schema["properties"]]
    if required or extra:
        raise AssertionError(f"metadata not schema-conformant: missing={required} extra={extra}")
    try:
        import jsonschema
    except ImportError:  # 结构化检查(必填/多余键)已过, 只是不做全量 jsonschema 校验
        print("jsonschema not installed; skipped full schema validation")
        return
    jsonschema.validate(metadata, schema)


def clip_to_capacity(values: np.ndarray, frame: pd.DataFrame, index) -> np.ndarray:
    limits = frame.loc[index, "rated_capacity_kw"].to_numpy(dtype=float)
    return np.clip(values, 0.0, limits)


def main() -> int:
    started = time.time()
    manifest = common.read_manifest()
    # 审计没过或批次不对就不许吃缓存(评审 P2#3);周基线函数挪去 common 并加 horizon(评审 P2#2)。
    frame = pd.read_pickle(common.require_prepared(manifest))
    hourly = common.load_hourly_metrics()
    power_lookup = hourly.set_index(["station_id", "recorded_at"])["mean_power_kw"]

    matrix = common.features_matrix(frame)
    calendar = common.build_calendar_lookup(frame)
    category_levels = {
        column: sorted(frame[column].astype(str).unique().tolist())
        for column in common.CATEGORICAL_FEATURES
    }

    mask_train = frame["split_24h"] == "TRAIN"
    mask_valid = frame["split_24h"] == "VALIDATION"

    models: dict[int, HistGradientBoostingRegressor] = {}
    metrics: dict[str, dict] = {}
    for horizon in range(1, 25):
        label_col = f"label_power_kw_h{horizon:02d}"
        label = frame[label_col]
        train_ok = mask_train & label.notna() & label.ne(np.inf) & label.ne(-np.inf)
        valid_ok = mask_valid & label.notna() & label.ne(np.inf) & label.ne(-np.inf)

        model = HistGradientBoostingRegressor(categorical_features="from_dtype", **PARAMS)
        model.fit(matrix.loc[train_ok], frame.loc[train_ok, label_col].to_numpy(dtype=float))
        models[horizon] = model

        valid_index = frame.index[valid_ok]
        y_valid = frame.loc[valid_ok, label_col].to_numpy(dtype=float)
        y_pred = clip_to_capacity(
            model.predict(matrix.loc[valid_ok]).astype(float), frame, valid_index
        )
        entry = {"gbdt": regression_metrics(y_valid, y_pred)}

        persistence = frame.loc[valid_ok, "lag_power_kw_h01"].to_numpy(dtype=float)
        entry["persistence"] = regression_metrics(
            y_valid, np.clip(persistence, 0.0, None)
        )

        week = common.last_week_predictions(frame.loc[valid_ok], power_lookup, horizon)
        found = np.isfinite(week)
        entry["same_hour_last_week"] = regression_metrics(y_valid[found], week[found])
        entry["same_hour_last_week"]["coverage"] = float(found.mean())
        metrics[f"h{horizon:02d}"] = entry

        line = (
            f"h{horizon:02d} valid  gbdt_mae={entry['gbdt']['mae']:.4f} kW  "
            f"gbdt_rmse={entry['gbdt']['rmse']:.4f}  (n={entry['gbdt']['n']})"
        )
        if horizon in (1, 6, 24):
            line += (
                f"  persist={entry['persistence']['mae']:.3f}"
                f"  week={entry['same_hour_last_week']['mae']:.3f}"
            )
        print(line)

    payload = {
        "models": models,
        "feature_columns": common.FEATURE_COLUMNS,
        "category_levels": category_levels,
        "calendar": calendar,
        "model_id": MODEL_ID,
        "model_version": MODEL_VERSION,
    }
    bounds = common.split_end_exclusive_utc(manifest)
    schema_meta = {
        "schemaVersion": "1.0.0",
        "featureVersion": manifest["featureVersion"],
        "modelId": MODEL_ID,
        "modelVersion": MODEL_VERSION,
        "target": "load",
        "datasetId": manifest["datasetId"],
        "sourceManifestSha256": manifest["sourceManifestSha256"],
        "trainingPublishedBatchId": manifest["publishedBatchId"],
        **bounds,
        "historyHours": int(manifest["mlHistoryHours"]),
        "supportedHorizons": list(CONTRACT_HORIZONS),
        "featureColumns": list(common.FEATURE_COLUMNS),
        "artifactFile": BUNDLE_PATH.name,
        "artifactSha256": "0" * 64,  # 占位符, 稳定化往返后替换
        "metrics": contract_metrics(metrics, {h: metrics[f"h{h:02d}"]["gbdt"]["n"] for h in CONTRACT_HORIZONS}),
        "dependencies": {
            "python": platform.python_version(),
            "scikit-learn": sklearn.__version__,
            "pandas": pd.__version__,
            "numpy": np.__version__,
            "joblib": joblib.__version__,
        },
    }
    # 稳定化往返: load 回来再 dumps 的 plain-pickle 字节才逐字节稳定
    # (刚 fit 的对象与 joblib 压缩流都不行), artifactSha256 因此可从交付文件重算。
    pre_bundle = {**payload, "metadata": schema_meta}
    canonical = pickle.loads(pickle.dumps(pre_bundle, protocol=5))
    clean_payload = {key: value for key, value in canonical.items() if key != "metadata"}
    schema_meta["artifactSha256"] = common.canonical_payload_digest(clean_payload)
    validate_metadata(schema_meta)
    bundle = {**clean_payload, "metadata": schema_meta}
    bundle_bytes = pickle.dumps(bundle, protocol=5)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    BUNDLE_PATH.write_bytes(bundle_bytes)
    with open(METADATA_PATH, "w", encoding="utf-8") as handle:
        json.dump(schema_meta, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    with open(OUT_DIR / "train_metrics.json", "w", encoding="utf-8") as handle:
        json.dump(
            {
                "validation": metrics,
                "seconds": round(time.time() - started, 1),
                "note": "simulated data; metrics are VALIDATION only (TEST never scored)",
                "artifactFile": BUNDLE_PATH.name,
                "artifactFileSha256": hashlib.sha256(BUNDLE_PATH.read_bytes()).hexdigest(),
                "artifactPayloadSha256": schema_meta["artifactSha256"],
                "provenance": {
                    "pipelineRunId": manifest["pipelineRunId"],
                    "trainingSplit": "split_24h=TRAIN (label-complete to 24h)",
                    "mlSplits": manifest["mlSplits"],
                    "trainWindow": {
                        "rows": int(mask_train.sum()),
                        "startDate": str(frame.loc[mask_train, "reference_dt"].min()),
                        "endDate": str(frame.loc[mask_train, "reference_dt"].max()),
                    },
                    "params": PARAMS,
                    "seed": SEED,
                    "trainedAt": datetime.now(timezone.utc).isoformat(),
                },
            },
            handle,
            indent=2,
        )
    print(f"bundle -> {BUNDLE_PATH}  ({round(time.time() - started, 1)}s)")
    print(f"metadata -> {METADATA_PATH} (schema-conformant, validated)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
