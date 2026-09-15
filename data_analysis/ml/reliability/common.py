"""桩级次日启动可靠性预测线（第七线）：今晚决策，明天这台桩会不会出现技术启动失败。

样本单位是**一台桩的一个北京日历日**（决策时点 = 当日 00:00+08 之前一刻），与已有六线的"一行一次
事件"完全不同——本线的运营动作是**巡检排程**（明天优先查哪几台桩），不是 App 侧提示。

与第六线的硬切割（实测核对在 build_panel 的结构断言里）：第六线预测 ``charging_attempts`` 的
**单行**会不会失败（决策时点是 ``attempted_at``，那时用户已经选好桩）；本线预测**某台桩某日至少
出现一次**技术启动失败（决策时点那天还没开始，一次尝试都还没发生）。两者共用同一批技术失败事件
作为事实来源，但样本单位、决策时点、可得的特征、以及要交付的动作都不一样：第六线输出"这次启动的
风险分"，本线输出"明天这几台桩要查"。

新增数据（本线唯一新增的东西，落在独立目录 ``data_analysis/derived/charger_day_panel_v1/``）：
clean 层没有任何日粒度表，"每台桩每天被尝试几次、失败几次"以及"每城每天天气聚合"都不存在，必须
聚合出来才能训本线。派生全部是对发布批次真实记录的**确定性聚合**（groupby/sum/mean），没有一行
编造的标签或伪造的事件；``derivation_manifest.json`` 记源批次、源表行数、逐文件 sha256 与口径说明。
本线只读发布批次与本目录，绝不写 ``datasets/``。

本线最容易死人的一次泄漏：**当日暴露度**。实测当日尝试 1–2 次的桩日失败概率 1.9%，20 次以上
52.9%——把 ``attempts_on_day`` 当特征能白拿一大截 AUC，可它在决策时点（昨天为止）还不存在，而且
与标签同窗（同一天的尝试里才有今天的失败）。所以它进 FORBIDDEN_FEATURES，只允许"截至昨天的日均
尝试量"这类因果量替代；同时留一条 **oracle 基线**（真用明天的真实尝试次数）单列在报告里做对照，
明确标注不可部署——这样"排序里有多少是真实可提前知道的、多少是暴露度"这件事是可读的，而不是靠
读者自己怀疑。

用法：本文件不直接跑，由 build_panel / features / train / evaluate / predict / rolling 引用。
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

#: 本线新增数据所在的独立目录（与发布批次目录平级、互不写入）。
DERIVED_DIR = DATA_ANALYSIS_ROOT / "derived" / "charger_day_panel_v1"
DERIVED_MANIFEST_PATH = DERIVED_DIR / "derivation_manifest.json"
PANEL_CSV = DERIVED_DIR / "panel_charger_day.csv"
CONTEXT_CSV = DERIVED_DIR / "day_context.csv"
PANEL_NAME = "panel_charger_day"
CONTEXT_NAME = "day_context"

OUT_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_reliability"
DATASET_ID = "analytics_full_180d_v1"
DERIVED_DATASET_ID = "charger_day_panel_v1"

#: 源数据绑定的发布批次；换批次必须重跑 build_panel 并重新发布派生集（见 verify_batch/verify_derived）。
EXPECTED_PUBLISHED_BATCH_ID = "analytics-298aa3ee1401461fb06ea2bb96930dcf"

MODEL_ID = "gbdt-charger-day-reliability-v1"
MODEL_VERSION = "0.1.0"
SEED = 20260914

FEATURES_PATH = OUT_DIR / "reliability_features.pkl"
BUILD_SUMMARY_PATH = OUT_DIR / "features_summary.json"
BUNDLE_PATH = OUT_DIR / f"{MODEL_ID}.joblib"
TRAIN_METRICS_PATH = OUT_DIR / "train_metrics.json"
TEST_REPORT_PATH = OUT_DIR / "evaluation_report.json"
TEST_REPORT_MD = OUT_DIR / "evaluation_report.md"
PREDICTIONS_CSV = OUT_DIR / "test_predictions.csv"

#: 十轮滚动重训单独一个输出目录（与盲测目录互不覆盖；轮次文件支持断点续跑）。
ROLLING_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_reliability_rolling"
ROLLING_ROUNDS_DIR = ROLLING_DIR / "rounds"
ROLLING_SUMMARY_PATH = ROLLING_DIR / "rolling_summary.json"
ROLLING_SUMMARY_MD = ROLLING_DIR / "rolling_summary.md"

BUSINESS_OFFSET_HOURS = 8

#: 与第六线同源的技术失败原因（口径必须一致，否则两条线在互相预测不同的东西）。
TECHNICAL_FAILURE_REASONS = ("CONNECTOR_HANDSHAKE", "APP_TIMEOUT", "AUTH_FAILED")

#: 留在表里做分组、溯源、评价与诊断，绝不进特征的列。``attempts_on_day`` / ``started_on_day`` /
#: ``tech_fails_on_day`` / ``fail_flag`` / ``users_on_day`` 全部落在标签窗口**之内**（本线的
#: "当日暴露度陷阱"就在 attempts_on_day 与 users_on_day），进特征就是泄答案。
NON_FEATURE_COLUMNS = frozenset({
    "charger_id", "station_id", "city_id", "business_date", "day_start_utc", "split",
    "attempts_on_day", "started_on_day", "tech_fails_on_day", "fail_flag", "y_fail", "y_fails",
    "users_on_day",
})

CATEGORICAL_FEATURES = ["site_type", "scenario_event", "weather_prev", "connector_type",
                        "manufacturer", "charger_model", "last_state_code", "is_weekend"]


class BatchMismatch(RuntimeError):
    """产物或数据层与本线绑定的发布批次不一致——宁可拒算，不服错配模型。"""


class DerivedMismatch(RuntimeError):
    """派生数据集与生成它的 manifest 对不上（行数/哈希/批次漂移）——宁可拒算，不用半截数据。"""


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


def read_derived_manifest() -> dict:
    if not DERIVED_MANIFEST_PATH.exists():
        raise DerivedMismatch(
            f"派生数据集缺失：{DERIVED_MANIFEST_PATH}；先跑 "
            f"python -m data_analysis.ml.reliability.build_panel")
    with open(DERIVED_MANIFEST_PATH, encoding="utf-8") as handle:
        return json.load(handle)


def verify_derived(manifest: dict | None = None) -> dict:
    """派生集自证：源批次、行数、逐文件 sha256 必须与 manifest 一致，否则拒用。

    这一层不是洁癖：派生 CSV 是**入仓的普通文件**，任何人手改一次或 checkout 截断一次，
    训练就会静默吃到与报告不符的数据。哈希对不上直接抛，逼着重跑 build_panel。
    """
    manifest = manifest or read_derived_manifest()
    if manifest.get("sourceBatchId") != EXPECTED_PUBLISHED_BATCH_ID:
        raise DerivedMismatch(
            f"派生集 sourceBatchId={manifest.get('sourceBatchId')!r} != 本线绑定的 "
            f"{EXPECTED_PUBLISHED_BATCH_ID!r}")
    for entry in manifest["files"]:
        path = DERIVED_DIR / entry["name"]
        if not path.exists():
            raise DerivedMismatch(f"派生集缺文件 {path}")
        digest = sha256_file(path)
        if digest != entry["sha256"]:
            raise DerivedMismatch(
                f"{entry['name']} sha256={digest[:12]}… 与 manifest 记录的 {entry['sha256'][:12]}… "
                f"不一致：文件被改过或不完整，请重跑 build_panel")
        rows = len(pd.read_csv(path))
        if rows != entry["rows"]:
            raise DerivedMismatch(f"{entry['name']} 实际 {rows} 行，manifest 记录 {entry['rows']} 行")
    return manifest


def load_panel() -> pd.DataFrame:
    """读桩×日面板（派生集），列型显式指定，不靠 pandas 猜。"""
    verify_derived()
    frame = pd.read_csv(PANEL_CSV, dtype={"charger_id": "string", "station_id": "string",
                                          "city_id": "string", "business_date": "string"})
    frame["business_date"] = pd.to_datetime(frame["business_date"])
    frame["day_start_utc"] = frame["business_date"] - pd.Timedelta(hours=BUSINESS_OFFSET_HOURS)
    # 面板自己的结构断言：密度、唯一性、标签与计数的闭合。派生集是普通入仓文件，读的时候必须复查
    # 一遍——只看 build 时的断言等于假设文件从此再没被人动过。
    assert frame.notna().all().all(), "派生面板存在空值"
    assert not frame.duplicated(subset=["charger_id", "business_date"]).any(), \
        "派生面板出现重复的 (桩, 日) 行，一行一天的前提不成立"
    attempts = frame["attempts_on_day"].to_numpy(dtype=float)
    fails = frame["tech_fails_on_day"].to_numpy(dtype=float)
    assert (attempts >= 0).all() and (fails >= 0).all(), "计数列出现负值"
    assert (fails <= attempts).all(), "当日技术失败数大于当日尝试数，标签口径出错"
    assert (frame["fail_flag"].to_numpy() == (fails > 0).astype(frame["fail_flag"].dtype)).all(), \
        "fail_flag 与 tech_fails_on_day 不一致"
    return frame


def load_context() -> pd.DataFrame:
    """读城市×日上下文（派生集）。"""
    frame = pd.read_csv(CONTEXT_CSV, dtype={"city_id": "string", "business_date": "string"})
    frame["business_date"] = pd.to_datetime(frame["business_date"])
    return frame


# --------------------------------------------------------------------------- 时间切分


def split_boundaries(manifest: dict) -> dict[str, pd.Timestamp]:
    """把 mlSplits 的北京日历日换算成与数据同基准（UTC naive）的边界。

    语义（第六线实测核对：118 + 30 + 30 = 178 天）：**start 含端、end 排端，每个边界日就是下一段
    的第一天**；日期按北京时间计而时间戳是 UTC，所以边界 = D 减 8 小时。本线的时间粒度是"北京日历日
    起点"（``day_start_utc``），换算规则与事件线一致，切分才不会差 8 小时。
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


