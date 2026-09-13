# 充电站小时级负荷预测 — 最终评测报告(EVALUATION)

> 本报告为团队答辩材料。所有数字均逐字摘自实际命令输出(prep/train/race/evaluate 的 JSON 与 stdout),无修饰、无外推。
> TEST 保留集仅由 `python -m data_analysis.ml.load.evaluate` 评分一次(2026-09-13),输出存于
> `data_analysis/outputs/ml_load/test_metrics.json`;模型选择全程只发生在 VALIDATION 上。
> 数据为分析链路模拟生成的数据,以下指标均为**模拟数据测试结果**。

---

## 1. 数据来源与批次标识

| 项 | 值 |
|---|---|
| datasetId | `charging_full_180d_v2` |
| publishedBatchId | `analytics-5f8e93429948404099b735999bfb6c4e` |
| pipelineRunId | `spark-625439908b6d4bcca1ae72e2e35ef27f` |
| sourceManifestSha256 | `1f03e37c644ec928f8de0511b614b316bcc35632056891173d2377b2733ccb95` |
| 数据集目录 | `data_analysis/datasets/analytics_full_180d_v1`(manifest 记 datasetId 见上) |
| 拼接后样本 | 107,425 行(features ⋈ targets 1:1),usableRows=107,425,rowsWithMissingLags=0 |
| 切分窗口 | start 2025-12-02 / trainEnd 2026-03-30 / validationEnd 2026-04-29 / end 2026-05-29(manifest mlSplits) |
| 模型工件 | `hgb-deep-history24-v1.joblib`,modelId=`hgb-deep-history24-v1`,version=`0.2.0`,featureVersion=`history24-v1` |
| 工件摘要 | payload sha256 `6ad8d6c9cd5b88c05d3f403f42b5574e0769bd1a68e0d6cff15ba2fb896a29e5`(可复算);整文件 sha256 `904b57915da0c9991450a80573e644075e33255f8ae0c9631da9f2d87232c1d9` |
| 环境 | Python 3.13.9 / scikit-learn 1.7.2 / pandas 2.3.3 / numpy 2.3.5 / joblib 1.5.2 |

切分行数(splitCounts,三列切分各自独立):

| 列 | TRAIN | VALIDATION | TEST | EXCLUDED |
|---|---|---|---|---|
| split_1h | 70,800 | 18,000 | 18,000 | 625 |
| split_6h | 70,675 | 17,875 | 17,875 | 1,000 |
| split_24h | 70,225 | 17,425 | 17,425 | 2,350 |

## 2. 任务与契约

- 任务:站-小时级负荷预测,预测未来第 1/6/24 小时的平均功率 `label_power_kw_h01/h06/h24`(单位 kW),服务批次在参考时刻 hh:55 触发。
- 输入:契约特征表 `ml_features_hourly` 导出的 39 个模型输入列(city_id/station_id 为 category 型 + 5 日历标志 + capacity/rated_capacity_kw/last_available_count + 24 个滞后功率 + 5 个滚动统计),特征窗严格为 `[ref-24h, ref)` 的 24 个完整小时;不使用任何 label_*/异常/清洗标记。
- 训练纪律:仅在 `split_24h==TRAIN`(标签对 24h 完整)上拟合;一切选择在 `split_24h==VALIDATION` 且标签非缺失的行上进行;预测值按行裁剪到 `[0, rated_capacity_kw]`。
- 每个契约时延在**自己的切分列**下考核:h01→split_1h、h06→split_6h、h24→split_24h。
- 契约校验:元数据符合 `data_analysis/contracts/model_metadata.schema.json`,预测结果通过 `prediction_result.schema.json`;离线/在线特征重建 parity 由 prepare_data 与 predict smoke 审计。

## 3. 模型竞赛表(VALIDATION,n=17,425/时延;MAE 单位 kW)

10 个候选脚本、统一纪律(只 fit TRAIN、只看 VALIDATION)。`mean` = (h01+h06+h24) MAE 均值。

