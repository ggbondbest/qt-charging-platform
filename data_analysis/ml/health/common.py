"""设备健康度 / 预测性维护线（第九线）：每晚算一遍——这台桩未来 7 天内来一张维修工单的概率。

样本单位是**一台桩 × 一个北京日历日**（稠密面板 75 桩 × 178 天），决策时点 = 当日起点（北京
零点）之前一刻——"今天还没开始"。标签 = ``y_ticket7``：该桩在 ``[D, D+6]``（含端，按北京时间
的工单 ``reported_at`` 日历日）内**出现至少一张新维修工单**。回归辅助目标 ``tickets_next7d``
（未来 7 天工单条数）。运营动作是**预测性维护排程**：高概率的桩排进近周的巡检/换件窗口。

监督信号来自真实维修台账：``maintenance_tickets`` 418 张工单、覆盖全部 75 台桩
（每张桩 1–15 张），报修→受理→开工→修复四个时刻齐全——这是发布批次里货真价实的
"发生了什么"，不是造出来的人为标签。

与已有各线的硬切割：

| 线 | 标签 | 决策时点 | 动作 |
| --- | --- | --- | --- |
| `reliability`（第七线） | 当天有没有技术启动**失败** | 当日起点前 | 明早巡检哪几台 |
| **本线（第九线）** | **未来 7 天有没有来**修**（工单）** | **当日起点前** | **近一周维护排程 + 备件预估** |

第七线的标签来自用户侧尝试的失败码（桩"不好用"），本线的标签来自运维侧工单（桩"坏了、有人来
修"）——因果上游不同、时间窗不同（1 天 vs 7 天）、动作预算不同，两线模型不可互换。本线与
第七线最大的工程差异是**标签窗重叠**：一天的 7 天窗和下一天的 7 天窗共享 6 天，同一张工单会被
最多 7 行样本"认领"为正类——横截面看是 19.86% 的正类率，信息量其实只有 418 张工单。这条
自相关不伪装成独立样本，本线用三件事直面它：① **purge**：段与段之间把标签窗会伸进下一段的
行整日剔除（各段尾部 6 天）；② **censor**：数据右端放不下完整 7 天窗的行直接不进评测；
③ 十轮滚动 + 符号检验，用"赢没赢"的稳态而不是单点 AUC 说话。

最容易死人的一处泄漏是**工单状态机**：``status``（RESOLVED/IN_PROGRESS 终态）、``accepted_at``
/ ``work_started_at`` / ``restored_at``、``labor_cost_cents`` / ``parts_cost_cents``、
``severity``、``fault_type``（**这张**未来工单的故障类型）全部是"工单发生之后"才存在的量——
一律禁入特征；工单历史只允许"报告时刻严格早于当日起点"的那部分的**日历日派生量**
（多少天前来过、上次修了多久、当时开没开工单）。``maintenance_events`` 表是工单状态流水
（note 全部"模拟维修流程"），信息是 tickets 四个时刻的重复，整表不读、只留原因。

用法：本文件不直接跑，由 features / train / evaluate / predict / rolling 引用。
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

import numpy as np
import pandas as pd

DATA_ANALYSIS_ROOT = Path(__file__).resolve().parents[2]
SOURCE_DATASET_DIR = DATA_ANALYSIS_ROOT / "datasets" / "analytics_full_180d_v1"
CLEAN_DIR = SOURCE_DATASET_DIR / "clean"

OUT_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_health"
DATASET_ID = "analytics_full_180d_v1"

#: 源数据绑定的发布批次；换批次必须整线重跑（见 verify_batch）。
EXPECTED_PUBLISHED_BATCH_ID = "analytics-298aa3ee1401461fb06ea2bb96930dcf"

MODEL_ID = "gbdt-charger-health-7d-v1"
MODEL_VERSION = "0.1.0"
SEED = 20260914

FEATURES_PATH = OUT_DIR / "health_features.pkl"
BUILD_SUMMARY_PATH = OUT_DIR / "features_summary.json"
BUNDLE_PATH = OUT_DIR / f"{MODEL_ID}.joblib"
TRAIN_METRICS_PATH = OUT_DIR / "train_metrics.json"
TEST_REPORT_PATH = OUT_DIR / "evaluation_report.json"
TEST_REPORT_MD = OUT_DIR / "evaluation_report.md"
PREDICTIONS_CSV = OUT_DIR / "test_predictions.csv"

#: 十轮滚动重训单独一个输出目录（与盲测目录互不覆盖；轮次文件支持断点续跑）。
ROLLING_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_health_rolling"
ROLLING_ROUNDS_DIR = ROLLING_DIR / "rounds"
ROLLING_SUMMARY_PATH = ROLLING_DIR / "rolling_summary.json"
ROLLING_SUMMARY_MD = ROLLING_DIR / "rolling_summary.md"

BUSINESS_OFFSET_HOURS = 8

#: 标签口径：从当日起点（含）往后 7 个北京日历日 [D, D+6] 内出现 ≥1 张新工单记为正类。
#: 7 天对应运维侧"下周维护窗口排程"的动作粒度——更短排不进班组日程，更长则坏桩白白多躺一周。
LABEL_HORIZON_DAYS = 7

#: purge 宽度：一行的标签窗最长伸到 D+6，所以**段尾 6 天**行的标签窗会咬进下一段的时间域。
#: 段间切分时把这样的行整日剔除（详见 ``purge_rows``）。取 7 是为了把"边界日"也算保守。
PURGE_DAYS = 7


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
    变成"这列不存在"，两种错都难查。
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


#: 本线读过的 clean 表与用途——换批次时先核对行数，再看特征。不在这份名单里的表一行都不读。
SOURCE_TABLES = {
    "maintenance_tickets": "标签与工单历史（只按 reported_at 可见；restored 时长仅在已修复后可作历史特征）",
    "charging_sessions": "负荷侧：按 ended_at 可见的会话数/电量/异常收尾（WAITING_PAYMENT）计数",
    "charging_attempts": "尝试侧：按 attempted_at 可见的尝试数与技术失败数（第六/七线同款失败码口径）",
    "charger_telemetry": "健康侧：388.8 万行 5 分钟栅格按分钟聚合——维修/离线态时长、充电功率、表码增量",
    "chargers": "接口类型 / 厂商 / 型号 / 额定功率（静态）",
    "stations": "site_type / 城 / 变压器容量（静态）",
    "calendar": "is_weekend / scenario_event（计划量，取当日）",
    "weather_hourly": "城×日聚合后**只取 T−1 日及更早**（今天的天气在决策时点不可得）",
}


# --------------------------------------------------------------------------- 时间切分


def split_boundaries(manifest: dict) -> dict[str, pd.Timestamp]:
    """把 mlSplits 的北京日历日换算成与数据同基准（UTC naive）的边界。

    语义（第五~八线实测核对：118 + 30 + 30 = 178 天）：**start 含端、end 排端，每个边界日就是
    下一段的第一天**；日期按北京时间计而时间戳是 UTC，所以边界 = D 减 8 小时。本线样本行按
    **北京日历日**（``business_date``）整段落段，边界换算与事件线一致。
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


