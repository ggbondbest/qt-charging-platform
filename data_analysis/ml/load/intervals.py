"""契约三步长(h01/h06/h24)× q{0.1,0.5,0.9} 各拟合一个 HistGradientBoostingRegressor(loss="quantile"),
hgb-deep 配方(见 PARAMS),只用 TRAIN 行,X 与 train.py 同一特征路径。
VALIDATION 评测(TEST 全程不参与):预测 clip 到 [0, rated_capacity_kw] 后按样本排序消分位数交叉,报告 pinball、80% coverage、平均宽度 kW。
产出只写 race2/:hgb-quantile-history24-v1.joblib、interval_metrics.json;在线服务见 IntervalLoadForecaster。
用法: python -m data_analysis.ml.load.intervals(仓库根目录)
"""

from __future__ import annotations

import json
import platform
import time
from datetime import datetime, timedelta, timezone

import joblib
import numpy as np
import pandas as pd
import sklearn
from sklearn.ensemble import HistGradientBoostingRegressor

from data_analysis.contracts.model import HISTORY_HOURS, PredictionContext

from . import common

SEED = 42
QUANTILES = (0.1, 0.5, 0.9)
CONTRACT_HORIZONS = (1, 6, 24)
MODEL_ID = "hgb-quantile-history24-v1"
MODEL_VERSION = "0.1.0"
BASE_MODEL_ID = common.MODEL_ID  # 已上线点模型 hgb-deep-history24-v1,沿用其 metadata 做批次溯源

OUT_DIR = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
RACE2_DIR = OUT_DIR / "race2"
BUNDLE_PATH = RACE2_DIR / f"{MODEL_ID}.joblib"
METRICS_PATH = RACE2_DIR / "interval_metrics.json"
BASE_BUNDLE_PATH = OUT_DIR / f"{BASE_MODEL_ID}.joblib"

PARAMS = {
    "max_iter": 600,
    "learning_rate": 0.03,
    "max_leaf_nodes": 63,
    "min_samples_leaf": 20,
    "l2_regularization": 0.5,
    "early_stopping": False,
    "random_state": SEED,
}


def new_quantile_model(quantile: float) -> HistGradientBoostingRegressor:
    return HistGradientBoostingRegressor(
        loss="quantile", quantile=quantile,
        categorical_features="from_dtype", **PARAMS,
    )


def pinball_loss(y_true: np.ndarray, y_pred: np.ndarray, quantile: float) -> float:
    error = y_true - y_pred
    return float(np.maximum(quantile * error, (quantile - 1.0) * error).mean())


