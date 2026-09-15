"""第九线（设备健康度）测试：purge/删失几何、双时间轴工单史、遥测滞后纪律、写入纪律、产物一致性。

命名按"回答什么问题"分组；每条本线踩过的真实坑（stack 顺序、NaN 静默、跨段 purge 错杀、
zero_power 真负结果）都有对应的回归条目。产物类测试在本地输出缺失时 skip 而非 fail——
outputs/ 被 data_analysis/.gitignore 排除，克隆仓库后需先跑 features → train → evaluate 才有。
"""

from __future__ import annotations

import hashlib
import json
import random

import joblib
import numpy as np
import pandas as pd
import pytest

from data_analysis.ml.health import common, evaluate, features, predict, rolling, train

ARTIFACTS_READY = (common.FEATURES_PATH.exists() and common.BUILD_SUMMARY_PATH.exists()
                   and common.BUNDLE_PATH.exists() and common.TRAIN_METRICS_PATH.exists())
HAS_REPORT = ARTIFACTS_READY and common.TEST_REPORT_PATH.exists()
HAS_ROLLING = ARTIFACTS_READY and common.ROLLING_SUMMARY_PATH.exists()
needs_artifacts = pytest.mark.skipif(not ARTIFACTS_READY, reason="需先跑 features + train")
needs_report = pytest.mark.skipif(not HAS_REPORT, reason="需先跑 evaluate（盲测）")
needs_rolling = pytest.mark.skipif(not HAS_ROLLING, reason="需先跑 rolling")


def ts(text: str) -> pd.Timestamp:
    return pd.Timestamp(text)


# ---------------------------------------------------------------- 时间切分与 purge 原语


def test_split_boundaries_utc_shift():
    manifest = {"mlSplits": {"start": "2026-01-02", "trainEnd": "2026-02-01",
                             "validationEnd": "2026-03-01", "end": "2026-04-01"}}
    bounds = common.split_boundaries(manifest)
    assert bounds["startInclusive"] == ts("2026-01-01 16:00")
    assert bounds["trainEndExclusive"] == ts("2026-01-31 16:00")
    assert bounds["validationEndExclusive"] == ts("2026-02-28 16:00")
    assert bounds["testEndExclusive"] == ts("2026-03-31 16:00")


def test_assign_split_membership_and_exclusion():
    manifest = {"mlSplits": {"start": "2026-01-02", "trainEnd": "2026-01-11",
                             "validationEnd": "2026-01-21", "end": "2026-01-31"}}
    bounds = common.split_boundaries(manifest)
    stamps = pd.to_datetime(["2026-01-01 15:59", "2026-01-01 16:00", "2026-01-10 15:59",
                             "2026-01-10 16:00", "2026-01-20 16:00", "2026-01-30 15:00",
                             "2026-01-31 00:00"])
    got = common.assign_split(stamps, bounds).tolist()
    assert got == ["EXCLUDED", "TRAIN", "TRAIN", "VALIDATION", "TEST", "TEST", "EXCLUDED"]


def test_purge_rows_boundary_inclusive():
    dates = pd.Series(pd.to_datetime(["2026-01-01", "2026-01-02", "2026-01-03"]))
    keep = common.purge_rows(dates, ts("2026-01-09"))
    assert keep.tolist() == [True, True, False]  # bd+7 ≤ end：恰好触界保留


def test_horizon_constants():
    assert common.PURGE_DAYS == common.LABEL_HORIZON_DAYS == 7
    assert common.MODEL_ID == "gbdt-charger-health-7d-v1"
    assert common.BUSINESS_OFFSET_HOURS == 8


# ---------------------------------------------------------------- AsOfCounts / 先验 / strict_last


@pytest.fixture()
def ticket_events() -> pd.DataFrame:
    return pd.DataFrame({
        "charger_id": ["c1"] * 4 + ["c2"] * 2,
        "reported_at": pd.to_datetime([
            "2026-01-01 00:00", "2026-01-04 12:00", "2026-01-05 00:00", "2026-01-05 00:00",
            "2026-01-03 00:00", "2026-01-20 00:00"]),
        "cost": [1.0, 2.0, 3.0, 4.0, 5.0, 6.0],
    })


def test_asof_strictly_before_same_instant_invisible(ticket_events):
    engine = common.AsOfCounts(ticket_events, "charger_id", "reported_at")
    query = pd.DataFrame({"charger_id": ["c1"], "__query_time": [ts("2026-01-05 00:00")]})
    counts, sums = engine.cumulative(query, "charger_id")
    assert counts[0] == 2.0 and sums[0] == 2.0  # 纯计数引擎：值和=条数；同刻并列不可见


def test_asof_window_half_open(ticket_events):
    engine = common.AsOfCounts(ticket_events, "charger_id", "reported_at")
    query = pd.DataFrame({"charger_id": ["c1"], "__query_time": [ts("2026-01-08 00:00")]})
    counts, _ = engine.window(query, "charger_id", 7)  # [01-01, 01-08)：左端点恰在窗口上
    assert counts[0] == 4.0
    query2 = pd.DataFrame({"charger_id": ["c1"], "__query_time": [ts("2026-01-08 01:00")]})
    counts2, _ = engine.window(query2, "charger_id", 7)  # 左端 01-01 01:00 把 01-01 00:00 掉出
    assert counts2[0] == 3.0


def test_asof_value_guard(ticket_events):
    engine = common.AsOfCounts(ticket_events, "charger_id", "reported_at", value="cost")
    query = pd.DataFrame({"charger_id": ["c1"], "__query_time": [ts("2026-01-06 00:00")]})
    with pytest.raises(AssertionError):
        engine.cumulative(query, "charger_id", value="other_column")
    _, sums = engine.window(query, "charger_id", 5, value="cost")
    assert sums[0] == 1.0 + 2.0 + 3.0 + 4.0  # 窗口 [01-01, 01-06) 覆盖 c1 全部 4 条


