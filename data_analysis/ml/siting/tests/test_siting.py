"""选址线单元测试（house 风格 unittest.TestCase，纯合成小数据）。

跑法（仓库根目录）：

    python -m unittest data_analysis.ml.siting.tests.test_siting -v

除了钉几何（haversine/衰减/覆盖/打分恒等式）与读数守卫，最有价值的是三个方向相反的合成用例：

* ``zero-sum``（每城总量恒定、城内切分）→ cityMean、其余站之和、需求场**全部反预测**，
  且 cityMean 与之**同值**（同一代数，不是两条独立证据）；
* ``gradient``（需求沿位置平滑变化、核半径小于站距）→ 需求场**为正**，而**不用坐标的**
  池化均值连排序都做不到（常数 → None）。旧版用"两簇水平不同"的合成，池化均值靠簇间水平差
  就能到 Pearson 0.98，所以那个用例证明不了"空间"；换成簇内梯度后才真正只考坐标。
* ``permutation_nulls``（城内置换零线）→ 它**不保证落在 0**：打乱站位只打破"谁站在哪"、保住每城总量，
  场的位置级正相关仍能蒙到一点（本批实测 +0.13~+0.23）。所以有内容的说法是"观察值落在零线 p5 之下"，
  不是"零线均值 ≈ 0"、也不是"比 0 差"；全局置换那一列打破城市零和结构，对本模型是错的对照。
"""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

try:
    import numpy as np
    import pandas as pd

    from data_analysis.ml.siting import backtest, common, field, score

    STACK = all(importlib.util.find_spec(name) is not None
                for name in ("numpy", "pandas", "scipy", "pyarrow"))
except ImportError:  # 生成器 job：裸 python，无数据科学栈
    STACK = False


# ------------------------------------------------------------------ 几何核
@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class Geometry(unittest.TestCase):
    def test_haversine_known_distance(self):
        d = field.haversine_km(39.9, 116.4, 31.2, 121.5)       # 北京→上海 ~1060km
        self.assertTrue(1050 < d < 1080, d)

    def test_haversine_identical_points_are_exactly_zero(self):
        self.assertEqual(field.haversine_km(22.5, 114.0, 22.5, 114.0), 0.0)

    def test_distance_matrix_symmetric_zero_diag_matches_scalar(self):
        coords = np.array([[39.9, 116.4], [31.2, 121.5], [22.5, 114.0]])
        matrix = field.distance_matrix(coords, coords)
        self.assertTrue(np.allclose(matrix.diagonal(), 0))
        self.assertTrue(np.allclose(matrix, matrix.T))
        self.assertLess(abs(matrix[0, 1] - field.haversine_km(39.9, 116.4, 31.2, 121.5)), 1e-6)

    def test_distance_matrix_finite_at_poles_and_antipodes(self):
        """asin 参数在浮点上可以算出 1+ε；不钳会得到 NaN——整行打分无声变 NaN。"""
        extreme = np.array([[0.0, 0.0], [0.0, 180.0], [90.0, 0.0], [-90.0, 0.0],
                            [89.9999999, 179.9999999], [-89.9999999, -179.9999999]])
        matrix = field.distance_matrix(extreme, extreme)
        self.assertTrue(bool(np.isfinite(matrix).all()), matrix)
        self.assertTrue(bool((matrix >= -1e-9).all()))
        self.assertAlmostEqual(field._clamped_h(np.array([1.0 + 1e-12])), 1.0)

    def test_decay_is_one_at_zero_and_monotone(self):
        weights = field.decay(np.array([0.0, 1.0, 5.0, 50.0]), 3.0)
        self.assertEqual(weights[0], 1.0)
        self.assertTrue(bool((weights > 0).all()))
        self.assertTrue(bool((np.diff(weights) < 0).all()))

    def test_field_absorb_dominant_near_source(self):
        src = np.array([[0.0, 0.0], [0.0, 50.0]])
        absorb = field.field_absorb(np.array([[0.0, 0.0]]), src, np.array([100.0, 100.0]), 3.0)
        self.assertGreater(absorb[0], 90)                     # 50km 外源 decay≈0.0036

    def test_coverage_discount_at_existing_station_and_isolated(self):
        served = np.array([[0.0, 0.0], [0.0, 50.0]])
        demand = np.array([30.0, 70.0])
        at = field.coverage_discount(np.array([[0.0, 0.0]]), served, demand, 3.0)
        self.assertAlmostEqual(at[0], 30 / 100)                # 只有 (0,0) 站落在 3km 内
        isolated = field.coverage_discount(np.array([[20.0, 20.0]]), served, demand, 3.0)
        self.assertEqual(isolated[0], 0.0)                     # 半径内无现有站

    def test_coverage_discount_zero_total_demand(self):
        served = np.array([[0.0, 0.0]])
        out = field.coverage_discount(np.array([[0.0, 0.0]]), served, np.array([0.0]), 3.0)
        self.assertEqual(out[0], 0.0)

    def test_score_opportunity_identity(self):
        cand = np.array([[0.0, 0.0], [10.0, 10.0]])
        src = np.array([[0.0, 0.0], [0.0, 8.0], [8.0, 0.0]])
        score = field.score_candidates(cand, src, np.array([40.0, 30.0, 30.0]), radius_km=3.0)
        self.assertTrue(np.allclose(score["opportunity"], score["absorb"] * (1.0 - score["coverage"])))

    def test_grid_candidates_lattice_size(self):
        cand = field.grid_candidates(0.0, 0.04, 0.0, 0.04, 0.02)      # 3×3 格
        self.assertEqual(cand.shape, (9, 2))

    def test_loso_predict_collinear_middle_highest(self):
        coords = np.array([[0.0, 0.0], [0.0, 0.02], [0.0, 0.04]])      # 每 0.02° ≈ 2.2km
        pred = field.loso_predict(coords, np.array([100.0, 100.0, 100.0]), radius_km=3.0)
        self.assertGreater(pred[1], pred[0])
        self.assertGreater(pred[1], pred[2])                            # 中站两个近邻

    def test_loso_predict_hides_the_station(self):
        """被藏站的自身需求不许进入它的预测值：把 i 的需求翻十倍，predicted[i] 不变。"""
        coords = np.array([[0.0, 0.0], [0.0, 0.02], [0.0, 0.04]])
        base = field.loso_predict(coords, np.array([100.0, 100.0, 100.0]), radius_km=3.0)
        bumped = field.loso_predict(coords, np.array([100.0, 1000.0, 100.0]), radius_km=3.0)
        self.assertAlmostEqual(base[1], bumped[1], places=9)


