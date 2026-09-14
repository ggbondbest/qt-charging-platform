# `ml/attempt/` · 插枪启动失败预测（第六线）

用户选定桩、把枪插进去的那一刻，App 该不该先说"这根桩最近启动不太稳，建议换隔壁"。
本线把这句话变成一个可复核的模型：**这次插枪启动会不会技术性失败**（二分类风险分）。

第二阶段"自己找新方向、自己找数据"的产物。前五线（`availability` / `load` / `recommend` /
`anomaly`+`churn` / `queue`）都没有把"启动这一步会不会失败"当标签：

| 线 | 决策时点 | 标签 |
| --- | --- | --- |
| `queue`（第五线） | 点"加入排队" | 这次排队会不会白排 |
| **本线（第六线）** | **插枪启动** | **这次启动会不会技术性失败** |

两条线**样本与标签互不相交**，而且是实测核对过的数据事实，不是口头声明：`charging_attempts` 里带
`queue_id` 的 36,594 行，其 `failure_reason` 只有排队侧三类（`QUEUE_PATIENCE`/`CALL_TIMEOUT`/
`NO_AVAILABLE_CHARGER`），**技术失败一次都没出现（技术失败率恰好 0.0）**；反过来本线的正类
（`CONNECTOR_HANDSHAKE`/`APP_TIMEOUT`/`AUTH_FAILED`）只出现在无排队关联的行上。这条断言写进
`build_data.load_attempts`，换批次后前提变了会直接构建失败。

## 数据与标签

| 项 | 口径 |
| --- | --- |
| 数据层 | `datasets/analytics_full_180d_v1/clean/`（只读），批次 `analytics-298aa3ee1401461fb06ea2bb96930dcf` · run `spark-6ed381b125034f9e95d726a74befb121` |
| 样本单位 | `charging_attempts` 一行 = 一次**已经选定桩**的启动尝试；决策时点 `attempted_at` |
| 样本量 | **102,801** 行 / 正类 **3,676**（基础率 **0.03576**）；覆盖 75 台桩、25 个站、5,986 个用户 |
| 标签 | `y_tech = failure_reason ∈ {CONNECTOR_HANDSHAKE, APP_TIMEOUT, AUTH_FAILED}`（1,248 / 1,178 / 1,250） |
| 切分 | 按 `serving_manifest.mlSplits` 换到 UTC（北京日历日 −8h）：TRAIN 66,816 / VALIDATION 17,658 / TEST 17,084，窗口外 1,243 行 EXCLUDED；三段基础率 0.03496 / 0.03834 / 0.03682 |
| 特征 | **70 数值 + 10 类别 = 80 列**候选；4 列因样本域内零方差在建表阶段剔除并逐个留痕 |
| 新增数据 | **无**。全部特征来自既有 clean 表（`charging_attempts` / `chargers` / `stations` / `vehicles` / `users` / `reservations` / `calendar` / `tariffs` / `weather_hourly` / `maintenance_tickets` / `charger_telemetry` 388.8 万行），不生成、不修改、不删除任何数据集文件 |

被排除的四块（`build_summary.scope.excluded`，合计 66,353 行，与 `totalAttempts 169,154 − sampleRows
102,801` 严格对账）：

| 排除项 | 行数 | 理由 |
| --- | --- | --- |
| `queueLinked_line5Sample` | 36,594 | 那是第五线的样本与标签，本线整块不碰 |
| `noAvailableCharger` | 24,380 | 压根没选到桩，不存在"插枪启动"这一步 |
| `reservationCancelledOrExpired` | 3,215 | 预约侧取消/过期，机制不同 |
| `failedWithoutCharger` | 2,164 | App 侧还没选桩就超时/鉴权失败，桩侧特征全不可得；三类原因占比（762/724/678）与在桩上的失败几乎同比例，硬塞进来只会让"缺 charger_id"这个掩码变成假特征 |

产物一律落 `data_analysis/outputs/ml_attempt/`（该目录 gitignore，不入仓），全部
`O_CREAT|O_EXCL` 独占创建；入口的 `require_empty_run_dir` 会拒绝往已发布目录里再写一遍。

## 泄漏纪律（本线的主要工程量）