def test_asof_rejects_nat(ticket_events):
    broken = ticket_events.assign(
        reported_at=ticket_events["reported_at"].astype("datetime64[ns]").where(
            ticket_events["reported_at"] != ts("2026-01-04 12:00")))
    with pytest.raises(AssertionError, match="NaT"):
        common.AsOfCounts(broken, "charger_id", "reported_at")


def test_asof_unknown_entity_zero(ticket_events):
    engine = common.AsOfCounts(ticket_events, "charger_id", "reported_at")
    query = pd.DataFrame({"charger_id": ["nobody"], "__query_time": [ts("2026-06-01")]})
    counts, sums = engine.cumulative(query, "charger_id")
    assert counts[0] == 0.0 and sums[0] == 0.0


def test_global_prior_strict_and_all_visible():
    events = pd.to_datetime(["2026-01-01", "2026-01-02", "2026-01-02", "2026-01-05"])
    queries = pd.to_datetime(["2026-01-01", "2026-01-02", "2026-01-06", "2099-01-01"])
    counts, _ = common.global_prior_at(queries, events)
    assert counts.tolist() == [0.0, 1.0, 4.0, 4.0]  # 末位：查询在全部事件之后也不越界


def test_global_prior_values():
    events = pd.to_datetime(["2026-01-01", "2026-01-03"])
    counts, sums = common.global_prior_at([ts("2026-01-04")], events,
                                          event_values=np.array([2.0, 5.0]))
    assert counts[0] == 2.0 and sums[0] == 7.0


def test_strict_last_excludes_equal_time():
    # 注意：字符串 "00:00.000000001" 会被静默截断成整分钟，纳秒必须用 Timedelta 显式加。
    one_ns_later = ts("2026-01-05 00:00") + pd.Timedelta(nanoseconds=1)
    left = pd.DataFrame({"charger_id": ["c1", "c1"],
                         "day_start_utc": [ts("2026-01-05 00:00"),
                                           one_ns_later]})
    events = pd.DataFrame({"charger_id": ["c1", "c1"],
                           "reported_at": [ts("2026-01-05 00:00"), ts("2026-01-03 00:00")],
                           "fault_type": ["COOLING", "CONNECTOR"]})
    got = common.strict_last(left, "day_start_utc", "charger_id", events, "reported_at",
                             ["fault_type"])
    assert got["fault_type"].iloc[0] == "CONNECTOR"          # 同刻 COOLING 被 +1ns 挪出可见范围，
    assert str(got["__last_time"].iloc[0]) == "2026-01-03 00:00:00"  # 只剩更早那条
    assert got["fault_type"].iloc[1] == "COOLING"            # 1ns 之后它就可见了
    assert str(got["__last_time"].iloc[1]) == "2026-01-05 00:00:00"


def test_strict_last_no_history_nan():
    left = pd.DataFrame({"charger_id": ["c9"], "day_start_utc": [ts("2026-01-01")]})
    events = pd.DataFrame({"charger_id": ["c1"], "reported_at": [ts("2026-01-03")],
                           "fault_type": ["COOLING"]})
    got = common.strict_last(left, "day_start_utc", "charger_id", events, "reported_at",
                             ["fault_type"])
    assert got["fault_type"].isna().all()


def test_smoothed_formula_and_prior_only():
    out = common.smoothed(np.array([10.0]), np.array([4.0]), 20.0, np.array([0.5]))
    assert out[0] == pytest.approx((4.0 + 20 * 0.5) / (10 + 20))
    assert common.smoothed(np.array([0.0]), np.array([0.0]), 20.0,
                           np.array([0.3]))[0] == pytest.approx(0.3)


# ---------------------------------------------------------------- 指标纯函数


def test_metrics_small_values():
    y = np.array([1, 0, 1, 0])
    p = np.array([0.9, 0.2, 0.8, 0.1])
    assert common.brier(y, p) == pytest.approx(np.mean((p - y) ** 2))
    assert common.mae(y, p) == pytest.approx(np.mean(np.abs(p - y)))
    assert common.lift_at(y, p, 0.5) == pytest.approx(1.0 / 0.5)
    assert np.isnan(common.lift_at(np.zeros(4, dtype=int), p, 0.5))


def test_calibration_bins_conserve():
    rng = np.random.default_rng(7)
    p = rng.random(200)
    y = (rng.random(200) < p).astype(int)
    bins = common.calibration_bins(y, p, bins=8)
    assert int(bins["n"].sum()) == 200
    assert set(bins.columns) >= {"bin", "n", "predicted", "observed"}


# ---------------------------------------------------------------- 冻结写入纪律


def test_write_new_json_excl(tmp_path):
    path = tmp_path / "a.json"
    common.write_new_json(path, {"x": 1})
    with pytest.raises(FileExistsError):
        common.write_new_json(path, {"x": 2})


def test_write_new_text_and_csv(tmp_path):
    common.write_new_text(tmp_path / "t.md", "中文内容")
    assert (tmp_path / "t.md").read_text(encoding="utf-8") == "中文内容"
    frame = pd.DataFrame({"a": [1, 2], "b": ["x", "y"]})
    common.write_new_csv(tmp_path / "t.csv", frame)
    back = pd.read_csv(tmp_path / "t.csv")
    assert back["a"].tolist() == [1, 2]
    with pytest.raises(FileExistsError):
        common.write_new_csv(tmp_path / "t.csv", frame)


def test_require_empty_run_dir(tmp_path):
    base = tmp_path / "run"
    common.require_empty_run_dir(base)  # 不存在 = 放行
    base.mkdir()
    common.require_empty_run_dir(base)  # 空目录 = 放行
    (base / "artifact.json").write_text("{}", encoding="utf-8")
    with pytest.raises(FileExistsError):
        common.require_empty_run_dir(base)
    common.require_empty_run_dir(base, extra_allowed=("artifact.json",))  # 白名单 = 放行