@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class RankEval(unittest.TestCase):
    def test_constant_returns_none_not_error(self):
        block = backtest._rank_eval(np.full(5, 3.0), np.array([1.0, 2.0, 3.0, 4.0, 5.0]))
        self.assertIsNone(block["spearman"])
        self.assertIn("note", block)

    def test_perfect_ranking_is_one(self):
        block = backtest._rank_eval(np.array([1.0, 2.0, 3.0, 4.0]), np.array([10.0, 20.0, 30.0, 40.0]))
        self.assertAlmostEqual(block["spearman"], 1.0)
        self.assertAlmostEqual(block["pearson"], 1.0)

    def test_p_value_carries_a_caveat(self):
        """LOSO 各预测共享同一批需求值 → scipy 的 p 只是名义显著性，产物里必须自带这句话。"""
        block = backtest._rank_eval(np.array([1.0, 2.0, 3.0, 4.0]), np.array([10.0, 20.0, 30.0, 40.0]))
        self.assertIn("pValueCaveat", block)


# ------------------------------------------------------------------ 合成强度帧
INTENSITY_COLUMNS = ["station_id", "city_id", "latitude", "longitude", "site_type",
                     "demand", "charging_ticks", "abandoned"]


def _intensity(rows) -> pd.DataFrame:
    """与 ``common.station_intensity()`` 同一列契约（含派生的需求口径列）。"""
    frame = pd.DataFrame(rows, columns=INTENSITY_COLUMNS)
    frame["demand"] = frame["demand"].astype(float)
    frame["demand_per_day"] = frame["demand"]
    frame["demand_incl_unmet"] = frame["demand"] + frame["abandoned"].astype(float)
    return frame


def _zero_sum_frame() -> pd.DataFrame:
    """两城、每城总量恒 1100、城内三站切分（复刻真实批的"近恒定总量 + 5 站零和"结构）。"""
    return _intensity([
        ("a1", "C1", 0.00, 0.00, "MALL", 800, 800, 10),
        ("a2", "C1", 0.02, 0.00, "STREET", 200, 200, 5),
        ("a3", "C1", 0.04, 0.00, "ROAD", 100, 100, 2),
        ("b1", "C2", 10.0, 10.0, "MALL", 700, 700, 10),
        ("b2", "C2", 10.02, 10.0, "STREET", 300, 300, 5),
        ("b3", "C2", 10.04, 10.0, "ROAD", 100, 100, 2),
    ])


@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class ZeroSumStructure(unittest.TestCase):
    """零和切分下"用其余站需求预测被藏站"是代数必然：三条读数全负，且两两同值。"""

    def test_neighbors_and_field_all_antipredict(self):
        frame = _zero_sum_frame()
        base = backtest.baseline_evals(frame)
        mech = backtest.mechanism_diagnostic(frame)
        radius = str(common.RADII_KM[2])
        loso = backtest.loso_by_radius(frame)
        self.assertLess(base["cityMean"]["spearman"], 0)
        self.assertLess(mech["cityOtherSumVsOwnDemand"]["spearman"], 0)
        self.assertLess(loso[radius]["field"]["spearman"], 0)     # 场模型同向：不是只测基线

    def test_city_mean_and_other_sum_are_the_same_algebra(self):
        """cityMean 与"其余站之和"只差一个常数 → 秩全等 → 它们是**一条**证据。"""
        frame = _zero_sum_frame()
        base = backtest.baseline_evals(frame)
        mech = backtest.mechanism_diagnostic(frame)
        self.assertEqual(base["cityMean"]["spearman"], mech["cityOtherSumVsOwnDemand"]["spearman"])
        self.assertIn("同值", base["sameAlgebraAsMechanism"])
        self.assertIn("不能当两个独立确认数", base["sameAlgebraAsMechanism"])

    def test_constant_predictor_is_none_not_zero(self):
        frame = _zero_sum_frame()
        self.assertIsNone(backtest.baseline_evals(frame)["globalMean"]["spearman"])

    def test_random_split_theory_mean_tracks_station_count(self):
        """``−1/(k−1)`` 随每城站数走，且实测城内置换零线均值应与它一致（**期望**不是下界）。"""
        frame = _zero_sum_frame()
        mech = backtest.mechanism_diagnostic(frame, nn_reps=300)
        self.assertAlmostEqual(mech["randomAssignmentTheoryMean"], -0.5)
        null = mech["randomAssignmentNull"]
        self.assertAlmostEqual(null["nullMean"], null["theoryMean"], delta=0.12)
        self.assertLess(null["nullMin"], null["theoryMean"])   # 零线能比期望更负 → 比它更负不算证据
        self.assertNotIn("randomSplitFloor", mech)              # 误称不许回来

    def test_loso_by_radius_keys_have_no_dead_field(self):
        """回归钉子：早先的 ``predictedMeanKw``（永远为 None 的死列）不许回来。"""
        loso = backtest.loso_by_radius(_zero_sum_frame())
        self.assertEqual(sorted(loso), sorted(str(r) for r in common.RADII_KM))
        for block in loso.values():
            self.assertEqual(set(block), {"field", "n"})
            self.assertNotIn("predictedMeanKw", block["field"])


