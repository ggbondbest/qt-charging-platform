"""变压器容量 / 全网功率分配线（第九线预留项转实做）：站×5 分钟栅格上两件真事。

决策单元是**一个站 × 一个 5 分钟 tick**（发布批实测：25 站 × 51,840 tick，稠密满格）。两腿：

* **预测腿**——下一 tick 的站级总充电负荷（kW）回归。真实连续标签（遥测实测功率之和），
  可盲测（TEST 段一次不碰）。回答运维那句"这个站负荷会冲到多高、离 360kW 变压器还剩多少余量"。
* **分配腿**——站内容量约束下的功率分配策略（`allocate.py`，纯函数）在**真实需求流**上的
  反事实回放：当前额定 360kW 下容量从不 binding（稠密 1,296,000 个站·tick 与其中 705,924 个
  "有桩在充"的站·tick 上，越限数都是 0，实测峰值 150.9kW——这是诚实的负事实），故真正的工程量
  在**收紧容量的压力测试**（stress-cap 网格 100/125/150kW）上比较策略——同样的需求流喂给不同
  策略，交付公平性差多少。

与已有各线的硬切割：`ml/load/`（成员 A）与 `ml/availability/` 都是**站×小时**粒度、目标是
可用桩数/小时负荷；本线是**站×5 分钟**粒度的总充电需求 + 站内多桩分配，是变压器的物理口径，
两者粒度、目标、动作都不同。本线与第九线（设备健康度）无耦合：健康度预测"哪台桩要来修"，
本线预测"这个站功率峰值多高、装不下时怎么分"。

零新增数据（写进每个产物）：样本、标签、特征、分配需求全是对发布批次 clean 表
（`charger_telemetry` / `stations`）的只读确定性聚合，不落新目录、不改原始数据。

用法：本文件不直接跑，由 features / train / evaluate / stress 引用。
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pandas as pd

# 契约中立的哈希 / 版本 / 命令戳 / 指标从共享层 import（health 线是各抄一份，本线不再复制）。
from data_analysis.ml.common.artifacts import dependency_versions, invocation, sha256_file, stamp
from data_analysis.ml.common import metrics

DATA_ANALYSIS_ROOT = Path(__file__).resolve().parents[2]
SOURCE_DATASET_DIR = DATA_ANALYSIS_ROOT / "datasets" / "analytics_full_180d_v1"
CLEAN_DIR = SOURCE_DATASET_DIR / "clean"

#: 预测腿的输出目录（features→train→evaluate 逐级续跑）；分配腿（stress 回放）独立一目录。
OUT_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_transformer"
STRESS_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_transformer_stress"
DATASET_ID = "analytics_full_180d_v1"

#: 源数据绑定的发布批次；换批次必须整线重跑（见 verify_batch）。与第九线同一发布批。
EXPECTED_PUBLISHED_BATCH_ID = "analytics-298aa3ee1401461fb06ea2bb96930dcf"

MODEL_ID = "gbdt-station-tick-load-v1"
MODEL_VERSION = "0.1.0"
SEED = 20260915

TICK_FEATURES_PATH = OUT_DIR / "tick_features.pkl"
TICK_SUMMARY_PATH = OUT_DIR / "features_summary.json"
DEMAND_LONG_PATH = OUT_DIR / "tick_demands.parquet"
BUNDLE_PATH = OUT_DIR / f"{MODEL_ID}.joblib"
TRAIN_METRICS_PATH = OUT_DIR / "train_metrics.json"
TEST_REPORT_PATH = OUT_DIR / "evaluation_report.json"
TEST_REPORT_MD = OUT_DIR / "evaluation_report.md"
STRESS_REPORT_PATH = STRESS_DIR / "policy_report.json"
STRESS_REPORT_MD = STRESS_DIR / "policy_report.md"

BUSINESS_OFFSET_HOURS = 8
TICK_SECONDS = 300
#: 站变压器容量：发布批内 25 站全为 360kW（零方差列，第九线建表时按 constantWhy 留档）。
#: 这是"过载从未发生"这一负事实的常量前提，也是压力测试的基线容量。
TRANSFORMER_KW = 360.0
#: 反事实压力网格：把容量收到实测分布的分位附近，策略才有区分度。
STRESS_CAPS = (100.0, 125.0, 150.0)


class BatchMismatch(RuntimeError):
    """产物或数据层与本线绑定的发布批次不一致——宁可拒算，不服错配模型。"""


# --------------------------------------------------------------------------- 数据装载


def read_source_manifest() -> dict:
    with open(SOURCE_DATASET_DIR / "serving_manifest.json", encoding="utf-8") as handle:
        return json.load(handle)


def verify_batch(manifest: dict | None = None) -> dict:
    """核对发布批次号，错配即抛 BatchMismatch（fail closed）。"""
    manifest = manifest or read_source_manifest()
    batch = manifest.get("publishedBatchId")
    if batch != EXPECTED_PUBLISHED_BATCH_ID:
        raise BatchMismatch(
            f"serving_manifest publishedBatchId={batch!r} != 本线绑定的 "
            f"{EXPECTED_PUBLISHED_BATCH_ID!r}；数据层换批次后本线必须重跑并重新发布")
    return manifest


def load_clean_table(name: str, columns: list[str] | None = None) -> pd.DataFrame:
    """clean 层某张表的全量拼接（只读）。目录即表名，分区 *.parquet。

    ``columns`` 是列裁剪请求；缺列先按 parquet schema 核对再报错——静默丢列会把"这列全是 NaN"
    变成"这列不存在"，两种错都难查。（本线读的是明细遥测，不在 export 里，故自带而非用 Export。）
    """
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


#: 本线读过的 clean 表与用途。不在这份名单里的表一行都不读。
SOURCE_TABLES = {
    "charger_telemetry": "站×tick 总负荷（CHARGING 态 power_kw 求和）= 预测目标；逐桩充电功率 = 分配需求；"
                         "session_id 用于会话已充电时长（priority_wait 的等待代理）",
    "stations": "station_id→city_id/site_type/latitude/longitude/transformer_kw（静态，仅绑定与容量）",
}

#: 读过但绝不进特征的列——留痕而非偷偷丢。
IGNORED_COLUMNS = {
    "energy_wh": "累计电能是 power 的积分，与当 tick 功率峰冗余，且会诱导把'历史大'当'当下高'",
    "grid_cost_cents": "成本是运营侧结果量，不是负荷驱动",
    "meter_wh": "表码增量与 energy_wh 同族，冗余",
    "grid_energy_wh": "同上",
    "online": "OFFLINE/AVAILABLE 已在状态里体现，CHARGING 判定用 state 即可",
}


# --------------------------------------------------------------------------- 时间切分


def split_boundaries(manifest: dict) -> dict[str, pd.Timestamp]:
    """把 mlSplits 的北京日历日换算成与遥测同基准（UTC naive）的边界（start 含、end 排）。

    语义与第九线一致：边界日 = D 减 8 小时。本线段按 tick 时间戳落段。
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


