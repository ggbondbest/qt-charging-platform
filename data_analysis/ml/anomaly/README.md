# 充电过程异常检测线(iforest-session-battery-v1)

契约 P2 项:IsolationForest 无监督识别**会话级**电池/充电异常,
`anomaly_labels` 只在评测时关联,绝不进训练(契约原文要求)。

## 协议

- 样本单元 = 会话:1,217,829 条 5 分钟电池采样 → 121,539 个会话 × 22 个曲线汇总特征
  (电压/电流统计、片间压差、温度 spread 与爬升、单位时间 SOC/能量、soc 增益等)。
  同会话窗口天然同侧(按 started_at 落时间三段),满足"不拆两边"纪律。
- TRAIN 拟合 scaler+IF(300 树,seed 42;中位数插补只从 TRAIN 学);
  阈值 = TRAIN 分数分布上分位,分位点从 {88..99} 在 **VALIDATION** 按 F1 选(选中 97)。
- 时间三段与负荷/推荐线同边界:05-01 / 05-15 / 05-30。TEST 407 正例,首盲一次已冻结。

## 结果(会话级,正例率 4%)

| | precision | recall | F1 |
| --- | --- | --- | --- |
| VALIDATION | 0.161 | 0.142 | **0.151** |
| TEST(首盲) | 0.154 | 0.140 | **0.147** |
| 规则基线(p99 温度/压差) | 0 | 0 | 0(验证/测试期一次未触发) |

分型召回(TEST):EARLY_STOP **26.5%**、POWER_DERATING 5.3%、THERMAL_STRESS 5.5%。

## 诚实结论:这是弱结果,且原因明确

1. **会话均值稀释瞬时异常**:三类标签都是"发生在会话中途某时刻"的事件
   (labels 带 recorded_at),而 v1 把 ~10 个采样点平均成一行;一次 5 分钟的热失控尖峰
   在会话均值里只剩 ~1/10 的高度,IF 自然抓不住。分型召回完全吻合这个机制:
   EARLY_STOP 改变整条曲线形态(提前终止→soc 增益/dV 特征全体漂移)所以最可检;
   DERATING/THERMAL 是瞬时段,会话级几乎不可检。
2. 无监督 vs 4% 稀薄正例的天花板本来就低(随机排序 F1≈0.04,模型 0.147 是 3.7 倍但远不够用)。
3. 规则基线用 TRAIN p99 过拟合分布尾,验证期一次都不触发——阈值分位必须在全量分布上校准,
   这个写法留作反面教材。

**改进路径(未在本 v1 里做,避免为 TEST 刷分)**:采样点级打 IF 分、会话取 max/TopK——
与"事件型标签"同粒度;或把 labels 的 recorded_at 邻域窗口特征(均值化会抹掉的部分)
作为 v2 的评测目标。v2 若上线将作为新 model_id 走独立首盲。

## 复现

```bash
python -m data_analysis.ml.anomaly.detect
python -m unittest data_analysis.tests.test_ml_anomaly_churn
```

产物:`outputs/ml_anomaly/`(bundle、train_metrics、冻结 test_metrics、session_features 缓存)。
重跑对冻结文件做逐字节等价校验,不等价必失败(本会话真拦下过一次文案漂移)。