def assign_split(day_start, bounds: dict[str, pd.Timestamp]) -> pd.Series:
    """桩日的 ``day_start_utc`` → TRAIN / VALIDATION / TEST / EXCLUDED。

    按日整块切：同一天的 75 台桩永远落在同一段，绝不会一半进 TRAIN 一半进 VALIDATION（本线的
    "样本"就是"某天某桩"，按行随机切会直接把未来切进过去）。窗口外一律 EXCLUDED。
    """
    stamps = pd.Series(pd.to_datetime(day_start))
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
    """独占创建文本（派生 CSV 走 pandas ``to_csv`` 的 StringIO 再一次性写，避免半截文件）。"""
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
# 与第六线同一套语义（"严格早于"），但查询时点是"桩日日起点"，见模块文档的泄漏纪律。


class AsOfCounts:
    """按实体分组的"严格早于 t"累计次数/累计值，以及半开窗 ``[t-days, t)`` 内的同样两量。

    实现：把每个实体的事件时间戳压成"唯一时刻 + 该时刻事件数/值和"做前缀和；查询时
    ``pos = searchsorted(uniq, t, side="left")`` 就是严格早于 t 的唯一时刻个数——同刻并列按
    "还不知道"处理。本线的同刻并列就是"同一天"：某桩今天的尝试次数绝不能算进"昨天的历史"。
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


def strict_last(left: pd.DataFrame, time: str, key: str, events: pd.DataFrame, event_time: str,
                columns: list[str]) -> pd.DataFrame:
    """每行取"同实体、时刻严格早于本行"的最后一条事件的取值（外加 ``__last_time``）。

    ``merge_asof`` 在这个 pandas 版本没有 ``allow_equal``，所以严格早于靠右表时间整体 +1ns 实现：
    同刻那条落到本行之后、取不到。本线里"同刻"就是同一天——昨天的最后一次失败可以看见，
    今天当刻的不行。
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
    """``(和 + α·先验) / (数 + α)``。先验逐行取"截至 t 之前的全量经验率"，只看过去。"""
    return (sums + alpha * priors) / (counts + alpha)


