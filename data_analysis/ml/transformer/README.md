# 变压器容量与全网功率分配线（`ml/transformer/`·由第九线预留项转实做）

> **口径声明**：本线全部指标为**模拟数据**测试结果（第二阶段发布批次
> `analytics-298aa3ee1401461fb06ea2bb96930dcf`），不代表真实运营数据表现。本线**未新增任何数据**——
> 样本、标签、特征与分配需求全是对发布批次 clean 表（`charger_telemetry`、`stations`）的只读确定性
> 聚合，不落新目录、不改原始数据。

## 这条线在做什么（以及为什么形态是"工具链 + 诚实负结果"）

决策单元是**一个站 × 一个 5 分钟 tick**（发布批实测 25 站 × 51,840 tick，稠密满格）。原需求分析
把它列为"本期不做"，理由是数据层撑不住——本线用一次只读探针把那句话从**猜测**坐实成**测量**，
然后在能测的范围内交付两样真东西：

* **预测腿**：站·tick 下一 tick 总充电负荷（kW）回归。这是有**真实连续标签**、可盲评的模型。
* **分配腿**：站内容量约束下的功率分配策略（`allocate.py`，纯函数）在**真实需求流**上的反事实回放。

两腿各自钉死一个诚实负事实（见下），这也是为什么它不是一个"报喜"的模型线。

## 两个诚实负事实（发布批实测）

1. **变压器过载从不发生。** 全场站 `transformer_kw` 恒 360kW、每站 3 桩、额定池最大 187kW；
   705,924 个"有充电的站·tick"里，站级总负荷峰值 **150.85kW**，**越 360kW 的 tick = 0**。
   → "预测过载"没有正样本，不是模型不行，是这件事在这批数据里根本没发生过。过载分析的价值
   因此转到**余量测算**（预测腿）与**收紧容量的反事实**（分配腿）。余量实测（VALIDATION 215,975
   tick，模型预测口径）：预测越 360kW 的 tick = **0**，预测余量中位数 **350.2kW**、最差 1% 分位
   仍有 **226.1kW**。

2. **负荷模型赢在整体、输在尖峰。** TEST 盲测（模型只在 TEST 打分一次，选型全在 VALIDATION）：

   | 打分（TEST，215,975 tick） | MAE | RMSE | 最高10% tick MAE | 偏差 |
   | --- | ---: | ---: | ---: | ---: |
   | model (big31) | 9.61 | **18.88** | 28.48 | 0.23 |
   | persistence（当 tick 实测） | **8.40** | 21.48 | **26.49** | 0.00 |
   | climatology（站×小时查表） | 21.86 | 31.84 | 65.87 | 0.99 |
   | globalMean（TRAIN 常数） | 31.89 | 38.57 | 88.48 | 0.99 |

   模型整体 RMSE 较 persistence 改善 **12.1%**，但在**负荷最高的 10% tick** 上反而略逊
   （28.5 vs 26.5）——树模型向均值收缩，尖峰被抹平。诚实结论：**persistence 是很强的下限，
   模型的净增益主要在中段而非峰值**；这恰是分配最在意的区段，故不把"改善 12%"当卖点。

## 样本口径（钉死，免得把 tick 读成分钟）

* 一行 = 一个**站 × 5 分钟 tick**：25 站 × 51,840 tick = **1,296,000** 稠密格（空闲格计 0kW），
  其中"至少一桩在充"的格 **705,924** 个。分配腿长表是逐 (站, tick, 在充桩) 一行 = **1,217,829** 行。
* `demand_kw` 是该桩该 tick 的**实测功率**（kW，不是电量）；`wait_min` 是该会话到当 tick 的
  **已充电时长**（分钟，不是"这五分钟"）；站级中位数 6.156kW（稠密口径，含空闲）、p99 144.0kW、
  峰值 150.85kW。
* 切分边界来自 `serving_manifest` 的 `assign_split`（业务日口径 = UTC+8，故边界落在 16:00 UTC），
  并对滞后窗口做 **1-tick purge**：本批丢 head 275 / tail 25 / 跨段 100 行，可用样本
  TRAIN 849,575 / VALIDATION 215,975 / TEST 215,975。TEST 只在 `evaluate.py` 里打分一次，
  容量档（big31/mid15）选择只用 VALIDATION。

