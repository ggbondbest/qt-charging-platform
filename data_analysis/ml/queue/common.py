"""排队结果与等待预测线（第五线）：用户点"加入排队"那一刻，预测这次会不会白排、还要等多久。

与已有四条线的边界（互不共用标签）：
``availability`` 预测**有没有空闲桩**（站点×小时）、``load`` 预测**用多少电**、
``recommend`` 决定**去哪个站**、``anomaly``/``churn`` 看**这趟充得健不健康 / 这个人还回不回来**。
本线看的是**这一次排队值不值得等**——样本单位是排队事件（``queue_entries`` 一行），
决策时点唯一且明确：``joined_at`` 那一刻。``recommend`` 线明确把 ABANDONED/FAILED 排除在标签外
（只当站点侧统计原料），本线正是接住它放弃的那个标签。

数据纪律：只读消费 ``datasets/analytics_full_180d_v1/clean``（当前批次 parquet 分区），
产物一律落 ``outputs/ml_queue/``，不写数据集目录、不改任何带哈希的正式包、不生成脏数据。

用法：本文件不直接跑，由 build_data / train / evaluate / predict 以 ``from . import common`` 引用。
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import numpy as np
import pandas as pd

DATA_ANALYSIS_ROOT = Path(__file__).resolve().parents[2]
DATASET_DIR = DATA_ANALYSIS_ROOT / "datasets" / "analytics_full_180d_v1"
CLEAN_DIR = DATASET_DIR / "clean"
OUT_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_queue"
DATASET_ID = "analytics_full_180d_v1"

#: 本线全部产物绑定的发布批次；换批次即全部重跑，错配一律拒收（见 verify_batch）。
EXPECTED_PUBLISHED_BATCH_ID = "analytics-298aa3ee1401461fb06ea2bb96930dcf"

MODEL_ID = "gbdt-queue-abandon-v1"
WAIT_MODEL_ID = "gbdt-queue-wait-v1"
MODEL_VERSION = "0.1.0"
SEED = 20260914

MATRIX_PATH = OUT_DIR / "queue_matrix.pkl"
BUILD_SUMMARY_PATH = OUT_DIR / "build_summary.json"
BUNDLE_PATH = OUT_DIR / f"{MODEL_ID}.joblib"
TRAIN_METRICS_PATH = OUT_DIR / "train_metrics.json"
TEST_REPORT_PATH = OUT_DIR / "evaluation_report.json"
TEST_REPORT_MD = OUT_DIR / "evaluation_report.md"

#: 十轮滚动重训研究单独一个输出目录（与盲测目录互不覆盖；轮次文件支持断点续跑）。
ROLLING_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_queue_rolling"
ROLLING_ROUNDS_DIR = ROLLING_DIR / "rounds"
ROLLING_SUMMARY_PATH = ROLLING_DIR / "rolling_summary.json"
ROLLING_SUMMARY_MD = ROLLING_DIR / "rolling_summary.md"

#: 业务时区：数据按 UTC 存储，mlSplits 的日期是北京时间日历日（见 split_boundaries 的换算）。
BUSINESS_OFFSET_HOURS = 8

#: 这些列留在表里做分组、溯源和评价，绝不能进特征——status 进特征就是直接泄答案。
NON_FEATURE_COLUMNS = frozenset({
    "queue_id", "user_id", "station_id", "city_id", "joined_at", "called_at", "resolved_at",
    "status", "session_id", "attempt_id", "vehicle_id", "reservation_id", "outcome", "y_waste",
    "wait_min", "split", "joined_local", "business_date",
})

CATEGORICAL_FEATURES = ["site_type", "membership", "segment", "vehicle_class", "scenario_event",
                        "period", "weather", "connector_share_type"]


class BatchMismatch(RuntimeError):
    """产物或数据层与本线绑定的发布批次不一致——宁可拒算，不服错配模型。"""


# --------------------------------------------------------------------------- 数据装载


def read_manifest() -> dict:
    with open(DATASET_DIR / "serving_manifest.json", encoding="utf-8") as handle:
        return json.load(handle)


def verify_batch(manifest: dict | None = None) -> dict:
    """核对 serving manifest 的批次号，错配即抛 BatchMismatch（fail closed）。"""
    manifest = manifest or read_manifest()
    batch = manifest.get("publishedBatchId")
    if batch != EXPECTED_PUBLISHED_BATCH_ID:
        raise BatchMismatch(
            f"serving_manifest publishedBatchId={batch!r} != 本线绑定的 "
            f"{EXPECTED_PUBLISHED_BATCH_ID!r}；数据层换批次后本线必须重跑并重新发布")
    return manifest


def load_table(name: str) -> pd.DataFrame:
    """clean 层某张表的全量拼接（只读）。目录即表名，分区 *.parquet。"""
    parts = sorted(str(p) for p in (CLEAN_DIR / name).glob("*.parquet"))
    if not parts:
        raise FileNotFoundError(f"clean 表缺失: {CLEAN_DIR / name}")
    return pd.concat([pd.read_parquet(p) for p in parts], ignore_index=True)


# --------------------------------------------------------------------------- 时间切分


def split_boundaries(manifest: dict) -> dict[str, pd.Timestamp]:
    """把 mlSplits 的北京日历日换算成与数据同基准（UTC naive）的边界。

    manifest 语义（实测核对：118 + 30 + 30 = 178 天，正好等于 start→end 的天数差）：
    **start 是含端、end 是排端，且每个边界日就是下一段的第一天**。日期按北京时间计，而
    数据时间戳是 UTC（`periodStart = 2025-11-30T16:00:00Z` 即北京 2025-12-01 00:00 可证），
    所以边界 = D 日 00:00+08 转 UTC = D 减 8 小时。直接用 UTC 零点会整体错 8 小时，
    把 TRAIN 尾巴漏进 VALIDATION（成员 A 那行 prepare_data 的注释里踩过同一个坑）。
    """
    splits = manifest["mlSplits"]

    def boundary(date_str: str) -> pd.Timestamp:
        return pd.Timestamp(date_str) - pd.Timedelta(hours=BUSINESS_OFFSET_HOURS)

    return {
        "startInclusive": boundary(splits["start"]),
        "trainEndExclusive": boundary(splits["trainEnd"]),
        "validationEndExclusive": boundary(splits["validationEnd"]),
        "testEndExclusive": boundary(splits["end"]),
    }


def assign_split(ts: pd.Series, bounds: dict[str, pd.Timestamp]) -> pd.Series:
    """事件时间（UTC naive）→ TRAIN / VALIDATION / TEST / EXCLUDED。

    窗口外（早于 `start` 或 ≥ `end`）一律 EXCLUDED：mlSplits 已经排除了首尾模拟边界日，
    把它们塞回 TRAIN 等于用被污染的头几天拟合。不做随机拆分——按时间切是硬要求，
    防止用未来排队算过去特征。
    """
    converted = pd.to_datetime(ts)
    stamps = converted if isinstance(converted, pd.Series) else pd.Series(converted)
    out = pd.Series("EXCLUDED", index=stamps.index, dtype=object)
    train = (stamps >= bounds["startInclusive"]) & (stamps < bounds["trainEndExclusive"])
    validation = (stamps >= bounds["trainEndExclusive"]) & (stamps < bounds["validationEndExclusive"])
    test = (stamps >= bounds["validationEndExclusive"]) & (stamps < bounds["testEndExclusive"])
    out[train] = "TRAIN"
    out[validation] = "VALIDATION"
    out[test] = "TEST"
    return out


# --------------------------------------------------------------------------- 冻结写入


def write_new_json(path: Path, payload: dict) -> None:
    """独占创建 JSON：已存在即报错，绝不就地改写已发布产物。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2, default=str)
        handle.write("\n")


