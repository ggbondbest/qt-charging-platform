"""Curve demo: actual vs predicted 24h load curve with the 80% quantile band.

Picks ONE VALIDATION station-hour window with a fixed random seed (never TEST),
then draws, for lead hours 1..24 of that reference instant:

- the actual curve  = label_power_kw_h01..h24 (solid blue line);
- the model forecast at the three supported contract horizons {+1h, +6h, +24h}
  = median of ``hgb-quantile-history24-v1`` (orange marker + 80% interval
  error bar; dashed connector between the three points is visual only), after
  the shared protocol: clip to [0, rated_capacity_kw] then sort so
  lower <= median <= upper;
- for reference, the shipped point model ``hgb-deep-history24-v1`` (v0.2)
  predictions at the same three horizons are printed and saved to the summary
  JSON (not drawn, to keep a two-series chart).

Feature rows are taken from the offline matrix (common.features_matrix), the
path used at training time; online/offline parity of both bundles is audited
by ``intervals.py`` and ``train_lean.py`` (max diff <= 1.42e-14).

Outputs (new files only):
  data_analysis/outputs/ml_load/analysis/curve_demo.png
  data_analysis/outputs/ml_load/analysis/curve_demo_summary.json

Usage (repo root):
    python -m data_analysis.ml.load.curve_demo
"""

from __future__ import annotations

import json
import os

os.environ.setdefault("KMP_DUPLICATE_LIB_OK", "TRUE")

import joblib  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

import matplotlib  # noqa: E402

matplotlib.use("Agg")
from matplotlib import font_manager, pyplot as plt  # noqa: E402

from . import common  # noqa: E402
from .intervals import (  # noqa: E402
    BUNDLE_PATH as QUANTILE_BUNDLE,
    CONTRACT_HORIZONS,
    QUANTILES,
    ordered_interval,
)

OUT_DIR = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load" / "analysis"
PNG_PATH = OUT_DIR / "curve_demo.png"
SUMMARY_PATH = OUT_DIR / "curve_demo_summary.json"
SHIPPED_BUNDLE = (
    common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load" / f"{common.MODEL_ID}.joblib"
)

SEED = 20260913

# Validated reference palette (light surface), slots 1/2; band is slot-2 alpha.
INK_PRIMARY = "#0b0b0b"
INK_MUTED = "#52514e"
GRID = "#e8e7e3"
SURFACE = "#fcfcfb"
SERIES_ACTUAL = "#2a78d6"
SERIES_PRED = "#eb6834"


def pick_validation_row(frame: pd.DataFrame) -> pd.Index:
    """One VALIDATION row (split_24h) with all 24 hourly labels finite."""
    labels = frame[common.TARGET_COLUMNS]
    pool = frame[
        (frame["split_24h"] == "VALIDATION")
        & labels.notna().all(axis=1)
        & np.isfinite(labels.to_numpy(dtype=float)).all(axis=1)
    ]
    return pool.sample(n=1, random_state=SEED).index


def setup_fonts() -> str:
    available = {f.name for f in font_manager.fontManager.ttflist}
    for name in ("Microsoft YaHei", "SimHei", "Noto Sans CJK SC"):
        if name in available:
            plt.rcParams["font.sans-serif"] = [name, "DejaVu Sans"]
            plt.rcParams["axes.unicode_minus"] = False
            return name
    print("WARNING: no CJK font found; Chinese labels may render as boxes")
    return "none"


