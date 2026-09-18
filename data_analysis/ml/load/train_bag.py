"""多种子 bagging 候选(hgb-q50bag):与 v0.4 同配方,每时距 3 个种子(q50 树随机性不同)预测取均值。
seed=42 直接复用已上线 bundle 里的模型,只补训 seed=7/2024 两套,共 48 个新模型。
选型只看 VALIDATION(逐时距对比单种子基线);胜出才谈转正+一次性 TEST 计分,否则归档为诊断。
不改动 shipped bundle,候选单独落盘 bag/,整包体积约 ×3(~345MB),转正须先和组长确认投递方式。
TEST 全程不参与。用法(仓库根目录,耗时约 = v0.4 训练 ×2):python -m data_analysis.ml.load.train_bag
"""

from __future__ import annotations

import json
import time
from datetime import datetime, timezone

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingRegressor

from . import common
from .train import PARAMS, regression_metrics

BAG_SEEDS = (42, 7, 2024)
HORIZONS = tuple(range(1, 25))
CANDIDATE_ID = "hgb-q50bag-history24-v1"
OUT_DIR = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
BAG_DIR = OUT_DIR / "bag"
BUNDLE_PATH = BAG_DIR / f"{CANDIDATE_ID}.joblib"
REPORT_PATH = BAG_DIR / "bag_metrics.json"


def main() -> int:
    started = time.time()
    BAG_DIR.mkdir(parents=True, exist_ok=True)
    shipped = joblib.load(OUT_DIR / f"{common.MODEL_ID}.joblib")
    frame = pd.read_pickle(OUT_DIR / "joined_usable.pkl")
    matrix = common.features_matrix(frame)
    mask_train = frame["split_24h"] == "TRAIN"

    with open(OUT_DIR / "train_metrics.json", encoding="utf-8") as handle:
        base_valid = json.load(handle)["validation"]

    per_horizon: dict[str, dict] = {}
    new_models: dict[tuple[int, int], HistGradientBoostingRegressor] = {}
    for horizon in HORIZONS:
        label_col = f"label_power_kw_h{horizon:02d}"
        label = frame[label_col]
        train_ok = mask_train & label.notna() & np.isfinite(label)
        valid_ok = (frame["split_24h"] == "VALIDATION") & label.notna() & np.isfinite(label)
        y = frame.loc[valid_ok, label_col].to_numpy(dtype=float)
        limits = frame.loc[valid_ok, "rated_capacity_kw"].to_numpy(dtype=float)
        x_valid = matrix.loc[valid_ok]

        # 均值走 clip 后:与单模型口径一致(先各自裁剪再平均,防止越界值互相拉回)
        preds = [
            np.clip(
                shipped["models"][horizon].predict(x_valid).astype(float), 0.0, limits
            )  # seed 42 = 上线同款,直接复用
        ]
        for seed in BAG_SEEDS[1:]:
            model = HistGradientBoostingRegressor(
                categorical_features="from_dtype", **{**PARAMS, "random_state": seed}
            ).fit(matrix.loc[train_ok], frame.loc[train_ok, label_col].to_numpy(dtype=float))
            new_models[(horizon, seed)] = model
            preds.append(np.clip(model.predict(x_valid).astype(float), 0.0, limits))

        bagged = np.mean(preds, axis=0)
        m_bag = regression_metrics(y, bagged)
        base_mae = base_valid[f"h{horizon:02d}"]["gbdt"]["mae"]
        per_horizon[f"h{horizon:02d}"] = {
            "maeSingle": round(base_mae, 4),
            "maeBag3": round(m_bag["mae"], 4),
            "rmseBag3": round(m_bag["rmse"], 4),
            "deltaKw": round(m_bag["mae"] - base_mae, 4),
            "deltaPct": round(100 * (m_bag["mae"] - base_mae) / base_mae, 2),
        }
        print(
            f"h{horizon:02d} single={base_mae:.4f} bag3={m_bag['mae']:.4f} "
            f"({per_horizon[f'h{horizon:02d}']['deltaPct']:+.2f}%) "
            f"[{time.time() - started:.0f}s]"
        )

    contract = [per_horizon[f"h{h:02d}"] for h in (1, 6, 24)]
    verdict = all(row["deltaKw"] < 0 for row in contract)
    report = {
        "candidateId": CANDIDATE_ID,
        "recipe": {**PARAMS, "bagSeeds": list(BAG_SEEDS)},
        "scope": "VALIDATION only; TEST never scored for this candidate",
        "note": "模拟数据。转正门槛=三个契约时距 VALIDATION MAE 全部改善",
        "contractHorizons": {f"h{h:02d}": per_horizon[f"h{h:02d}"] for h in (1, 6, 24)},
        "allHorizonsWin": int(sum(r["deltaKw"] < 0 for r in per_horizon.values())),
        "promotableOnValidation": bool(verdict),
        "bundleSizeConcernMb": "~345 (3x shipped), leader decision before delivery",
        "perHorizon": per_horizon,
        "trainedAt": datetime.now(timezone.utc).isoformat(),
        "runtimeSeconds": round(time.time() - started, 1),
    }

    # 候选 bundle:补齐三套完整(42 复用对象引用),供后续消融/转正用;转正与否以 report 为准
    bundle = {
        "models": {
            h: {s: (shipped["models"][h] if s == 42 else new_models[(h, s)]) for s in BAG_SEEDS}
            for h in HORIZONS
        },
        "feature_columns": common.FEATURE_COLUMNS,
        "category_levels": shipped["category_levels"],
        "calendar": shipped["calendar"],
        "model_id": CANDIDATE_ID,
        "base_model_id": common.MODEL_ID,
        "metadata": {**shipped["metadata"], "modelId": CANDIDATE_ID, "metrics": None},
    }
    joblib.dump(bundle, BUNDLE_PATH)
    with open(REPORT_PATH, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)
    print(
        f"\nverdict(VALIDATION 三契约时距全改善才 True): {verdict}\n"
        f"candidate -> {BUNDLE_PATH}\nreport  -> {REPORT_PATH}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