def ordered_interval(raw: np.ndarray, limits: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """raw (n,3)=[q10,q50,q90] 先按列 clip 到 [0,limit] 再沿轴排序,强制 lower<=median<=upper:clip 单调不改顺序,独立 pinball 模型的分位数交叉只能靠排序消。"""
    clipped = np.clip(raw, 0.0, limits[:, None])
    sorted_vals = np.sort(clipped, axis=1)
    return sorted_vals[:, 0], sorted_vals[:, 1], sorted_vals[:, 2]


def evaluation_report(
    models: dict[tuple[int, float], HistGradientBoostingRegressor],
    matrix: pd.DataFrame,
    frame: pd.DataFrame,
) -> dict:
    """全部指标只在 split_24h==VALIDATION 且目标有限的行上计算,TEST 不参与。"""
    mask_valid = frame["split_24h"] == "VALIDATION"
    report: dict[str, dict] = {}
    for horizon in CONTRACT_HORIZONS:
        label_col = f"label_power_kw_h{horizon:02d}"
        label = frame[label_col]
        valid_ok = mask_valid & label.notna() & np.isfinite(label)
        valid_index = frame.index[valid_ok]
        y = frame.loc[valid_ok, label_col].to_numpy(dtype=float)
        limits = frame.loc[valid_index, "rated_capacity_kw"].to_numpy(dtype=float)
        x_valid = matrix.loc[valid_ok]
        raw = np.column_stack(
            [models[(horizon, q)].predict(x_valid).astype(float) for q in QUANTILES]
        )
        lower, median, upper = ordered_interval(raw, limits)
        entry: dict = {
            "n": int(len(y)),
            "pinball": {
                f"q{q:.1f}": pinball_loss(y, pred, q)
                for q, pred in zip(QUANTILES, (lower, median, upper))
            },
            "coverage80": float(np.mean((y >= lower) & (y <= upper))),
            "meanWidthKw": float(np.mean(upper - lower)),
            "medianMae": float(np.mean(np.abs(y - median))),
            "crossingFixRate": float(np.mean(np.any(np.diff(raw, axis=1) < 0, axis=1))),
        }
        report[f"h{horizon:02d}"] = entry
    return report


def train_quantile_models(
    matrix: pd.DataFrame, frame: pd.DataFrame
) -> tuple[dict, dict]:
    """只在 TRAIN 行上拟合 9 个分位数模型(3 步长 × 3 分位数)。返回 (models, train_info)。"""
    mask_train = frame["split_24h"] == "TRAIN"
    models: dict[tuple[int, float], HistGradientBoostingRegressor] = {}
    train_info: dict[str, dict] = {}
    for horizon in CONTRACT_HORIZONS:
        label_col = f"label_power_kw_h{horizon:02d}"
        label = frame[label_col]
        train_ok = mask_train & label.notna() & np.isfinite(label)
        x_train = matrix.loc[train_ok]
        y_train = frame.loc[train_ok, label_col].to_numpy(dtype=float)
        for quantile in QUANTILES:
            started = time.time()
            model = new_quantile_model(quantile)
            model.fit(x_train, y_train)
            models[(horizon, quantile)] = model
            print(
                f"h{horizon:02d} q{quantile:.1f} fit n={int(train_ok.sum())} "
                f"in {time.time() - started:.1f}s"
            )
        train_info[f"h{horizon:02d}"] = {"trainRows": int(train_ok.sum())}
    return models, train_info


class IntervalLoadForecaster:
    """在线区间预测器:只加载受信任 bundle(pickle 加载即执行任意代码);特征与点预测器同经 common.build_feature_row 从 24 条原始 history 重建,与离线同一代码路径,parity 由此保证。"""

    def __init__(self, bundle_path=None):
        bundle_path = bundle_path or BUNDLE_PATH
        bundle = joblib.load(bundle_path)
        self.models = bundle["quantile_models"]
        self.feature_columns = bundle["feature_columns"]
        self.calendar = bundle["calendar"]
        self.model_id = bundle["model_id"]
        self.model_version = bundle.get("model_version", "0.1.0")
        self.metadata = bundle["metadata"]
        self._levels = bundle["category_levels"]

    def _matrix(self, feature_row: dict) -> pd.DataFrame:
        matrix = pd.DataFrame([feature_row], columns=self.feature_columns)
        for column in common.CATEGORICAL_FEATURES:
            matrix[column] = pd.Categorical(matrix[column], categories=self._levels[column])
        return matrix

    def predict_interval(self, history: list[dict], context: PredictionContext) -> list[dict]:
        row = common.build_feature_row(history, context.reference_time, self.calendar)
        matrix = self._matrix(row)
        ref = common.parse_utc(context.reference_time)
        capacity_limit = float(row["rated_capacity_kw"])
        if context.horizon_hours not in CONTRACT_HORIZONS:
            raise ValueError("horizonHours must be 1, 6 or 24")
        points = []
        for horizon in CONTRACT_HORIZONS:
            if horizon > context.horizon_hours:
                break
            raw = np.array(
                [self.models[(horizon, q)].predict(matrix)[0] for q in QUANTILES], dtype=float
            )
            lower, median, upper = ordered_interval(
                raw[None, :], np.array([capacity_limit])
            )
            points.append(
                {
                    "timestamp": common.format_utc(ref + timedelta(hours=horizon - 1)),
                    "value": float(median[0]),
                    "lower": float(lower[0]),
                    "upper": float(upper[0]),
                }
            )
        return points


def _parity_check(models, frame, matrix, calendar, category_levels, hourly) -> int:
    """抽 8 个 VALIDATION 窗口(跳过历史不完整的)在线重算,9 个分位数原始输出与离线矩阵逐一比对 parity;TEST 不碰。"""
    pool = frame[frame["split_24h"] == "VALIDATION"]
    hourly_by_station = {k: g for k, g in hourly.groupby("station_id")}
    max_diff = 0.0
    checked = 0
    sample = pool.sample(n=8, random_state=13)
    for idx, row in sample.iterrows():
        station_hours = hourly_by_station[row.station_id]
        window = station_hours[
            (station_hours["recorded_at"] >= row.reference_dt - pd.Timedelta(hours=HISTORY_HOURS))
            & (station_hours["recorded_at"] < row.reference_dt)
        ]
        if len(window) != HISTORY_HOURS:
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
        built = common.build_feature_row(history, row.reference_time, calendar)
        for column in common.NUMERIC_FEATURES:
            diff = abs(float(built[column]) - float(getattr(row, column)))
            if diff > 1e-9:
                print(f"FEATURE MISMATCH {column}: {diff}")
                return 1
        online = pd.DataFrame([built], columns=common.FEATURE_COLUMNS)
        for column in common.CATEGORICAL_FEATURES:
            online[column] = pd.Categorical(online[column], categories=category_levels[column])
        offline = matrix.loc[[idx]]
        for horizon in CONTRACT_HORIZONS:
            for quantile in QUANTILES:
                model = models[(horizon, quantile)]
                diff = abs(float(model.predict(online)[0]) - float(model.predict(offline)[0]))
                max_diff = max(max_diff, diff)
        checked += 1
    print(f"online/offline parity: {checked} windows, max raw diff {max_diff:.2e}")
    return 0 if max_diff < 1e-9 else 1


def main() -> int:
    started = time.time()
    RACE2_DIR.mkdir(parents=True, exist_ok=True)
    frame = pd.read_pickle(OUT_DIR / "joined_usable.pkl")
    matrix = common.features_matrix(frame)
    calendar = common.build_calendar_lookup(frame)
    category_levels = {
        column: sorted(frame[column].astype(str).unique().tolist())
        for column in common.CATEGORICAL_FEATURES
    }

    models, train_info = train_quantile_models(matrix, frame)
    report = evaluation_report(models, matrix, frame)
    for key, entry in report.items():
        print(
            f"{key} valid  pinball q0.1={entry['pinball']['q0.1']:.4f} "
            f"q0.5={entry['pinball']['q0.5']:.4f} q0.9={entry['pinball']['q0.9']:.4f}  "
            f"cov80={entry['coverage80']:.4f}  width={entry['meanWidthKw']:.3f} kW  "
            f"medMAE={entry['medianMae']:.4f}  n={entry['n']}"
        )

    base_bundle = joblib.load(BASE_BUNDLE_PATH)
    base_meta = base_bundle["metadata"]
    pooled = {
        "coverage80Mean": float(np.mean([e["coverage80"] for e in report.values()])),
        "meanWidthKwMean": float(np.mean([e["meanWidthKw"] for e in report.values()])),
        "pinballMean": {
            f"q{q:.1f}": float(np.mean([e["pinball"][f"q{q:.1f}"] for e in report.values()]))
            for q in QUANTILES
        },
        "medianMaeMean": float(np.mean([e["medianMae"] for e in report.values()])),
    }
    metadata = {
        "schemaVersion": "1.0.0",
        "featureVersion": base_meta["featureVersion"],
        "modelId": MODEL_ID,
        "modelVersion": MODEL_VERSION,
        "target": "load",
        "datasetId": base_meta["datasetId"],
        "sourceManifestSha256": base_meta["sourceManifestSha256"],
        "trainingPublishedBatchId": base_meta["trainingPublishedBatchId"],
        "trainEndExclusive": base_meta["trainEndExclusive"],
        "validationEndExclusive": base_meta["validationEndExclusive"],
        "testEndExclusive": base_meta["testEndExclusive"],
        "historyHours": int(base_meta["historyHours"]),
        "supportedHorizons": list(CONTRACT_HORIZONS),
        "quantiles": list(QUANTILES),
        "featureColumns": list(common.FEATURE_COLUMNS),
        "baseModelId": BASE_MODEL_ID,
        "artifactFile": BUNDLE_PATH.name,
        "metrics": {**pooled, "unit": "kW", "scope": "VALIDATION (TEST never scored)"},
        "dependencies": {
            "python": platform.python_version(),
            "scikit-learn": sklearn.__version__,
            "pandas": pd.__version__,
            "numpy": np.__version__,
            "joblib": joblib.__version__,
        },
        "provenance": {
            "params": {**PARAMS, "loss": "quantile"},
            "seed": SEED,
            "trainingSplit": "split_24h=TRAIN (label-complete to 24h)",
            "trainRowsByHorizon": train_info,
            "trainedAt": datetime.now(timezone.utc).isoformat(),
        },
    }
    bundle = {
        "quantile_models": models,
        "feature_columns": common.FEATURE_COLUMNS,
        "category_levels": category_levels,
        "calendar": calendar,
        "model_id": MODEL_ID,
        "model_version": MODEL_VERSION,
        "metadata": metadata,
    }
    joblib.dump(bundle, BUNDLE_PATH)

    with open(METRICS_PATH, "w", encoding="utf-8") as handle:
        json.dump(
            {
                "modelId": MODEL_ID,
                "validation": report,
                "pooled": pooled,
                "protocol": "clip to [0, rated_capacity_kw] then sort per sample so lower<=median<=upper",
                "note": "simulated data; metrics are VALIDATION only (TEST never scored)",
                "seconds": round(time.time() - started, 1),
            },
            handle,
            indent=2,
        )
    print(f"bundle -> {BUNDLE_PATH}")
    print(f"metrics -> {METRICS_PATH} ({round(time.time() - started, 1)}s)")

    hourly = common.load_hourly_metrics()
    return _parity_check(models, frame, matrix, calendar, category_levels, hourly)


if __name__ == "__main__":
    raise SystemExit(main())