def global_causal_ratio(nums: np.ndarray, dns: np.ndarray, times: pd.Series) -> np.ndarray:
    """每行的"截至 t 之前的 ``sum(nums)/sum(dns)``"；无历史则为 0.0。同刻（同一天）互相不可见。

    尝试加权的失败率先验要用这个，不能用 ``global_causal_rate`` 的按行平均：后者把"1 次尝试失败
    1 次"和"28 次尝试失败 1 次"当成等权的两天，得到的先验系统性偏高，拿去当收缩目标会把小样本
    桩的率往错的方向拽。
    """
    stamps = pd.to_datetime(times).to_numpy(dtype="datetime64[ns]").astype("int64")
    order = np.argsort(stamps, kind="stable")
    uniq, inverse = np.unique(stamps[order], return_inverse=True)
    totals = np.bincount(inverse, weights=np.asarray(dns, dtype=float)[order], minlength=len(uniq))
    numerators = np.bincount(inverse, weights=np.asarray(nums, dtype=float)[order], minlength=len(uniq))
    prior_totals = np.concatenate(([0.0], np.cumsum(totals)[:-1]))
    prior_numbers = np.concatenate(([0.0], np.cumsum(numerators)[:-1]))
    rate = np.where(prior_totals > 0, prior_numbers / np.maximum(prior_totals, 1e-12), 0.0)
    pos = np.searchsorted(uniq, stamps, side="left")
    return rate[pos]


