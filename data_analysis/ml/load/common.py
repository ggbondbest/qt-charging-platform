"""负荷预测任务共用的数据加载与特征构造代码。
约定已对照 analytics_full_180d_v1 逐条验证(验证脚本见 prepare_data.py):
- rolling_std_kw_24h 用总体标准差(ddof=0),同导出口径,线上重算必须一致,否则特征系统性偏差。
- hour_of_day/day_of_week/is_weekend 按北京时间算(负荷跟当地作息走);
  reference_time 与表内时间戳仍存 UTC,两套口径不混。
- build_feature_row 从最近 24 个原始电站小时重建模型输入;audit_offline_parity 训练前校验重建与导出表的 parity。
"""

from __future__ import annotations

import glob
import hashlib
import json
import pickle
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np
import pandas as pd

DATA_ANALYSIS_ROOT = Path(__file__).resolve().parents[2]
DATASET_DIR = DATA_ANALYSIS_ROOT / "datasets" / "analytics_full_180d_v1"
BUSINESS_TZ_OFFSET = timedelta(hours=8)

LAG_FEATURES = [f"lag_power_kw_h{i:02d}" for i in range(1, 25)]
ROLLING_FEATURES = [
    "rolling_mean_kw_3h",
    "rolling_mean_kw_6h",
    "rolling_mean_kw_24h",
    "rolling_std_kw_24h",
    "rolling_max_kw_24h",
]
CALENDAR_FEATURES = [
    "hour_of_day",
    "day_of_week",
    "is_weekend",
    "is_public_holiday",
    "is_adjusted_workday",
]
STATION_FEATURES = ["capacity", "rated_capacity_kw", "last_available_count"]
CATEGORICAL_FEATURES = ["city_id", "station_id"]
NUMERIC_FEATURES = CALENDAR_FEATURES + STATION_FEATURES + LAG_FEATURES + ROLLING_FEATURES
FEATURE_COLUMNS = CATEGORICAL_FEATURES + NUMERIC_FEATURES
TARGET_COLUMNS = [f"label_power_kw_h{i:02d}" for i in range(1, 25)]
HORIZON_SPLITS = {1: "split_1h", 6: "split_6h", 24: "split_24h"}

# 每 horizon 单训一个 HistGradientBoostingRegressor;v0.2 选 L2 损失最优的 hgb-deep,
# v0.4 换 quantile-0.5(中位数):直接优化合同考核的 MAE,VALIDATION 12.942 vs 13.294 kW。
MODEL_ID = "hgb-q50-history24-v1"
MODEL_VERSION = "0.4.0"


def read_manifest() -> dict:
    with open(DATASET_DIR / "serving_manifest.json", encoding="utf-8") as handle:
        return json.load(handle)


def load_table(name: str) -> pd.DataFrame:
    """glob 拼接导出表的全部 *.csv.gz 分片;只读单个 part 会静默丢数据。"""
    files = sorted(glob.glob(str(DATASET_DIR / "csv" / name / "*.csv.gz")))
    if not files:
        raise FileNotFoundError(f"No shards found for table {name} under {DATASET_DIR}")
    return pd.concat((pd.read_csv(path) for path in files), ignore_index=True)


def load_hourly_metrics() -> pd.DataFrame:
    hourly = load_table("station_hourly_metrics")
    hourly["recorded_at"] = pd.to_datetime(hourly["recorded_at"])
    return hourly


def load_training_frame() -> pd.DataFrame:
    """特征表与目标表按 (station_id, reference_time) 1:1 join,带各 horizon 切分列。"""
    features = load_table("ml_features_hourly")
    targets = load_table("ml_targets_hourly")
    # one_to_one 在重复/缺配时直接报错,防导出有问题时无声训练。
    # split_1h/6h/24h 按时间标 TRAIN/VALIDATION/TEST/EXCLUDED,防未来数据预测过去。
    frame = features.merge(
        targets[["station_id", "reference_time", *TARGET_COLUMNS, *HORIZON_SPLITS.values()]],
        on=["station_id", "reference_time"],
        validate="one_to_one",
    )
    frame["reference_dt"] = pd.to_datetime(frame["reference_time"])
    return frame


