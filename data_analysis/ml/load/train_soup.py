"""参数扰动的模型汤候选(ensemble 最后一搏;seed bagging 已证实对 HistGB 无效,见 bag_metrics.json)。
HistGB 无行/列采样且 early_stopping=False 时近似确定性,换种子=复制模型,均值无增益。
多样性改从超参来:M1=上线原配方(复用 bundle),M2 更深更慢,M3 宽叶子强正则,M4 低学习率长跑,
四成员等权平均(各自先 clip 再平均,同 v0.4 口径),VALIDATION 逐时距对比单模型。
第一轮只算指标不落成员 bundle(省盘);有转正价值再重训序列化。TEST 不参与。
用法(仓库根目录,约 45 分钟):python -m data_analysis.ml.load.train_soup
"""

from __future__ import annotations

import json
import time

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingRegressor

from . import common
from .train import PARAMS, regression_metrics

SOUP_MEMBERS = {
    "M2": {**PARAMS, "max_leaf_nodes": 31, "learning_rate": 0.02, "max_iter": 900},
    "M3": {**PARAMS, "max_leaf_nodes": 127, "l2_regularization": 2.0, "learning_rate": 0.05, "max_iter": 400},
    "M4": {**PARAMS, "learning_rate": 0.015, "max_iter": 1200},
}
HORIZONS = tuple(range(1, 25))
CANDIDATE_ID = "hgb-q50soup-history24-v1"
OUT_DIR = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
SOUP_DIR = OUT_DIR / "soup"
REPORT_PATH = SOUP_DIR / "soup_metrics.json"


def main() -> int:
    started = time.time()
    SOUP_DIR.mkdir(parents=True, exist_ok=True)
    shipped = joblib.load(OUT_DIR / f"{common.MODEL_ID}.joblib")
    frame = pd.read_pickle(OUT_DIR / "joined_usable.pkl")
    matrix = common.features_matrix(frame)
    mask_train = frame["split_24h"] == "TRAIN"
    with open(OUT_DIR / "train_metrics.json", encoding="utf-8") as handle:
        base_valid = json.load(handle)["validation"]

    per_horizon: dict[str, dict] = {}
    for horizon in HORIZONS:
        label_col = f"label_power_kw_h{horizon:02d}"
        label = frame[label_col]
        train_ok = mask_train & label.notna() & np.isfinite(label)
        valid_ok = (frame["split_24h"] == "VALIDATION") & label.notna() & np.isfinite(label)
        y = frame.loc[valid_ok, label_col].to_numpy(dtype=float)
        limits = frame.loc[valid_ok, "rated_capacity_kw"].to_numpy(dtype=float)
        x_valid = matrix.loc[valid_ok]
        x_train = matrix.loc[train_ok]
        y_train = frame.loc[train_ok, label_col].to_numpy(dtype=float)

        preds = [np.clip(shipped["models"][horizon].predict(x_valid).astype(float), 0.0, limits)]
        member_mae: dict[str, float] = {}
        for name, params in SOUP_MEMBERS.items():
            model = HistGradientBoostingRegressor(categorical_features="from_dtype", **params)
            model.fit(x_train, y_train)
            p = np.clip(model.predict(x_valid).astype(float), 0.0, limits)
            member_mae[name] = regression_metrics(y, p)["mae"]
            preds.append(p)

        m = regression_metrics(y, np.mean(preds, axis=0))
        base_mae = base_valid[f"h{horizon:02d}"]["gbdt"]["mae"]
        per_horizon[f"h{horizon:02d}"] = {
            "maeShipped": round(base_mae, 4),
            "maeSoup4": round(m["mae"], 4),
            "deltaKw": round(m["mae"] - base_mae, 4),
            "deltaPct": round(100 * (m["mae"] - base_mae) / base_mae, 2),
            "memberMae": {k: round(v, 4) for k, v in member_mae.items()},
        }
        print(
            f"h{horizon:02d} shipped={base_mae:.4f} soup={m['mae']:.4f} "
            f"({per_horizon[f'h{horizon:02d}']['deltaPct']:+.2f}%) "
            f"[{time.time() - started:.0f}s]"
        )

    contract = [per_horizon[f"h{h:02d}"] for h in (1, 6, 24)]
    report = {
        "candidateId": CANDIDATE_ID,
        "members": {"M1(shipped)": PARAMS, **SOUP_MEMBERS},
        "blend": "equal weight, clip-then-average",
        "scope": "VALIDATION only; TEST never scored",
        "note": "模拟数据。转正门槛=三个契约时距全部改善(bagging 那轮全 0,已排除)",
        "contractHorizons": {f"h{h:02d}": per_horizon[f"h{h:02d}"] for h in (1, 6, 24)},
        "improvedHorizons": int(sum(r["deltaKw"] < 0 for r in per_horizon.values())),
        "promotableOnValidation": bool(all(r["deltaKw"] < 0 for r in contract)),
        "perHorizon": per_horizon,
        "runtimeSeconds": round(time.time() - started, 1),
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    with open(REPORT_PATH, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)
    print(f"\npromotable: {report['promotableOnValidation']}  improved {report['improvedHorizons']}/24")
    print("report ->", REPORT_PATH)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