def test_sha256_and_data_note(tmp_path):
    path = tmp_path / "x.bin"
    path.write_bytes(b"abc")
    assert common.sha256_file(path) == hashlib.sha256(b"abc").hexdigest()
    note = common.data_note()
    assert common.EXPECTED_PUBLISHED_BATCH_ID in note
    assert "模拟数据" in note and "未新增任何数据" in note
    assert "maintenance_tickets" in note and "charger_telemetry" in note


# ---------------------------------------------------------------- attach_label：前向窗、删失、逐段 purge


@pytest.fixture()
def label_fixture():
    """2 桩 × 29 天的迷你面板 + 手工工单：前向窗两端、删失、逐段 purge 全用它。"""
    bounds = {"startInclusive": ts("2026-01-01 16:00"),       # 北京日 01-02 起
              "trainEndExclusive": ts("2026-01-16 16:00"),    # TRAIN 到北京日 01-16
              "validationEndExclusive": ts("2026-01-23 16:00"),
              "testEndExclusive": ts("2026-01-30 16:00")}
    days = pd.date_range("2026-01-02", "2026-01-30", freq="D")
    frame = pd.DataFrame([(c, d) for c in ("c1", "c2") for d in days],
                         columns=["charger_id", "business_date"])
    frame["day_start_utc"] = frame["business_date"] - pd.Timedelta(hours=8)
    frame["split"] = common.assign_split(frame["business_date"], bounds)
    tickets = pd.DataFrame({
        "charger_id": ["c1", "c1", "c1", "c2", "c2"],
        "reported_at": pd.to_datetime(["2026-01-05 12:00",   # c1 第 1 张
                                       "2026-01-11 16:00",   # 恰好等于 01-05 行的窗尾 / 01-12 行的窗头
                                       "2026-01-12 05:00",   # c1 第 2 张
                                       "2026-01-20 00:00",
                                       "2026-01-29 23:00"]),  # 数据右端
    })
    return frame, tickets, bounds


def test_label_window_left_closed_right_open(label_fixture):
    frame, tickets, bounds = label_fixture
    got, notes = features.attach_label(frame.copy(), tickets, bounds)
    def count(charger: str, day: str) -> int:
        return int(got.loc[(got["charger_id"] == charger)
                           & (got["business_date"] == ts(day)), "tickets_next7d"].iloc[0])
    assert count("c1", "2026-01-02") == 1     # [01-01 16:00, 01-08 16:00) 含 01-05 12:00
    assert count("c1", "2026-01-05") == 1     # 窗尾 01-11 16:00 恰有票 → 右开不含
    assert count("c1", "2026-01-12") == 2     # 窗头 01-11 16:00 恰有票 → 左闭含，双票行
    assert count("c2", "2026-01-05") == 0     # 别桩的工单不进本桩标签
    assert got["y_ticket7"].sum() > 0 and notes["horizonDays"] == 7
    # c1 的 01-11 16:00 与 01-12 05:00 两张同落 01-06..01-12 七个窗（间隔 13h < 7d）
    assert int((got["tickets_next7d"] > 1).sum()) == 7


def test_label_end_equals_day_start_plus_7d(label_fixture):
    frame, tickets, bounds = label_fixture
    got, _ = features.attach_label(frame.copy(), tickets, bounds)
    assert (got["label_end"] == got["day_start_utc"] + pd.Timedelta(days=7)
            + pd.Timedelta(hours=8)).all()


def test_censor_only_beyond_data_right_edge(label_fixture):
    frame, tickets, bounds = label_fixture
    got, notes = features.attach_label(frame.copy(), tickets, bounds)
    # 末张工单北京日 01-30 → 可见终点北京 01-31 → 起点+7d 越过它的行删失
    assert notes["observationEndBeijing"].startswith("2026-01-31")
    cen = got[got["censored"]]
    assert cen["business_date"].min() == ts("2026-01-25")  # 01-24 起点+7 = 01-30T16:00 恰含
    assert got.loc[~got["censored"], "business_date"].max() == ts("2026-01-24")


def test_purge_is_per_segment_and_spares_test(label_fixture):
    frame, tickets, bounds = label_fixture
    got, _ = features.attach_label(frame.copy(), tickets, bounds)
    # TRAIN 段（北京 ≤01-16）：keep bd ≤ 01-10（01-10+7=01-17 恰触界保留）
    tr = got[got["split"] == "TRAIN"]
    assert tr.loc[tr["purged"], "business_date"].min() == ts("2026-01-11")
    assert tr.loc[~tr["purged"], "business_date"].max() == ts("2026-01-10")
    # VALIDATION 段（01-17..01-22）：keep 只有 01-17；TRAIN 的界不得错杀 01-10 之前的行，
    # VAL 的行也不得因为 TRAIN purge 而被标掉（曾经的真实 bug）。
    va = got[got["split"] == "VALIDATION"]
    assert va.loc[va["purged"], "business_date"].min() == ts("2026-01-18")
    assert sorted(set(va.loc[~va["purged"], "business_date"])) == [ts("2026-01-17")]  # 两桩各一行
    # TEST 是最后一段：只删失、永不 purge
    assert not got.loc[got["split"] == "TEST", "purged"].any()


# ---------------------------------------------------------------- train.py 原语


def _mini_split_frame() -> pd.DataFrame:
    return pd.DataFrame({"charger_id": ["c1", "c1", "c2", "c2"],
                         "station_id": ["s1", "s1", "s2", "s2"],
                         "business_date": pd.to_datetime(["2026-01-02", "2026-01-03"] * 2
                                                         ).to_list(),
                         "split": ["TRAIN", "TRAIN", "VALIDATION", "VALIDATION"],
                         "purged": [False, True, False, False],
                         "censored": [False, False, False, True],
                         "y_ticket7": [0, 1, 0, 1]})


