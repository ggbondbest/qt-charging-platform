"""168h(上周同一时刻功率)长窗特征的离线探路,只做诊断不发货。
契约 FEATURE_VERSION=history24-v1 钉死 39 列,加列=破坏合同,转正须组长先放行新 FEATURE_VERSION;
这里给 3 个契约时距各训一个 40 列探针(其余参数与 v0.4 完全一致),VALIDATION 与单种子基线同口径对比。
上周同时刻值从 station_hourly 以 (station_id, reference_dt-168h) 回连,缺失留 NaN(HistGB 原生吃 NaN)。
TEST 全程不参与。用法(仓库根目录):python -m data_analysis.ml.load.lag168_probe
"""

from __future__ import annotations

import json
import time

import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingRegressor

from . import common
from .train import PARAMS, SEED, regression_metrics

OUT_DIR = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
PROBE_DIR = OUT_DIR / "probe"
CONTRACT_HORIZONS = (1, 6, 24)


def add_lag168(frame: pd.DataFrame, hourly: pd.DataFrame) -> np.ndarray:
    lookup = hourly.set_index(["station_id", "recorded_at"])["mean_power_kw"]
    wanted = pd.MultiIndex.from_arrays(
        [frame["station_id"], frame["reference_dt"] - pd.Timedelta(hours=168)]
    )
    return lookup.reindex(wanted).to_numpy(dtype=float)


def main() -> int:
    started = time.time()
    PROBE_DIR.mkdir(parents=True, exist_ok=True)
    frame = pd.read_pickle(OUT_DIR / "joined_usable.pkl")
    hourly = common.load_hourly_metrics()

    base = common.features_matrix(frame)
    x = base.copy()
    x["lag_power_kw_h168"] = add_lag168(frame, hourly)

    with open(OUT_DIR / "train_metrics.json", encoding="utf-8") as handle:
        baseline_validation = json.load(handle)["validation"]

    mask_train = frame["split_24h"] == "TRAIN"
    result: dict[str, dict] = {}
    for horizon in CONTRACT_HORIZONS:
        label_col = f"label_power_kw_h{horizon:02d}"
        label = frame[label_col]
        train_ok = mask_train & label.notna() & np.isfinite(label)
        valid_ok = (frame["split_24h"] == "VALIDATION") & label.notna() & np.isfinite(label)

        model = HistGradientBoostingRegressor(categorical_features="from_dtype", **PARAMS).fit(
            x.loc[train_ok], frame.loc[train_ok, label_col].to_numpy(dtype=float)
        )
        index = frame.index[valid_ok]
        y = frame.loc[valid_ok, label_col].to_numpy(dtype=float)
        pred = np.clip(
            model.predict(x.loc[valid_ok]).astype(float),
            0.0,
            frame.loc[valid_ok, "rated_capacity_kw"].to_numpy(dtype=float),
        )
        m = regression_metrics(y, pred)
        ref_mae = baseline_validation[f"h{horizon:02d}"]["gbdt"]["mae"]
        result[f"h{horizon:02d}"] = {
            "mae40col": round(m["mae"], 4),
            "maeShipped39col": ref_mae,
            "deltaKw": round(m["mae"] - ref_mae, 4),
            "deltaPct": round(100 * (m["mae"] - ref_mae) / ref_mae, 2),
        }
        print(
            f"h{horizon:02d}  40col={m['mae']:.4f}  shipped={ref_mae:.4f}  "
            f"delta={m['mae'] - ref_mae:+.4f} kW ({time.time() - started:.0f}s)"
        )

    payload = {
        "probe": "lag_power_kw_h168 appended to the shipped 39-column contract matrix",
        "status": "diagnostic only; FEATURE_VERSION blocks shipping without a leader contract release",
        "note": "模拟数据 VALIDATION 口径;TEST never scored",
        "params": {**PARAMS, "seed": SEED},
        "result": result,
    }
    with open(PROBE_DIR / "lag168_probe.json", "w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)
    print("saved ->", PROBE_DIR / "lag168_probe.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