| 方案 | 方法 | h01 | h06 | h24 | mean |
|---|---|---|---|---|---|
| ridge-eng | Ridge+工程特征 | 13.527 | 15.522 | 15.543 | 14.864 |
| randomforest | RandomForest | 12.957 | 14.200 | 14.368 | 13.842 |
| extratrees | ExtraTrees | 12.794 | 13.924 | 14.267 | 13.661 |
| hgb-strongreg | HistGBR 强正则 | 12.539 | 13.687 | 13.827 | 13.351 |
| hgb-current | HistGBR(现契约参数) | 12.544 | 13.679 | 13.827 | 13.350 |
| hgb-rich | HistGBR 富参数 | 12.526 | 13.656 | 13.803 | 13.328 |
| **hgb-deep(胜出)** | **HistGBR 深装配 600×0.03×63叶** | **12.490** | **13.636** | **13.756** | **13.294** |
| lstm-tiny(最好一版 v2) | Torch GRU(CPU) | 13.582 | 19.834 | 18.537 | 17.318 |
| persistence 基线 | lag_h01 | 16.639 | 27.298 | 18.102 | 20.680 |
| last-week 基线 | 上周同时段 | 17.848 | 17.834 | 17.829 | 17.837 |

消融诊断(不计入交付候选):`hgb-recentlags` 仅用 18 个近期特征(去掉 h07–h24 滞后与 24h 滚动量)得 mean=13.153(12.396/13.407/13.655),说明远滞后贡献很小;但其特征子集与交付 schema 的 39 列 `featureColumns` 不一致,不能作为交付工件,故胜出在全部满足契约特征集的候选中按 mean 判定,为 hgb-deep。

**胜出配方**(已固化为 train.py):3 个独立 `HistGradientBoostingRegressor`(h01/h06/h24),`max_iter=600, learning_rate=0.03, max_leaf_nodes=63, min_samples_leaf=20, l2_regularization=0.5, early_stopping=False, random_state=42, categorical_features='from_dtype'`,fit 于 split_24h=TRAIN 且标签有限的行,预测裁剪 [0, rated_capacity_kw]。train.py 全程 128.4 秒。

**LSTM 如实落败与 6 版迭代说明**:Torch CPU GRU(hidden 96)共迭代 6 版,v1(线性头)meanValMae=17.673 → v2(目标小时编码头)17.318 → v3(双层GRU+逐步历法)17.376 → v4(气候学基+零初始化增量头)17.604 → v5 因 target-hour 编码 off-by-one 直接废弃(未出分) → v6(修复 off-by-one + 两阶段训练,456.3 秒)17.579。最好一版(v2)仍比 hgb-deep 差 4.02 kW(mean 17.318 vs 13.294),主要输在 h06/h24(19.834/18.537 vs 13.636/13.756);对照实验显示"站点+小时+星期"气候学基线 mean 已达 13.48,小 GRU 学到的增量修正无法超过表格数据的先验,故深度学习方案败给梯度提升树,如实报告、不用于交付。

## 4. 最终 TEST 结果(一次性保留集评分)

命令 `python -m data_analysis.ml.load.evaluate` 实际输出(test_metrics.json 同值)。提升% =(基线MAE−模型MAE)/基线MAE。

| 时延(切分列) | n | 模型 MAE | 模型 RMSE | persistence MAE/RMSE | last-week MAE/RMSE(覆盖率) | vs persistence | vs last-week |
|---|---|---|---|---|---|---|---|
| h01(split_1h) | 18,000 | **12.297** | **18.047** | 16.34 / 25.258 | 18.095 / 27.537(1.0) | **+24.7%** | +32.0% |
| h06(split_6h) | 17,875 | **13.385** | **19.115** | 26.597 / 36.321 | 25.974 / 35.914(1.0) | **+49.7%** | +48.5% |
| h24(split_24h) | 17,425 | **13.67** | **19.562** | 17.518 / 26.763 | 18.972 / 28.613(1.0) | **+22.0%** | +27.9% |