def test_usable_frame_excludes_purged_and_censored():
    frame = _mini_split_frame()
    train_rows = train.usable_frame(frame, "TRAIN")
    assert len(train_rows) == 1 and train_rows["charger_id"].iloc[0] == "c1"
    assert train_rows.index.tolist() == [0]          # 索引复位，不留空洞
    val_rows = train.usable_frame(frame, "VALIDATION")
    assert len(val_rows) == 1 and val_rows["y_ticket7"].iloc[0] == 0


def test_design_coercion_and_mask():
    frame = pd.DataFrame({"n": [1.5, np.nan, "3.2"],
                          "b": pd.array([True, False, True], dtype="boolean"),
                          "cat": ["a", None, "b"], "y": [0, 1, 0]})
    x, mask = train.design(frame, ["n", "b"], ["cat"])
    assert x["n"].dtype == "float64" and np.isnan(x["n"].iloc[1])  # NaN 留给 HGBT 原生处理
    assert x["n"].iloc[2] == 3.2
    assert x["b"].dtype == "float64"
    assert set(x["cat"].cat.categories) == {"a", "b", "MISSING"}
    assert mask.tolist() == [False, False, True]
    with pytest.raises(KeyError):
        train.design(frame, ["ghost"], ["cat"])


def test_split_group_unknown_raises():
    with pytest.raises(AssertionError):
        train.split_group(["a", "ghost"], ["a", "b"], ["c"])
    num, cat = train.split_group(["c", "a"], ["a", "b"], ["c"])
    assert num == ["a"] and cat == ["c"]


def test_hyper_grid_and_construction():
    stump = next(h for h in train.HYPER_GRID if h["name"] == "stump3")
    params = train._hyper_params(stump)
    assert "name" not in params
    model = train.new_classifier(np.array([False]), stump)
    assert model.max_leaf_nodes == 3 and model.max_iter == 400
    default = train.new_classifier(np.array([False]))
    assert default.max_leaf_nodes == 31
    assert len(train.HYPER_GRID) == 4
    assert {h["name"] for h in train.HYPER_GRID} == {"big31", "mid15", "small7", "stump3"}


def test_fit_predict_baseline_range_and_ordering():
    frame = pd.DataFrame({"charger_id": ["c1"] * 10 + ["c2"] * 10,
                          "station_id": ["s1"] * 20, "site_type": ["DEPOT"] * 20,
                          "day_of_week": [0] * 10 + [3] * 10,
                          "y_ticket7": [1] * 10 + [0] * 10})
    baselines = train.fit_baselines(frame)
    assert {"global", "charger", "station", "site_dow"} <= set(baselines)
    seen = pd.DataFrame({"charger_id": ["c1", "c2"], "station_id": ["s1", "s1"],
                         "site_type": ["DEPOT", "DEPOT"], "day_of_week": [0, 3]})
    unseen = pd.DataFrame({"charger_id": ["cZ"], "station_id": ["sZ"],
                           "site_type": ["UNKNOWN"], "day_of_week": [6]})
    blend_seen = train.predict_baseline(baselines, seen)
    blend_unseen = train.predict_baseline(baselines, unseen)
    assert blend_seen[0] > blend_seen[1]             # 全正历史 > 全零历史
    assert 0.0 < blend_unseen[0] < 1.0               # 全未见键 → 回落全局率，不是 NaN


def test_oracle_uses_all_rows_and_alpha():
    frame_all = pd.DataFrame({"charger_id": ["c1"] * 4 + ["c2"] * 4,
                              "y_ticket7": [1, 1, 0, 0, 0, 0, 0, 0]})
    target = pd.DataFrame({"charger_id": ["c1", "c2", "cZ"]})
    got = train.oracle_charger_rate(frame_all, target, 30.0)
    # 收缩先验是 NEUTRAL_PRIOR=0.5（与第七/八线同口径），不是本批样本率
    assert got[0] == pytest.approx((2 + 30 * train.NEUTRAL_PRIOR) / (4 + 30))
    assert got[1] == pytest.approx((0 + 30 * train.NEUTRAL_PRIOR) / (4 + 30))
    assert got[2] == pytest.approx(0.25)             # 未见桩回落全样本率


def test_baseline_scores_contract():
    frame = pd.DataFrame({"charger_id": ["cZ"], "station_id": ["sZ"], "site_type": ["DEPOT"],
                          "day_of_week": [2],
                          "charger_ticket_rate_shrunk": [0.1],
                          "station_ticket_rate_shrunk": [0.2],
                          "usage_energy_kwh_30d": [100.0],
                          "days_since_last_report": [np.nan]})
    seed_train = pd.DataFrame({"charger_id": ["c1"] * 4 + ["c2"] * 4,
                               "station_id": ["s1"] * 8, "site_type": ["DEPOT"] * 8,
                               "day_of_week": [0] * 4 + [3] * 4,
                               "y_ticket7": [1, 1, 0, 0, 0, 0, 0, 0]})
    baselines = train.fit_baselines(seed_train)
    got = train.baseline_scores(frame, baselines, np.array([0.3]))
    assert set(got) == {"globalBaseRate", "trainCellBlend", "asOfChargerPriorOnly",
                        "asOfStationPriorOnly", "usageOnly", "naiveRecentTicket",
                        "oracleChargerRateFullSampleNONDEPLOYABLE"}
    assert got["naiveRecentTicket"][0] == 0.0        # 从未报修 → 最近性 0，不是 NaN
    assert got["globalBaseRate"][0] == 0.25          # seed_train 的 y 均值
    assert got["oracleChargerRateFullSampleNONDEPLOYABLE"][0] == 0.3


