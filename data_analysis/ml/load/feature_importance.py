"""出厂 hgb-deep bundle 的 permutation importance,只在 split_24h==VALIDATION 完整切片(目标非空、不抽样)上跑
n_repeats=5、seed=42、scoring 负 MAE;不拟合、不读 TEST。
每时距原始降幅 clip 到 0 后归一化为份额,特征得分=三时距份额均值。
单列重要性≈0 多因强相关列分摊信息,单列为零不等于整块特征可删,整块去留须做组级消融重训。
baseline MAE 是 VALIDATION 裸分(未 clip 到 [0, rated_capacity_kw]),比 train_metrics.json 的交付数字略高,口径差非 bug。
用法(仓库根目录):python -m data_analysis.ml.load.feature_importance
"""

from __future__ import annotations

import json
import time
from datetime import datetime, timezone

import joblib
import numpy as np
import pandas as pd
import sklearn
from sklearn.inspection import permutation_importance
from sklearn.metrics import get_scorer

from . import common

SEED = 42
N_REPEATS = 5
HORIZONS = (1, 6, 24)
OUT_DIR = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
ANALYSIS_DIR = OUT_DIR / "analysis"
BUNDLE_PATH = OUT_DIR / f"{common.MODEL_ID}.joblib"
REPORT_PATH = ANALYSIS_DIR / "feature_importance.json"


def validation_slice(frame: pd.DataFrame, horizon: int) -> tuple[pd.DataFrame, np.ndarray]:
    """取 split_24h 窗口下、*horizon* 目标非空的 VALIDATION 行(含离线特征矩阵)。"""
    label_col = f"label_power_kw_h{horizon:02d}"
    ok = (frame["split_24h"] == "VALIDATION") & frame[label_col].notna()
    matrix = common.features_matrix(frame.loc[ok])
    assert list(matrix.columns) == list(bundle_feature_columns), (
        "offline matrix columns disagree with the shipped bundle feature_columns"
    )
    return matrix, frame.loc[ok, label_col].to_numpy(dtype=float)


def main() -> int:
    started = time.time()
    global bundle_feature_columns
    bundle = joblib.load(BUNDLE_PATH)
    bundle_feature_columns = bundle["feature_columns"]
    models = bundle["models"]
    frame = pd.read_pickle(OUT_DIR / "joined_usable.pkl")

    scoring = get_scorer("neg_mean_absolute_error")
    raw: dict[int, dict[str, dict[str, float]]] = {}
    baselines: dict[str, dict] = {}
    for horizon in HORIZONS:
        x_valid, y_valid = validation_slice(frame, horizon)
        result = permutation_importance(
            models[horizon],
            x_valid,
            y_valid,
            scoring=scoring,
            n_repeats=N_REPEATS,
            random_state=SEED,
            n_jobs=-1,
        )
        # sklearn 1.7 删了 baseline_score 字段;基线 MAE 用未置换预测重算(与内部同口径)
        baseline_mae = float(
            np.abs(models[horizon].predict(x_valid) - y_valid).mean()
        )
        baselines[f"h{horizon:02d}"] = {
            "baseline_mae_kw": baseline_mae,
            "rows": int(len(y_valid)),
        }
        raw[horizon] = {
            column: {
                "importance_mean": float(mean),
                "importance_std": float(std),
            }
            for column, mean, std in zip(
                bundle_feature_columns,
                result.importances_mean,
                result.importances_std,
            )
        }
        print(
            f"h{horizon:02d} done  baseline_mae={baseline_mae:.4f} kW"
            f"  rows={len(y_valid)}  ({time.time() - started:.0f}s elapsed)"
        )

    # clip 负值(降幅为负=抽样噪声)再除以总和,使不同量纲时距可比;
    # 份额是相对量,低份额只说明单列置换时相关列仍撑得住,不说明信息冗余
    shares = {
        horizon: {
            column: values["importance_mean"] for column, values in per_feature.items()
        }
        for horizon, per_feature in raw.items()
    }
    normalized: dict[str, dict[str, float]] = {}
    for horizon, per_column in shares.items():
        clipped = {column: max(value, 0.0) for column, value in per_column.items()}
        total = sum(clipped.values())
        normalized[horizon] = {
            column: (value / total if total > 0 else 0.0)
            for column, value in clipped.items()
        }
    avg_share = {
        column: float(np.mean([normalized[horizon][column] for horizon in HORIZONS]))
        for column in bundle_feature_columns
    }

    table = sorted(
        (
            {
                "feature": column,
                "avg_normalized_importance": avg_share[column],
                **{
                    f"h{horizon:02d}": {
                        "raw_mae_increase_kw": raw[horizon][column]["importance_mean"],
                        "raw_mae_increase_std_kw": raw[horizon][column]["importance_std"],
                        "normalized_share": normalized[horizon][column],
                    }
                    for horizon in HORIZONS
                },
            }
            for column in bundle_feature_columns
        ),
        key=lambda row: row["avg_normalized_importance"],
        reverse=True,
    )
    for rank, row in enumerate(table, start=1):
        row["rank"] = rank

    report = {
        "modelId": common.MODEL_ID,
        "modelVersion": common.MODEL_VERSION,
        "scoring": "neg_mean_absolute_error",
        "nRepeats": N_REPEATS,
        "randomState": SEED,
        "split": 'split_24h == "VALIDATION" and label notna (full slice, no subsampling)',
        "horizons": list(HORIZONS),
        "baselines": baselines,
        "normalization": (
            "per horizon: raw drop clipped at 0, rescaled to sum 1; "
            "avg_normalized_importance = mean of the 3 horizon shares"
        ),
        "nFeatures": len(bundle_feature_columns),
        "runtimeSeconds": round(time.time() - started, 1),
        "environment": {
            "python": __import__("platform").python_version(),
            "scikit-learn": sklearn.__version__,
            "pandas": pd.__version__,
            "numpy": np.__version__,
        },
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "note": "VALIDATION only; TEST never read, no model refit performed",
        "ranking": table,
    }
    ANALYSIS_DIR.mkdir(parents=True, exist_ok=True)
    with open(REPORT_PATH, "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2)
        handle.write("\n")

    print(f"\nrank  feature                          avg_norm   "
          f"{'h01_raw':>9} {'h06_raw':>9} {'h24_raw':>9}  (kW MAE increase)")
    for row in table:
        print(
            f"{row['rank']:>4}  {row['feature']:<30} "
            f"{row['avg_normalized_importance']:.5f}   "
            + "  ".join(
                f"{row[f'h{h:02d}']['raw_mae_increase_kw']:+8.4f}" for h in HORIZONS
            )
        )
    print(f"\nreport -> {REPORT_PATH}  ({time.time() - started:.0f}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