@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class RealGradient(unittest.TestCase):
    """需求沿位置平滑变化、核半径小于站距 → 场为正；而**不用坐标**的打分连排序都做不到。

    3×3 格（站距≈3.3km、r=3km）上按"离中心越远越低"铺一个真实梯度。旧合成用例只让两簇
    水平不同，池化均值靠"哪个簇"就能蒙到 Pearson 0.98——它考的不是空间，故弃用。
    """

    def _frame(self) -> pd.DataFrame:
        rows = []
        for i in range(3):
            for j in range(3):
                ring = abs(i - 1) + abs(j - 1)
                rows.append((f"s{3 * i + j}", "C1", 39.90 + 0.03 * i, 116.40 + 0.03 * j,
                             "MALL", 300.0 - 80.0 * ring, 300 - 80 * ring, 0))
        return _intensity(rows)

    def test_field_is_predictive_under_a_local_gradient(self):
        from scipy.stats import spearmanr
        frame = self._frame()
        coords = frame[["latitude", "longitude"]].astype(float).to_numpy()
        demand = frame["demand"].to_numpy(dtype=float)
        rho = spearmanr(field.loso_predict(coords, demand, radius_km=3.0), demand).correlation
        self.assertGreater(rho, 0.8, rho)

    def test_coordinate_blind_predictor_cannot_rank_it(self):
        """同一批数据、不用坐标的池化均值 = 常数 → 无排序能力（None），而不是"略差一点"。"""
        frame = self._frame()
        demand = frame["demand"].to_numpy(dtype=float)
        block = backtest._rank_eval(np.full(len(demand), demand.mean()), demand)
        self.assertIsNone(block["spearman"])


@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class PermutationNulls(unittest.TestCase):
    def test_in_city_null_is_the_ruler_and_observed_sits_below_it(self):
        """零线的正确语义：城内置换打破"谁站在哪"、保住每城总量 → 这才是"无信号"的对照。

        但这条零线**不保证落在 0**：打乱站位后，场的位置级正相关（中心位置天然吸得多）仍能蒙到
        一点——实跑本批它是 +0.13~+0.23。所以有内容的说法只能是"观察值落在零线 p5 之下"，
        绝不能写成"零线均值 ≈ 0"（早期版本就犯过，与自己的产物矛盾）。全局置换那一列打破城市
        结构，对本模型是错的对照。
        """
        frame = _zero_sum_frame()
        nulls = backtest.permutation_nulls(frame, reps=20)
        radius = str(common.RADII_KM[2])
        block = nulls[radius]["nulls"]
        self.assertGreater(block["inCity"]["nullMean"], -0.35, block)   # 不是"深负的零线"
        self.assertLess(abs(block["inCity"]["nullMean"]), 0.35, block)
        observed = nulls[radius]["observedSpearman"]
        self.assertLess(observed, block["inCity"]["nullP5"])
        self.assertLessEqual(block["inCity"]["shareOfNullAtOrBelowObserved"], 0.05)
        for key in ("nullMean", "nullP5", "nullP95", "shareOfNullAtOrBelowObserved"):
            self.assertIn(key, block["inCity"])
            self.assertIn(key, block["global"])

    def test_null_sign_phrase_follows_the_data_not_a_preset(self):
        """措辞函数必须能在零线为负时**改口**——否则换一批数据就会再次自相矛盾。"""
        self.assertIn("弱正", backtest._null_sign_phrase([0.13, 0.23]))
        self.assertIn("跨零", backtest._null_sign_phrase([-0.02, 0.23]))
        negative = backtest._null_sign_phrase([-0.40, -0.30])
        self.assertIn("偏负", negative)
        self.assertIn("不再是独立证据", negative)

    def test_drift_phrase_names_the_counter_step_instead_of_claiming_monotone(self):
        """实跑前两个半径是 −0.524 → −0.522（往上一步）；写死"单调恶化"会与 verdict 打架。"""
        mono = backtest._drift_phrase([-0.3, -0.5, -0.6, -0.7])
        self.assertIn("单调", mono)
        self.assertNotIn("非严格", mono)
        self.assertIn("基本持平", backtest._drift_phrase([-0.5, -0.5, -0.5]))
        loose = backtest._drift_phrase([-0.524, -0.522, -0.589, -0.677, -0.789])
        self.assertIn("非严格单调", loose)
        self.assertIn("2→3km", loose)                                  # 反向台阶被点名
        improving = backtest._drift_phrase([-0.9, -0.8, -0.5, -0.3, -0.2])
        self.assertIn("未见零和代数接管", improving)                    # 越走越不负时不许说"接管"

    def test_field_worsens_with_radius_toward_the_zero_sum_limit(self):
        """半径越大，场越接近"其余站之和 = T_c − 本站"这个代数极限 → 读数应更负。

        这条把"负"的来源拆开：小半径读到的是真实的倒置邻接结构，长半径才是零和代数接管。
        """
        frame = _zero_sum_frame()
        loso = backtest.loso_by_radius(frame)
        rhos = [loso[str(r)]["field"]["spearman"] for r in common.RADII_KM]
        self.assertTrue(all(later <= earlier for earlier, later in zip(rhos[:-1], rhos[1:])), rhos)
        self.assertLess(rhos[-1], rhos[0] - 0.3, rhos)      # 长半径明显更负（零和代数接管）
        self.assertLess(rhos[-1], -0.8, rhos)

    def test_nulls_are_deterministic_given_the_seed(self):
        frame = _zero_sum_frame()
        first = backtest.permutation_nulls(frame, reps=3)
        second = backtest.permutation_nulls(frame, reps=3)
        self.assertEqual(json.dumps(first, sort_keys=True), json.dumps(second, sort_keys=True))
        self.assertEqual(common.SEED, 20260915)


