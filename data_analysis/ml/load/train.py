"""Train one gradient-boosting model per forecast horizon (h01..h24).

Recipe = v0.4 "hgb-q50": HistGradientBoostingRegressor per horizon
(loss="quantile", quantile=0.5, max_iter=600, lr=0.03, max_leaf_nodes=63,
min_samples_leaf=20, l2=0.5, early_stopping=False, seed=42). The median
objective directly optimizes MAE — the metric the contract reports — and beat
the v0.2 L2-loss race winner "hgb-deep" on VALIDATION (see EVALUATION.md §9/§10).
On the shared ``ml_features_hourly`` columns
with city_id/station_id as native categoricals. All 24 models fit on the
strictest ``split_24h`` TRAIN window so every model only ever saw samples with
complete 24h-ahead labels, and are all scored on ``split_24h`` VALIDATION rows
with a non-null target. Persistence and same-hour-last-week baselines are
recorded next to each model. Predictions are clipped to
[0, rated_capacity_kw], exactly what the online predictor will do.

The bundle carries ``model_metadata.schema.json``-conformant metadata under
``metadata`` and, as a file, ``<model_id>.metadata.json`` next to the joblib
(contract README §6: deliver schema metadata with the model file). The
schema's single ``metrics`` object holds the pooled VALIDATION numbers of the
three contract horizons (h01/h06/h24) — TEST was never scored — and
``testSamples`` is the count of scored (row, horizon) pairs on VALIDATION;
per-horizon detail lives in ``train_metrics.json``. ``artifactSha256`` is the
canonical payload digest from :func:`common.canonical_payload_digest` (a file
cannot embed the hash of its own bytes); the bundle is therefore written as
plain protocol-5 pickle — stabilized once so the digest is re-computable by
any consumer — which ``joblib.load`` reads transparently. The whole-file
SHA-256 is recorded in ``train_metrics.json`` as ``artifactFileSha256``.

Usage (repo root):
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
    """Pool VALIDATION MAE/RMSE of the three contract horizons into the single
    ``metrics`` object that model_metadata.schema.json allows."""
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
    """Hard-check the emitted metadata against the shipped JSON schema."""
    with open(SCHEMA_PATH, encoding="utf-8") as handle:
        schema = json.load(handle)
    required = [key for key in schema["required"] if key not in metadata]
    extra = [key for key in metadata if key not in schema["properties"]]
    if required or extra:
        raise AssertionError(f"metadata not schema-conformant: missing={required} extra={extra}")
    try:
        import jsonschema
    except ImportError:  # contract-test dep absent: structural check above still ran
        print("jsonschema not installed; skipped full schema validation")
        return
    jsonschema.validate(metadata, schema)


def clip_to_capacity(values: np.ndarray, frame: pd.DataFrame, index) -> np.ndarray:
    limits = frame.loc[index, "rated_capacity_kw"].to_numpy(dtype=float)
    return np.clip(values, 0.0, limits)


def last_week_predictions(frame: pd.DataFrame, power_lookup: pd.Series) -> np.ndarray:
    # keep tz-aware dtypes (no .to_numpy()) so reindex aligns with the UTC index
    wanted = pd.MultiIndex.from_arrays(
        [frame["station_id"], frame["reference_dt"] - pd.Timedelta(days=7)]
    )
    return power_lookup.reindex(wanted).to_numpy(dtype=float)


def main() -> int:
    started = time.time()
    manifest = common.read_manifest()
    frame = pd.read_pickle(OUT_DIR / "joined_usable.pkl")
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

        week = last_week_predictions(frame.loc[valid_ok], power_lookup)
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
        "artifactSha256": "0" * 64,  # replaced below, after one stabilization round-trip
        "metrics": contract_metrics(metrics, {h: metrics[f"h{h:02d}"]["gbdt"]["n"] for h in CONTRACT_HORIZONS}),
        "dependencies": {
            "python": platform.python_version(),
            "scikit-learn": sklearn.__version__,
            "pandas": pd.__version__,
            "numpy": np.__version__,
            "joblib": joblib.__version__,
        },
    }
    # Stabilize the serialization once: plain-pickle bytes taken from a loaded
    # state are byte-stable (unlike fresh-fit objects and joblib streams), so
    # the recorded artifact digest is re-computable from the delivered file.
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