三时延 MAE 均值 13.117 kW;预测值范围合法率(裁剪后∈[0,额定容量])= 1.0。全 24 个时延的 TEST 明细见 `test_metrics.json`(h02–h05、h07–h23 借用 split_24h=TEST,`splitColumn` 字段已逐条注明)。

与 VALIDATION 对照:h01 12.490→12.297、h06 13.636→13.385、h24 13.756→13.67,TEST 不劣于验证,无过拟合式塌方。

## 5. 按城市误差(TEST,契约三时延,MAE/RMSE kW)

| city_id | h01 MAE/RMSE (n=3600) | h06 MAE/RMSE (n=3575) | h24 MAE/RMSE (n=3485) |
|---|---|---|---|
| BJ | 12.792 / 18.806 | 13.745 / 19.644 | 13.965 / 20.094 |
| DL | 12.161 / 17.74 | 13.24 / 18.748 | 13.556 / 19.251 |
| SH | 12.266 / 17.918 | 13.383 / 19.092 | 13.624 / 19.434 |
| SY | 12.131 / 17.683 | 13.206 / 18.809 | 13.525 / 19.202 |
| SZ | 12.135 / 18.063 | 13.348 / 19.267 | 13.678 / 19.814 |

各城市误差水平接近;BJ 系统性略高(约 +0.5~0.6 kW)。

## 6. 四路审计结论(各一句)

- **泄漏审计**:特征窗严格为 `[ref-24h, ref)` 的完整小时,lag/滚动特征与原始小时表逐列对账 worstAbsDiff=0.0、missingRawHistoryHours=0,无任何 label/异常/未来信息入模,指标无"近零泄漏"特征 — 结论"无泄漏"经受住反证尝试。
- **切分审计**:三种切分列均严格 TRAIN<VALIDATION<TEST 零重叠,行级嵌套成立(任何 TRAIN/VALIDATION 行在所有切分列下同为 TRAIN/VALIDATION),全部拟合与选择从未触碰 TEST 行,TEST 仅由 evaluate.py 本次评分一次。
- **契约审计**:元数据 18 个必填字段与 `model_metadata.schema.json` 完全一致(MISSING=[] EXTRA=[]、jsonschema VALID,sidecar 与工件内双份),预测路径通过契约校验,离线/在线 parity 最大差 0.00e+00。
- **可复现审计**:payload sha256 `6ad8…29e5` 可从交付 joblib 跨多次加载稳定复算,固定 random_state=42 重训复现同一验证指标,训练/依赖版本全量记录在案。

(修复阶段:blocker-1 元数据 schema 已修复并复核;suggested minor-1 已在 prepare_data 中加入 `audit_raw_alignment` 原始小时对账,其结果即上行泄漏审计的 0.0 对账数字。)

## 7. 复现命令(repo 根目录;Windows conda 环境,脚本内已处理 KMP_DUPLICATE_LIB_OK)

```bash
python -m data_analysis.ml.load.prepare_data   # 重建 joined_usable.pkl + 离线重建/原始小时对账审计
python -m data_analysis.ml.load.train          # 重训 hgb-deep(仅 TRAIN 拟合,VALIDATION 选择)
python -m data_analysis.ml.load.predict        # 冒烟:契约 schema 校验 + 离线/在线 parity
python -m data_analysis.ml.load.evaluate       # TEST 一次性保留集报告 -> test_metrics.json
```

依赖:交付链路仅需 scikit-learn 1.7.2 / pandas 2.3.3 / numpy 2.3.5 / joblib 1.5.2(Python 3.13.9);LSTM 竞赛脚本额外用 torch 2.11.0+cpu(不属交付)。仓库侧 `data_analysis/requirements-api.txt`、`requirements-contract-test.txt`、`requirements-spark.txt` 覆盖服务/契约/数据生成链路。lightgbm/xgboost 未安装、未使用。

## 8. 局限性