def assign_split(stamps, bounds: dict[str, pd.Timestamp]) -> pd.Series:
    """行的北京日历日（或可转日期的时间戳）→ TRAIN / VALIDATION / TEST / EXCLUDED。

    面板行是整日粒度，落段天然按日；窗口外的桩日不进样本，但工单/会话/遥测历史源保留全窗。
    """
    stamps = pd.Series(pd.to_datetime(stamps))
    out = pd.Series("EXCLUDED", index=stamps.index, dtype=object)
    train = (stamps >= bounds["startInclusive"]) & (stamps < bounds["trainEndExclusive"])
    validation = (stamps >= bounds["trainEndExclusive"]) & (stamps < bounds["validationEndExclusive"])
    test = (stamps >= bounds["validationEndExclusive"]) & (stamps < bounds["testEndExclusive"])
    out[train] = "TRAIN"
    out[validation] = "VALIDATION"
    out[test] = "TEST"
    return out


def purge_rows(business_date: pd.Series, segment_end: pd.Timestamp) -> np.ndarray:
    """返回该段内**保留**的行掩码：标签窗 ``[D, D+6]`` 完整落在段内的才留。

    不 purge 的后果不是"样本相关"这么温和：TRAIN 段尾一行的正类事件其实发生在 VALIDATION 的
    时间域里，模型等于偷看了验证段第一周的工单——跨段泄漏。段尾 PURGE_DAYS 天整日剔除后，
    每行标签窗的事件时刻严格早于下一段起点。返回布尔掩码（True=保留）。
    """
    horizon_end = business_date + pd.Timedelta(days=LABEL_HORIZON_DAYS)
    return (horizon_end <= segment_end).to_numpy()


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


def write_new_csv(path: Path, frame: pd.DataFrame) -> None:
    buffer = frame.to_csv(index=False, lineterminator="\n")
    assert isinstance(buffer, str)
    write_new_text(path, buffer)


def write_new_bytes(path: Path, blob: bytes) -> None:
    """独占创建二进制（joblib bundle 走这里）。"""
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


# --------------------------------------------------------------------------- as-of 原语
# 与第七/八线同一套"严格早于"语义。本线的可见性时刻按实体族分：工单按 reported_at、
# 会话按 ended_at、尝试按 attempted_at、遥测按 recorded_at——查询时刻统一是当桩当日的
# 起点（business_date − 8h，UTC naive）。