def test_pick_threshold_ideal_and_fallback():
    rng = np.random.default_rng(3)
    y = np.r_[np.ones(200, dtype=int), np.zeros(800, dtype=int)]
    good = np.where(y == 1, 0.6, 0.3 + rng.random(1000) * 0.04)
    op = train.pick_threshold(good, y)
    assert op["targetMet"] and op["precision"] >= 2 * y.mean() and op["alerts"] >= train.MIN_FLAGGED
    noise = rng.random(1000)
    op2 = train.pick_threshold(noise, y)
    assert op2["targetMet"] is False and "退回最高精确率档" in op2["rule"]


def test_daily_alert_cap():
    frame = pd.DataFrame({"business_date": pd.to_datetime(["2026-01-02"] * 75
                                                          + ["2026-01-03"] * 10),
                          "charger_id": [f"c{i}" for i in range(75)]
                          + [f"d{i}" for i in range(10)]})
    assert train.daily_alert_cap(frame).tolist() == [19, 3]  # ceil(0.25×75)、ceil(0.25×10)


def test_metrics_block_perfect_and_regress_baselines():
    y = np.array([1, 0, 1, 0])
    block = train.metrics_block(y, np.array([0.9, 0.1, 0.8, 0.2]))
    assert block["auc"] == 1.0 and {"prAuc", "logLoss", "brier"} <= set(block)
    frame = pd.DataFrame({"charger_id": ["c1"] * 6 + ["c2"] * 6,
                          "station_id": ["s1"] * 12,
                          "tickets_next7d": [3] * 6 + [1] * 6})
    baselines = train.fit_regress_baselines(frame)
    got = train.predict_regress_baseline(baselines, pd.DataFrame(
        {"charger_id": ["c1", "c2", "cZ"], "station_id": ["s1", "s1", "s1"]}))
    assert got[0] == pytest.approx((18 + 20 * 2.0) / 26)   # 桩 c1 平滑均值
    assert got[1] == pytest.approx((6 + 20 * 2.0) / 26)    # 桩 c2
    assert got[2] == pytest.approx(2.0)                    # 未见桩回落站层（站层=全局）


# ---------------------------------------------------------------- evaluate / rolling 纯函数


def test_evaluate_score_metrics_probability_split():
    y = np.array([1, 0])
    got = evaluate.score_metrics(y, np.array([0.9, 0.2]), probabilistic=False)
    assert got["logLoss"] is None and got["brier"] is None and "分数不是概率" in got["note"]
    prob = evaluate.score_metrics(y, np.array([0.9, 0.2]), probabilistic=True)
    assert prob["logLoss"] == pytest.approx(-(np.log(0.9) + np.log(0.8)) / 2, abs=1e-4)
    assert prob["brier"] == pytest.approx((0.01 + 0.04) / 2)


def test_evaluate_budget_recall():
    days = pd.to_datetime(["2026-01-02"] * 5 + ["2026-01-03"] * 5)
    test = pd.DataFrame({"business_date": days})
    y = np.array([1, 0, 0, 0, 0, 0, 1, 0, 0, 0])
    scores = np.array([0.9, 0.8, 0.7, 0.6, 0.5, 0.9, 0.8, 0.7, 0.6, 0.5])
    rows = evaluate.maintenance_budget(test, {"m": scores}, y, budgets=(0.2,))
    row = rows[0]["m"]
    assert row["recall"] == 0.5 and row["precision"] == 0.5 and row["alerts"] == 2


def test_evaluate_bucket_rows_minimums():
    test = pd.DataFrame({"y_ticket7": [1, 0] * 10})
    rows = evaluate.bucket_rows(test, np.linspace(0.2, 0.8, 20),
                                [("太小", np.ones(20, dtype=bool))])
    assert rows[0]["auc"] is None and "不足" in rows[0]["note"]
    y = np.r_[np.ones(30, dtype=int), np.zeros(30, dtype=int)]
    big = pd.DataFrame({"y_ticket7": y})
    rows2 = evaluate.bucket_rows(big, np.linspace(1, 0, 60), [("够", np.ones(60, dtype=bool))])
    assert rows2[0]["auc"] == 1.0


def test_evaluate_rank_agreement():
    a = np.arange(50, dtype=float)
    out = evaluate.rank_agreement(a, {"same": a.copy(), "flip": -a})
    assert out["same"] == pytest.approx(1.0) and out["flip"] == pytest.approx(-1.0)


def test_rolling_sign_test():
    assert rolling.sign_test_p(10, 10) == round(2 / 1024, 6)
    assert rolling.sign_test_p(7, 10) == 0.34375
    assert rolling.sign_test_p(5, 10) == 1.0
    assert rolling.sign_test_p(9, 10) == round(2 * 11 / 1024, 6)


def test_rolling_fold_plan_geometry():
    days = pd.DatetimeIndex(pd.date_range("2026-01-02", periods=136, freq="D"))
    plan = rolling.fold_plan(days)
    assert len(plan) == 10
    starts = [w[0] for w in plan]
    assert all(b > a for a, b in zip(starts, starts[1:]))
    assert plan[-1][1] - plan[-1][0] == pd.Timedelta(days=rolling.WINDOW_DAYS)
    assert all(e - s == pd.Timedelta(days=rolling.WINDOW_DAYS) for s, e in plan)
    with pytest.raises(AssertionError):
        rolling.fold_plan(days[:100])