1. **遥测同刻陷阱**（这条线最容易死在这儿）。模拟器在 `attempted_at` 那一刻把桩状态翻成 `CHARGING`。
   若按 `recorded_at <= attempted_at` 关联"最后一条遥测"，`FAILED` 行的上一条状态全是 `AVAILABLE`
   ——正类 **3,676/3,676 命中**，看着像神模型，其实是答案抄进特征。本线一律**严格早于**（同刻视作还
   不知道），实现收在 `strict_last` 一处：`merge_asof` 在这个 pandas 版本没有 `allow_equal`，所以把右表
   时间整体 `+1ns`。修完之后重测，"上一条桩态"的单变量 AUC 只有 **0.5018**——**遥测在这条线没有独立
   信息**，这一点如实写进结果，不拿它当特征卖点。
2. **比率特征的收缩目标必须是因果量**。`(失败数 + α·先验)/(尝试数 + α)` 里的先验取"截至该行时刻之前的
   全量经验失败率"（`global_causal_rate`，只看过去，无历史取 0.0，实测区间 0.0 → 0.06452），
   不是全表标签率——后者会把 TEST 的分布漏进 TRAIN 的特征。取常数 0.5 在这条线也不合适：基础率 3.6%、
   每台桩历史中位数才几百条，0.5 会把小样本桩的率推到失真。
3. **只用 `reported_at` / `restored_at`，绝不读工单 `status`**。"现在已解决"不等于"尝试当时已解决"；
   `charger_ticket_open_now` 是 `报修数(严格早于 t) − 已恢复数(恢复时刻严格早于 t)`，同刻按"还没恢复"算。
   工单成本、`accepted_at`、`work_started_at` 一并宣布为"读过但不用"（`IGNORED_COLUMNS`）。
4. 事后字段与身份外键双名单封死（`NON_FEATURE_COLUMNS` + `FORBIDDEN_FEATURES`，含 `outcome`/
   `failure_reason`/`tech_reason_code`/`session_id`/`queue_id`/所有原始时间戳/`city_id` 与 `home_city_id`
   ——城市是站点属性的代理，留在表里做关联键但不当特征），`pick_features` 再断言一次。
5. **两条独立路径自证**：`verify_no_future_leak` 随机抽 250 行，用全表布尔掩码（不排序、不二分、
   不 `merge_asof`、不复用 `AsOfCounts`）重算 9 个最危险的量，要求逐条 0 偏差。本次实测
   `maxAbsDiff = 0.0`。这条审计真的抓到一个 bug：`global_causal_rate` 最初把 exclusive 前缀和数组多退了
   一格（`rate[pos-1]`），249/250 行对不上、`charger_fail_rate_prior` 最大偏差 **1.86e-4**；改成
   `rate[pos]` 后重建为 0.0。测试里留了两处回归：合成小表的 `strict_last` 同刻断言，和
   `global_causal_rate` 与暴力重算的逐点比对。

## 结果（TEST 盲测，一次算完）

> 全部指标为模拟数据测试结果（第二阶段发布批次 `analytics-298aa3ee1401461fb06ea2bb96930dcf`），不代表真实运营数据表现。

n = 17,084，正类 629，TEST 基础失败率 **0.0368**。特征组 `staticOnly`（18 数值 + 10 类别 = 28 列，
按 VALIDATION AUC 选定）：

| 指标 | 模型 | TRAIN 查表基线（桩/型号/站三级平滑） | 纯 as-of 桩先验 |
| --- | --- | --- | --- |
| **ROC AUC** | **0.6985** | 0.6351（模型 **+9.98%**） | 0.6498（模型 **+7.50%**） |
| PR-AUC | **0.0686** | 0.0573 | 0.0562 |
| lift@2% | **2.38×** | 2.07× | 1.59× |
| lift@5% | **2.23×** | 2.04× | 1.81× |
| lift@10% | **2.15×** | 1.70× | 1.65× |
| Brier / LogLoss | 0.03506 / 0.14815 | 0.03518 / 0.15409 | 0.03508 / 0.15195 |

VALIDATION 上的同一组比较（选组依据）：模型 `staticOnly` **0.6951** > `full` 0.6742 > `asOfOnly`
0.6076；查表基线 0.6468、纯 as-of 桩先验 0.6595。

冻结运营阈值 **0.1305**（在 VALIDATION 上按"精确率 ≥ 0.20 取召回最大"选定，TEST 只验不改）：

