"""新站选址线（第九线预留项转实做）：站级真实需求反推的空间覆盖模型 + 留一站回测。

诚实前提（预留时写"真没人流量数据"，本线不假装解决它）：仓库没有候选地块的人流/POI/交通量
任何一张表，需求**只能从既有 25 站的真实服务记录反推**（会话量、5 分钟充电 tick 数、排队弃队
`ABANDONED` 作为"未满足需求"代理）。因此本线交付的是**确定性的场模型 + 一次真盲测回测**，
不是一个已被验证有外推力的模型——回测结论（见 README）是负的。

已知的口径脏处（写在这里、也写进产物，不留到 README 才说）：
* ``abandoned`` 里 476/10,143（4.69%）的弃队，同一 user 在同一站 24h 内又开成了会话——这部分
  "想充没充上"后来充上了，``demand_incl_unmet`` 会把它们双计；
* ``CALL_EXPIRED``（4,037 条）同为未满足需求，本线**没有**计入 ``abandoned``——并入会改变站间
  排序、而本线没做那份敏感性分析，故只披露不合并。

样本单位：一站一行（站级需求强度）+ 候选网格点。本线**读** `charging_sessions` /
`queue_entries` / `charger_telemetry` / `stations`，全部只读，零新增数据。

用法：本文件不直接跑，由 backtest / score 引用。
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pandas as pd

from data_analysis.ml.common.artifacts import dependency_versions, invocation, sha256_file, stamp

DATA_ANALYSIS_ROOT = Path(__file__).resolve().parents[2]
SOURCE_DATASET_DIR = DATA_ANALYSIS_ROOT / "datasets" / "analytics_full_180d_v1"
CLEAN_DIR = SOURCE_DATASET_DIR / "clean"

OUT_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_siting"
DATASET_ID = "analytics_full_180d_v1"

#: 全线唯一随机源（random 基线的置换）。写进产物，换 seed 即换产物。
SEED = 20260915
#: 业务日历基准：与发布批其它表同口径（北京 = UTC+8），观测天数按业务日数，不按 UTC 日历日。
BUSINESS_OFFSET_HOURS = 8

#: 与第九/变压器线同一发布批；换批次必须重跑（见 verify_batch）。
EXPECTED_PUBLISHED_BATCH_ID = "analytics-298aa3ee1401461fb06ea2bb96930dcf"

BACKTEST_JSON = OUT_DIR / "backtest.json"
BACKTEST_MD = OUT_DIR / "backtest.md"
OPPORTUNITIES_CSV = OUT_DIR / "candidate_opportunities.csv"
SCORE_JSON = OUT_DIR / "score_summary.json"

#: 距离衰减吸引半径（km）网格：站最近邻均值≈4.2km、城内站距 p50≈6.6km（本批实测），故取 2..12km 一档。
#: 半径档位本身是模型假设，非数据事实。
RADII_KM = (2.0, 3.0, 5.0, 8.0, 12.0)
CAPTURE_KM = 3.0

SOURCE_TABLES = {
    "stations": "station_id→city_id/latitude/longitude/site_type（坐标与分组）",
    "charging_sessions": "站级需求强度：会话数（主口径）",
    "charger_telemetry": "5 分钟充电 tick 数（独立于会话计数的需求强度交叉核对；"
                         "本表 interval_seconds 恒为 300，一行 = 一个 tick ≠ 一分钟）",
    "queue_entries": "ABANDONED 弃队数：站级'未满足需求'代理，叠加到需求强度",
}


class BatchMismatch(RuntimeError):
    """产物或数据层与本线绑定的发布批次不一致——宁可拒算，不服错配。"""


def read_source_manifest() -> dict:
    with open(SOURCE_DATASET_DIR / "serving_manifest.json", encoding="utf-8") as handle:
        return json.load(handle)


def verify_batch(manifest: dict | None = None) -> dict:
    manifest = manifest or read_source_manifest()
    batch = manifest.get("publishedBatchId")
    if batch != EXPECTED_PUBLISHED_BATCH_ID:
        raise BatchMismatch(
            f"serving_manifest publishedBatchId={batch!r} != 本线绑定的 {EXPECTED_PUBLISHED_BATCH_ID!r}")
    return manifest


def load_clean_table(name: str, columns: list[str] | None = None) -> pd.DataFrame:
    """clean 层某张表全量拼接（只读）。缺列先按 parquet schema 核对再报错。"""
    import pyarrow.parquet as pq

    parts = sorted(str(p) for p in (CLEAN_DIR / name).glob("*.parquet"))
    if not parts:
        raise FileNotFoundError(f"clean 表缺失: {CLEAN_DIR / name}")
    if columns:
        available = set(pq.read_schema(parts[0]).names)
        missing = [c for c in columns if c not in available]
        if missing:
            raise KeyError(f"clean 表 {name} 没有这些列：{missing}")
    frames = [pd.read_parquet(p, columns=columns) for p in parts]
    return pd.concat(frames, ignore_index=True)


def _naive_utc(series: pd.Series) -> pd.Series:
    return pd.to_datetime(series, utc=True).dt.tz_localize(None)


def station_intensity() -> pd.DataFrame:
    """站级需求强度表：每站一行——坐标、城、会话数、充电 tick 数、弃队数、观测业务天数。

    ``demand`` 主口径 = 会话数；``demand_per_day`` 按**业务日**（北京日历日，=UTC+8）折算，
    与数据集名 ``180d`` 及发布批其它表的日口径一致（按 UTC 日历日数会得到 181，系统性低 0.55%）。
    充电 tick 数是**独立计数口径**（不共享会话计数的噪声），用于交叉核对场模型是否只对某一口径敏感；
    遥测 ``interval_seconds`` 恒为 300，故一行 = 一个 5 分钟 tick，**不是**一分钟。
    ``abandoned`` 是未满足需求代理，供打分腿叠加（其双计问题见模块 docstring）。
    """
    stations = load_clean_table("stations", ["station_id", "city_id", "latitude", "longitude", "site_type"])
    sessions = load_clean_table("charging_sessions", ["station_id", "started_at"])
    sessions["started_at"] = _naive_utc(sessions["started_at"])
    counts = sessions.groupby("station_id").size().rename("sessions")
    business_days = sessions["started_at"] + pd.Timedelta(hours=BUSINESS_OFFSET_HOURS)
    days = int(pd.DatetimeIndex(business_days).normalize().nunique())

    tel = load_clean_table("charger_telemetry", ["station_id", "state", "recorded_at"])
    charging_tick_count = (tel[tel["state"] == "CHARGING"].groupby("station_id").size()
                           .rename("charging_ticks"))

    queue = load_clean_table("queue_entries", ["station_id", "status"])
    abandoned = (queue[queue["status"] == "ABANDONED"].groupby("station_id").size()
                 .rename("abandoned"))

    frame = stations.set_index("station_id")
    frame = frame.join([counts, charging_tick_count, abandoned], how="left")
    for column in ["sessions", "charging_ticks", "abandoned"]:
        frame[column] = frame[column].fillna(0).astype("int64")
    frame = frame.reset_index()
    frame["obs_days"] = days
    frame["demand"] = frame["sessions"].astype(float)
    frame["demand_per_day"] = frame["demand"] / max(days, 1)
    frame["demand_incl_unmet"] = frame["demand"] + frame["abandoned"].astype(float)
    return frame


def data_note() -> str:
    return ("全部指标为模拟数据测试结果（第二阶段发布批次 "
            f"{EXPECTED_PUBLISHED_BATCH_ID}），不代表真实运营数据表现。本线未新增任何数据：需求强度、"
            "弃队代理与坐标都是对发布批次 clean 表的只读确定性聚合；本线读的表：" + "、".join(SOURCE_TABLES))


def source_table_digests() -> dict[str, str]:
    """本线读的每张 clean 表的内容摘要（表级 = 各分区文件 sha256 串起来的 sha256）。

    批次号只回答"哪一批"，不回答"这批的表有没有被换过/写坏"。产物里冻一份表摘要，
    才谈得上"报告与输入同生共死"（对齐 ``ml/common/artifacts`` 的 sourceManifestSha256 纪律）。
    """
    digests = {}
    for name in SOURCE_TABLES:
        parts = sorted((CLEAN_DIR / name).glob("*.parquet"))
        joined = "\n".join(sha256_file(p) for p in parts)
        digests[name] = sha256_bytes(joined.encode("utf-8"))
    return digests


def sha256_bytes(blob: bytes) -> str:
    import hashlib

    return hashlib.sha256(blob).hexdigest()


__all__ = [
    "DATA_ANALYSIS_ROOT", "SOURCE_DATASET_DIR", "CLEAN_DIR", "OUT_DIR", "DATASET_ID",
    "SEED", "BUSINESS_OFFSET_HOURS",
    "EXPECTED_PUBLISHED_BATCH_ID", "BACKTEST_JSON", "BACKTEST_MD", "OPPORTUNITIES_CSV",
    "SCORE_JSON", "RADII_KM", "CAPTURE_KM", "SOURCE_TABLES", "BatchMismatch",
    "read_source_manifest", "verify_batch", "load_clean_table", "station_intensity",
    "data_note", "source_table_digests", "sha256_bytes",
    "write_new_json", "write_new_text", "write_new_csv", "require_empty_run_dir",
    "sha256_file", "dependency_versions", "invocation", "stamp",
]


# --------------------------------------------------------------------------- 冻结写入
def write_new_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2, default=str)
        handle.write("\n")


def write_new_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def write_new_csv(path: Path, frame: pd.DataFrame) -> None:
    write_new_text(path, frame.to_csv(index=False, lineterminator="\n"))


def require_empty_run_dir(base: Path, extra_allowed: tuple[str, ...] = ()) -> None:
    if not base.exists():
        return
    strays = [p.name for p in base.iterdir() if p.name not in extra_allowed]
    if strays:
        raise FileExistsError(f"{base} 已有内容 {strays[:6]}；本线禁止就地改写已发布目录")