def test_features_forbidden_and_ignored():
    assert "restored_at" in features.FORBIDDEN_FEATURES
    assert "labor_cost_cents" in features.FORBIDDEN_FEATURES
    assert "y_ticket7" in common.NON_FEATURE_COLUMNS      # 标签本身走 NON_FEATURE 禁入
    assert "tickets_next7d" in common.NON_FEATURE_COLUMNS
    assert "severity" in features.FORBIDDEN_FEATURES
    assert "fault_type" in features.FORBIDDEN_FEATURES
    assert all(reason for reason in features.IGNORED_COLUMNS.values())
    assert "demand_multiplier" in features.IGNORED_COLUMNS
    assert features.TECH_FAIL_REASONS == {"CONNECTOR_HANDSHAKE", "APP_TIMEOUT", "AUTH_FAILED"}
    assert set(features.FAULT_TYPES) == {"COMMUNICATION", "CONNECTOR", "POWER_MODULE", "COOLING"}


def test_candidate_set_rules():
    numeric = ["rated_power_kw", "day_index", "is_weekend", "tickets_30d",
               "tel_maint_min_prev", "usage_sessions_30d", "temp_c_mean_prev"]
    categorical = ["site_type", "scenario_event", "connector_type"]
    groups = features.candidate_sets(numeric, categorical)
    assert set(groups) == {"staticOnly", "ticketOnly", "opsOnly", "full"}
    assert "day_index" not in groups["staticOnly"]       # 日历位置不是桩属性
    assert "scenario_event" not in groups["staticOnly"]
    assert "temp_c_mean_prev" in groups["staticOnly"]    # 环境天气算桩-日的"静态语境"
    assert "tickets_30d" in groups["ticketOnly"] and groups["ticketOnly"] == ["tickets_30d"]
    assert set(groups["opsOnly"]) == {"tel_maint_min_prev", "usage_sessions_30d"}
    assert set(groups["full"]) == set(numeric) | set(categorical)


# ---------------------------------------------------------------- 已发布产物一致性


@pytest.fixture(scope="module")
def artifacts():
    frame = pd.read_pickle(common.FEATURES_PATH)
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    bundle = joblib.load(common.BUNDLE_PATH)
    with open(common.TRAIN_METRICS_PATH, encoding="utf-8") as handle:
        train_metrics = json.load(handle)
    return frame, summary, bundle, train_metrics


@needs_artifacts
def test_panel_geometry(artifacts):
    frame, summary, _, _ = artifacts
    assert len(frame) == 13_350 == 75 * 178
    assert not frame.duplicated(subset=["charger_id", "business_date"]).any()
    assert frame["business_date"].nunique() == 178
    assert (frame["day_start_utc"] == frame["business_date"] - pd.Timedelta(hours=8)).all()
    assert summary["notes"]["panel"]["rows"] == 13_350
    assert set(frame["split"]) == {"TRAIN", "VALIDATION", "TEST"}


@needs_artifacts
def test_purge_censor_artifact_counts(artifacts):
    frame, summary, _, _ = artifacts
    scope = summary["scope"]
    purged, censored = frame[frame["purged"]], frame[frame["censored"]]
    assert len(purged) == scope["purgedRows"] == 900 == 75 * 12       # 两段各 6 天
    assert len(censored) == scope["censoredRows"] == 375 == 75 * 5
    assert set(purged["split"]) == {"TRAIN", "VALIDATION"}
    assert purged.groupby("split").size().tolist() == [450, 450]
    assert (censored["split"] == "TEST").all()
    assert censored["business_date"].min() == ts("2026-05-24")        # 窗尾越过工单数据右端
    kept = frame[~frame["purged"] & ~frame["censored"]]
    assert kept.groupby("split").size().to_dict() == scope["keptBySplit"]
    for name in ("TRAIN", "VALIDATION", "TEST"):
        part = kept[kept["split"] == name]
        assert round(float(part["y_ticket7"].mean()), 5) == scope["usableBaseRate"][name]
        assert int(part["y_ticket7"].sum()) == scope["usablePositives"][name]


@needs_artifacts
def test_no_day_has_mixed_usability(artifacts):
    frame, _, _, _ = artifacts
    per_day = frame.groupby("business_date")[["purged", "censored"]].nunique()
    assert (per_day <= 1).all().all()  # purge/删失整日进出，同日 75 行必须同进同出


@needs_artifacts
def test_label_recomputation_spot(artifacts):
    frame, _, _, _ = artifacts
    tickets, _ = features.load_tickets()
    engine = common.AsOfCounts(tickets, "charger_id", "reported_at")
    sample = frame.sample(200, random_state=common.SEED).copy()
    sample["__query_time"] = sample["day_start_utc"]
    hi = sample.assign(__query_time=sample["day_start_utc"] + pd.Timedelta(days=7))
    cum_hi, _ = engine.cumulative(hi, "charger_id")
    cum_lo, _ = engine.cumulative(sample, "charger_id")
    assert np.array_equal(cum_hi - cum_lo, sample["tickets_next7d"].to_numpy(dtype=float))
    assert sample["y_ticket7"].tolist() \
        == (np.asarray(cum_hi - cum_lo) >= 1).astype(int).tolist()


@needs_artifacts
def test_hash_pairing_triple(artifacts):
    _, summary, bundle, train_metrics = artifacts
    digest = common.sha256_file(common.FEATURES_PATH)
    assert summary["featuresSha256"] == digest == bundle["featuresSha256"]
    assert train_metrics["bundleSha256"] == common.sha256_file(common.BUNDLE_PATH)
    assert summary["publishedBatchId"] == bundle["publishedBatchId"] \
        == common.EXPECTED_PUBLISHED_BATCH_ID


@needs_artifacts
def test_fault_dummies_and_ranges(artifacts):
    frame, _, _, _ = artifacts
    dummies = [c for c in frame.columns if str(c).startswith("last_ticket_fault_")]
    assert len(dummies) == 4
    assert (frame[dummies].fillna(0).sum(axis=1) <= 1).all()   # 至多一个故障类型点亮
    assert (frame["tickets_30d"].dropna() >= 0).all()
    assert (frame["open_ticket_at_start"].fillna(0) >= 0).all()
    assert (frame["charger_age_days"] >= 0).all()               # commissioned < 当日起点
    rate = frame["charger_ticket_rate_shrunk"]
    assert ((rate > 0) & (rate < 1)).all()