| 阈值 | 告警率 | 精确率 | 召回 | F1 | 达标 |
| --- | --- | --- | --- | --- | --- |
| 0.1305 | 1.38% | **0.0975** | 0.0366 | 0.0532 | **否** |

VALIDATION 上没有任何一档能同时满足"精确率 ≥ 0.20 且告警 ≥ 30 条"，所以 `pick_threshold` **没有改口径去
凑达标**，而是退回"精确率最高档"并把这句话冻进 bundle——TEST 报告原样带着它。0.0975 是基础率 0.0368 的
**2.65 倍**，作为"提示换桩"的话术够用，作为"派工单"不够用。

**校准**（TEST 十分位）：实测失败率 0.0 → 0.0064 → 0.0199 → 0.024 → 0.031 → 0.0334 → 0.0585 →
0.0586 → 0.0597 → 0.0796，**单调不交叉**；最大偏差在最高桶（预测 0.1130 / 实测 0.0796，高估 0.0334），
即"分数偏高、排序方向正确"。所以风险分可以用来**排序**（先查最危险的那批桩），不可以直接当概率报给用户。

**消融**（同一 TEST，缩减模型只用于归因、不参与发布）：

| 特征组 | 特征数 | TEST AUC | VALIDATION AUC |
| --- | --- | --- | --- |
| 不用 as-of 历史（发布组） | 28 | **0.6985** | 0.6951 |
| 静态 + 全部 as-of 历史 | 80 | 0.6707 | 0.6742 |
| 只用 as-of 历史（含工单/遥测） | 52 | 0.6021 | 0.6076 |

VAL 上选中的组在 TEST 上也最强，方向一致——但见下面滚动研究：这个"选中"**不稳定**。

## 信号来自哪里（机制）

发布组里逐列做单变量 AUC（VALIDATION，类别列用 TRAIN 经验率编码），只有两列真的有用：

| 列 | 单变量 AUC | 该列各类失败率区间 |
| --- | --- | --- |
| `charger_model`（3 档） | **0.6503** | 0.0147 → 0.0566（**3.8 倍**） |
| `manufacturer`（3 档） | **0.6487** | 0.0155 → 0.0566 |
| `connector_type`（2 档） | 0.5598 | 0.0147 → 0.0395 |
| `weather` / `is_weekend` / `dow_local` / `charger_age_days` | 0.52–0.53 | — |
| 其余 20+ 列 | 0.47–0.51 | — |

也就是说：这批数据里"启动会不会失败"几乎完全由**插在哪种硬件上**决定（型号级失败率差 3.8 倍），
年龄、时段、天气、电价、预约、工单都近似噪声。同一口径（VALIDATION）下模型 0.6951 比单列型号编码
0.6503 还高 0.045，这部分增量来自型号×接口×时段的交互。

由此得到本线的核心判断，也是它跟第五线最不一样的地方：**桩级 as-of 历史在这条线上是负资产**。
75 台桩 × 118 天，每台只有约 49 个正样本，桩级经验率的噪声大于它的信息量；型号/厂商级聚合才是稳定来源。
所以"只用 as-of 历史"那组只有 0.6021，比查表基线还差——它把噪声全喂给了模型。

## 十轮滚动重训研究（`rolling.py`）

一次冻结的模型只回答"这一刀准不准"，回答不了"重训十次还准不准"。折设计与第五线完全一致：
warmup 58 天起，每轮 `fitEnd` 之后 12 天做评测窗，拟合窗尾 12 天选特征集与阈值；
58 + 12×10 = 178 天**正好铺满** `mlSplits` 全窗（10 轮评测合计 68,559 行 / 2,495 个正样本，
2026-01-28 → 2026-05-28）。每轮结果 `outputs/ml_attempt_rolling/rounds/round_NN.json` 用 O_EXCL 落盘，
中断可续跑；十轮约 3 分钟。口径警告：这些折**重复用到**了上面那次盲测的 TEST 窗口，所以轮次均值
**不是盲测指标**，只回答"稳不稳 / 每轮该选哪套 / 历史会不会变有用"。

