"""异常检测线公共件:会话级电池曲线特征、时间三段、冻结写入、PRF 指标。
契约纪律:anomaly_labels 只在评测时关联,绝不进训练特征;同一会话的所有采样窗口天然整体
归入该会话所属时间段(按 session started_at 切),不会拆到训练/测试两边。
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pandas as pd

DATA_ANALYSIS_ROOT = Path(__file__).resolve().parents[2]
CLEAN_DIR = DATA_ANALYSIS_ROOT / "datasets" / "analytics_full_180d_v1" / "clean"
OUT_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_anomaly"

DATASET_ID = "analytics_full_180d_v1"
MODEL_ID = "iforest-session-battery-v1"
MODEL_VERSION = "0.1.0"

TRAIN_END_EXCLUSIVE = pd.Timestamp("2026-05-01")
VALID_END_EXCLUSIVE = pd.Timestamp("2026-05-15")
TEST_END_EXCLUSIVE = pd.Timestamp("2026-05-30")

FEATURE_TABLE = OUT_DIR / "session_features.pkl"
BUNDLE_PATH = OUT_DIR / f"{MODEL_ID}.joblib"
TRAIN_METRICS_PATH = OUT_DIR / "train_metrics.json"
TEST_METRICS_PATH = OUT_DIR / f"test_metrics_{MODEL_ID}.json"

FEATURES = [
    "dur_min", "n_samples",
    "soc_start", "soc_end", "soc_gain", "dsoc_per_min",
    "cur_mean", "cur_max", "cur_std", "cur_cv", "cur_last",
    "volt_mean", "volt_min", "dvolt_per_min",
    "celldiff_mean", "celldiff_max",
    "temp_max", "temp_spread_mean", "temp_spread_max", "temp_rise_per_min",
    "avg_power_kw", "energy_per_min",
]


def load_table(name: str) -> pd.DataFrame:
    d = CLEAN_DIR / name
    parts = sorted(str(p) for p in d.glob("*.parquet"))
    if not parts:
        raise FileNotFoundError(f"clean 表缺失: {d}")
    return pd.concat([pd.read_parquet(p) for p in parts], ignore_index=True)


def split_of(ts: pd.Series) -> pd.Series:
    out = pd.Series("EXCLUDED", index=ts.index, dtype=object)
    out[ts < TRAIN_END_EXCLUSIVE] = "TRAIN"
    ok = (ts >= TRAIN_END_EXCLUSIVE) & (ts < VALID_END_EXCLUSIVE)
    out[ok] = "VALIDATION"
    ok = (ts >= VALID_END_EXCLUSIVE) & (ts < TEST_END_EXCLUSIVE)
    out[ok] = "TEST"
    return out


def session_features(samples: pd.DataFrame, sessions: pd.DataFrame) -> pd.DataFrame:
    """1.2M 条 5 分钟电池采样 → 每会话一行特征。相邻样本差分都在会话内做(组内 shift)。"""
    s = samples.sort_values(["session_id", "recorded_at"]).copy()
    g = s.groupby("session_id", sort=False)
    s["cell_diff"] = s["max_cell_voltage_v"] - s["min_cell_voltage_v"]
    s["temp_spread"] = s["max_temperature_c"] - s["min_temperature_c"]
    s["d_min"] = g["recorded_at"].diff().dt.total_seconds() / 60.0
    s["dsoc"] = g["soc_pct"].diff()
    s["dvolt"] = g["pack_voltage_v"].diff()
    s["dtemp"] = g["max_temperature_c"].diff()

    agg = g.agg(
        dur_min=("recorded_at", lambda x: (x.max() - x.min()).total_seconds() / 60.0),
        n_samples=("recorded_at", "size"),
        soc_start=("soc_pct", "first"), soc_end=("soc_pct", "last"),
        cur_mean=("charge_current_a", "mean"), cur_max=("charge_current_a", "max"),
        cur_std=("charge_current_a", "std"), cur_last=("charge_current_a", "last"),
        volt_mean=("pack_voltage_v", "mean"), volt_min=("pack_voltage_v", "min"),
        celldiff_mean=("cell_diff", "mean"), celldiff_max=("cell_diff", "max"),
        temp_max=("max_temperature_c", "max"),
        temp_spread_mean=("temp_spread", "mean"), temp_spread_max=("temp_spread", "max"),
        dsoc_per_min=("dsoc", "mean"), dvolt_per_min=("dvolt", "mean"),
        temp_rise_per_min=("dtemp", "mean"),
    ).reset_index()
    agg["soc_gain"] = agg["soc_end"] - agg["soc_start"]
    agg["cur_cv"] = agg["cur_std"] / agg["cur_mean"].replace(0, np.nan)

    ses = sessions[["session_id", "started_at", "ended_at", "energy_wh"]].copy()
    ses["started_at"] = pd.to_datetime(ses["started_at"])
    out = agg.merge(ses, on="session_id", how="inner")
    out["energy_per_min"] = out["energy_wh"] / out["dur_min"].replace(0, np.nan) / 60.0
    out["avg_power_kw"] = out["energy_wh"] / 1000.0 / (out["dur_min"] / 60.0).replace(0, np.nan)
    out["split"] = split_of(out["started_at"])
    return out


def prf(scores: np.ndarray, y: np.ndarray, threshold: float) -> dict:
    """会话级二分类 precision/recall/F1;scores>y 判为异常。"""
    flag = scores > threshold
    tp = int((flag & (y == 1)).sum())
    fp = int((flag & (y == 0)).sum())
    fn = int((~flag & (y == 1)).sum())
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return {"threshold": round(float(threshold), 4), "flagged": int(flag.sum()),
            "precision": round(precision, 4), "recall": round(recall, 4),
            "f1": round(f1, 4), "tp": tp, "fp": fp, "fn": fn}


def write_new_json(path: Path, payload: dict) -> None:
    """冻结语义:文件不存在才写入;已存在则要求逐字节等价(确定性重跑可通过,
    上游变过导致的差异必须显式归档新文件名,绝不静默覆盖首盲分数)。"""
    path = Path(path)
    if path.exists():
        # 与"序列化之后"比:int 键/numpy 标量在 json 往返后会变形,直接 == 会误判
        normalized = json.loads(json.dumps(payload, ensure_ascii=False, default=float))
        same = json.loads(path.read_text(encoding="utf-8")) == normalized
        if not same:
            raise FileExistsError(f"拒绝覆盖已冻结产物且重算不等价: {path}")
        print(f"复用已冻结文件(与重算一致): {path.name}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)