1. **模拟数据**:上游数据集由分析链路模拟生成,本报告全部 TEST 数字是"模拟数据测试结果",不代表真实电网/真实运营负荷水平。
2. **合成规律不外推**:模型吃到的城市/站点画像、节假日标志、时段形态均为合成规律;迁移到真实城市需重训与重评,当前绝对误差(kW)无现实标尺意义,只有方法学与相对提升可借鉴。
3. **覆盖度/EXCLUDED**:EXCLUDED 行(625/1,000/2,350)位于切分边界附近、标签窗不完整,未参与训练/选择/评分;h24 的 TEST 行仅 17,425(<18,000),各城市 n 亦相应缩水(3600/3575/3485),对比时以 n 为准。
4. **strongreg 验证掩码小瑕疵(竞赛阶段)**:hgb-strongreg 的 run.py 把三个时延统一用 `split_24h==VALIDATION 且标签有限`(n=17,425)打分,未按各自契约切分列(h01 应为 split_1h 的 18,000、h06 为 split_6h 的 17,875);因行级嵌套,所用行在契约列下同样全是 VALIDATION,不涉 TEST/EXCLUDED,属"评估样本集略窄"而非泄漏,不影响名次(强正则本就不是最优)与最终交付(train.py/evaluate.py 均按契约列执行);报告中其数字与此口径一致。
5. **hgb-recentlags 更低分但未交付**:消融 mean=13.153 表明远滞后基本可弃,但该配方与 39 列交付 schema 不符,不作为 Ship 候选;这同时提示现交付模型仍有特征精简空间(未做,属后续工作)。
6. TEST 仅评分一次是纪律而非统计:单次保留集结果存在抽样波动,13.1 kW 量级均值在时间外推下应视为乐观-中性区间。

---

## 9. 第二轮升级(特征精简 / 分布指标 / 场景拆解 / 区间预测 / 曲线演示)

> 本轮新增工件均在 `outputs/ml_load/race2/` 与 `outputs/ml_load/analysis/`;下表每个数字都可在对应
> JSON 工件中原样复算(命令见 9.7)。交付点模型不变:**v0.2 `hgb-deep-history24-v1` 仍为 SHIP**。

### 9.1 TEST 富化指标(冻结 v0.2 单次评分的派生指标,`test_metrics.json.contractHorizons`)

A1 仅对 2026-09-13 已冻结的一次性 TEST 评分补算派生指标(WAPE=Σ|误差|/Σ|y|,P90/P95=绝对误差分位数);
结构性 diff 证明富化前后 0 个既有字段被改动、仅新增 wape/smape/p90abs/p95abs(384 键)。MAE/RMSE/n 逐字不变。

| 时延(切分列,n) | 方案 | MAE | WAPE | P90abs | P95abs |
|---|---|---|---|---|---|
| h01(split_1h,18000) | 模型 gbdt | **12.297** | **0.4683** | **31.447** | **39.494** |
| | persistence | 16.34 | 0.6223 | 44.555 | 56.549 |
| | last-week | 18.095 | 0.6891 | 48.598 | 60.975 |
| h06(split_6h,17875) | 模型 gbdt | **13.385** | **0.5082** | **33.336** | **41.488** |
| | persistence | 26.597 | 1.0099 | 62.637 | 75.379 |
| | last-week | 25.974 | 0.9862 | 62.494 | 74.74 |
| h24(split_24h,17425) | 模型 gbdt | **13.67** | **0.5227** | **34.218** | **42.505** |
| | persistence | 17.518 | 0.6698 | 47.28 | 59.859 |
| | last-week | 18.972 | 0.7254 | 50.306 | 63.586 |

sMAPE 口径说明(如实报告):模型 sMAPE h01/h06/h24 = 0.9999/0.9982/1.0046,**劣于**基线
(0.6725/0.7789、1.1499/1.1051、0.7566/0.8119)。原因是近零负荷小时占样本相当比例,`2|e|/(|y|+|ŷ|)`
在 y≈0、ŷ>0 处被钉在 ~2 附近,属指标分母伪影而非模型方向性错误(MAE/WAPE/RMSE/分位数全部占优);
评审主指标维持 MAE/WAPE,P90/P95 用于尾部风险沟通。