@needs_artifacts
def test_telemetry_lag_regression(artifacts):
    """swaplevel 事故的回归锁：矩阵滞后曾整族静默 NaN → 被当'常量'剔除。"""
    frame, _, _, _ = artifacts
    for column in ("tel_maint_min_prev", "tel_offline_min_prev", "tel_charging_min_prev",
                   "tel_meter_kwh_7d"):
        assert frame[column].notna().mean() > 0.9, f"{column} 又整族缺失了"
    assert (frame["tel_charging_min_prev"].dropna() <= 1440).all()
    assert (frame["tel_charging_min_7d"].dropna() <= 7 * 1440).all()
    first_day = frame["business_date"].min()
    assert first_day == ts("2025-12-02")
    early = frame[frame["business_date"] <= ts("2025-12-07")]
    # 遥测自北京日 12-01 起铺：面板首日 12-02 的 D−1 存在（prev 有值），
    # 但 [D−7, D−1] 要 7 个完整日 → 12-02..12-07 的 7 日累计必须缺，不许填 0。
    assert early["tel_charging_min_prev"].notna().all()
    assert early["tel_charging_min_7d"].isna().all()
    late = frame[frame["business_date"] == ts("2025-12-08")]
    assert late["tel_charging_min_7d"].notna().all()  # 12-01..12-07 七个完整日就位


@needs_artifacts
def test_constants_dropped_honestly(artifacts):
    _, summary, bundle, _ = artifacts
    dropped = set(summary["features"]["droppedConstant"])
    assert {"transformer_kw", "n_chargers_in_station",
            "tel_zero_power_min_prev", "tel_zero_power_min_7d"} == dropped
    assert set(summary["constantWhy"]) == dropped                    # 每个都留了原因
    assert not (dropped & set(summary["features"]["numeric"]))       # 真从名单里出去
    assert not (dropped & set(bundle["featureNames"]))
    assert "zero_power" in json.dumps(summary)                       # 真负结果可检索


@needs_artifacts
def test_leak_audit_clean(artifacts):
    _, summary, _, _ = artifacts
    audit = summary["leakAudit"]
    assert audit["sampled"] == 250
    assert set(audit["checked"]) >= set(features.AUDITED_FEATURES) | set(features.AUDITED_LABELS)
    assert max(audit["maxAbsDiff"].values()) <= 1e-8
    nonzero = {k: v for k, v in audit["maxAbsDiff"].items() if v > 0}
    assert set(nonzero) <= {"usage_energy_kwh_30d"}   # 唯一豁免：浮点求和顺序 2.1e-10


@needs_artifacts
def test_audit_catches_corruption_and_silent_nan():
    """污染任意审计列，审计必须抛 AssertionError——含曾经的"NaN 静默通过"漏洞。

    用真源数据重放 verify_no_future_leak；采样集与 build 时同 seed 可复现，
    所以污染**精确打在被抽中的行上**，测试没有概率性。
    """
    frame = pd.read_pickle(common.FEATURES_PATH)
    tickets, _ = features.load_tickets()
    sessions, _ = features.load_sessions_stream()
    attempts, _ = features.load_attempts_stream()
    daily, _ = features.build_telemetry_daily()
    context = features.build_day_context()
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        bounds = {k: pd.Timestamp(v) for k, v in json.load(handle)["boundaries"].items()}
    audit = features.verify_no_future_leak(frame, tickets, sessions, attempts, daily,
                                           context, bounds, sample=40)
    assert audit["sampled"] == 40
    rows = sorted(random.Random(common.SEED).sample(range(len(frame)), 40))
    corrupt = frame.copy()
    corrupt["days_since_last_report"] = 999.0          # 数值→篡改
    with pytest.raises(AssertionError, match="泄漏"):
        features.verify_no_future_leak(corrupt, tickets, sessions, attempts, daily,
                                       context, bounds, sample=40)
    nan_broken = frame.copy()
    target = nan_broken.iloc[rows]
    nan_rows = target.index[target["last_repair_hours"].isna()]
    if len(nan_rows) == 0:                              # 抽样里无修复历史 → 退而篡改数值
        nan_rows = pd.Index(rows)
    nan_broken.loc[nan_rows, "last_repair_hours"] = 12.0  # NaN→数值：旧版审计的静默盲区
    with pytest.raises(AssertionError, match="泄漏"):
        features.verify_no_future_leak(nan_broken, tickets, sessions, attempts, daily,
                                       context, bounds, sample=40)


@needs_artifacts
def test_bundle_selection_contract(artifacts):
    _, summary, bundle, train_metrics = artifacts
    assert bundle["chosenSet"] == "staticOnly" and bundle["chosenHyper"] == "stump3"
    assert train_metrics["chosenHyper"] == "stump3"
    assert len(bundle["featureNames"]) == 12
    assert not (set(bundle["featureNames"]) & features.FORBIDDEN_FEATURES)
    assert not (set(bundle["featureNames"]) & common.NON_FEATURE_COLUMNS)
    assert "oracle" not in " ".join(bundle["featureNames"]).lower()
    assert len(train_metrics["hyperGrid"]) == 16      # 4 组 × 4 档全网格无遗漏
    assert set(train_metrics["candidateValidation"]) == {"staticOnly", "ticketOnly",
                                                         "opsOnly", "full"}
    assert set(summary["features"]["groups"]) == set(train_metrics["candidateValidation"])


