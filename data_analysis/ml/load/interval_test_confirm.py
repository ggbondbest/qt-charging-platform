"""对区间产出物 hgb-quantile-history24-v1 做唯一一次确认性(非盲)TEST 评分。
切分按契约列取(h01->split_1h、h06->split_6h、h24->split_24h,TEST 且目标有限),协议与 intervals.py 一致:clip 到 [0, rated_capacity_kw] 后按样本排序消交叉。
不拟合、不选型:VALIDATION 晋升已定,这是对冻结产出物的描述性评分,只批这一次且必须披露;本任务首盲归点模型 hgb-deep v0.2(test_metrics.json),TEST 再被用于选型或多看一次都会让泛化指标失真。
输出 data_analysis/outputs/ml_load/race2/interval_test_summary.json;用法(仓库根目录): python -m data_analysis.ml.load.interval_test_confirm
"""

from __future__ import annotations

import json
import os

os.environ.setdefault("KMP_DUPLICATE_LIB_OK", "TRUE")

import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402
import joblib  # noqa: E402

from . import common  # noqa: E402
from .intervals import (  # noqa: E402
    BUNDLE_PATH,
    CONTRACT_HORIZONS,
    QUANTILES,
    ordered_interval,
    pinball_loss,
)

OUT_PATH = BUNDLE_PATH.parent / "interval_test_summary.json"
SPLIT_BY_HORIZON = {1: "split_1h", 6: "split_6h", 24: "split_24h"}


def main() -> int:
    bundle = joblib.load(BUNDLE_PATH)
    models = bundle["quantile_models"]
    frame = pd.read_pickle(common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load" / "joined_usable.pkl")
    matrix = common.features_matrix(frame)

    report: dict[str, dict] = {}
    for horizon in CONTRACT_HORIZONS:
        label_col = f"label_power_kw_h{horizon:02d}"
        split_col = SPLIT_BY_HORIZON[horizon]
        label = frame[label_col]
        mask = (frame[split_col] == "TEST") & label.notna() & np.isfinite(label)
        y = frame.loc[mask, label_col].to_numpy(dtype=float)
        limits = frame.loc[mask, "rated_capacity_kw"].to_numpy(dtype=float)
        x_test = matrix.loc[mask]
        raw = np.column_stack(
            [models[(horizon, q)].predict(x_test).astype(float) for q in QUANTILES]
        )
        lower, median, upper = ordered_interval(raw, limits)
        report[f"h{horizon:02d}"] = {
            "splitColumn": split_col,
            "n": int(len(y)),
            "pinball": {
                f"q{q:.1f}": pinball_loss(y, pred, q)
                for q, pred in zip(QUANTILES, (lower, median, upper))
            },
            "coverage80": float(np.mean((y >= lower) & (y <= upper))),
            "meanWidthKw": float(np.mean(upper - lower)),
            "medianMae": float(np.mean(np.abs(y - median))),
            "crossingFixRate": float(np.mean(np.any(np.diff(raw, axis=1) < 0, axis=1))),
            "rangeLegalRate": float(np.mean((median >= 0.0) & (median <= limits))),
        }
        entry = report[f"h{horizon:02d}"]
        print(
            f"h{horizon:02d} TEST({split_col}) n={entry['n']}  "
            f"pinball q0.1={entry['pinball']['q0.1']:.4f} q0.5={entry['pinball']['q0.5']:.4f} "
            f"q0.9={entry['pinball']['q0.9']:.4f}  cov80={entry['coverage80']:.4f}  "
            f"width={entry['meanWidthKw']:.3f} kW  medMAE={entry['medianMae']:.4f}"
        )

    pooled = {
        "coverage80Mean": float(np.mean([e["coverage80"] for e in report.values()])),
        "meanWidthKwMean": float(np.mean([e["meanWidthKw"] for e in report.values()])),
        "pinballMean": {
            f"q{q:.1f}": float(np.mean([e["pinball"][f"q{q:.1f}"] for e in report.values()]))
            for q in QUANTILES
        },
        "medianMaeMean": float(np.mean([e["medianMae"] for e in report.values()])),
    }
    payload = {
        "modelId": bundle["model_id"],
        "modelVersion": bundle["model_version"],
        "scope": "TEST",
        "gradingStatus": "CONFIRMATORY, NON-BLIND — the first-blind TEST grading of this "
        "task belongs to the shipped point model hgb-deep-history24-v1 v0.2 "
        "(test_metrics.json, 2026-09-13). This file is the single disclosed "
        "confirmation pass for the promoted interval analysis artifact and was "
        "computed AFTER its VALIDATION promotion decision; it did not "
        "participate in any selection.",
        "protocol": "clip to [0, rated_capacity_kw] then sort per sample so lower<=median<=upper; "
        "contract split columns (h01->split_1h, h06->split_6h, h24->split_24h)",
        "note": "simulated data; TEST numbers are descriptive only",
        "test": report,
        "pooled": pooled,
    }
    # 确认性评分同样一次性的:文件存在即拒绝重写(评审 P2#4 的同一规则)
    common.write_new_json(OUT_PATH, payload)
    print(f"-> {OUT_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