### 9.2 场景拆解(冻结 v0.2,TEST/VALIDATION 描述性切片,`analysis/scenario_analysis.json`)

整体与冻结评分逐字一致(TEST 12.297/13.385/13.67,n=18000/17875/17425,mean 13.1173;
VALIDATION 12.487/13.685/13.756,mean 13.3093)。TEST 关键切片 MAE(kW):

| 切片 | h01 | h06 | h24 | 结论 |
|---|---|---|---|---|
| 时段 daytime_10_16 | 16.503 | 17.966 | 18.715 | **全时延最差**,高波动平顶段 |
| 时段 evening_peak_17_21 | 15.881 | 17.03 | 17.557 | 次差 |
| 时段 late_night_0_6 | 6.723 | 7.463 | 7.371 | 最好,与最差差 ≈2.5 倍 |
| 时段 morning_peak_7_9 / night_22_23 | 10.784 / 10.393 | 11.644 / 11.077 | 11.981 / 10.979 | 中间 |
| 大容量站(rated 187kW)/ 小站(74kW) | 13.503 / 7.475 | 14.536 / 8.777 | 14.91 / 8.709 | 大站误差 ≈1.8 倍(绝对 kW;fleet 只有两档额定) |
| 节假日 / 非节假日 | 10.565 / 12.643 | 11.519 / 13.761 | 12.994 / 13.81 | **节假日反而更容易**,合成日历无"节假日难预测"效应 |
| 调休工作日 / 非调休日 | 12.8 / 11.428 | 13.882 / 12.535 | 13.874 / 13.334 | 工作日形态最难 |
| 周末 / 非周末 | 11.671 | 12.868 | 13.231 | 周末略易 |
| 城市(TEST 最差 BJ / 最好 SY) | 12.792 / 12.131 | 13.745 / 13.206 | 13.965 / 13.525 | 城市间 spread 小;VALIDATION 最差为 SZ(h24 14.214) |

P95abs 各切片约 32–46 kW,排序与 MAE 一致;全部切片 n≥1500,无 low_n 标记。运营含义:误差集中在
日间高波动时段与大容量站,尾部告警(如容量裕度提示)应按"时段×容量档"分层设置。

### 9.3 特征重要性(VALIDATION permutation,`analysis/feature_importance.json`)

基线 VALIDATION MAE(未裁剪)h01 12.4933 / h06 13.6417 / h24 13.7618。Top-6(归一化平均份额;
打乱该列后 MAE 最大增幅 kW,h01/h06/h24):

| 特征 | 份额 | MAE 增幅 kW |
|---|---|---|
| station_id | 0.27310 | +2.97 / +3.52 / +4.60 |
| hour_of_day | 0.20142 | +1.78 / +3.26 / +3.08 |
| rolling_mean_kw_3h | 0.09777 | +0.88 / +0.18 / +3.13 |
| last_available_count | 0.09421 | +3.54 / +0.03 / +0.11 |
| day_of_week | 0.03698 | +0.26 / +0.60 / +0.64 |
| lag_power_kw_h24 | 0.03411 | +0.62 / +0.03 / +0.76 |

零/近零贡献(份额,三时延全为 0 或 ≈0):is_weekend 0.00000、is_adjusted_workday 0.00000、
capacity 0.00000、city_id 0.00029、rated_capacity_kw 0.00063;滞后 h02–h16 各 ≤0.0048。
结构:station_id+hour_of_day 合占 0.4745;24 个滞后合计 0.1508;远滞后(≥h07)基本可弃 —— 与消融
诊断一致,直接引出 9.4 的精简实验。

### 9.4 精简候选 `hgb-lean-v1-candidate`(19 特征)与晋级决定