def parse_utc(stamp: str) -> datetime:
    return datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def format_utc(stamp: datetime) -> str:
    return stamp.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def split_end_exclusive_utc(manifest: dict) -> dict[str, str]:
    """把 mlSplits 的切分日期换算成带 Z 后缀的 UTC 字符串。"""
    # manifest 规定:日期是北京时间日历日,start inclusive / end exclusive,边界 = D 00:00+08 转 UTC(-8h),
    # 如 2026-03-30 -> 2026-03-29T16:00:00Z;用 UTC 零点会错 8h。训练帧验证:无 TRAIN 行业务日期 ≥ trainEnd 当天。
    splits = manifest["mlSplits"]

    def boundary(date_str: str) -> str:
        local_midnight = datetime.strptime(date_str, "%Y-%m-%d").replace(
            tzinfo=timezone(BUSINESS_TZ_OFFSET)
        )
        return format_utc(local_midnight)

    return {
        "trainEndExclusive": boundary(splits["trainEnd"]),
        "validationEndExclusive": boundary(splits["validationEnd"]),
        "testEndExclusive": boundary(splits["end"]),
    }


def canonical_payload_digest(bundle: dict) -> str:
    """bundle 去 metadata 稳定化,对 protocol-5 pickle 字节求 SHA-256;bundle 即普通 pickle,joblib.load 可读。"""
    # 稳定化必需:fit 后 sklearn 对象与 joblib 压缩流不保证字节稳定,普通 pickle 往返一次后才幂等。
    # artifactSha256 记此摘要而非文件哈希;整文件哈希另见 train_metrics.json:artifactFileSha256(传输完整性)。
    payload = {key: value for key, value in bundle.items() if key != "metadata"}
    stabilized = pickle.loads(pickle.dumps(payload, protocol=5))
    return hashlib.sha256(pickle.dumps(stabilized, protocol=5)).hexdigest()


def audit_raw_alignment(
    frame: pd.DataFrame, hourly: pd.DataFrame, sample: int | None = None
) -> dict:
    """证明 24 列 lag 特征全落在参考时刻之前的严格过去,报导出值与原始表的最大绝对差。"""
    # 补 parity 盲区:其 rolling 由 lag 列重算,lag 块平移贴到标签小时(泄漏、指标虚高)检不出,故直接核对原始表。
    # last_available_count 按 -1h 回连同查;有 lag 值但对应原始历史小时缺失也计不一致(missingRawHistoryHours)。
    if sample is not None:
        frame = frame.sample(n=min(sample, len(frame)), random_state=7)
    lookup = hourly.set_index(["station_id", "recorded_at"])
    result: dict[str, float] = {}
    missing_total = 0
    for lag_index in range(1, 25):
        wanted = pd.MultiIndex.from_arrays(
            [frame["station_id"], frame["reference_dt"] - pd.Timedelta(hours=lag_index)]
        )
        raw = lookup["mean_power_kw"].reindex(wanted).to_numpy(dtype=float)
        missing = int(np.isnan(raw).sum()) if raw.size else 0
        missing_total += missing
        table = frame[f"lag_power_kw_h{lag_index:02d}"].to_numpy(dtype=float)
        finite = np.isfinite(raw)
        result[f"lag_power_kw_h{lag_index:02d}"] = (
            float(np.abs(table[finite] - raw[finite]).max()) if finite.any() else float("inf")
        )
    wanted = pd.MultiIndex.from_arrays(
        [frame["station_id"], frame["reference_dt"] - pd.Timedelta(hours=1)]
    )
    raw_avail = lookup["end_available_count"].reindex(wanted).to_numpy(dtype=float)
    missing_total += int(np.isnan(raw_avail).sum()) if raw_avail.size else 0
    finite = np.isfinite(raw_avail)
    result["last_available_count"] = (
        float(
            np.abs(
                frame["last_available_count"].to_numpy(dtype=float)[finite] - raw_avail[finite]
            ).max()
        )
        if finite.any()
        else float("inf")
    )
    return {
        "maxAbsDiffByColumn": result,
        "worstAbsDiff": max(result.values()),
        "missingRawHistoryHours": missing_total,
    }


def build_calendar_lookup(frame: pd.DataFrame) -> dict[tuple[str, str], tuple[int, int]]:
    """构建 (city_id, business_date) -> (is_public_holiday, is_adjusted_workday) 查找表。"""
    # key 带 city_id:导出表日历按城市给出;同 key 标记矛盾直接抛错,宁可失败也不用错日历。
    lookup: dict[tuple[str, str], tuple[int, int]] = {}
    for city, date, holiday, workday in frame[
        ["city_id", "business_date", "is_public_holiday", "is_adjusted_workday"]
    ].itertuples(index=False, name=None):
        key = (str(city), str(date)[:10])
        value = (int(bool(holiday)), int(bool(workday)))
        existing = lookup.get(key)
        if existing is not None and existing != value:
            raise ValueError(f"Conflicting calendar flags for {key}")
        lookup[key] = value
    return lookup