@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class SiteTypeSignal(unittest.TestCase):
    def test_type_recovers_within_city_share_order(self):
        """每城每型一站 → 同类型留一只能跨城池化；类型完全决定份额时读数应接近 +1。"""
        frame = _intensity([
            ("a1", "C1", 0.00, 0.00, "MALL", 600, 600, 0),
            ("a2", "C1", 0.02, 0.00, "STREET", 300, 300, 0),
            ("a3", "C1", 0.04, 0.00, "ROAD", 100, 100, 0),
            ("b1", "C2", 10.0, 10.0, "MALL", 550, 550, 0),
            ("b2", "C2", 10.02, 10.0, "STREET", 330, 330, 0),
            ("b3", "C2", 10.04, 10.0, "ROAD", 120, 120, 0),
        ])
        block = backtest.site_type_signal(frame)
        self.assertEqual(block["coveredStations"], 6)
        self.assertGreater(block["losoOnWithinCityShare"]["spearman"], 0.8)
        self.assertEqual(sorted(block["shareByType"]), ["MALL", "ROAD", "STREET"])
        # 份额按类型分层，而空间腿在同一批数据上是负的——这才是本批可学的规律
        self.assertLess(backtest.loso_by_radius(frame)[str(common.RADII_KM[2])]["field"]["spearman"], 0)


@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class Mechanism(unittest.TestCase):
    """机制诊断的**说法强度**必须由现算的单侧 P 决定。

    这里是个真实踩过的坑：旧版把 ``−1/(k−1)`` 叫"随机切分的理论地板"，再用"最近邻读数比它更负"
    当"倒置梯度"的证据。那个数是零线**期望**，随机切分经常比它更负——下面的合成帧就是活例子：
    ρ 打到理论下限 −1，可它在随机置换里出现的概率并不小，于是"深负于地板"这句话什么也没排除。
    """

    _TIED_PAIRS = [
        ("a1", "C1", 0.000, 0.00, "MALL", 900, 900, 0),
        ("a2", "C1", 0.002, 0.00, "STREET", 10, 10, 0),
        ("a3", "C1", 0.050, 0.00, "ROAD", 500, 500, 0),
        ("a4", "C1", 0.052, 0.00, "DEPOT", 20, 20, 0),
        ("b1", "C2", 10.000, 10.0, "MALL", 10, 10, 0),
        ("b2", "C2", 10.002, 10.0, "STREET", 900, 900, 0),
        ("b3", "C2", 10.050, 10.0, "ROAD", 20, 20, 0),
        ("b4", "C2", 10.052, 10.0, "DEPOT", 500, 500, 0),
    ]

    def test_inversion_claim_carries_its_own_p_value(self):
        mech = backtest.mechanism_diagnostic(_intensity(self._TIED_PAIRS), nn_reps=300)
        rho = mech["nearestNeighbourDemandVsOwn"]["spearman"]
        null = mech["randomAssignmentNull"]
        self.assertAlmostEqual(rho, -1.0)
        self.assertEqual(mech["stationsPerCity"], 4)
        self.assertAlmostEqual(mech["randomAssignmentTheoryMean"], -0.3333)
        self.assertIn("shareOfNullAtOrBelowObserved", null)
        self.assertEqual(null["reps"], 300)
        # 结论文句与 P 必须是同一套数：不许一句话讲一个更强的结论
        self.assertEqual(mech["inversionVerdict"], backtest._inversion_phrase(rho, null))
        self.assertIn(mech["inversionVerdict"], mech["interpretation"])
        small_p = null["shareOfNullAtOrBelowObserved"] <= 0.05
        self.assertEqual("倒置梯度" in mech["inversionVerdict"], small_p)

    def test_inversion_phrase_scales_with_the_measured_p(self):
        """三档措辞 + 退化档：P 大就得收回结论，P 小才许说倒置。"""
        base = {"reps": 2000, "nullMean": -0.25, "nullP5": -0.6, "nullP95": 0.1, "nullMin": -0.87}
        strong = dict(base, shareOfNullAtOrBelowObserved=0.0005)
        marginal = dict(base, shareOfNullAtOrBelowObserved=0.04)
        weak = dict(base, shareOfNullAtOrBelowObserved=0.3)
        self.assertIn("证据较强", backtest._inversion_phrase(-0.9, strong))
        self.assertIn("勉强", backtest._inversion_phrase(-0.9, marginal))
        self.assertIn("据此断言", backtest._inversion_phrase(-0.6, weak))
        self.assertIn("无空间信号", backtest._inversion_phrase(-0.6, weak))
        self.assertIn("不下", backtest._inversion_phrase(float("nan"), dict(weak, reps=0)))
        self.assertIn("不下", backtest._inversion_phrase(
            -0.6, dict(weak, shareOfNullAtOrBelowObserved=None)))

    def test_mechanism_geometry_and_two_rulers_are_reported(self):
        mech = backtest.mechanism_diagnostic(_intensity(self._TIED_PAIRS), nn_reps=200)
        self.assertEqual(mech["stationsPerCity"], 4)
        self.assertIn("cityTotalSpread", mech)
        self.assertGreater(mech["interCityPairKmP50"], mech["intraCityPairKmP50"])
        self.assertIn("nearestNeighborKm", mech)