## 分配腿：一条必须先讲清的不变式

单 tick 交付总功率 = `min(cap, Σdemand)`，**与策略无关**。所以绑定时 `servedFraction` 与欠供
总量在四策略间**恒等**——`stress.py` 把这条不变式**实测**出来（同一 tick 跨策略 servedFraction
最大离差 = 0.0，三档收紧容量全是）。策略真正的分化只在**欠供怎么分**：

真实需求流（705,924 个"在充"站·tick、1,217,829 行逐桩需求）在 **cap=100kW**（91,813 个 binding
tick，占在充 tick 的 13.0%）下的对比：

| 策略 | servedFraction | meanShortfallKw | 不公平 Gini | 等待加权欠供 | 等待最久反被欠更多 |
| --- | ---: | ---: | ---: | ---: | ---: |
| greedy_fcfs | 0.818437 | 24.111 | 0.586 | **166.36** | 0.0% |
| proportional | 0.818437 | 24.111 | **0.0** | 370.66 | 0.0% |
| maxmin | 0.818437 | 24.111 | 0.5885 | 254.14 | **15.96%** |
| priority_wait | 0.818437 | 24.111 | 0.586 | **166.36** | 0.0% |

四点读法：

* 前两列四策略完全相同 → 印证不变式；比"谁能多送电"是**伪命题**，要比的是欠供怎么分。
* **Gini 在全体参与者上算**（有需求的桩，含被喂满者）。早期版本只在"欠供 > 0"的子集上算，
  恰好会在"一桩给满、其余全饿"那一刻归零——把赢家通吃误报成完全公平。修正后：greedy/priority
  0.586、proportional 0.0（均摊）、maxmin 0.5885。
* **等待加权欠供只是加权和，看不见配对**。`longestWaitStarvedPct` 直接数"等待最久的参与者是否比
  等待最短的那个被欠得更多"的 tick 占比：maxmin 有 **15.96%** 如此（它按需求份额抬平，把需求大的
  久等者排在后面），两条贪心策略与 proportional 都是 0.0%。镜像互换时加权和能相等，这一列不能。
* **`priority_wait` 与 `greedy_fcfs` 在本批是同一条策略，而且这是实测不是注释**：逐 tick 分配不同的
  tick 数 = **0 / 91,813**（另两档 0 / 44,178、0 / 5,389）。原因写在 `policy_report.json` 的
  `policyEquivalence.sessionOrderCheck` 里：121,539 个会话上 rank(`session_id`) 与 rank(会话开始)
  的 Spearman = **+1.000**，而需求长表按 `session_id` 稳定排序 ⇒ 组内行序**就是**等待降序
  （89,911/91,813 个 tick 组内等待确有差异，不是并列造成的假象）⇒ "先来先服务"已经等于"等待最久优先"。
  两条策略仍分开实现（换到 id 不随开始时间递增的真实数据会分道），但**本批四种策略只有三种行为**。
  真正分开的对：greedy 家族 vs proportional（**100%** 的 binding tick 分配不同）、vs maxmin（**45.5%**）。

一句话方法论：**聚合指标相等 ≠ 策略等价；两个策略名重合 ≠ 它们是两条策略**——两个方向都得测，
所以报告里既有逐 tick 两两差异计数，也有排序键本身的对账。

## 复现（仓库根目录，`PYTHONPATH` 指向仓库根）

```
$env:PYTHONPATH = "C:\Users\admin\qt-charging-platform"
python -m data_analysis.ml.transformer.features     # 站×tick 帧 + 分配需求长表（只读确定性聚合）
python -m data_analysis.ml.transformer.train        # VALIDATION 选容量档，存 bundle
python -m data_analysis.ml.transformer.evaluate     # TEST 一次性盲测
python -m data_analysis.ml.transformer.stress       # 分配策略压力回放（独立输出目录，只需 features）
python -m unittest data_analysis.ml.transformer.tests.test_transformer -v   # 37 项纯合成单元测试
```

