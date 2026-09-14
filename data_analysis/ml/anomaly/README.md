# 充电过程异常检测线(契约 P2,盲评链 v1→v5)

无监督识别**会话级**电池/充电异常,`anomaly_labels` 只在评测时关联,绝不进训练(契约原文)。
正例口径全程一致:会话内出现任一标签事件即为正(121,539 会话,TEST 段 407 正例)。

## 最终成绩(v5,独立首盲一次已冻结)

| 版本 | model_id | TEST F1 | 说明 |
| --- | --- | --- | --- |
| v1 | iforest-session-battery-v1 | 0.147 | 会话均值 × 22 特征 |
| v2 | iforest-pointmax-battery-v2 | 0.096 | 点级+会话内 z(负结果,见机制) |
| v3 | iforest-session-rankfuse-v3 | 0.149 | 7 信号融合,贪心退回纯 v1(负结果) |
| v4 | context-baseline-rankfuse-v4 | 0.3095 | 语境基线,precision 0.479(transductive 批筛口径) |
| **v5** | **context-weather-fixedthr-v5** | **0.3655** | **precision 1.0 / recall 0.224**,固定阈值可部署形态 |

v5 TEST 告警 91 会话、91 全中、**0 误报**;分型召回:**THERMAL_STRESS 100%**(91/91),
EARLY_STOP 0、POWER_DERATING 0——热应力之外的两类**v5 依旧没抓到**(池里有压流/压差信号,
VAL 上并入只稀释 F1 被 tie-break 淘汰,曲线截断的专门特征仍是欠账,需 v6+ 新 id 新首盲)。
随机参照同预算 F1≈0.0121,即 **30 倍**。每一版独立 model_id、独立 TEST 首盲、
重跑等价校验(json 往返后语义相等);前一版成绩不追改。

## v5 的两个实锤升级

**1. 气象上下文——拆掉热浪 FP 放大器。** 诊断:TRAIN 是 2-4 月(逐时气温均值 7.4°C),
TEST 是 5 月中下旬(23.1°C),v4 的"温度−期望表"超额在 TEST 全体会话虚抬 **+2.93°C**
(0.781→3.708)——天热冒充过热,正是 v4 precision 0.48 的误报来源。v5 把气温直接建进
期望表:温度按 (charger×电流档×三档气温 cold≤10/mid/hot>22,右闭)、压差按 (charger×三档气温)
的 TRAIN 中位数,空格退回低阶表(实测定格 704 格、逐时气温 join 覆盖 100%)。
修正结构三选一在 VALIDATION 上测(V1 点级 g 残差扣减 / V2 三元组表 / V3 二维表),
VAL F1 并列 0.3435 分不出胜负,按**跨窗虚抬机制指标**定选:V1 仍有 +2.11、
**V2 压到 +0.53**——升幅随电流乘性放大,加法点修正扣不动,建进表里才干净。
对照证据随表存 `context_meta_v5.pkl:adjustmentStudy`(建表时复算,非手工记录)。
异常筛查评的是**已发生**会话,子时刻天气是已观测事实,不触"未来预报当已知"的合同红线。

**2. 固定阈值——还 v3/v4 的 transductive 口径债。** v5 不做 split 内秩化:信号只在
TRAIN 做稳健标准化(z=(x−med)/1.4826MAD,±10 截断,缺信号 z=0 并披露缺失数),融合=所选
子集等权均值,阈值=TRAIN 融合分 p99,**定死后原样套任何窗口**。告警率成为部署结果而非
预算设定:TRAIN 1.00% / VALIDATION 0.88% / TEST 0.92%——跨窗稳定是"天气修正生效"的
第二重证据。子集(2⁶−1 全穷举 × 6 档分位 = 378 候选)只在 VALIDATION 按
(F1, −FP, 结构简单) 定选,选中 (tempW, v1Mean) p99;TEST 依旧只批一次。

## 口径勘误(v3/v4 冻结件不追改,此段为准)

- **v3/v4 的阈值语义是 transductive 批筛**:split 内百分位秩把告警率钉死在预算上
  (v4 TEST 实告警 1.96%),正确读法是"每批固定 ~2% 告警预算的离线筛选",不是在线固定
  阈值。v1 不受影响;v5 起为真实固定阈值形态。
- 缺信号会话 fillna(0) 钉底 = 静默 FN 风险,v5 保留该行为(等价"训练中等")并在
  train_metrics 披露计数;合成数据缺失为 0,真实数据新 charger 会先咬这里。
- v1-v4 随机参照的告警预算 off-by-one(prf 严格 `>`)量级 1/515 不改结论;v5 已修。

## 为什么中间两版是负的,以及 v4 凭什么赢

- v1(会话均值):抓得住"整条曲线形态改变",但 ~10 点均值把瞬时事件稀释 ~10 倍。
- v2(会话内 robust z + 点级 TopK):与标签同粒度打分反而更差——**会话内重归一化恰好
  抹掉"持续偏移"**(尾段电流减半时中位数自己跟着挪,POWER_DERATING 召回 0.7%,实锤)。
- v3(两视角 rank 融合):VALIDATION 贪心证明叠加只有稀释,最优子集退回纯 v1。
- v4(语境参照):逐点对比 (charger×SOC档) 期望曲线中位数,过热不再被"自身基线"抹掉,
  temp_excess 单信号 VAL F1 0.292 超过 v1-v3 全部。但其期望表看不见天气,v5 补上。

## 协议(全链共用)

- 时间三段与负荷线同边界:05-01 / 05-15 / 05-30;会话按 started_at 整段落侧,
  满足"同一会话不拆两边"。
- 插补中位数、StandardScaler、IsolationForest(300 树,seed 42)、z 常数、阈值分位:只在
  TRAIN 拟合;候选/子集/阈值只在 VALIDATION 选;TEST 每 model_id 只批一次。
- 规则基线(TRAIN p99 温度/压差)验证期一次未触发的反面教材见 v3 记录。

## 复现

```bash
python -m data_analysis.ml.anomaly.detect        # v1
python -m data_analysis.ml.anomaly.detect_v2     # v2(点级+TopK 候选池)
python -m data_analysis.ml.anomaly.detect_v3     # v3(rank 融合)
python -m data_analysis.ml.anomaly.detect_v4     # v4(语境基线,transductive)
python -m data_analysis.ml.anomaly.detect_v5     # v5(气象上下文+固定阈值,最终)
python -m unittest data_analysis.tests.test_ml_anomaly_churn
python -m unittest data_analysis.tests.test_ml_anomaly_v5
```

产物:`outputs/ml_anomaly/`(各 bundle、train_metrics、五份冻结 test_metrics、
session/point/context 特征缓存与 context_meta_v5.pkl 选型证据)。
合成数据,表述上限"模拟数据测试结果"。