# ------------------------------------------------------------------ 数据层聚合口径
@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class StationIntensity(unittest.TestCase):
    def _fake_clean(self, name, columns=None):
        if name == "stations":
            frame = pd.DataFrame({"station_id": ["s1", "s2"], "city_id": ["C", "C"],
                                  "latitude": [30.0, 30.02], "longitude": [110.0, 110.02],
                                  "site_type": ["MALL", "STREET"]})
        elif name == "charging_sessions":
            # 三条会话：两条 UTC 日历日（01-01 / 01-02），但折算北京业务日后全落在 01-02 一天
            stamps = pd.to_datetime(["2025-01-01T20:00:00Z", "2025-01-02T04:00:00Z",
                                     "2025-01-02T15:59:59Z"]).tz_localize(None)
            frame = pd.DataFrame({"station_id": ["s1", "s1", "s2"], "started_at": stamps})
        elif name == "charger_telemetry":
            frame = pd.DataFrame({"station_id": ["s1", "s1", "s2"],
                                  "state": ["CHARGING", "CHARGING", "AVAILABLE"],
                                  "recorded_at": pd.to_datetime(
                                      ["2025-01-01T00:00:00Z"] * 3).tz_localize(None)})
        elif name == "queue_entries":
            frame = pd.DataFrame({"station_id": ["s1", "s1", "s2"],
                                  "status": ["ABANDONED", "SERVED", "ABANDONED"]})
        else:
            raise AssertionError(name)
        return frame[columns] if columns else frame

    def test_columns_and_unmet_overlay(self):
        with mock.patch.object(common, "load_clean_table", self._fake_clean):
            frame = common.station_intensity()
        self.assertLessEqual({"sessions", "charging_ticks", "abandoned", "demand",
                              "demand_incl_unmet", "obs_days"}, set(frame.columns))
        first = frame[frame["station_id"] == "s1"].iloc[0]
        self.assertEqual(int(first["sessions"]), 2)
        self.assertEqual(int(first["abandoned"]), 1)
        self.assertEqual(float(first["demand_incl_unmet"]), float(first["demand"]) + 1.0)

    def test_obs_days_counts_business_days_not_utc_days(self):
        """三条会话跨两个 UTC 日历日，但 +8h 后都落在同一个北京业务日 → obs_days = 1。

        按 UTC 日历日数会得到 2（真实批上就是 181 vs 180 的那个 0.55% 系统性偏差）。
        """
        with mock.patch.object(common, "load_clean_table", self._fake_clean):
            frame = common.station_intensity()
        self.assertEqual(int(frame["obs_days"].iloc[0]), 1)
        naive_utc = pd.to_datetime(["2025-01-01T20:00:00Z", "2025-01-02T04:00:00Z",
                                    "2025-01-02T15:59:59Z"]).tz_localize(None)
        self.assertEqual(pd.DatetimeIndex(naive_utc).normalize().nunique(), 2)
        self.assertEqual(float(frame[frame["station_id"] == "s1"]["demand_per_day"].iloc[0]), 2.0)

    def test_telemetry_rows_are_ticks_not_minutes(self):
        """口径命名：一行 = 一个 5 分钟 tick。列名必须是 charging_ticks，不许回退成 minutes。"""
        with mock.patch.object(common, "load_clean_table", self._fake_clean):
            frame = common.station_intensity()
        self.assertIn("charging_ticks", frame.columns)
        self.assertNotIn("charging_minutes", frame.columns)
        self.assertEqual(int(frame[frame["station_id"] == "s1"]["charging_ticks"].iloc[0]), 2)
        self.assertIn("tick", common.SOURCE_TABLES["charger_telemetry"])

    def test_business_offset_is_eight_hours(self):
        self.assertEqual(common.BUSINESS_OFFSET_HOURS, 8)