def rolling_from_powers(powers: np.ndarray) -> dict[str, float]:
    """从 24 个功率值重算全部 rolling 统计量。"""
    # powers 须按时间旧->新、恰覆盖 h24..h01:3h/6h 窗口取数组末尾,反了全错。std 用 ddof=0,同导出口径。
    return {
        "rolling_mean_kw_3h": float(powers[-3:].mean()),
        "rolling_mean_kw_6h": float(powers[-6:].mean()),
        "rolling_mean_kw_24h": float(powers.mean()),
        "rolling_std_kw_24h": float(powers.std(ddof=0)),
        "rolling_max_kw_24h": float(powers.max()),
    }


def build_feature_row(
    history: list[dict],
    reference_time: str,
    calendar_lookup: dict[tuple[str, str], tuple[int, int]],
    rated_capacity_kw: float | None = None,
) -> dict:
    """线上推理入口:用 reference_time 前 24 个原始电站小时重建一行模型输入。"""
    # 只用原始小时重算,与离线导出表逐列同构(audit_offline_parity 成立的前提)。
    # 小时字段来自 station_hourly_metrics,rated_capacity_kw 来自 station_snapshot;校验不过抛错,宁拒不喂半截。
    ref = parse_utc(reference_time)
    ordered = sorted(history, key=lambda row: row["recorded_at"])
    if len(ordered) != 24:
        raise ValueError(f"HISTORY_TOO_SHORT: expected 24 complete hours, got {len(ordered)}")
    expected = [format_utc(ref - timedelta(hours=offset)) for offset in range(24, 0, -1)]
    stamps = [row["recorded_at"] for row in ordered]
    if stamps != expected:
        raise ValueError("history hours are not the 24 consecutive intervals before reference_time")
    powers = np.array([float(row["mean_power_kw"]) for row in ordered], dtype=float)
    if not np.isfinite(powers).all():
        raise ValueError("history contains non-finite mean_power_kw values")

    last = ordered[-1]
    business = ref + BUSINESS_TZ_OFFSET
    city_id = str(last["city_id"])
    holiday, adjusted = calendar_lookup.get((city_id, business.strftime("%Y-%m-%d")), (0, 0))

    row: dict = {
        "city_id": city_id,
        "station_id": str(last["station_id"]),
        "hour_of_day": business.hour,
        "day_of_week": business.weekday(),
        "is_weekend": int(business.weekday() >= 5),
        "is_public_holiday": holiday,
        "is_adjusted_workday": adjusted,
        "capacity": int(last["capacity"]),
        "rated_capacity_kw": float(
            rated_capacity_kw
            if rated_capacity_kw is not None
            else last["rated_capacity_kw"]
        ),
        "last_available_count": int(last["end_available_count"]),
        **rolling_from_powers(powers),
    }
    for index in range(1, 25):
        row[f"lag_power_kw_h{index:02d}"] = float(powers[-index])
    return row


def features_matrix(frame: pd.DataFrame) -> pd.DataFrame:
    """从导出特征表(离线路径)拼出模型输入矩阵。"""
    # 转 category 是为配合 categorical_features;训练和评估同走此函数,防两条路径列不一致。
    matrix = frame[FEATURE_COLUMNS].copy()
    for column in CATEGORICAL_FEATURES:
        matrix[column] = matrix[column].astype("category")
    return matrix


def audit_offline_parity(frame: pd.DataFrame, sample: int | None = None) -> dict[str, float]:
    """向量化重算数值特征,报告重建值 vs 导出表值的最大绝对差(parity)。"""
    # 差值应≈0(调用方以 1e-9 判失败)。注意:看不出 lag 块整体平移,那一面由 audit_raw_alignment 负责。
    if sample is not None:
        frame = frame.sample(n=min(sample, len(frame)), random_state=7)
    lag_matrix = frame[[f"lag_power_kw_h{i:02d}" for i in range(24, 0, -1)]].to_numpy()
    rebuilt = {
        "rolling_mean_kw_3h": lag_matrix[:, -3:].mean(axis=1),
        "rolling_mean_kw_6h": lag_matrix[:, -6:].mean(axis=1),
        "rolling_mean_kw_24h": lag_matrix.mean(axis=1),
        "rolling_std_kw_24h": lag_matrix.std(axis=1, ddof=0),
        "rolling_max_kw_24h": lag_matrix.max(axis=1),
    }
    business = frame["reference_dt"] + BUSINESS_TZ_OFFSET
    rebuilt.update(
        {
            "hour_of_day": business.dt.hour.to_numpy(),
            "day_of_week": business.dt.dayofweek.to_numpy(),
            "is_weekend": (business.dt.dayofweek >= 5).astype(int).to_numpy(),
        }
    )
    return {
        column: float(np.abs(frame[column].to_numpy(dtype=float) - values).max())
        for column, values in rebuilt.items()
    }