def assign_split(stamps) -> pd.Series:
    """tick 时间戳 → TRAIN / VALIDATION / TEST / EXCLUDED（按发布批 mlSplits 边界）。"""
    bounds = split_boundaries(read_source_manifest())
    stamps = pd.Series(pd.to_datetime(stamps))
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
    """独占创建 JSON：已存在即报错，绝不就地改写已发布产物/数据。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2, default=str)
        handle.write("\n")


def write_new_pickle(path: Path, frame: pd.DataFrame) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    with os.fdopen(fd, "wb") as handle:
        frame.to_pickle(handle)


def write_new_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def write_new_parquet(path: Path, frame: pd.DataFrame) -> None:
    """独占创建 parquet（分配需求长表走这里）：存在即 FileExistsError。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL)
        os.close(fd)
    except FileExistsError:
        raise FileExistsError(f"{path} 已存在，本线不就地改写")
    frame.to_parquet(path)


def write_new_bytes(path: Path, blob: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    with os.fdopen(fd, "wb") as handle:
        handle.write(blob)


def require_empty_run_dir(base: Path, extra_allowed: tuple[str, ...] = ()) -> None:
    """脚本入口的覆盖护栏：目标目录非空就拒绝运行（除非只含已批准的续跑文件）。"""
    if not base.exists():
        return
    strays = [p.name for p in base.iterdir() if p.name not in extra_allowed]
    if strays:
        raise FileExistsError(
            f"{base} 已有内容 {strays[:6]}；本线禁止就地改写已发布目录，请换新目录或先归档")


# --------------------------------------------------------------------------- 诚实声明


def data_note() -> str:
    """对外指标一律带的口径声明。本线的"新增数据"是**零**——这句话必须每次报告都说。"""
    return ("全部指标为模拟数据测试结果（第二阶段发布批次 "
            f"{EXPECTED_PUBLISHED_BATCH_ID}），不代表真实运营数据表现。"
            "本线未新增任何数据：样本、标签、特征与分配需求都是对发布批次 clean 表的只读"
            "确定性聚合（as-of），不落新目录、不改原始数据；本线读的表："
            + "、".join(SOURCE_TABLES))


#: 留表做分组/溯源/评价、绝不进特征的列。total_kw_next 是预测目标；tick 与 split 只做时序与落段。
NON_FEATURE_COLUMNS = frozenset({
    "station_id", "city_id", "tick_ts", "tick_index", "split", "dropped", "n_active",
    "total_kw", "total_kw_next", "transformer_kw", "site_type",
})

CATEGORICAL_FEATURES: list[str] = []   # 站×tick 负荷全数值（站型/城市留作分组键，不进模型）

__all__ = [
    "DATA_ANALYSIS_ROOT", "SOURCE_DATASET_DIR", "CLEAN_DIR", "OUT_DIR", "STRESS_DIR",
    "DATASET_ID", "EXPECTED_PUBLISHED_BATCH_ID", "MODEL_ID", "MODEL_VERSION", "SEED",
    "TICK_FEATURES_PATH", "TICK_SUMMARY_PATH", "DEMAND_LONG_PATH", "BUNDLE_PATH",
    "TRAIN_METRICS_PATH", "TEST_REPORT_PATH", "TEST_REPORT_MD", "STRESS_REPORT_PATH",
    "STRESS_REPORT_MD", "TICK_SECONDS", "TRANSFORMER_KW", "STRESS_CAPS",
    "BatchMismatch", "read_source_manifest", "verify_batch", "load_clean_table",
    "SOURCE_TABLES", "IGNORED_COLUMNS", "split_boundaries", "assign_split",
    "write_new_json", "write_new_pickle", "write_new_text", "write_new_bytes", "write_new_parquet",
    "require_empty_run_dir", "data_note", "NON_FEATURE_COLUMNS", "CATEGORICAL_FEATURES",
    # 从 ml.common 转发，调用方 common.sha256_file(...) 即可，无需各自 import 子模块
    "sha256_file", "dependency_versions", "invocation", "stamp", "metrics",
]