@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class UnmetProxyCaveats(unittest.TestCase):
    """弃队代理的脏处必须**算出来**，不许是抄进来的字面量。

    旧版在 `backtest.py` 里写死 `476` / `4037`，而 476 是以 `joined_at`（入队）为锚算的——
    排队超过窗口才放弃的记录会被漏出窗外。这一组用例专门盯住"换数据就得换数字"。
    """

    def _loader(self, queue_rows, session_rows):
        def load(name, columns=None):
            if name == "queue_entries":
                frame = pd.DataFrame(queue_rows)
            elif name == "charging_sessions":
                frame = pd.DataFrame(session_rows)
            else:
                raise AssertionError(name)
            return frame[columns] if columns else frame
        return load

    def _stamps(self, values):
        return pd.to_datetime(values).tz_localize(None)

    def test_double_count_is_measured_after_the_abandonment_not_before(self):
        # u1：入队 30h 前、离队 1h 前、复充在离队后 2h → 只有离队锚能抓到（旧口径会漏算）
        # u2：正常事后复充；u3：会话早于入队（两个锚都不算双计，但离队锚把它列进"弃队前已有会话"）
        # u4：复充在窗口外
        queue_rows = {
            "queue_id": ["q1", "q2", "q3", "q4", "q5"],
            "user_id": ["u1", "u2", "u3", "u4", "u9"],
            "station_id": ["s1", "s1", "s1", "s1", "s1"],
            "joined_at": self._stamps(["2025-01-01T00:00:00", "2025-01-02T00:00:00",
                                       "2025-01-02T00:00:00", "2025-01-02T00:00:00",
                                       "2025-01-02T00:00:00"]),
            "called_at": self._stamps([None, None, None, None, "2025-01-02T06:00:00"]),
            "resolved_at": self._stamps(["2025-01-02T05:00:00", "2025-01-02T06:00:00",
                                         "2025-01-02T12:00:00", "2025-01-02T06:00:00",
                                         "2025-01-02T07:00:00"]),
            "status": ["ABANDONED", "ABANDONED", "ABANDONED", "ABANDONED", "CALL_EXPIRED"],
            "session_id": [None, None, None, None, None],
        }
        session_rows = {
            "user_id": ["u1", "u2", "u3", "u4"],
            "station_id": ["s1", "s1", "s1", "s1"],
            "started_at": self._stamps(["2025-01-02T07:00:00", "2025-01-02T20:00:00",
                                        "2025-01-01T20:00:00", "2025-01-06T00:00:00"]),
        }
        with mock.patch.object(common, "load_clean_table", self._loader(queue_rows, session_rows)):
            caveats = common.unmet_proxy_caveats()
        self.assertEqual(caveats["abandonedRechargedAfter"], 2)          # q1 + q2，q4 在窗口外
        self.assertEqual(caveats["abandonedWithSessionBeforeLeaving"], 1)  # q3 会话早于离队
        self.assertEqual(caveats["statusCounts"], {"SERVED": 0, "ABANDONED": 4, "CALL_EXPIRED": 1})
        # 锚点差异被披露出来：入队锚会漏掉 q1（30h 前入队）
        self.assertEqual(caveats["anchorVariants"]["ABANDONED"]["joined_at"]["rechargedAfter"], 1)
        self.assertEqual(caveats["anchorVariants"]["ABANDONED"]["resolved_at"]["rechargedAfter"], 2)
        # 语义区分：ABANDONED 没被叫号，CALL_EXPIRED 被叫过号
        self.assertEqual(caveats["semantics"]["ABANDONED"]["withCalledAt"], 0)
        self.assertEqual(caveats["semantics"]["CALL_EXPIRED"]["withCalledAt"], 1)
        self.assertEqual(caveats["semantics"]["ABANDONED"]["withSessionId"], 0)

    def test_numbers_move_when_the_window_moves(self):
        """窗口是假设，不是常数：换成 12h 必须改变读数（写死的字面量做不到这点）。"""
        queue_rows = {
            "queue_id": ["a", "b"], "user_id": ["u1", "u2"], "station_id": ["s"] * 2,
            "joined_at": self._stamps(["2025-01-01T00:00:00"] * 2),
            "called_at": self._stamps([None, None]),
            "resolved_at": self._stamps(["2025-01-02T00:00:00"] * 2),
            "status": ["ABANDONED"] * 2, "session_id": [None, None],
        }
        session_rows = {"user_id": ["u1", "u2"], "station_id": ["s", "s"],
                        "started_at": self._stamps(["2025-01-02T05:00:00", "2025-01-02T20:00:00"])}
        with mock.patch.object(common, "load_clean_table", self._loader(queue_rows, session_rows)):
            wide = common.unmet_proxy_caveats(window_hours=24.0)
            narrow = common.unmet_proxy_caveats(window_hours=12.0)
        self.assertEqual(wide["abandonedRechargedAfter"], 2)
        self.assertEqual(narrow["abandonedRechargedAfter"], 1)
        self.assertGreater(wide["abandonedRechargedAfterPct"], narrow["abandonedRechargedAfterPct"])