def main() -> int:
    frame = pd.read_pickle(common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load" / "joined_usable.pkl")
    matrix = common.features_matrix(frame)
    idx = pick_validation_row(frame)[0]
    row = matrix.loc[[idx]]
    origin = frame.loc[idx]
    capacity = float(origin["rated_capacity_kw"])

    point_bundle = joblib.load(SHIPPED_BUNDLE)
    quantile_bundle = joblib.load(QUANTILE_BUNDLE)

    actual = np.array(
        [float(origin[f"label_power_kw_h{h:02d}"]) for h in range(1, 25)], dtype=float
    )
    horizons = list(CONTRACT_HORIZONS)
    medians, lowers, uppers, point_preds = [], [], [], []
    for h in horizons:
        raw = np.array(
            [
                [float(quantile_bundle["quantile_models"][(h, q)].predict(row)[0]) for q in QUANTILES]
            ],
            dtype=float,
        )
        lower, median, upper = ordered_interval(raw, np.array([capacity]))
        lowers.append(float(lower[0]))
        medians.append(float(median[0]))
        uppers.append(float(upper[0]))
        point_preds.append(
            float(np.clip(point_bundle["models"][h].predict(row)[0], 0.0, capacity))
        )

    font = setup_fonts()
    fig, ax = plt.subplots(figsize=(9.2, 5.2), dpi=160)
    fig.patch.set_facecolor(SURFACE)
    ax.set_facecolor(SURFACE)

    hours = np.arange(1, 25)
    ax.plot(
        hours, actual, color=SERIES_ACTUAL, lw=2.0, marker="o", ms=5,
        markerfacecolor="white", markeredgecolor=SERIES_ACTUAL, markeredgewidth=1.5,
        label="实际负荷(逐小时)", zorder=3,
    )
    # Visual-only connector through the three modeled leads.
    ax.plot(
        horizons, medians, color=SERIES_PRED, lw=1.6, ls="--", zorder=3,
        label="模型预测(仅 +1/+6/+24h,虚线为视觉连接)",
    )
    ax.errorbar(
        horizons, medians, yerr=[np.array(medians) - lowers, np.array(uppers) - np.array(medians)],
        fmt="o", ms=9, mfc=SERIES_PRED, mec="white", mew=2, ls="none",
        ecolor=SERIES_PRED, elinewidth=2.0, capsize=6, capthick=2.0, zorder=4,
        label="中位数 ± 80% 区间(q0.1–q0.9)",
    )
    for x, m, lo, up in zip(horizons, medians, lowers, uppers):
        right_side = x >= 20
        ax.annotate(
            f"{m:.1f}\n[{lo:.1f}, {up:.1f}]", xy=(x, up),
            xytext=(x - 0.35 if right_side else x + 0.35, up + 2.5),
            fontsize=8, color=INK_MUTED, ha="right" if right_side else "left",
        )

    ax.set_xlabel("预测时距(自参考时刻起,小时)", fontsize=11, color=INK_PRIMARY)
    ax.set_ylabel("平均充电负荷 (kW)", fontsize=11, color=INK_PRIMARY)
    ax.set_title(
        f"VALIDATION 样例:站点 {origin['station_id']}(额定 {capacity:.0f} kW),"
        f"参考时刻 {origin['reference_time']}",
        fontsize=11.5, color=INK_PRIMARY, pad=12,
    )
    ax.set_xticks(hours)
    ax.set_xlim(0.5, 24.5)
    ax.set_ylim(0, max(max(actual), max(uppers)) + 12)
    ax.grid(True, axis="y", color=GRID, lw=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=INK_MUTED)
    handles, labels = ax.get_legend_handles_labels()
    fig.legend(
        handles, labels, loc="lower center", ncol=3, fontsize=9,
        frameon=False, bbox_to_anchor=(0.5, 0.028),
    )
    fig.text(
        0.01, 0.005,
        "数据为分析链路模拟生成的数据;区间为分位数模型 q0.1/q0.9(裁剪至 [0, 额定容量] 后排序)。",
        fontsize=8, color=INK_MUTED,
    )
    fig.tight_layout(rect=(0, 0.075, 1, 1))
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    fig.savefig(PNG_PATH, facecolor=SURFACE)
    plt.close(fig)

    summary = {
        "script": "data_analysis/ml/load/curve_demo.py",
        "seed": SEED,
        "fontUsed": font,
        "rowIndex": int(idx),
        "split": "VALIDATION (split_24h)",
        "stationId": str(origin["station_id"]),
        "cityId": str(origin["city_id"]),
        "referenceTime": str(origin["reference_time"]),
        "ratedCapacityKw": capacity,
        "horizons": horizons,
        "actualAtHorizons": [float(origin[f"label_power_kw_h{h:02d}"]) for h in horizons],
        "quantileMedian": medians,
        "intervalLower80": lowers,
        "intervalUpper80": uppers,
        "shippedPointPred_hgb_deep_v02": point_preds,
        "actualCurve_h01_to_h24": [float(v) for v in actual],
        "png": str(PNG_PATH),
        "note": "simulated data; row chosen from VALIDATION only, TEST never read here",
    }
    with open(SUMMARY_PATH, "w", encoding="utf-8") as handle:
        json.dump(summary, handle, ensure_ascii=False, indent=2)
    print(json.dumps(summary, ensure_ascii=False, indent=1))
    print(f"png -> {PNG_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
