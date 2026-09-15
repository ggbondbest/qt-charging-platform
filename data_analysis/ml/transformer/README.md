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
   因此转到**余量测算**（预测腿）与**收紧容量的反事实**（分配腿）。

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

## 分配腿：一条必须先讲清的不变式

单 tick 交付总功率 = `min(cap, Σdemand)`，**与策略无关**。所以绑定时 `servedFraction` 与欠供
总量在四策略间**恒等**——`stress.py` 把这条不变式**实测**出来（同一 tick 跨策略 servedFraction
最大离差 = 0.0）。策略真正的分化只在**欠供怎么分**：

真实需求流在 **cap=100kW**（91,813 个 binding tick）下的策略对比：

| 策略 | servedFraction | meanShortfallKw | 不公平 Gini | 等待加权欠供 |
| --- | ---: | ---: | ---: | ---: |
| greedy_fcfs | 0.818437 | 24.111 | 0.0304 | **166.36** |
| proportional | 0.818437 | 24.111 | **0.0** | 370.66 |
| maxmin | 0.818437 | 24.111 | 0.0809 | 254.14 |
| priority_wait | 0.818437 | 24.111 | 0.0304 | **166.36** |

三点读法：

* 前两列四策略完全相同 → 印证不变式；比较"谁能多送电"是**伪命题**，要比的是公平性。
* **Gini 测不出"谁被欠供"**：proportional 把欠供均摊（Gini=0）却把痛苦也加到久等者头上
  （等待加权欠供最高）；winner-take-most 类（greedy/priority）Gini 非零但等待加权欠供最低。
  → 单看 Gini 会选错，必须配合等待加权那一列。
* **`priority_wait` 与 `greedy_fcfs` 逐 tick 完全相同**（166.36=166.36，采样 20,000 个 binding
  tick 零差异）。这不是 bug，是仿真结构：`session_id` 与 `session_start` 的 Spearman = **1.0**
  （会话按起始时刻全局编号），需求长表按 `session_id` 排序，于是"按索引 FCFS"已经就是
  "按等待时长降序"——**本批数据里先到先得即最久优先**。两条策略仍分开实现（换到 session_id
  不单调的真实数据上会分道），但当前数值重合。cap=125/150kW 下差异更细（见 `policy_report.md`）。

## 复现（仓库根目录，`PYTHONPATH` 指向仓库根）

```
$env:PYTHONPATH = "C:\Users\admin\qt-charging-platform"
python -m data_analysis.ml.transformer.features     # 站×tick 帧 + 分配需求长表（只读聚合）
python -m data_analysis.ml.transformer.train        # VALIDATION 选容量档，存 bundle
python -m data_analysis.ml.transformer.evaluate     # TEST 一次性盲测
python -m data_analysis.ml.transformer.stress       # 分配策略压力回放（独立输出目录）
python -m pytest data_analysis/ml/transformer/tests -q     # 111 项纯单元测试
```

产物落在 gitignored 的 `data_analysis/outputs/ml_transformer{,_stress}/`（`O_EXCL` 独占写，
脚本入口 `require_empty_run_dir` 拒绝就地改写已发布目录；`train`/`evaluate` 靠 `extra_allowed`
白名单支持断点续跑，不靠"允许覆盖"）。

## 文件

| 文件 | 职责 |
| --- | --- |
| `common.py` | 批次绑定（fail-closed）、clean 装载、时间切分、`write_new_*`/`require_empty_run_dir`；指标与命令戳从 `ml.common` import（不复制 `sha256_file`/`mae`） |
| `allocate.py` | 四种分配策略 + `policy_metrics`（纯函数，零数据依赖） |
| `features.py` | 遥测→站×tick 宽矩阵→长表：滞后（只用过去）+ 下一 tick 目标 + 1-tick purge；另出分配需求长表 |
| `train.py` | 下一 tick 负荷 GBDT，三基线（globalMean/persistence/climatology），VAL RMSE 选容量档，余量读数 |
| `evaluate.py` | TEST 盲测：模型 vs 基线、top-decile 误差、越限复核 |
| `stress.py` | 分配腿反事实：真实需求流 × {360,125,100,150}kW × 四策略；实测不变式 |
| `tests/test_transformer.py` | 分配不变式 + max-min/比例解析解 + 合成站×tick 帧滞后/目标/边界 |

## 已知不足

- **过载分析缺正样本**：受限于本批"从不越限"，本线无法训练/盲评"越限预测"，只能做余量与反事实；
  真部署需一个容量更紧或负荷更高的批次（或真实限流事件台账）才能把这条腿变成监督模型。
- **等待代理是会话已充电时长**，非排队等待（本批无"排队等功率"的直接记录）；`wait_min` 语义是
  "这桩这会话已充多久"，作为"谁更该被优先"的一种合理但非唯一的代理。
- 负荷模型与成员 A 的 `ml/load/`、`ml/availability/` 是不同粒度/目标（站×5min 总需求 vs 站×小时），
  但同吃遥测——若日后统一时频底座，本线 `features.py` 应改读那张共享站×tick 事实表，避免三处重算。
- 契约边界：本线产物用 `write_new_bytes`/`write_new_json` 自存，**不写 `model_metadata.json`、不进
  `delivery/`、不动 `contracts/`**——`model_metadata.schema.json` 的 `target`/`metrics` 键仍锁死在
  load/availability，新目标上线需要先由 owner 扩契约（本线只提案，不改）。
