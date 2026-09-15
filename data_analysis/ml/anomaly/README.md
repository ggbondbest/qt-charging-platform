# 充电过程异常检测线(契约 P2,盲评链 v1→v4)

IsolationForest 无监督识别**会话级**电池/充电异常,`anomaly_labels` 只在评测时关联,
绝不进训练(契约原文)。正例口径全程一致:会话内出现任一标签事件即为正
(121,539 会话,TEST 段 407 正例、告警预算约 190)。

## 最终成绩(v4,独立首盲一次已冻结)

| 版本 | model_id | TEST F1 | 说明 |
| --- | --- | --- | --- |
| v1 | iforest-session-battery-v1 | 0.147 | 会话均值 × 22 特征 |
| v2 | iforest-pointmax-battery-v2 | 0.096 | 点级+会话内 z(负结果,见机制) |
| v3 | iforest-session-rankfuse-v3 | 0.149 | 7 信号融合,贪心退回纯 v1(负结果) |
| **v4** | **context-baseline-rankfuse-v4** | **0.3095** | precision 0.479 / recall 0.229 |

v4 分型召回(TEST):**THERMAL_STRESS 100%**(91/91 全中,误报仅 101)、
EARLY_STOP 0、POWER_DERATING 1.3%。随机参照同告警预算 F1≈0.05,即 **6.1 倍**。
每一版都是独立 model_id、独立 TEST 首盲、重跑等价校验(json 往返后语义相等,非严格逐字节);前一版成绩不追改。

## 口径勘误(自查评审实锤,冻结件不追改、此处为准)

- **v3/v4 的阈值语义是 transductive 批筛,不是可部署固定阈值**:融合分 = split 内百分位秩,
  "TRAIN p96"恒等于 0.9600003…,套到任何窗口都是把告警率钉在预算上(v4 TEST 实告警 1.96%)。
  成绩的正确读法:**"每批固定 ~2% 告警预算下的离线会话筛选"**——对运营批筛成立,对
  "新会话来了就能判"的在线部署不成立(那需要原始分/固定阈值形态,是 v5 的工程量)。
  v1 不受此影响(原始 IF 分 + TRAIN 阈值,真实迁移 3%→3.75%)。
- 缺信号会话被 fillna(0) 钉底为"永不可告警",与旧 NaN 语义同为静默 FN;`train_metrics` 现在
  披露缺失计数。当前数据缺失为 0,合成场景不触发,真实数据上新 charger/新分箱格会先咬这里。
- 随机参照的告警预算差一(prf 严格 `>`),量级 1/515,不改"6.1 倍"结论。

## 为什么中间两版是负的,以及 v4 凭什么赢

- v1(会话均值):抓得住"整条曲线形态改变"(EARLY_STOP 召回 26.5%),
  但 ~10 点均值把瞬时事件稀释 ~10 倍(热应力仅 5.5%)。
- v2(会话内 robust z + 点级 TopK):与标签同粒度打分,预期"稀释消失"——实际更差。
  机制:**会话内重归一化恰好抹掉"持续偏移"**——尾段电流减半时中位数自己跟着挪,
  z 分数看不见(POWER_DERATING 召回 0.7%,实锤)。
- v3(两视角 rank 融合):假设互补,VALIDATION 贪心却证明叠加只有稀释——
  最优子集退回纯 v1(0.149≈0.147)。互补性要等参照系换对才出现。
- v4(**语境参照**):逐点对比"(charger_id × SOC 档) 期望电流 / 温度 / 压差"中位数表,
  参考分布只在 TRAIN 点上学。过热不再被任何"自身基线"抹掉——temp_excess 单信号
  VALID F1 0.292 就超过 v1/v2/v3 全部,贪心到它为止(加别的反而降)。
  诚实边界:这是**单信号强、多类型仍不全**——EARLY_STOP/DERATING 的检出需要
  曲线截断/电流洼地的专门语境特征,留作 v5 方向(新 id 新首盲,不追改 v4 分)。

## 协议(全链共用)

- 时间三段与负荷线同边界:05-01 / 05-15 / 05-30;会话按 started_at 整段落侧,
  满足"同一会话不拆两边"。
- 插补中位数、StandardScaler、IsolationForest(300 树,seed 42)、阈值分位:只在 TRAIN 拟合;
  候选/子集/阈值只在 VALIDATION 按 F1 选(PERCENTILE 网格 × topk × 特征集 × 融合子集)。
- 规则基线(TRAIN p99 温度/压差)在验证期一次未触发——阈值必须在部署分布校准,反面教材。

## 复现

```bash
python -m data_analysis.ml.anomaly.detect        # v1
python -m data_analysis.ml.anomaly.detect_v2     # v2(点级+TopK 候选池)
python -m data_analysis.ml.anomaly.detect_v3     # v3(rank 融合)
python -m data_analysis.ml.anomaly.detect_v4     # v4(语境基线,最终)
python -m unittest data_analysis.tests.test_ml_anomaly_churn
```

产物:`outputs/ml_anomaly/`(各 bundle、train_metrics、四份冻结 test_metrics、
session/point/context 三张特征缓存)。合成数据,表述上限"模拟数据测试结果"。
