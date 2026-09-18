"""给已上线的 q50 点模型加 split conformal 区间,不重训任何模型。
做法:VALIDATION 上逐 horizon 算点预测的符号残差 y-pred,取 q10/q90 作下/上偏移
(非对称,80% 名义覆盖),中位数与线上点输出逐比特一致。
契约只允许 {timestamp,value} 的 point(validate_prediction 精确匹配键集),
所以区间不进 /predict/load 响应,定位是离线分析与大屏演示层,产出 conformal_intervals.json。
TEST 不参与:覆盖率的无偏依据就是校准用的 VALIDATION 本身;分布漂移(时间外推)可能压低实际覆盖,如实标注。
用法(仓库根目录):python -m data_analysis.ml.load.conformal
"""

from __future__ import annotations

import json
import time

import joblib
import numpy as np
import pandas as pd

from . import common

ALPHA = 0.20
HORIZONS = tuple(range(1, 25))
OUT_DIR = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
BUNDLE_PATH = OUT_DIR / f"{common.MODEL_ID}.joblib"
REPORT_PATH = OUT_DIR / "conformal_intervals.json"


def horizon_offsets(
    frame: pd.DataFrame, matrix: pd.DataFrame, model, horizon: int
) -> dict:
    """一个 horizon 的偏移量 + 在 VALIDATION 上的经验覆盖/宽度(样本内,校准即评估,自洽)。"""
    label_col = f"label_power_kw_h{horizon:02d}"
    ok = (frame["split_24h"] == "VALIDATION") & frame[label_col].notna()
    y = frame.loc[ok, label_col].to_numpy(dtype=float)
    limits = frame.loc[ok, "rated_capacity_kw"].to_numpy(dtype=float)
    pred = np.clip(model.predict(matrix.loc[ok]).astype(float), 0.0, limits)
    resid = y - pred
    lower_shift = float(np.quantile(resid, ALPHA / 2))
    upper_shift = float(np.quantile(resid, 1.0 - ALPHA / 2))
    lower, upper = apply_offsets(pred, lower_shift, upper_shift, limits)
    inside = (y >= lower) & (y <= upper)
    return {
        "lowerShiftKw": round(lower_shift, 4),
        "upperShiftKw": round(upper_shift, 4),
        "coverage80": round(float(inside.mean()), 4),
        "meanWidthKw": round(float((upper - lower).mean()), 3),
        "n": int(len(y)),
    }


def apply_offsets(
    pred: np.ndarray, lower_shift: float, upper_shift: float, limits: np.ndarray
) -> tuple[np.ndarray, np.ndarray]:
    """点值加偏移后 clip 到 [0,额定],再强制 lower<=pred<=upper(clip 可能把边界翻过点值)。"""
    lower = np.clip(pred + lower_shift, 0.0, limits)
    upper = np.clip(pred + upper_shift, 0.0, limits)
    return np.minimum(lower, pred), np.maximum(upper, pred)


def main() -> int:
    started = time.time()
    bundle = joblib.load(BUNDLE_PATH)
    models = bundle["models"]
    frame = pd.read_pickle(OUT_DIR / "joined_usable.pkl")
    matrix = common.features_matrix(frame)

    per_horizon = {
        f"h{h:02d}": horizon_offsets(frame, matrix, models[h], h) for h in HORIZONS
    }
    report = {
        "modelId": common.MODEL_ID,
        "modelVersion": common.MODEL_VERSION,
        "method": "split conformal on VALIDATION signed residuals, asymmetric q10/q90 offsets",
        "nominalCoverage": 1 - ALPHA,
        "intendedUse": "offline analysis & dashboard; contract point payload has no interval fields",
        "note": "模拟数据测试结果口径下的离线工件; coverage 为校准集(VALIDATION)样本内值",
        "perHorizon": per_horizon,
        "pooled": {
            "coverage80Mean": round(
                float(np.mean([e["coverage80"] for e in per_horizon.values()])), 4
            ),
            "meanWidthKwMean": round(
                float(np.mean([e["meanWidthKw"] for e in per_horizon.values()])), 3
            ),
        },
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "runtimeSeconds": round(time.time() - started, 1),
    }
    with open(REPORT_PATH, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)

    print(f"{'hor':5}{'lowerShift':>11}{'upperShift':>11}{'cov80':>8}{'width':>8}")
    for key, e in per_horizon.items():
        print(
            f"{key:<5}{e['lowerShiftKw']:>11.3f}{e['upperShiftKw']:>11.3f}"
            f"{e['coverage80']:>8.4f}{e['meanWidthKw']:>8.2f}"
        )
    print("saved ->", REPORT_PATH, f"({round(time.time() - started, 1)}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