class AsOfCounts:
    """按实体分组的"严格早于 t"累计次数/累计值，以及半开窗 ``[t-days, t)`` 内的同样两量。

    实现：把每个实体的事件时间戳压成"唯一时刻 + 该时刻事件数/值和"做前缀和；查询时
    ``pos = searchsorted(uniq, t, side="left")`` 就是严格早于 t 的唯一时刻个数——同刻并列按
    "还不知道"处理。
    """

    def __init__(self, events: pd.DataFrame, key: str, stamps: str, value: str | None = None) -> None:
        self.value = value
        self.tables: dict[object, tuple[np.ndarray, np.ndarray, np.ndarray]] = {}
        columns = [key, stamps] + ([value] if value else [])
        for entity, part in events[columns].groupby(key, observed=True, sort=False):
            times = pd.to_datetime(part[stamps]).to_numpy(dtype="datetime64[ns]").astype("int64")
            assert (times >= 0).all(), ("事件时间戳出现 NaT：NaT→int64 是哨兵最小值，searchsorted 会把它当成"
                                        "'永远可见'的一条事件——构造引擎前必须先筛掉缺失时刻")
            values = (np.ones(len(part)) if value is None
                      else pd.to_numeric(part[value], errors="raise").to_numpy(dtype=float))
            order = np.argsort(times, kind="stable")
            times, values = times[order], values[order]
            uniq, inverse = np.unique(times, return_inverse=True)
            self.tables[entity] = (
                uniq,
                np.cumsum(np.bincount(inverse, minlength=len(uniq)).astype(float)),
                np.cumsum(np.bincount(inverse, weights=values, minlength=len(uniq))),
            )

    def _prefix(self, entity, times_ns: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """严格早于 ``times_ns`` 的 (事件数, 值和)。"""
        table = self.tables.get(entity)
        if table is None:
            return np.zeros(len(times_ns)), np.zeros(len(times_ns))
        uniq, cum_counts, cum_sums = table
        pos = np.searchsorted(uniq, times_ns, side="left")
        index = np.maximum(pos - 1, 0)
        return (np.where(pos > 0, cum_counts[index], 0.0),
                np.where(pos > 0, cum_sums[index], 0.0))

    def _groups(self, query: pd.DataFrame, key: str) -> tuple[dict, np.ndarray]:
        times = pd.to_datetime(query["__query_time"]).to_numpy(dtype="datetime64[ns]").astype("int64")
        return query.groupby(key, observed=True, sort=False).indices, times

    def _check_value(self, value: str | None) -> None:
        """``value`` 是调用点的自述，不是重新选择：取值列在构造时就绑定了。

        不一致必须报错而不是忽略——静默接受会让人以为拿到的是另一列的和，而 ``[0]``/``[1]``
        哪个是哪个恰好是这类代码最容易看错的地方。
        """
        if value is not None and value != self.value:
            raise AssertionError(f"该引擎的取值列是 {self.value!r}，不能按 {value!r} 求和")

    def cumulative(self, query: pd.DataFrame, key: str, value: str | None = None):
        """严格早于 ``query["__query_time"]`` 的 (事件数, 值和)；引擎纯计数时二者相同。"""
        self._check_value(value)
        indices, times = self._groups(query, key)
        counts = np.zeros(len(query))
        sums = np.zeros(len(query))
        for entity, index in indices.items():
            counts[index], sums[index] = self._prefix(entity, times[index])
        return counts, sums

    def window(self, query: pd.DataFrame, key: str, days: float, value: str | None = None):
        """半开窗 ``[t-days, t)`` 内的 (事件数, 值和)：两次前缀和相减，两端都严格早于 t。"""
        self._check_value(value)
        indices, times = self._groups(query, key)
        lo_target = times - np.int64(round(days * 86_400e9))
        counts = np.zeros(len(query))
        sums = np.zeros(len(query))
        for entity, index in indices.items():
            hi_counts, hi_sums = self._prefix(entity, times[index])
            lo_counts, lo_sums = self._prefix(entity, lo_target[index])
            counts[index] = hi_counts - lo_counts
            sums[index] = hi_sums - lo_sums
        return counts, sums


def global_prior_at(query_times, event_times, event_values: np.ndarray | None = None
                    ) -> tuple[np.ndarray, np.ndarray]:
    """全体事件（不分实体）在**查询时刻**之前的 (条数, 值和)——收缩先验专用。

    与 ``AsOfCounts`` 的"实体表 + 查询时刻"等价，但先验是全局的，不该按实体分组。事件时刻与
    查询时刻是**两个参数**：本线工单先验的事件时刻是 ``reported_at``（当日首个可见时刻之后
    报出来的工单，对当天凌晨的查询不可见），查询时刻是桩日起点，两轴天然不同。
    ``event_values=None`` 时纯计数（值和=条数）。严格早于 + 同刻不可见。
    """
    events_ns = (pd.to_datetime(pd.Series(event_times)).to_numpy(dtype="datetime64[ns]")
                 .astype("int64"))
    queries_ns = (pd.to_datetime(pd.Series(query_times)).to_numpy(dtype="datetime64[ns]")
                  .astype("int64"))
    order = np.argsort(events_ns, kind="stable")
    uniq, inverse = np.unique(events_ns[order], return_inverse=True)
    counts = np.bincount(inverse, minlength=len(uniq)).astype(float)
    values = np.ones(len(events_ns)) if event_values is None else np.asarray(event_values, dtype=float)
    sums = np.bincount(inverse, weights=values[order], minlength=len(uniq))
    # exclusive 前缀：块 b 的先验只含块 0..b-1，所以同刻事件互相不可见。数组多备一格：
    # 查询晚于**全部**事件时 pos == len(uniq)，答案是"全部可见"而不是越界。
    prefix_counts = np.concatenate(([0.0], np.cumsum(counts)))
    prefix_sums = np.concatenate(([0.0], np.cumsum(sums)))
    pos = np.searchsorted(uniq, queries_ns, side="left")
    return prefix_counts[pos], prefix_sums[pos]


def strict_last(left: pd.DataFrame, time: str, key: str, events: pd.DataFrame, event_time: str,
                columns: list[str]) -> pd.DataFrame:
    """每行取"同实体、时刻严格早于本行"的最后一条事件的取值（外加 ``__last_time``）。

    ``merge_asof`` 在这个 pandas 版本没有 ``allow_equal``，所以严格早于靠右表时间整体 +1ns 实现：
    同刻那条落到本行之后、取不到。本线用它取"该桩**上一张已开工单**的修复时长"——右表时间轴是
    ``restored_at``（修完那一刻时长才存在），查询轴是桩日起点，跨轴语义由调用方保证。
    """
    helper = pd.DataFrame({"__row": np.arange(len(left)), key: left[key].to_numpy(),
                           time: pd.to_datetime(left[time])})
    right = events.sort_values(event_time, kind="stable")
    joined = pd.DataFrame({"__join": pd.to_datetime(right[event_time]) + pd.Timedelta(nanoseconds=1),
                           key: right[key].to_numpy(),
                           "__last_time": pd.to_datetime(right[event_time]).to_numpy()})
    for column in columns:
        joined[column] = right[column].to_numpy()
    assert joined[key].notna().all(), "as-of 关联键出现空值，'上一次'无定义"
    matched = pd.merge_asof(helper.sort_values(time, kind="stable"), joined,
                            left_on=time, right_on="__join", by=key, direction="backward")
    matched = matched.sort_values("__row")
    assert len(matched) == len(left) and (matched["__row"].to_numpy() == np.arange(len(left))).all()
    return matched[columns + ["__last_time"]].set_index(left.index)


def smoothed(counts: np.ndarray, sums: np.ndarray, alpha: float, priors: np.ndarray) -> np.ndarray:
    """``(和 + α·先验) / (数 + α)``。先验逐行取"截至查询时刻之前的全量经验率"，只看过去。"""
    return (sums + alpha * priors) / (counts + alpha)


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


def mae(y_true: np.ndarray, pred: np.ndarray) -> float:
    return float(np.mean(np.abs(np.asarray(pred, dtype=float) - np.asarray(y_true, dtype=float))))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def data_note() -> str:
    """对外指标一律带的口径声明。本线的"新增数据"是**零**——这句话必须每次报告都说。"""
    return ("全部指标为模拟数据测试结果（第二阶段发布批次 "
            f"{EXPECTED_PUBLISHED_BATCH_ID}），不代表真实运营数据表现。"
            "本线未新增任何数据：样本、标签与全部特征都是对发布批次 clean 表的只读确定性聚合"
            "（as-of），不落新目录、不改原始数据；本线读的表："
            + "、".join(SOURCE_TABLES))


#: 留在表里做分组、溯源、评价与诊断，绝不进特征的列。工单四时刻与终态列是答案本身或其马甲；
#: tickets_next7d/y_ticket7 是标签；label_end/censored 只做 purge 与右端删失判定。
NON_FEATURE_COLUMNS = frozenset({
    "charger_id", "station_id", "city_id", "business_date", "day_start_utc", "split",
    "purged", "censored", "y_ticket7", "tickets_next7d", "label_end",
    "last_ticket_time", "last_restore_time",
})

#: 面板里需要独热/序号编码的列。上一张工单的故障类型改走 4 个数值哑变量（``last_ticket_fault_*``，
#: 随工单历史组进候选集），is_weekend / day_of_week 走数值（周期结构树自己学）。
CATEGORICAL_FEATURES = ["site_type", "scenario_event", "weather_prev", "connector_type",
                        "manufacturer", "charger_model"]
