"""推荐线公共件:数据装载、时间三段切分、排序指标、冻结写入。
只读消费 analytics_full_180d_v1/clean(parquet 分区),任何产物都落 outputs/ml_recommend/,不碰数据集目录。
与空闲桩预测的边界:这里只用"截至事件时刻已发生"的站点统计做排序特征,不做任何空闲/可用性预测模型。

用法:本文件不直接跑,由 build_data / train / evaluate 以 `from . import common` 引用。
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import numpy as np
import pandas as pd

DATA_ANALYSIS_ROOT = Path(__file__).resolve().parents[2]
CLEAN_DIR = DATA_ANALYSIS_ROOT / "datasets" / "analytics_full_180d_v1" / "clean"
OUT_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_recommend"
DATASET_ID = "analytics_full_180d_v1"

MODEL_ID = "gbdt-rank-incity-v1"
MODEL_VERSION = "0.1.0"

# 事件流(STARTED 选址)按时间三段:训练 / 选型 / 冻结盲评。TEST 只在定稿后批阅一次。
TRAIN_END_EXCLUSIVE = pd.Timestamp("2026-05-01")
VALID_END_EXCLUSIVE = pd.Timestamp("2026-05-15")
TEST_END_EXCLUSIVE = pd.Timestamp("2026-05-30")

ROLLING_DAYS = 28          # 站点日级滚动窗
HOUR_LOOKBACK_DAYS = 7     # 同时段近一周特征的回看

LONG_TABLE = OUT_DIR / "recommend_matrix.pkl"
SUMMARY_PATH = OUT_DIR / "build_summary.json"
BUNDLE_PATH = OUT_DIR / f"{MODEL_ID}.joblib"
TRAIN_METRICS_PATH = OUT_DIR / "train_metrics.json"


def load_table(name: str) -> pd.DataFrame:
    """clean 层某张表的全量拼接(只读)。目录即表名,分区 *.parquet。"""
    d = CLEAN_DIR / name
    parts = sorted(str(p) for p in d.glob("*.parquet"))
    if not parts:
        raise FileNotFoundError(f"clean 表缺失: {d}")
    return pd.concat([pd.read_parquet(p) for p in parts], ignore_index=True)


def split_of(ts: pd.Series) -> pd.Series:
    """事件时间 → TRAIN/VALIDATION/TEST;TEST_END 之外归 EXCLUDED(留头寸给未来日切)。"""
    out = pd.Series("EXCLUDED", index=ts.index, dtype=object)
    out[ts < TRAIN_END_EXCLUSIVE] = "TRAIN"
    ok = (ts >= TRAIN_END_EXCLUSIVE) & (ts < VALID_END_EXCLUSIVE)
    out[ok] = "VALIDATION"
    ok = (ts >= VALID_END_EXCLUSIVE) & (ts < TEST_END_EXCLUSIVE)
    out[ok] = "TEST"
    return out


def ranking_metrics(ranks: np.ndarray, n_candidates: int) -> dict:
    """ranks=每个事件中正确候选的名次(1 最好)。五个指标全是"猜得越靠前越高"。"""
    ranks = np.asarray(ranks, dtype=float)
    return {
        "n": int(len(ranks)),
        "hitAt1": round(float(np.mean(ranks <= 1)), 4),
        "hitAt3": round(float(np.mean(ranks <= 3)), 4),
        "mrr": round(float(np.mean(1.0 / ranks)), 4),
        "ndcgAt5": round(float(np.mean(1.0 / np.log2(np.minimum(ranks, n_candidates) + 1.0))), 4),
        "meanRank": round(float(ranks.mean()), 3),
        "medianRank": float(np.median(ranks)),
    }


def rank_within_groups(scores: np.ndarray, onehot_labels: np.ndarray) -> np.ndarray:
    """输入按 (事件×候选) 长表排列、每事件恰好 5 行且顺序一致的矩阵化排名。
    返回每个事件正确候选的名次(1 最好);并列取最坏名次(悲观平局),不给模型留侥幸。"""
    n_candidates = 5
    matrix = np.asarray(scores, dtype=float).reshape(-1, n_candidates)
    onehot = np.asarray(onehot_labels, dtype=float).reshape(-1, n_candidates)
    if not np.isfinite(matrix).all():
        raise ValueError("分数含 NaN/Inf:NaN 会让比较全 False、名次塌成 0,mrr 出现 inf")
    if not np.all(onehot.sum(axis=1) == 1):
        raise ValueError("每个事件必须恰有一个正候选")
    chosen = (matrix * onehot).sum(axis=1, keepdims=True)
    rank = (matrix > chosen).sum(axis=1) + 1
    return rank + ((matrix == chosen).sum(axis=1) - 1)


def write_new_json(path: Path, payload: dict) -> None:
    """冻结语义:文件已存在就拒绝,首盲分数不可被重跑覆盖(与负荷线同一纪律)。
    O_EXCL 独占创建,并发同路径恰好一个成功——exists()->open(w) 的 check-then-act
    两个进程能同时过检,后写者静默替换首盲评分(负荷线复审 P2#B 同款修复)。"""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(payload, ensure_ascii=False, indent=2)
    try:
        fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    except FileExistsError:
        raise FileExistsError(f"拒绝覆盖已冻结产物: {path}")
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        handle.write(text)


def ensure_out_dir() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
