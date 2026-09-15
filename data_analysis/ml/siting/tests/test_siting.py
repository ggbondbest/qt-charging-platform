"""选址线纯单元测试：几何核 + 打分恒等式 + LOSO 机制的双向回归。

除了钉几何（haversine/衰减/覆盖/打分），最有价值的是两个方向相反的合成用例：

* ``zero-sum`` 合成（每城总量近恒定、城内切分）→ 复现真实批的结论：cityMean 与需求场都**反预测**；
* ``gradient`` 合成（两簇真实空间聚集、簇间远大于吸引半径）→ 需求场**为正**。

两者一起证明：负结果是**这份数据没有空间需求信号**，不是场模型写错了。
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from data_analysis.ml.siting import backtest, common, field


# ------------------------------------------------------------------ 几何核
def test_haversine_known_distance():
    d = field.haversine_km(39.9, 116.4, 31.2, 121.5)     # 北京→上海 ~1060km
    assert 1050 < d < 1080


def test_distance_matrix_symmetric_zero_diag_matches_scalar():
    coords = np.array([[39.9, 116.4], [31.2, 121.5], [22.5, 114.0]])
    M = field.distance_matrix(coords, coords)
    assert np.allclose(M.diagonal(), 0) and np.allclose(M, M.T)
    assert abs(M[0, 1] - field.haversine_km(39.9, 116.4, 31.2, 121.5)) < 1e-6


def test_decay_is_one_at_zero_and_monotone():
    w = field.decay(np.array([0.0, 1.0, 5.0, 50.0]), 3.0)
    assert w[0] == 1.0 and (w > 0).all() and np.all(np.diff(w) < 0)


def test_field_absorb_dominant_near_source():
    src = np.array([[0.0, 0.0], [0.0, 50.0]])
    ab = field.field_absorb(np.array([[0.0, 0.0]]), src, np.array([100.0, 100.0]), 3.0)
    assert ab[0] > 90          # 近源几乎全收，50km 外源 decay≈0.0036


def test_coverage_discount_at_existing_station_and_isolated():
    served = np.array([[0.0, 0.0], [0.0, 50.0]])
    demand = np.array([30.0, 70.0])
    at = field.coverage_discount(np.array([[0.0, 0.0]]), served, demand, 3.0)
    assert abs(at[0] - 30 / 100) < 1e-9                  # 只有 (0,0) 站落在 3km 内
    isolated = field.coverage_discount(np.array([[20.0, 20.0]]), served, demand, 3.0)
    assert isolated[0] == 0.0                            # 半径内无现有站


def test_score_opportunity_identity():
    cand = np.array([[0.0, 0.0], [10.0, 10.0]])
    src = np.array([[0.0, 0.0], [0.0, 8.0], [8.0, 0.0]])
    score = field.score_candidates(cand, src, np.array([40.0, 30.0, 30.0]), radius_km=3.0)
    assert np.allclose(score["opportunity"], score["absorb"] * (1.0 - score["coverage"]))


def test_grid_candidates_lattice_size():
    cand = field.grid_candidates(0.0, 0.04, 0.0, 0.04, 0.02)   # 3×3 格
    assert cand.shape == (9, 2)


def test_loso_predict_collinear_middle_highest():
    coords = np.array([[0.0, 0.0], [0.0, 0.02], [0.0, 0.04]])   # 每 0.02° ≈ 2.2km
    pred = field.loso_predict(coords, np.array([100.0, 100.0, 100.0]), radius_km=3.0)
    assert pred[1] > pred[0] and pred[1] > pred[2]              # 中站两个近邻，预测最高


# ------------------------------------------------------------------ _rank_eval 常数守卫
def test_rank_eval_constant_returns_none_not_error():
    block = backtest._rank_eval(np.full(5, 3.0), np.array([1.0, 2.0, 3.0, 4.0, 5.0]))
    assert block["spearman"] is None and "note" in block


# ------------------------------------------------------------------ 合成强度帧
def _intensity(rows) -> pd.DataFrame:
    return pd.DataFrame(rows, columns=["station_id", "city_id", "latitude", "longitude",
                                       "demand", "charging_ticks", "abandoned"])


def test_zero_sum_structure_makes_neighbors_antipredict():
    """每城总量恒定=1100、在 3 站间切分 → cityMean（不含坐标）与 cityOtherSum 都反预测。"""
    rows = [
        ("a1", "C1", 0.00, 0.00, 800, 800, 10),
        ("a2", "C1", 0.02, 0.00, 200, 200, 5),
        ("a3", "C1", 0.04, 0.00, 100, 100, 2),
        ("b1", "C2", 10.0, 10.0, 700, 700, 10),
        ("b2", "C2", 10.02, 10.0, 300, 300, 5),
        ("b3", "C2", 10.04, 10.0, 100, 100, 2),
    ]
    frame = _intensity(rows)
    base = backtest.baseline_evals(frame)
    mech = backtest.mechanism_diagnostic(frame)
    assert base["cityMean"]["spearman"] is not None and base["cityMean"]["spearman"] < 0
    assert mech["cityOtherSumVsOwnDemand"]["spearman"] < 0
    # globalMean 常数 → None，不抛异常
    assert base["globalMean"]["spearman"] is None


def test_real_gradient_makes_field_predictive():
    """两簇真实空间聚集（簇内≈km、簇间≈1000km）、需求随簇而变 → 需求场为正。

    与 zero-sum 用例相反，证明场模型在有空间信号时是对的——真实批为负是缺信号，不是 bug。
    """
    busy = np.column_stack([40.0 + 0.01 * np.arange(4), 116.0 + 0.01 * np.arange(4)])   # 近聚一簇
    quiet = np.column_stack([20.0 + 0.01 * np.arange(4), 100.0 + 0.01 * np.arange(4)])   # 远另一簇
    coords = np.vstack([busy, quiet])
    demand = np.array([600, 550, 500, 480, 60, 55, 50, 48], dtype=float)
    pred = field.loso_predict(coords, demand, radius_km=30.0)
    assert field.distance_matrix(busy, quiet).min() > 200       # 簇间距远大于半径
    assert np.corrcoef(pred, demand)[0, 1] > 0.5                # 有真梯度时场为正
    from scipy.stats import spearmanr
    assert spearmanr(pred, demand).correlation > 0.5


def test_station_intensity_shape(monkeypatch):
    """monkeypatch 掉四张 clean 表，验证聚合列与未满足叠加口径都齐。"""
    def fake_clean(name, columns=None):
        if name == "stations":
            df = pd.DataFrame({"station_id": ["s1", "s2"], "city_id": ["C", "C"],
                               "latitude": [30.0, 30.02], "longitude": [110.0, 110.02],
                               "site_type": ["MALL", "STREET"]})
        elif name == "charging_sessions":
            df = pd.DataFrame({"station_id": ["s1", "s1", "s2"],
                               "started_at": pd.to_datetime(
                                   ["2025-01-01T00:00:00Z", "2025-01-02T00:00:00Z", "2025-01-01T00:00:00Z"])
                               .tz_localize(None)})
        elif name == "charger_telemetry":
            df = pd.DataFrame({"station_id": ["s1", "s2"], "state": ["CHARGING", "AVAILABLE"],
                               "recorded_at": pd.to_datetime(["2025-01-01T00:00:00Z"] * 2).tz_localize(None)})
        elif name == "queue_entries":
            df = pd.DataFrame({"station_id": ["s1", "s1", "s2"],
                               "status": ["ABANDONED", "SERVED", "ABANDONED"]})
        else:
            raise AssertionError(name)
        return df[columns] if columns else df

    monkeypatch.setattr(common, "load_clean_table", fake_clean)
    frame = common.station_intensity()
    assert set(["sessions", "charging_ticks", "abandoned", "demand", "demand_incl_unmet", "obs_days"]) \
        <= set(frame.columns)
    s1 = frame[frame["station_id"] == "s1"].iloc[0]
    assert s1["sessions"] == 2 and s1["abandoned"] == 1
    assert s1["demand_incl_unmet"] == s1["demand"] + s1["abandoned"]
    assert s1["obs_days"] == 2
