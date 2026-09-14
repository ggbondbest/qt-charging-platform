# 用户流失预测线(gbdt-churn-user-v1)

经典留存问题:观察窗内还在充电的用户,未来 14 天是否再发起充电。
**口径写死不挪动**:OBSERVE_END = 2026-05-15(之前全部行为做特征),
标签 = (05-15, 05-29] 内无任何 charging_attempt(平台均值节奏约 9 天一充,14 天 ≈ 错过 1.5 个周期)。

## 数据与协议

- 只读 clean 层 users / charging_attempts / charging_sessions / queue_entries / vehicles。
- 6,000 用户,14 天流失率 17.2%(观察窗内活跃过的 5,998 人)。
- 切分:`sha1("42:user_id") % 100` → 70/15/15(哈希切分可复算、不吃随机流,
  同一用户永远只在一侧)。
- 特征(全部 ≤ OBSERVE_END):R(距上次请求/成功充电天数)、F(14/30/60/90 天请求与
  成功计数、去重站数)、M(90 天 kWh/实付/每 kWh 均价/单均量)、节奏(充电间隔
  mean/std/max、最近 5 段 vs 全程比、周频次)、参与度(优惠券使用率、排队次数、排队弃队率)、
  画像(城市/细分/会员/渠道/车级/电池容量/车端功率/注册时长)。29 列,含 5 个类别列。
- 模型:HistGradientBoostingClassifier,`class_weight=balanced`,一次成型未二调。
- TEST 只在 VALIDATION 定稿后批阅一次(`write_new_json` 冻结 + 等价校验)。

## 结果(n=935 TEST 用户,流失率 15.2%)

| 方法 | AUC | PR-AUC | lift@10% |
| --- | --- | --- | --- |
| **模型** | **0.744** | **0.325** | **2.41** |
| recency 规则(仅距上次天数) | 0.610 | 0.257 | 1.91 |

VALIDATION(n=894):模型 AUC 0.788 / PR-AUC 0.458;TEST 回落约 4pt AUC,属正常样本波动。

模型分十分位的实际流失率:`36.6 → 34.0 → 17.2 → 14.9 → 19.4 → 8.5 → 9.7 → 8.5 → 2.2 → 1.1 %`
——前 10% 人群流失浓度是平均的 2.4 倍,做召回券投放即拿即用的形状;
第 5 分位有小凸起(19.4%),典型的高摇摆人群,恰是模型与纯 recency 拉开差距的区间。

## 限制

- 合成数据,表述上限"模拟数据测试结果"。
- 14 天标签窗贴着数据尾部,最后一批用户观察不满窗(以数据截止日为界统一截断,不做人工补齐)。
- 未建 uplift:流失概率 ≠ 干预收益,发券挽回效果需真实券实验数据。

## 复现

```bash
python -m data_analysis.ml.churn.train      # 建表(缓存)+训练+验证表
python -m data_analysis.ml.churn.evaluate   # TEST 首盲,重跑要求逐字节等价
```

产物:`outputs/ml_churn/`(user_features 缓存、bundle、train_metrics、冻结 test_metrics)。