| 指标 | 均值 | 标准差 | 首轮 | 末轮 | 最低 | 最高 |
| --- | --- | --- | --- | --- | --- | --- |
| AUC | **0.6842** | 0.0177 | 0.6691 | 0.7116 | 0.6636 | 0.7116 |
| 同轮查表基线 | 0.6369 | 0.0163 | 0.6113 | 0.6360 | 0.6113 | 0.6646 |
| 同轮纯 as-of 桩先验 | 0.6490 | 0.0106 | 0.6357 | 0.6485 | 0.6319 | 0.6658 |
| 相对查表基线增益 | **+7.49%** | 3.67pp | +9.44% | +11.89% | +0.31% | +11.89% |
| PR-AUC | 0.0648 | 0.0064 | 0.0638 | 0.0780 | 0.0574 | 0.0780 |
| Brier | 0.0348 | 0.0027 | 0.0359 | 0.0385 | 0.0311 | 0.0385 |
| lift@2% / @5% | 2.23× / 1.94× | 0.72 / 0.34 | 2.58 / 1.99 | 2.79 / 2.03 | 1.29 / 1.46 | 3.50 / 2.44 |
| 告警率 / 精确率 / 召回 | 3.08% / 0.0734 / 0.0625 | 2.6pp / 0.020 / 0.052 | — | — | 0.26% / 0.0392 / 0.0040 | 7.65% / 0.0964 / 0.1747 |
| 每轮重选阈值 | 0.0976 | 0.0326 | 0.0375 | 0.0938 | 0.0375 | 0.1466 |

四个结论：

1. **"打过基线"这件事是稳的**：10/10 轮 AUC 都高于同轮查表基线（符号检验 p = 2⁻¹⁰ ≈ **0.002**），
   也都高于纯 as-of 桩先验（10/10）。一次盲测的 +9.98% 落在十轮 +7.49%±3.67% 的区间内。
2. **但"staticOnly 打赢 full"是运气**：10 轮里 VAL 选 full **6** 次、staticOnly **4** 次、asOfOnly **0** 次；
   逐轮"静态组 − 全量组"的 VAL AUC 差是
   `[+0.0004, −0.0030, +0.0002, +0.0120, −0.0033, −0.0169, −0.0084, +0.0231, −0.0105, −0.0070]`，
   均值 **−0.0013**（10 轮里 6 轮是全量更强）。所以 v0.1 发布用 staticOnly 这件事，证据强度只等于
   一片 12 天的 VALIDATION——**方向上正确的结论是"as-of 桩级历史没有增量"（asOfOnly 10/10 轮垫底），
   而不是"必须把静态之外的列全砍掉"**。下一版应按整套折的配对差定特征集，不能按单轮 VAL。
3. **桩级历史不会因为数据变多而变有用（本线特有的实验）**：第 0 轮评测时每台桩的历史尝试数中位数
   583，到第 9 轮涨到 1,520（**2.6 倍**），而纯 as-of 桩先验的 AUC 只是 0.6357 → 0.6485，
   前 3 轮均值 0.6462 → 后 3 轮 0.6496（**+0.0034**，远小于轮间标准差）。这从时间维度重复了上节的
   横截面结论：瓶颈是**每台桩正样本太薄**，不是看得不够久。
4. **阈值最不稳，而且没有一轮达到过精确率目标**：十轮各自选出 0.0375–0.1466（均值 0.0976，
   一次盲测冻的 0.1305 在区间内），评测窗精确率 0.0392–0.0964、召回 0.0040–0.1747，
   **达标轮数 0/10**（目标 0.20）。这条线的运营承诺不该是"精确率≥X"，而该是"按风险分排序巡检"。

## 已知不足（如实呈报）

- **正类只有 3.68%，且信号只有"硬件身份"一根柱子**：型号 3 档、厂商 3 档，粒度极粗。
  如果真实数据里型号失败率差没有这么大，本线可学的东西会立刻塌一半。
- **一次盲测的 +9.98% 与十轮的 +7.49%±3.67% 都在同一批模拟数据上**，且 10 轮里最差一轮只比基线高
  0.31%（第 6 轮：模型 0.6667 vs 查表 0.6646）——增益方向稳定，**幅度不稳定**，不该对外承诺具体百分比。