@needs_report
def test_report_scoreboard_contract(artifacts):
    with open(common.TEST_REPORT_PATH, encoding="utf-8") as handle:
        report = json.load(handle)
    board = report["scoreboard"]
    assert len(board) == 9
    assert board["oracleChargerRateFullSampleNONDEPLOYABLE"]["deployable"] is False
    assert "不可部署" in board["oracleChargerRateFullSampleNONDEPLOYABLE"]["note"]
    assert board["globalBaseRate"]["auc"] == 0.5
    risk = report["risk"]
    assert risk["auc"] > max(board[k]["auc"] for k in
                             ("trainCellBlend", "asOfChargerPriorOnly", "usageOnly",
                              "naiveRecentTicket", "asOfStationPriorOnly"))
    point = risk["frozenOperatingPoint"]
    assert point["targetMet"] is True                        # VAL 口径（发布口径）
    assert point["targetMetOnTest"] == bool(                 # TEST 口径独立复核
        point["precision"] >= 2.0 * risk["baseRate"])
    assert report["headline"]["conclusion"]
    assert all(bool(v) for v in report["invariants"].values())   # numpy bool 也算数
    assert sum(row["n"] for row in report["calibration"]) == report["testRows"] == 1875
    assert len(report["budgetRanking"]) == 5
    ab = report["ablation"]
    assert ab["staticOnly"] == pytest.approx(risk["auc"], abs=1e-4)  # 消融=同容量重拟合=模型本身
    assert "purge" in report["excludedBrief"] and "删失" in report["excludedBrief"]
    assert report["purgeCensor"] == {"purgedRows": 900, "censoredRows": 375}


@needs_report
def test_markdown_honesty_lines():
    md = common.TEST_REPORT_MD.read_text(encoding="utf-8")
    assert "诚实补记" in md              # TEST 侧精确率未达标必须写在报告正文里
    assert "TEST 侧达标" in md
    assert "不可部署" in md              # oracle 的降级说明
    assert "模拟数据" in md and common.EXPECTED_PUBLISHED_BATCH_ID in md
    assert "未新增任何数据" in md


@needs_rolling
def test_rolling_artifacts_stay_out_of_test(artifacts):
    with open(common.ROLLING_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    _, _, bundle, _ = artifacts
    val_end_beijing = pd.Timestamp(bundle["splits"]["validationEndExclusive"]) \
        + pd.Timedelta(hours=common.BUSINESS_OFFSET_HOURS)   # 北京 2026-04-29 零点
    rounds = summary["rounds"]
    assert len(rounds) == 10
    for row in rounds:
        assert pd.Timestamp(row["evalEnd"]) + pd.Timedelta(days=1) <= val_end_beijing
        assert pd.Timestamp(row["trainStartDate"]) + pd.Timedelta(days=7) \
            <= pd.Timestamp(row["evalStart"])                  # 逐轮 7 日 purge 距离
        assert row["models"] and row["evalRows"] > 0
    q1 = summary["questions"]["q1_winVsCheapTable"]
    wins = sum(1 for row in rounds if row["winVsBlend"])
    assert q1["winsVsBlend"] == f"{wins}/10"
    assert q1["pVsBlend"] == rolling.sign_test_p(wins, 10)
    assert summary["notABlindTest"]
    assert (common.ROLLING_ROUNDS_DIR / "round_00.json").exists()
    md = common.ROLLING_SUMMARY_MD.read_text(encoding="utf-8")
    assert "不能当盲测" in md


@needs_report
def test_serving_paths(artifacts):
    frame, _, _, _ = artifacts
    bundle = joblib.load(common.BUNDLE_PATH)
    checks = predict.self_check(frame, bundle)
    assert all(v is True for k, v in checks.items() if isinstance(v, bool))
    with pytest.raises(SystemExit, match="查无此桩"):
        predict.main(["--charger-id", "NOT_A_CHARGER", "--date", "2026-05-10"])
    with pytest.raises(SystemExit, match="不在可打分日历"):
        predict.main(["--schedule-day", "2099-01-01"])
    test_day = str(pd.Timestamp(frame.loc[frame["split"] == "TEST", "business_date"]
                                .min()).date())
    predict.main(["--schedule-day", test_day])                 # 真日期不许抛
    if common.PREDICTIONS_CSV.exists():
        table = pd.read_csv(common.PREDICTIONS_CSV)
        assert len(table) == 1875
        assert table["prob_ticket7"].between(0, 1).all()
        assert int(table["flagged"].sum()) > 0
        threshold = bundle["operatingPoint"]["threshold"]
        assert (table["flagged"] == (table["prob_ticket7"] >= threshold - 1e-6)).all()


@needs_artifacts
def test_features_summary_source_rows(artifacts):
    _, summary, _, _ = artifacts
    rows = summary["sourceRows"]
    assert rows["maintenance_tickets"] == 418
    assert rows["chargers"] == 75 and rows["stations"] == 25
    assert "maintenance_events" not in summary["sourceTables"]
    assert summary["notes"]["tickets"]["unrestored"] == 1      # NaT 修复时刻的那张被记录在案
    assert summary["notes"]["tickets"]["severityMix"] == {"MEDIUM": 418}


@needs_artifacts
def test_every_report_carries_the_disclaimer(artifacts):
    _, summary, _, train_metrics = artifacts
    assert "模拟数据" in summary["disclaimer"]
    assert "模拟数据" in train_metrics["disclaimer"]
    if common.TEST_REPORT_PATH.exists():
        with open(common.TEST_REPORT_PATH, encoding="utf-8") as handle:
            report = json.load(handle)
        assert "未新增任何数据" in report["dataDiscipline"]
        assert "未新增任何数据" in report["disclaimer"]
    if common.ROLLING_SUMMARY_PATH.exists():
        with open(common.ROLLING_SUMMARY_PATH, encoding="utf-8") as handle:
            roll = json.load(handle)
        assert "模拟数据" in roll["dataNote"]