`train_lean.py` 一次性 sweep(全 39 特征对照 + L1/L2/L3 三个剪枝子集 × 24 次按胜出配方拟合,
521.5 秒,只用 VALIDATION 选择,TEST 未读;`race2/train_lean_results.json`):

| 方案(特征数) | h01 | h06 | h24 | mean(契约切分列) |
|---|---|---|---|---|
| 对照 full39 | 12.4874 | 13.6850 | 13.7559 | 13.3094 |
| **L3(19)= 胜出** | **12.3964** | **13.4680** | **13.6738** | **13.1794** |
| L2(19) | 12.5107 | 13.6173 | 13.6842 | 13.2707 |
| L1(24) | 12.5099 | 13.6909 | 13.7139 | 13.3049 |

L3 = 日历 5 + city/station + capacity/rated/last_available_count + 滞后 h01–h06 + 滚动 mean/std/max 24h;
丢弃 lags h07–h24 与 rolling 3h/6h 共 20 列。契约口径 −0.1300 kW vs 对照;split_24h 口径同样胜出
(13.1645 vs 对照 13.2938 = 交付基线逐字复现),选择对口径稳健。门槛 valMean≤13.2438 **通过**,
`improved=true`;离/在线 parity 通过(5 窗,最大数值差 7.11e-15 kW)。

**晋级决定:不替换交付点模型,v0.2 保持 SHIP;L3 工件记为诊断/候选。**理由(审计 V2-P4,经执行证实):
该 bundle 的 featureVersion=`history24-lean-v1` 与契约 `contracts/__init__.py` 的
`FEATURE_VERSION="history24-v1"` 冲突,`LoadForecastPredictor.predict` 实际抛
`ValueError: Feature contract mismatch`,且元数据有意不满足 `model_metadata.schema.json`
(const 钉死);当前轮无权改契约文件。它是**未解除的交付阻塞项**,故按规则(改进且无未解除阻塞才可
SHIP)留在候选位;后续做特征版本契约发布(schema+常量+回归测试一次改齐)后,13.1794 的配方即可直接切换。
因未晋级,本轮**没有**对该候选做任何 TEST 评分(工件内亦不存在任何 TEST 打分记录,审计确认)。

### 9.5 区间预测工件 `hgb-quantile-history24-v1`(晋级为附加分析工件)

9 个分位模型(q0.1/q0.5/q0.9 × h01/h06/h24,胜出配方,TRAIN 拟合,VALIDATION 决策;263 秒)。
不替代点模型,作为风险带宽的**分析工件**晋级交付。协议:裁剪 [0,额定] 后逐样本排序保证 lower≤median≤upper。

VALIDATION(`race2/interval_metrics.json`,n=17425/时延):

| 时延 | cov80(目标 0.80) | 平均宽度 kW | pinball q0.1/q0.5/q0.9 | median MAE |
|---|---|---|---|---|
| h01 | 0.8923 | 46.618 | 2.7389 / 6.1015 / 3.1519 | 12.2030 |
| h06 | 0.8851 | 48.262 | 2.7374 / 6.6354 / 3.4148 | 13.2709 |
| h24 | 0.8849 | 48.427 | 2.7358 / 6.6760 / 3.4578 | 13.3520 |
| 汇总 | 0.8874 | 47.769 | 2.7374 / 6.4710 / 3.3415 | 12.9420 |

TEST 一次性**确认性(非盲)**评分(`race2/interval_test_summary.json`,由
`interval_test_confirm.py` 产出,契约切分列;裁剪+排序协议同上):

| 时延(n) | cov80 | 平均宽度 kW | pinball q0.1/q0.5/q0.9 | median MAE |
|---|---|---|---|---|
| h01(18000) | 0.8977 | 45.501 | 2.6259 / 5.9886 / 3.1489 | 11.9773 |
| h06(17875) | 0.8948 | 47.685 | 2.6337 / 6.4875 / 3.3689 | 12.9750 |
| h24(17425) | 0.8954 | 47.821 | 2.6152 / 6.5902 / 3.4055 | 13.1805 |
| 汇总 | 0.8960 | 47.002 | 2.6250 / 6.3554 / 3.3078 | 12.7109 |