> 为什么用 `unittest` 而不是 `pytest`：仓库 CI 跑的是 `python -m unittest discover -s data_analysis/tests`
> 与显式模块清单，**pytest 不在声明依赖里**——`pytest` 风格的测试在这个仓库里等于没跑。

产物落在 gitignored 的 `data_analysis/outputs/ml_transformer{,_stress}/`：预测腿
`features_summary.json`（内含 `featuresSha256` / `demandLongSha256` 两条哈希链）、`tick_features.pkl`、
`tick_demands.parquet`、`gbdt-station-tick-load-v1.joblib`、`train_metrics.json`、
`evaluation_report.{json,md}`；分配腿 `ml_transformer_stress/policy_report.{json,md}`。
换表即中止：`train`/`evaluate` 走 `train.load_matrix` 复核 `featuresSha256`（bundle 里另存一份，
两边对不上即中止），`stress` 读长表前先对 `demandLongSha256`。`O_EXCL` 独占写，脚本入口
`require_empty_run_dir` 拒绝就地改写已发布目录；`train`/`evaluate` 靠 `extra_allowed` 白名单支持
断点续跑，不靠"允许覆盖"。

## 文件

| 文件 | 职责 |
| --- | --- |
| `common.py` | 批次绑定（fail-closed）、clean 装载、时间切分、`write_new_*`/`require_empty_run_dir`；指标与命令戳从 `ml.common` import（不复制 `sha256_file`/`mae`） |
| `allocate.py` | 四种分配策略 + `policy_metrics`（Gini 在**全体参与者**上算、配对敏感的 `longestWaitStarved`）+ `priority_order`（排序键单点定义，策略与对账共用）；纯函数、零数据依赖 |
| `features.py` | 遥测→站×tick 宽矩阵→长表：滞后（只用过去）+ 下一 tick 目标 + 1-tick purge；另出分配需求长表 |
| `train.py` | 下一 tick 负荷 GBDT，三基线（globalMean/persistence/climatology），VAL RMSE 选容量档，余量读数 |
| `evaluate.py` | TEST 盲测：模型 vs 基线、top-decile 误差、越限复核 |
| `stress.py` | 分配腿反事实：真实需求流 × {360,100,125,150}kW × 四策略；实测不变式 + 逐 tick 两两差异 + **策略名对账**（`policyEquivalence`） |
| `tests/test_transformer.py` | 分配不变式（逐策略×逐需求向量×逐容量全对）+ 指标语义（Gini 含被喂满者、饿死列配对敏感）+ 站×tick 帧滞后/目标/边界 + 分配腿入口端到端（`stress.main()` 落两份报告并拒绝重跑） |

## 已知不足

- **过载分析缺正样本**：受限于本批"从不越限"，本线无法训练/盲评"越限预测"，只能做余量与反事实；
  真部署需一个容量更紧或负荷更高的批次（或真实限流事件台账）才能把这条腿变成监督模型。
- **等待代理是会话已充电时长**，非排队等待（本批无"排队等功率"的直接记录）；`wait_min` 语义是
  "这桩这会话已充多久"，作为"谁更该被优先"的一种合理但非唯一的代理。而且在本批它与"先来先服务"
  给出**同一个排序**（`session_id` 随会话开始时间递增），所以"按等待优先"这条策略在本批无独立可测
  行为——要真正比较等待敏感性，需要排队等待时间（或用户未开成的会话）这类本批没有的记录。
- 负荷模型与成员 A 的 `ml/load/`、`ml/availability/` 是不同粒度/目标（站×5min 总需求 vs 站×小时），
  但同吃遥测——若日后统一时频底座，本线 `features.py` 应改读那张共享站×tick 事实表，避免三处重算。
- 契约边界：本线产物用 `write_new_bytes`/`write_new_json` 自存，**不写 `model_metadata.json`、不进
  `delivery/`、不动 `contracts/`**——`model_metadata.schema.json` 的 `target`/`metrics` 键仍锁死在
  load/availability，新目标上线需要先由 owner 扩契约（本线只提案，不改）。
