"""流失预测线公共件:观察窗/标签窗定义、用户级特征表、冻结写入、排序指标。
口径:OBSERVE_END=2026-05-15 前的全部行为做特征;标签=之后 14 天内是否再发起充电
(平台均值节奏约 9 天一充,14 天≈错过 1.5 个周期,该定义写死在常量与文档里,不随结果挪动)。
用户级 70/15/15 随机切分(seed 固定);同一用户只会出现在一个侧。
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

import numpy as np
import pandas as pd

DATA_ANALYSIS_ROOT = Path(__file__).resolve().parents[2]
CLEAN_DIR = DATA_ANALYSIS_ROOT / "datasets" / "analytics_full_180d_v1" / "clean"
OUT_DIR = DATA_ANALYSIS_ROOT / "outputs" / "ml_churn"

DATASET_ID = "analytics_full_180d_v1"
# v2:自查评审实锤 v1 两处窗口越界(queues_90 无上界泄进特征;标签窗无上界误标 106 人),
# v1 冻结件按纪律不追改、标记隔离;v2 修口径后走独立 TEST 首盲。
MODEL_ID = "gbdt-churn-user-v2"
MODEL_VERSION = "0.2.0"

OBSERVE_END = pd.Timestamp("2026-05-15")
LABEL_HORIZON_DAYS = 14
SEED = 42

USER_TABLE = OUT_DIR / "user_features.pkl"
SUMMARY_PATH = OUT_DIR / "build_summary.json"
BUNDLE_PATH = OUT_DIR / f"{MODEL_ID}.joblib"
TRAIN_METRICS_PATH = OUT_DIR / "train_metrics.json"
TEST_METRICS_PATH = OUT_DIR / f"test_metrics_{MODEL_ID}.json"

CATEGORICAL = ["home_city_id", "segment", "membership", "acquisition_channel", "vehicle_class"]
NON_FEATURE = {"user_id", "split", "churned_14d", "first_attempt", "last_attempt"}
# 入模列白名单:build_user_table 的产出一旦扩列,必须显式加进这里并复批,
# 防止"顺手加一列"绕过评审就溜进模型(黑名单模式是敞口)。
FEATURE_COLUMNS = [
    "home_city_id", "segment", "acquisition_channel", "membership",
    "attempts_90", "attempts_60", "attempts_30", "attempts_14", "started_90",
    "days_since_last", "days_since_last_start", "distinct_stations",
    "energy_kwh_90", "spend_yuan_90", "fee_per_kwh_90", "avg_session_kwh",
    "campaign_share", "gap_mean", "gap_std", "gap_max", "gap_mean_last5",
    "gap_recent_vs_overall", "sessions_per_week", "queues_90", "queue_abandon_share",
    "battery_kwh", "max_charge_kw", "vehicle_class", "account_age_days",
]


def load_table(name: str) -> pd.DataFrame:
    d = CLEAN_DIR / name
    parts = sorted(str(p) for p in d.glob("*.parquet"))
    if not parts:
        raise FileNotFoundError(f"clean 表缺失: {d}")
    return pd.concat([pd.read_parquet(p) for p in parts], ignore_index=True)


def user_split(user_ids: pd.Series) -> pd.Series:
    """按 user_id 的 sha1 十六进制前 8 位取模,70/15/15;切分对任何人可复算,与随机流无关。"""
    def bucket(uid: str) -> int:
        return int(hashlib.sha1(f"{SEED}:{uid}".encode()).hexdigest()[:8], 16) % 100
    pct = user_ids.map(bucket)
    return pd.Series(np.select([pct < 70, pct < 85], ["TRAIN", "VALIDATION"], "TEST"),
                     index=user_ids.index)


def build_user_table() -> pd.DataFrame:
    """全部特征只用 attempted_at/started_at < OBSERVE_END 的行;标签只看之后有没有再来。"""
    att = load_table("charging_attempts")
    sess = load_table("charging_sessions")
    users = load_table("users")
    vehicles = load_table("vehicles")
    queue = load_table("queue_entries")
    att["attempted_at"] = pd.to_datetime(att["attempted_at"])
    sess["started_at"] = pd.to_datetime(sess["started_at"])
    queue["joined_at"] = pd.to_datetime(queue["joined_at"])
    sess["day"] = sess["started_at"].dt.normalize()

    a = att[att.attempted_at < OBSERVE_END].copy()
    a["is_start"] = (a.outcome == "STARTED").astype(float)
    # 标签窗 = [OBSERVE_END, OBSERVE_END + 14 天),必须有上界:否则数据尾超出 14 天的
    # 用户(实测 106 人)在超窗期才回来、却被判"未流失",等于把标签泄漏进来。
    label_end = OBSERVE_END + pd.Timedelta(days=LABEL_HORIZON_DAYS)
    past = att[(att.attempted_at >= OBSERVE_END) & (att.attempted_at < label_end)]

    def wc(mask: pd.Series) -> pd.Series:
        return a[mask].groupby("user_id").size()

    t_end = a.attempted_at.max()
    feats = pd.DataFrame({"attempts_90": wc(a.attempted_at >= t_end - pd.Timedelta(days=90)),
                          "attempts_60": wc(a.attempted_at >= t_end - pd.Timedelta(days=60)),
                          "attempts_30": wc(a.attempted_at >= t_end - pd.Timedelta(days=30)),
                          "attempts_14": wc(a.attempted_at >= t_end - pd.Timedelta(days=14)),
                          "started_90": a[a.is_start == 1].groupby("user_id").size()})
    g = a.groupby("user_id")
    feats["days_since_last"] = (OBSERVE_END - g.attempted_at.max()).dt.total_seconds() / 86400.0
    last_start = a[a.is_start == 1].groupby("user_id").attempted_at.max()
    feats["days_since_last_start"] = (OBSERVE_END - last_start).dt.total_seconds() / 86400.0
    feats["distinct_stations"] = g.station_id.nunique()
    s = sess[sess.started_at < OBSERVE_END]
    s90 = s[s.started_at >= t_end - pd.Timedelta(days=90)]
    feats["energy_kwh_90"] = s90.groupby("user_id").energy_wh.sum() / 1000.0
    feats["spend_yuan_90"] = s90.groupby("user_id").total_fee_cents.sum() / 100000.0
    feats["fee_per_kwh_90"] = (feats.spend_yuan_90 / feats.energy_kwh_90.replace(0, np.nan))
    feats["avg_session_kwh"] = s.groupby("user_id").energy_wh.mean() / 1000.0
    feats["campaign_share"] = (s.assign(hit=s.campaign_id.notna())
                               .groupby("user_id").hit.mean())
    # 节奏:相邻充电间隔(全历史,会话流)
    ss = s.sort_values(["user_id", "started_at"])
    gaps = ss.groupby("user_id").started_at.diff().dt.total_seconds() / 86400.0
    tail5 = ss.assign(gap=gaps).groupby("user_id")["gap"].tail(5)
    feats["gap_mean"] = gaps.groupby(ss.user_id).mean()
    feats["gap_std"] = gaps.groupby(ss.user_id).std()
    feats["gap_max"] = gaps.groupby(ss.user_id).max()
    feats["gap_mean_last5"] = tail5.groupby(ss.loc[tail5.index, "user_id"]).mean()
    feats["gap_recent_vs_overall"] = feats.gap_mean_last5 / feats.gap_mean.replace(0, np.nan)
    feats["sessions_per_week"] = s90.groupby("user_id").size() / 13.0  # 90d≈13周
    # 上界必须加:排队记录会延伸进标签窗(实测 2,576 条、2,005 用户),漏了就等于把未来行为喂进特征
    q90 = queue[(queue.joined_at >= t_end - pd.Timedelta(days=90)) & (queue.joined_at < OBSERVE_END)]
    feats["queues_90"] = q90.groupby("user_id").size()
    qall = queue[queue.joined_at < OBSERVE_END]
    feats["queue_abandon_share"] = (qall.assign(ab=qall.status.eq("ABANDONED"))
                                    .groupby("user_id").ab.mean())

    out = users[["user_id", "home_city_id", "registered_at", "segment",
                 "acquisition_channel", "membership"]].set_index("user_id")
    out = out.join(feats, how="left")
    v = vehicles.set_index("vehicle_id")
    one_per_user = vehicles.drop_duplicates("user_id").set_index("user_id")
    out["battery_kwh"] = one_per_user.battery_capacity_kwh
    out["max_charge_kw"] = one_per_user.max_charge_kw
    out["vehicle_class"] = one_per_user.vehicle_class
    out["account_age_days"] = (OBSERVE_END - pd.to_datetime(out.registered_at)).dt.total_seconds() / 86400.0
    out = out.drop(columns=["registered_at"])

    churned = set(past.user_id.unique())
    out["churned_14d"] = (~out.index.isin(churned)).astype(int)
    out = out.reset_index()
    # 必须在 reset_index 之后做:直接给以 user_id 为索引的 out["split"] 赋值会按索引对齐出全 NaN
    out["split"] = user_split(out["user_id"])
    return out


def ranking_free_metrics(y: np.ndarray, p: np.ndarray) -> dict:
    """二分类指标:AUC、PR-AUC、Brier、top-decile 提升(前 10% 分数里流失占比/整体流失占比)。"""
    from sklearn.metrics import average_precision_score, brier_score_loss, roc_auc_score

    order = np.argsort(-p)
    k = max(1, int(0.1 * len(p)))
    base = float(y.mean())
    top_rate = float(y[order[:k]].mean())
    in_unit = bool(np.all((0.0 <= p) & (p <= 1.0)))  # 基线分数可能是"天数"量纲,Brier 只对概率算
    return {"auc": round(float(roc_auc_score(y, p)), 4),
            "prAuc": round(float(average_precision_score(y, p)), 4),
            "brier": round(float(brier_score_loss(y, p)), 4) if in_unit else None,
            "liftAt10pct": round(top_rate / base, 3) if base > 0 else None,
            "n": int(len(y)), "churnRate": round(base, 4)}


def write_new_json(path: Path, payload: dict) -> None:
    """冻结语义:不存在才写;已存在要求语义等价(json 往返后 ==,非严格逐字节),不等价必拒——首盲不可被追改。
    创建走 tmp 全量落盘 + os.link 原子挂入(负荷线复审 P2#B 同款修复的加强版):
    exists()->open(w) 两进程可双双过检、后写者静默覆盖;O_EXCL+随后写仍留
    "文件已在、内容未写完"的中间态,复用比对会读到半截文件——os.link 目标已存在即
    FileExistsError,路径只随完整内容原子出现,并发同路径恰好一个成功。"""
    path = Path(path)
    normalized = json.loads(json.dumps(payload, ensure_ascii=False, default=float))
    tmp = path.with_name(f"{path.name}.tmpcreate.{os.getpid()}.{os.urandom(4).hex()}")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(tmp, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)
        os.link(tmp, path)
    except FileExistsError:
        same = json.loads(path.read_text(encoding="utf-8")) == normalized
        if not same:
            raise FileExistsError(f"拒绝覆盖已冻结产物且重算不等价: {path}")
        print(f"复用已冻结文件(与重算一致): {path.name}")
        return
    finally:
        tmp.unlink(missing_ok=True)