def write_new_pickle(path: Path, frame: pd.DataFrame) -> None:
    """独占创建 pickle：pandas 接受文件对象，所以 O_EXCL 护栏照样生效。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    with os.fdopen(fd, "wb") as handle:
        frame.to_pickle(handle)


def write_new_bytes(path: Path, blob: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    with os.fdopen(fd, "wb") as handle:
        handle.write(blob)


def require_empty_run_dir(extra_allowed: tuple[str, ...] = ()) -> None:
    """脚本入口的覆盖护栏：输出目录非空就拒绝运行（除非只含已批准的续跑文件）。"""
    if not OUT_DIR.exists():
        return
    strays = [p.name for p in OUT_DIR.iterdir() if p.name not in extra_allowed]
    if strays:
        raise FileExistsError(
            f"{OUT_DIR} 已有产物 {strays[:6]}；本线禁止就地改写已发布目录，请换新目录或先归档")


# --------------------------------------------------------------------------- 评价指标


def lift_at(y_true: np.ndarray, score: np.ndarray, q: float) -> float:
    """Top-q 命中率 / 基础率。>1 才是真收益。"""
    y_true = np.asarray(y_true)
    score = np.asarray(score, dtype=float)
    n = len(y_true)
    k = max(1, int(round(n * q)))
    top = np.argsort(-score, kind="stable")[:k]
    base = y_true.mean()
    if base <= 0:
        return float("nan")
    return float(y_true[top].mean() / base)


def brier(y_true: np.ndarray, prob: np.ndarray) -> float:
    return float(np.mean((np.asarray(prob, dtype=float) - np.asarray(y_true, dtype=float)) ** 2))


def calibration_bins(y_true: np.ndarray, prob: np.ndarray, bins: int = 10) -> pd.DataFrame:
    """期望校准表：每桶预测概率均值 vs 实测频率 vs 样本数。"""
    frame = pd.DataFrame({"y": np.asarray(y_true), "p": np.asarray(prob, dtype=float)})
    frame["bin"] = pd.qcut(frame["p"], bins, duplicates="drop")
    grouped = frame.groupby("bin", observed=True).agg(n=("y", "size"), predicted=("p", "mean"),
                                                      observed=("y", "mean"))
    return grouped.reset_index(drop=False)


def sha256_file(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def simulated_note() -> str:
    """所有对外指标都要带的口径声明，写死在这里免得哪个环节漏了。"""
    return ("全部指标为模拟数据测试结果（第二阶段发布批次 "
            f"{EXPECTED_PUBLISHED_BATCH_ID}），不代表真实运营数据表现。")
