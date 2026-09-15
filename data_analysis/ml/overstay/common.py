"""会话占桩超时预测线（第八线）：一次充电开始时，预测充完电后这台车会不会长时间占着桩。

样本单位是**一次充电会话**（``charging_sessions`` 一行），决策时点 = ``started_at``——插枪起充的
一刻。标签 = ``over_min = unplugged_at − ended_at``（充完到拔枪之间占着桩的分钟数），二分类
``y_over = over_min ≥ 30``，另有 ``over_min`` 回归目标。运营动作是**移车提醒 / 排队优先调度**：
预计会超时占桩的会话，在充电快结束时提前推送移车提醒，并把被占桩排队的下一位用户优先导到别处。

与已有各线的硬切割：第五线排"多久能轮上"、第六线排"这次启动会不会失败"、第七线排"这台桩明天
要不要查"——都发生在**会话开始前后**的供给侧；本线预测的是**会话结束之后**的用户行为，样本、
时点、动作三样都不同。样本表与第五/六/七线共享发布批次，但一行"会话"与一行"尝试"不是一对一
（``attempt_id`` 全表唯一对应，会话只由成功启动的尝试产生）。

本线**不新增任何数据**：全部特征是对发布批次 clean 表的只读确定性聚合（as-of），不落任何新目录、
不改任何原始数据——"新数据放单独文件夹"的规则在这里不会被触发。这也是报告里必须明说的口径。

最容易死人的一处泄漏：**费用与结果列**。``parking_fee_cents`` 与 ``over_min`` 的 Spearman 相关高达
0.442——占桩费就是按超时时长收的钱，把它当特征等于用答案预测答案；同理 ``energy_wh`` /
``end_soc_pct`` / ``status`` / ``stop_reason`` / ``ended_at`` / ``unplugged_at`` 全部是"充完之后"
才存在的量，一律进禁入名单。第二处微妙泄漏比第七线的"当日暴露度"更隐蔽：**用户历史超占率的
可见时刻不是会话开始、而是那次会话拔枪的一刻**——上一次充完电把车忘在桩上，这件事只有在车主
回来拔枪之后系统才知道。所以本线的 as-of 引擎有**两套时间戳**：次数类按 ``started_at``、超占
数值类按 ``unplugged_at``，查询时刻统一是当行的 ``started_at``，任何一条晚于它的历史都不可见。

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

OUT_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_overstay"
DATASET_ID = "analytics_full_180d_v1"

#: 源数据绑定的发布批次；换批次必须整线重跑（见 verify_batch）。
EXPECTED_PUBLISHED_BATCH_ID = "analytics-298aa3ee1401461fb06ea2bb96930dcf"

MODEL_ID = "gbdt-session-overstay-v1"
MODEL_VERSION = "0.1.0"
SEED = 20260914

FEATURES_PATH = OUT_DIR / "overstay_features.pkl"
BUILD_SUMMARY_PATH = OUT_DIR / "features_summary.json"
BUNDLE_PATH = OUT_DIR / f"{MODEL_ID}.joblib"
TRAIN_METRICS_PATH = OUT_DIR / "train_metrics.json"
TEST_REPORT_PATH = OUT_DIR / "evaluation_report.json"
TEST_REPORT_MD = OUT_DIR / "evaluation_report.md"
PREDICTIONS_CSV = OUT_DIR / "test_predictions.csv"

#: 十轮滚动重训单独一个输出目录（与盲测目录互不覆盖；轮次文件支持断点续跑）。
ROLLING_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_overstay_rolling"
ROLLING_ROUNDS_DIR = ROLLING_DIR / "rounds"
ROLLING_SUMMARY_PATH = ROLLING_DIR / "rolling_summary.json"
ROLLING_SUMMARY_MD = ROLLING_DIR / "rolling_summary.md"

BUSINESS_OFFSET_HOURS = 8

#: 标签口径：充完电后占用 ≥ 30 分钟记为正类。30 分钟是运营侧"移车提醒"最小可行动作的时间粒度
#: （更短的挪车属于正常收尾，提醒既来不及也没必要）。
OVERSTAY_THRESHOLD_MIN = 30.0

#: 查表基线用的时段块宽（小时）：24 小时切 6 块，配合 5 类 site_type 与 4 类 vehicle_class
#: 得到 120 个 cell，摊到 8 万条 TRAIN 会话上每格约 660 条，查表才不虚。
HOUR_BLOCK_WIDTH = 4

#: 留在表里做分组、溯源、评价与诊断，绝不进特征的列。时间戳与费用/能量/SOC 结局列全部落在
#: 答案窗口之内（``ended_at`` 之后才有 ``over_min``），``duration_min`` 是 oracle 对照专用列。
NON_FEATURE_COLUMNS = frozenset({
    "session_id", "attempt_id", "user_id", "vehicle_id", "station_id", "charger_id", "city_id",
    "started_at", "ended_at", "unplugged_at", "business_date", "split",
    "over_min", "y_over", "duration_min",
})

CATEGORICAL_FEATURES = ["vehicle_class", "site_type", "scenario_event", "weather_prev",
                        "connector_type", "manufacturer", "charger_model",
                        "segment", "membership", "is_weekend"]


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
    "charging_sessions": "样本与标签（started/ended/unplugged 三时间戳）",
    "vehicles": "车辆画像：vehicle_class / 电池容量 / 最大功率",
    "users": "用户画像：segment / membership / registered_at（决策时点早已存在，无泄漏）",
    "stations": "site_type / 城 / 变压器容量",
    "chargers": "接口类型 / 厂商 / 型号 / 额定功率",
    "calendar": "is_weekend / scenario_event（计划量，取当日）",
    "weather_hourly": "城×日聚合后**只取 T−1 日及更早**（今天的天气在决策时点不可得）",
    "queue_entries": "排队侧运营量：joined_at/resolved_at 两套 as-of + 本会话是否排队而来",
    "campaigns": "核对 campaign_id 标记的窗口覆盖（只产 has_campaign / 折扣两个决策时点已知的量）",
}


# --------------------------------------------------------------------------- 时间切分


def split_boundaries(manifest: dict) -> dict[str, pd.Timestamp]:
    """把 mlSplits 的北京日历日换算成与数据同基准（UTC naive）的边界。

    语义（第五/六/七线实测核对：118 + 30 + 30 = 178 天）：**start 含端、end 排端，每个边界日就是
    下一段的第一天**；日期按北京时间计而时间戳是 UTC，所以边界 = D 减 8 小时。本线按会话的
    ``started_at`` 落段——与事件线（第五/六线）同一套切法，切分才不会差 8 小时。
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
    """会话的 ``started_at`` → TRAIN / VALIDATION / TEST / EXCLUDED。

    边界都是北京零点（= UTC 16:00），会话一天内从早到晚连续发生，按 ``started_at`` 落段天然等价
    于"按日整块切"：跨越边界的只有边界日当天零点前后的会话，而那天整日归后一段。窗口外一律
    EXCLUDED——本线特征全部是事件流上的 as-of 量，EXCLUDED 行留着**只当历史来源**（早于窗口的
    会话可以成为窗口内会话的"过去"），不进任何训练/评测样本。
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
# 与第七线同一套语义（"严格早于"），但本线的关键点不同：**事件有两个时间戳**——
# 会话次数按 started_at 可见、超占数值按 unplugged_at 才可见，两套引擎分开建（见模块文档）。


class AsOfCounts:
    """按实体分组的"严格早于 t"累计次数/累计值，以及半开窗 ``[t-days, t)`` 内的同样两量。

    实现：把每个实体的事件时间戳压成"唯一时刻 + 该时刻事件数/值和"做前缀和；查询时
    ``pos = searchsorted(uniq, t, side="left")`` 就是严格早于 t 的唯一时刻个数——同刻并列按
    "还不知道"处理。事件表与查询表用的是**同一个**时间戳列时，"同刻"就是同一次；本线事件与
    查询用不同时间戳列（started vs unplugged），语义见 ``global_prior_at`` 的文档。
    """

    def __init__(self, events: pd.DataFrame, key: str, stamps: str, value: str | None = None) -> None:
        self.value = value
        self.tables: dict[object, tuple[np.ndarray, np.ndarray, np.ndarray]] = {}
        columns = [key, stamps] + ([value] if value else [])
        for entity, part in events[columns].groupby(key, observed=True, sort=False):
            times = pd.to_datetime(part[stamps]).to_numpy(dtype="datetime64[ns]").astype("int64")
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

    与 ``AsOfCounts`` 的"实体表 + 查询时刻"等价，但先验是全局的，不该按实体分组。第七线里
    先验行与查询行是同一张表（都是桩日），直接传面板行即可；本线**先验事件与查询分属两个时间
    轴**（超占在拔枪时刻可见、查询在会话开始时刻），所以这里把"事件时刻"和"查询时刻"拆成两个
    参数，杜绝把 ``unplugged_at`` 的先验错配到 ``unplugged_at`` 的查询上。``event_values=None``
    时纯计数（值和=条数）。严格早于 + 同刻不可见。
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
    # 查询晚于**全部**事件时 pos == len(uniq)，答案是"全部可见"而不是越界（本批数据里
    # 最后一场拔枪总在最后一场开始之后，测试却会构造"查询在一切之后"的极端情形）。
    prefix_counts = np.concatenate(([0.0], np.cumsum(counts)))
    prefix_sums = np.concatenate(([0.0], np.cumsum(sums)))
    pos = np.searchsorted(uniq, queries_ns, side="left")
    return prefix_counts[pos], prefix_sums[pos]


def strict_last(left: pd.DataFrame, time: str, key: str, events: pd.DataFrame, event_time: str,
                columns: list[str]) -> pd.DataFrame:
    """每行取"同实体、时刻严格早于本行"的最后一条事件的取值（外加 ``__last_time``）。

    ``merge_asof`` 在这个 pandas 版本没有 ``allow_equal``，所以严格早于靠右表时间整体 +1ns 实现：
    同刻那条落到本行之后、取不到。本线用它取"该用户**上一次已拔枪**会话的超占分钟"——右表时间
    轴是 ``unplugged_at``，查询轴是 ``started_at``，跨轴语义靠调用方保证（本线只有一种用法）。
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
