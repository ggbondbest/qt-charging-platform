"""v5(气象上下文+固定阈值)纯函数单测:robust_z 的"TRAIN 定死"语义、_amb3 分箱边界。
robust_z 是固定决策函数的地基——med/MAD 只许来自 TRAIN,任何窗口原样套用;若实现偷偷
用全量数据拟合,TEST 又变成 transductive 自稀释,口径债回来了单测要能抓住。
import 链带上 detect_v3(sklearn/joblib);裸 CI 无依赖自跳。
"""

import unittest

try:
    import numpy as np
    import pandas as pd

    from data_analysis.ml.anomaly import detect_v5

    HAS_DEPS = True
except ImportError:
    HAS_DEPS = False


@unittest.skipUnless(HAS_DEPS, "pandas/numpy/sklearn not installed")
class RobustZ(unittest.TestCase):
    def setUp(self):
        self.tr = pd.Series([10.0, 12.0, 11.0, 13.0, 12.5])

    def test_constants_come_from_train_only(self):
        """核心不变量:换任何"未来窗口"数据,拟合常数纹丝不动(=可部署固定函数)。"""
        _, const_a = detect_v5.robust_z(self.tr, self.tr)
        _, const_b = detect_v5.robust_z(pd.Series([1e6] * 999), self.tr)
        self.assertEqual(const_a, const_b)
        med, mad = const_a
        self.assertAlmostEqual(med, float(self.tr.median()))
        self.assertAlmostEqual(mad, float((self.tr - med).abs().median()))

    def test_missing_scores_to_zero_and_clip(self):
        med = float(self.tr.median())
        scale = 1.4826 * float((self.tr - med).abs().median()) + detect_v5.EPS
        sig = pd.Series([np.nan, med + 1e6 * scale, med - 1e6 * scale])
        z, _ = detect_v5.robust_z(sig, self.tr)
        self.assertEqual(z.tolist(), [0.0, 10.0, -10.0])

    def test_train_median_maps_to_zero(self):
        z, _ = detect_v5.robust_z(pd.Series([med := self.tr.median()]), self.tr)
        self.assertAlmostEqual(float(z.iloc[0]), 0.0)


@unittest.skipUnless(HAS_DEPS, "pandas/numpy/sklearn not installed")
class AmbientBins(unittest.TestCase):
    def test_edges_right_closed_three_bins(self):
        t = pd.Series([-5.0, 10.0, 10.01, 22.0, 22.01, 45.0])
        got = detect_v5._amb3(t).astype(str).tolist()
        self.assertEqual(got, ["cold", "cold", "mid", "mid", "hot", "hot"])

    def test_nan_amb3_falls_back_to_base_table(self):
        # _amb3 对 NaN 给 NA → 三元组格 join 落空 → 代码路径 fillna(expT2) 退低阶表,
        # 不得静默产出 NaN 信号(z 会 fillna(0) 掩盖,这里只钉 pd.cut 行为本身)
        self.assertTrue(bool(detect_v5._amb3(pd.Series([np.nan])).isna().iloc[0]))


if __name__ == "__main__":
    unittest.main()