def global_causal_rate(y: np.ndarray, times: pd.Series) -> np.ndarray:
    """每行的"截至 t 之前的全量经验率"（按行等权）；没有历史则为 0.0。同刻的行互相不可见。"""
    stamps = pd.to_datetime(times).to_numpy(dtype="datetime64[ns]").astype("int64")
    order = np.argsort(stamps, kind="stable")
    uniq, inverse = np.unique(stamps[order], return_inverse=True)
    counts = np.bincount(inverse, minlength=len(uniq)).astype(float)
    sums = np.bincount(inverse, weights=y[order].astype(float), minlength=len(uniq))
    prior_counts = np.concatenate(([0.0], np.cumsum(counts)[:-1]))
    prior_sums = np.concatenate(([0.0], np.cumsum(sums)[:-1]))
    rate = np.where(prior_counts > 0, prior_sums / np.maximum(prior_counts, 1.0), 0.0)
    # rate[b] 已是"块 0..b-1"的 exclusive 前缀和，故直接取 pos（本行所在块不进自己）。
    pos = np.searchsorted(uniq, stamps, side="left")
    return rate[pos]


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


def mae(y_true: np.ndarray, pred: np.ndarray) -> float:
    return float(np.mean(np.abs(np.asarray(pred, dtype=float) - np.asarray(y_true, dtype=float))))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def data_note() -> str:
    """对外指标一律带的口径声明：源批次 + 本线新增数据的确切来源，两件事分开说。"""
    return ("全部指标为模拟数据测试结果（第二阶段发布批次 "
            f"{EXPECTED_PUBLISHED_BATCH_ID}），不代表真实运营数据表现。"
            f"本线特征来自新增派生数据集 {DERIVED_DATASET_ID}（"
            "data_analysis/derived/charger_day_panel_v1/）：对发布批次真实记录的确定性聚合"
            "（按桩×北京日历日、城市×日历日 groupby），无编造标签、无新增事件。")