结论与如实说明:①区间系统性**过覆盖**(~89% vs 名义 80%,TEST 与 VALIDATION 一致稳定),宽度
45–48 kW 相对小站额定(74 kW)偏宽,可直接用于告警但精细调度需更高分辨率分位;②分位穿越
(独立三模型交叉)按样本排序修复,h01 修复率 ~6.2–6.5%;③q0.5 中位数的 MAE(VAL 12.9420 /
TEST 确认 12.7109)低于 L2 损失点模型(VAL 13.2938 / TEST 13.1173),但那是**不同训练目标**的产物,
且本轮晋级决策(点模型仍为 v0.2)在其 VALIDATION 报告之后未改变,该观察仅作为下一轮点模型候选
线索记录,未据此改判。

### 9.6 曲线演示

`python -m data_analysis.ml.load.curve_demo` →
`data_analysis/outputs/ml_load/analysis/curve_demo.png`(明细数值 `curve_demo_summary.json`)。
固定 random_state=20260913 从 **VALIDATION** 抽 1 个站-小时窗(ST-SH-03,额定 74 kW,参考时刻
2026-04-23T11:00:00Z):蓝线为 +1…+24h 实际负荷曲线,橙点为区间模型中位数 ± 80% 区间(模型仅有
+1/+6/+24h 三个输出时距,虚线为视觉连接),同时打印 v0.2 点模型三时距预测(38.90 / 25.62 / 32.94 kW)。
该样例是**高方差站点**的真实写照:+6h 中位数 28.05 对实际 3.85 偏大,但 80% 区间 [0.0, 46.6] 仍覆盖,
展示"点估计会错、带宽知道自己错"的演示意图。

### 9.7 新增复现命令(repo 根目录;脚本内已处理 KMP_DUPLICATE_LIB_OK)

```bash
python -m data_analysis.ml.load.feature_importance     # -> analysis/feature_importance.json(VALIDATION permutation)
python -m data_analysis.ml.load.scenario_analysis      # -> analysis/scenario_analysis.json(冻结评分的描述性切片)
python -m data_analysis.ml.load.train_lean             # -> race2/hgb-lean-v1-candidate.* + train_lean_results.json
python -m data_analysis.ml.load.intervals              # -> race2/hgb-quantile-history24-v1.joblib + interval_metrics.json
python -m data_analysis.ml.load.interval_test_confirm  # TEST 确认性(非盲)单次 -> race2/interval_test_summary.json
python -m data_analysis.ml.load.curve_demo             # -> analysis/curve_demo.png + curve_demo_summary.json
```

### 9.8 TEST 评分披露(重要)

- **首盲评分唯一**:v0.2 `hgb-deep-history24-v1` 于 2026-09-13 由 `evaluate.py` 做了一次、也是唯一一次
  首盲 TEST 评分(`test_metrics.json`);其派生指标富化不改变任何既有数值。本轮全部拟合/选择在冻结
  该评分之后发生,未回看 TEST。
- **确认性(非盲)**:9.5 的 TEST 区间表是最终定级代理对**已晋级分析工件**的一次性确认评分,在其
  VALIDATION 晋级决定**之后**执行,只披露、不参与任何选择;工件内 `gradingStatus` 字段已注明。
- 精简候选 L3 **未晋级,故未做 TEST 评分**。除上述两处外,任何工件未再读取 TEST。

### 9.9 局限性补充(接第 8 节)

7. **模拟数据同样约束本轮工件**:区间覆盖率、sMAPE 伪影、场景切片形态均为合成规律产物;"节假日更
   易""大站误差 ~1.8 倍"等结论迁移真实电网前需复验。
8. **区间宽度与过覆盖**:80% 名义带宽 45–48 kW(站额定 74–187 kW),对 74 kW 小站几乎覆盖大半量程,
   作告警带尚可、作精细功率预案偏粗;q0.5 中位数 MAE 占优是训练目标差异,非免费收益,未据此改判交付。