@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class Digests(unittest.TestCase):
    def test_sha256_bytes_known_vector(self):
        self.assertEqual(common.sha256_bytes(b""),
                         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
        self.assertEqual(common.sha256_bytes(b"abc"),
                         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")

    def test_digests_are_content_bound_not_just_key_shaped(self):
        """摘要必须**随字节变**。

        旧测试把 ``sha256_bytes`` 换成 ``blob.decode()``、又把 ``CLEAN_DIR`` 指向空目录，于是
        所有摘要都成空串、断言只剩键名——把 ``source_table_digests`` 整体换成
        ``{name: "" for ...}`` 也照样通过。它守的正是"换表即产物失效"那条链，不能是恒真的。
        """
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            for name in common.SOURCE_TABLES:
                (base / name).mkdir()
                pd.DataFrame({"a": [1]}).to_parquet(base / name / "p0.parquet")
            with mock.patch.object(common, "CLEAN_DIR", base):
                first = common.source_table_digests()
            self.assertEqual(sorted(first), sorted(common.SOURCE_TABLES))
            for name, value in first.items():
                self.assertRegex(value, r"^[0-9a-f]{64}$", name)
            # 改一张表的字节 → 只有那张表的摘要动
            (base / "stations" / "p1.parquet").write_bytes(b"different bytes")
            with mock.patch.object(common, "CLEAN_DIR", base):
                second = common.source_table_digests()
            self.assertEqual([k for k in first if first[k] != second[k]], ["stations"])
            # 多一个分区文件也算换表（拼接顺序由文件名决定，写进产物的是全表摘要）
            (base / "charging_sessions" / "p0.parquet").write_bytes(b"rewritten")
            with mock.patch.object(common, "CLEAN_DIR", base):
                third = common.source_table_digests()
            self.assertNotEqual(third["charging_sessions"], second["charging_sessions"])


# ------------------------------------------------------------------ score 的引用纪律
@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class ScoreBacktestReadback(unittest.TestCase):
    """打分腿引用回测结论：缺文件、批次不符、**表换了**、口径读不到——四种都得中止。

    ``source_table_digests`` 会 sha256 全部 clean 分区，测试里换成固定字典：既省钱，也让
    "表变了但批次号没变"这个场景可造（真实场景正是它）。
    """

    DIGESTS = {"stations": "1" * 64, "charging_sessions": "2" * 64,
               "charger_telemetry": "3" * 64, "queue_entries": "4" * 64}
    CAVEATS = {"windowHours": 24.0,
               "statusCounts": {"SERVED": 91000, "ABANDONED": 10143, "CALL_EXPIRED": 4037},
               "abandonedRechargedAfter": 492, "abandonedRechargedAfterPct": 4.85,
               "callExpiredRechargedAfter": 226, "callExpiredRechargedAfterPct": 5.6,
               "abandonedWithSessionBeforeLeaving": 294}

    def _write(self, directory: Path, payload: dict) -> Path:
        path = directory / "backtest.json"
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path

    def _payload(self) -> dict:
        radius = str(common.CAPTURE_KM)
        return {"publishedBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
                "sourceTablesSha256": dict(self.DIGESTS),
                "unmetProxyCaveats": dict(self.CAVEATS),
                "losobyRadius": {radius: {"field": {"spearman": -0.88}, "n": 25}},
                "permutationNulls": {radius: {"nulls": {"inCity": {"nullMean": -0.45,
                                                                   "shareOfNullAtOrBelowObserved": 0.0}}}},
                "siteTypeSignal": {"losoOnWithinCityShare": {"spearman": 0.8969}}}

    def _guards(self, path, digests=None):
        return mock.patch.multiple(common, BACKTEST_JSON=path,
                                   source_table_digests=lambda: dict(
                                       digests if digests is not None else self.DIGESTS))

    def test_missing_backtest_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self._guards(Path(tmp) / "backtest.json"):
                with self.assertRaises(FileNotFoundError):
                    score.read_backtest_verdict()

    def test_wrong_batch_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self._write(Path(tmp), {**self._payload(),
                                           "publishedBatchId": "analytics-deadbeef"})
            with self._guards(path):
                with self.assertRaises(common.BatchMismatch):
                    score.read_backtest_verdict()

    def test_stale_input_tables_raise_even_same_batch(self):
        """批次号没变、表内容变了 → 那份 backtest.json 的读数不再代表要打分的数据。

        README 里"换表即产物失效"此前只有代码做一半：``score`` 只查批次号与字段存在性。
        """
        drifted = dict(self.DIGESTS, charging_sessions="9" * 64)
        with tempfile.TemporaryDirectory() as tmp:
            path = self._write(Path(tmp), self._payload())
            with self._guards(path, digests=drifted):
                with self.assertRaises(common.BatchMismatch) as caught:
                    score.read_backtest_verdict()
        self.assertIn("charging_sessions", str(caught.exception))

    def test_missing_caveats_or_digest_raise(self):
        for key in ("unmetProxyCaveats", "sourceTablesSha256"):
            payload = self._payload()
            del payload[key]
            with tempfile.TemporaryDirectory() as tmp, self.subTest(missing=key):
                path = self._write(Path(tmp), payload)
                with self._guards(path):
                    with self.assertRaises(common.BatchMismatch):
                        score.read_backtest_verdict()

    def test_malformed_field_is_a_refusal_not_a_keyerror(self):
        """必需读数缺字段要给出**可解释的拒算**，不是裸 KeyError 堆栈。"""
        payload = self._payload()
        payload["permutationNulls"][str(common.CAPTURE_KM)]["nulls"]["inCity"].pop("nullMean")
        with tempfile.TemporaryDirectory() as tmp:
            path = self._write(Path(tmp), payload)
            with self._guards(path):
                with self.assertRaises(common.BatchMismatch):
                    score.read_backtest_verdict()

    def test_missing_radius_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            payload = self._payload()
            payload["permutationNulls"] = {}
            path = self._write(Path(tmp), payload)
            with self._guards(path):
                with self.assertRaises(common.BatchMismatch):
                    score.read_backtest_verdict()

    def test_readback_returns_the_numbers_the_csv_repeats(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self._write(Path(tmp), self._payload())
            with self._guards(path):
                read = score.read_backtest_verdict()
            self.assertEqual(read["fieldSpearmanAtCaptureKm"], -0.88)
            self.assertEqual(read["inCityNullMean"], -0.45)
            self.assertEqual(read["siteTypeShareSpearman"], 0.8969)
            self.assertEqual(read["unmetProxyCaveats"], self.CAVEATS)

    def test_demand_basis_text_is_made_of_readback_numbers(self):
        """口径句里的每个数字都从产物取：换 dict 就换句子，**不许**留手抄字面量。

        回归钉子：旧版在 ``score_summary.json.demandBasis`` 里写死 ``476``，而同目录的
        ``backtest.json`` 已经现算出 ``492``——同一次运行、同一件事、两个数。
        """
        text = score.demand_basis_text(self.CAVEATS)
        self.assertIn("492", text)
        self.assertIn("4.85", text)
        self.assertIn("4,037", text)
        self.assertNotIn("476", text)
        moved = dict(self.CAVEATS, abandonedRechargedAfter=500, abandonedRechargedAfterPct=4.93)
        moved_text = score.demand_basis_text(moved)
        self.assertIn("500", moved_text)
        self.assertNotIn("492", moved_text)

    def test_main_stamps_status_into_every_csv_row(self):
        """状态标记必须活在 CSV 表体里：CSV 会被单独拷走、单独打开，不能只活在 JSON 侧文件。

        合成两城站点，patch 掉数据层与批次校验，跑 score.main() 落 tmp——不碰已发布产物目录。
        """
        def fake_intensity():
            return _intensity([
                ("a1", "C1", 0.00, 0.00, "MALL", 600, 600, 4),
                ("a2", "C1", 0.02, 0.00, "STREET", 300, 300, 2),
                ("a3", "C1", 0.04, 0.04, "ROAD", 100, 100, 1),
                ("b1", "C2", 10.0, 10.0, "MALL", 550, 550, 3),
                ("b2", "C2", 10.02, 10.0, "STREET", 330, 330, 2),
                ("b3", "C2", 10.04, 10.04, "ROAD", 120, 120, 1),
            ])

        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            backtest_json = self._write(base, self._payload())
            with mock.patch.object(common, "OUT_DIR", base), \
                    mock.patch.object(common, "BACKTEST_JSON", backtest_json), \
                    mock.patch.object(common, "OPPORTUNITIES_CSV", base / "candidate_opportunities.csv"), \
                    mock.patch.object(common, "SCORE_JSON", base / "score_summary.json"), \
                    mock.patch.object(common, "verify_batch", lambda manifest=None: {}), \
                    mock.patch.object(common, "read_source_manifest", lambda: {"pipelineRunId": "test-run"}), \
                    mock.patch.object(common, "station_intensity", fake_intensity), \
                    mock.patch.object(common, "source_table_digests", lambda: dict(self.DIGESTS)):
                summary = score.main()
                ranked = pd.read_csv(base / "candidate_opportunities.csv")
                with self.assertRaises(FileExistsError):        # 冻结写入：第二次跑必须拒绝覆盖
                    score.main()
                # 端到端再走一遍"表换了即中止"：批次号、文件、字段全都在，只有内容动了
                with mock.patch.object(common, "source_table_digests",
                                       lambda: dict(self.DIGESTS, stations="f" * 64)):
                    (base / "candidate_opportunities.csv").unlink()
                    (base / "score_summary.json").unlink()
                    with self.assertRaises(common.BatchMismatch):
                        score.main()
            self.assertEqual(set(ranked["model_status"]), {"NOT-BACKTEST-VALIDATED"})
            self.assertEqual(set(ranked["backtest_field_spearman"]), {-0.88})
            self.assertEqual(set(ranked["backtest_incity_null_spearman"]), {-0.45})
            self.assertEqual(summary["status"], "NOT-BACKTEST-VALIDATED")
            self.assertEqual(summary["backtestReadback"]["fieldSpearmanAtCaptureKm"], -0.88)
            self.assertGreater(ranked["nearest_station_km"].min(), score.MIN_DIST_FROM_EXISTING_KM)
            self.assertLessEqual(len(ranked), 5 * ranked["city_id"].nunique())


@unittest.skipUnless(STACK, "numpy/pandas/scipy not installed")
class WriteDiscipline(unittest.TestCase):
    def test_require_empty_run_dir_refuses_strays(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            (base / "backtest.json").write_text("{}", encoding="utf-8")
            with self.assertRaises(FileExistsError):
                common.require_empty_run_dir(base)
            common.require_empty_run_dir(base, extra_allowed=("backtest.json",))

    def test_write_new_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "x.json"
            common.write_new_json(path, {"a": 1})
            with self.assertRaises(FileExistsError):
                common.write_new_json(path, {"a": 2})
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), {"a": 1})


if __name__ == "__main__":
    unittest.main()