- **"该不该再做一个原因分类器"**：报告里按预注册判据（占比与均匀最大偏差 <0.06 **且** 类内 AUC 极差
  <0.05）算出的结论是 `aucSpreadWithinPositives = 0.0572` 略过线，于是写成"原因分类器值得再评估"。
  我的解读是**这句结论不该当真**：0.0572 的极差来自 APP_TIMEOUT 单类 0.5364，而 629 个正样本下 AUC 的
  标准误约 0.03，这完全是噪声；三类原因占比 0.33/0.30/0.37、类内 AUC 0.479/0.536/0.487 都贴着 0.5。
  **v0.1 仍只发二分类风险分**，不改判据也不换结论。
- `demand_multiplier` 是 `calendar` 表里的**计划性放大系数**，仿真侧生成负荷时就是按它来的，
  真实系统里能不能拿到同等质量的输入需要与运营确认。它在本线单变量 AUC 0.4932（等于没有），
  留在特征里只是占位，v0.2 建议直接删。
- **TEST 域内 75 台桩全部在 TRAIN 出现过**（`testRowsWithUnseenCharger = 0`）。所以本模型从没被测过
  "新桩冷启动"；型号级身份能一定程度替代，但没有实测证据。
- 4 列零方差：`home_city_matches_station`（恒 1）、`reservation_lead_min`（模拟器在尝试当刻建预约，
  恒缺失）、`service_price_cents_per_kwh`（全站同一服务价）、`tel_age_min`（遥测严格 5 分钟栅格，恒 5）。
  后果与第五线相同：站点/用户侧个性化空间被数据本身封住。
- 遥测"上一条桩态"在因果口径下 AUC 0.5018，等于没有信息；但**这不等于真实系统里遥测没用**，
  只是这份仿真的桩态翻转与失败没有因果关系。换真实数据要重测这一条。
- 无对外接口：`contracts/` 归负责人，本线**不擅自加端点**，只在下面提案。

## 契约提案（只提案，不改 `contracts/`）

`GET /predict/start-failure-risk?chargerId=&attemptedAt=&userId=&vehicleId=` 返回
`{riskProbability, rankThreshold, modelVersion, publishedBatchId}`；或者更省事的离线交付：
每天跑一次打分，把风险分 Top-N 的桩并进巡检工单（`pile_concentration` 显示 TEST 里失败最集中的
10 台桩占全部正类的 **36.4%**，冻结阈值下 236 条告警分散在 **30 台**桩上、最大单桩只占告警 8.1%——
"盯少数坏桩"在这条线是有效的，但也说明阈值不该只服务那几台）。两者都需负责人点头；
SQLite 服务库里**不放**本线产物，也不伪造看板数字。

## 复现

```bash
# 环境：Python 3.13.9 / numpy 2.4.4 / pandas 2.3.3 / scikit-learn 1.7.2 / joblib 1.5.2 / pyarrow 21.0.0
python -m data_analysis.ml.attempt.build_data    # 建矩阵 + 泄漏审计（attempt_matrix.pkl / build_summary.json）
python -m data_analysis.ml.attempt.train         # 只吃 TRAIN/VALIDATION（gbdt-attempt-techfail-v1.joblib / train_metrics.json）
python -m data_analysis.ml.attempt.evaluate      # TEST 盲测一次（evaluation_report.json / .md）
python -m data_analysis.ml.attempt.rolling --rounds 10   # 十轮滚动重训（outputs/ml_attempt_rolling/，可续跑）
python -m data_analysis.ml.attempt.predict --self-check
python -m data_analysis.ml.attempt.predict --verify-report   # 新进程重算指标并与发布报告逐项比对
python -m data_analysis.ml.attempt.predict --attempt-id AT-000123
python -m data_analysis.ml.attempt.predict --dump-test-table 200
python -m unittest discover -s data_analysis/ml/attempt/tests -t .   # 58 项（无产物/无 sklearn 时自动 skip）
```

产物：`attempt_matrix.pkl`（sha256 `aa09b84208ccd500…`）、`build_summary.json`、
`gbdt-attempt-techfail-v1.joblib`（模型 + 基线表 + 阈值 + 批次/矩阵哈希）、`train_metrics.json`、
`evaluation_report.json`、`evaluation_report.md`、`test_predictions.csv`；滚动研究另落
`outputs/ml_attempt_rolling/`。模型包与矩阵按 sha256 **成对校验**，`--verify-report` 已实测：新进程重算的
行数/AUC/基础率/告警率/精确率/召回与发布报告逐项一致（最大偏差 4.24e-5，即报告四舍五入到 4 位的误差）。