9. **L3 精简配方被契约版本钉住**:验证增益(−0.130 kW mean)真实且口径稳健,但受
   `FEATURE_VERSION`/schema const 阻塞,当前只能以诊断工件形式交付;解锁需一次契约特征版本发布。
10. **演示样例非"最典型"窗**:随机种子一次抽中高方差样本(区间下界裁剪为 0 即其表现),数字虽如实,
    解释曲线形态时不宜以其单点外推全局。

## 10. v0.4 转正:分位数中位数目标成为交付模型(负责人裁决)

§9 的裁决是"v0.2 保持交付、q0.5 仅作分析工件"。此后负责人(B 方案)裁决转正,依据如下,全程可审计:

- **转正资格**:q0.5 模型使用完整 39 列契约特征、`featureVersion=history24-v1` 不变——不触发 §9.8 的 L3 契约阻塞;晋升依据只有 VALIDATION(12.942 vs 13.294,−0.352 kW)与"分位数损失直接优化交付指标 MAE"的方法论论证,**TEST 数字未参与该决定**。
- **重训**:train.py 配方改为 `loss="quantile", quantile=0.5`(其余超参不变),全 24 视野在 `split_24h==TRAIN` 重拟合(364.9 秒);VALIDATION 池化 MAE **12.942**,与 §9.7 三时延实验值一致(逐位复现,证明非抽样运气)。元数据经 `model_metadata.schema.json` 校验,工件 `hgb-q50-history24-v1.joblib`(v0.4.0)。
- **冒烟**:`python -m data_analysis.ml.load.predict` 在 3 个 VALIDATION 窗上契约通过、离线/在线 parity 最大差 0.00e+00。
- **确认性 TEST 评分(仅一次,非盲测)**:`test_metrics_hgb-q50-history24-v1.json`,模拟数据测试结果:

| 时延(切分列) | n | MAE | RMSE | WAPE | P90/P95 (kW) | 合法率 | vs persistence | vs last-week |
|---|---|---|---|---|---|---|---|---|
| h01(split_1h) | 18,000 | **11.977** | 18.531 | 0.4561 | 32.49 / 40.57 | 1.0 | **+26.7%** | +33.8% |
| h06(split_6h) | 17,875 | **12.975** | 19.469 | 0.4927 | 34.16 / 42.80 | 1.0 | **+51.2%** | +50.0% |
| h24(split_24h) | 17,425 | **13.181** | 19.848 | 0.5040 | 34.83 / 43.59 | 1.0 | **+24.8%** | +30.5% |

三时延 MAE 均值 **12.711 kW**(v0.2 为 13.117)。全 24 视野 MAE 11.98–13.28,依旧近乎平坦。

- **诚实的代价**:中位数目标以 MAE 换 RMSE——h01/h06/h24 RMSE 18.531/19.469/19.848,比 v0.2(18.047/19.115/19.562)高约 0.3–0.4 kW,尾部大误差略微变胖。契约与大屏展示以 MAE/WAPE 为主口径,该权衡被接受并记录在案;若未来业务口径转向 RMSE(如容量裕度平方罚),应回退或双模型并行。
- **盲测披露(最终版)**:**首次盲测荣誉归 v0.2**(§4,mean 13.117);v0.4 的 TEST 是在已知 v0.2 TEST 结果、且其区间工件已做过一次 TEST 确认评分之后进行的,**属确认性、非盲测评分**,只批了一次。此后 ml/load 不再对 TEST 做任何评分。
- **§9.7 相应修正**:"未据此改判交付"自本节起失效,保留原文以忠实记录决策时序。分位数区间工件(q10/q90)继续作为附加分析工件与 v0.4 点预测配套使用。

复现:`python -m data_analysis.ml.load.train && python -m data_analysis.ml.load.predict`(种子 42,重跑逐位一致;evaluate 仅在明确需要时再跑,输出文件名已按模型隔离,不会覆盖任何历史评分工件)。
