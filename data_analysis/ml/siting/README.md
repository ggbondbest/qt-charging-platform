# 新站选址线（`ml/siting/`·由第九线预留项转实做）

> **口径声明**：本线全部指标为**模拟数据**测试结果（第二阶段发布批次
> `analytics-298aa3ee1401461fb06ea2bb96930dcf`），不代表真实运营数据表现。本线**未新增任何数据**——
> 需求强度、弃队代理、坐标全是对发布批次 clean 表（`charging_sessions` / `charger_telemetry` /
> `queue_entries` / `stations`）的只读确定性聚合。

## 一句话结论

**留一站回测（LOSO）证伪了选址的核心前提——在这批数据上，空间需求场不能外推到一个没见过的位置的需求。**
更准确地说：**任何从"其它站观测需求"构造的预测器（含不含坐标都一样）都会反预测被藏站**。这把当初
预留时写的"真没人流量数据"从推测升级成了**可复算的实证**。本线因此交付的是**真工具链 + 真负结果**，
不是一个号称能选址的模型。

## 回测怎么做的（真盲测）

对每个站 i：**只拿其余 24 站**用距离衰减核重建需求场，读 i 坐标处的可吸收需求来预测 i 的真实
会话量，再与真实值算秩相关。同时跑三个对照，其中 **cityMean 不含任何坐标**（只用"哪个城"）：

| 打分 | 吸引半径 | Spearman | p |
| --- | ---: | ---: | ---: |
| field | 2.0 km | −0.524 | 0.007 |
| field | 3.0 km | −0.522 | 0.007 |
| field | 5.0 km | −0.589 | 0.002 |
| field | 8.0 km | −0.677 | <0.001 |
| field | 12.0 km | **−0.789** | <0.001 |
| **cityMean（不含坐标）** | — | **−0.879** | — |
| globalMean | — | 无定义（常数） | — |
| random（零信息，200 次） | — | +0.023 | — |

半径越大越负；需求场**比纯随机还差**。而连不带空间的 cityMean 都几乎同幅度为负 → 病灶不在空间模型。

## 机制：为什么必然为负（可复算）

仿真把**每个城市的需求总量近似恒定**地在其 5 个站之间切分（`users_per_city` 固定）。于是一城之内
"其余站需求之和 vs 本站需求"是**强负相关**（实测 Spearman ≈ **−0.88**）——本站忙是因为别人不忙。站间距
还近于均匀（站最近邻≈4km、城内站距 p50≈6.6km、跨城 p50≈1127km），**没有真实世界那种人流梯度**。
场模型靠邻居外推，就继承了这个零和结构：邻居越忙 → 反证本站越闲 → 预测反了。cityMean 因为同样在用
"其余站"，也掉进同一个坑。

补充一条更朴素的坏消息：连"需求"本身都没有一把干净的尺子——**会话数**与**充电分钟**两种真实口径的
站级 Spearman 只有 **0.47**。

## 双向单元测试：负结果不是模型 bug

`siting/tests/test_siting.py` 用两个方向相反的合成用例把这件事钉死：

* **zero-sum 合成**（每城总量恒定、城内切分）→ `field` 与 `cityMean` 都为负（复现真实批结论为回归）；
* **gradient 合成**（两簇真实空间聚集、簇间距远大于吸引半径）→ `field` 为正（Pearson/Spearman > 0.5）。

即：**场模型在有空间信号时是对的**；真实批为负是**数据里没有可从邻居推断的信号**，不是代码写错。

## 机会打分（工具，但 `NOT-BACKTEST-VALIDATED`）

`score.py` 在每城包围盒内撒网格（步长≈2.2km），按 `opportunity = 可吸收需求 ×（1 − 现有覆盖）` 排序，
需求口径叠加了 **ABANDONED 弃队数**（网络计 10,143）作为"想充没充上"的未满足需求代理。它**能跑、确定、
可复现**——但产物顶部明写 `status: NOT-BACKTEST-VALIDATED`：在上面的 LOSO 为负被解决之前，这些排序
**只是机械演示，不构成可信选址建议**；要真正决策，需要本批没有的人流/POI/路网需求面。

## 复现（仓库根目录，`PYTHONPATH` 指向仓库根）

```
$env:PYTHONPATH = "C:\Users\admin\qt-charging-platform"
python -m data_analysis.ml.siting.backtest     # LOSO 盲测 + 基线 + 机制诊断
python -m data_analysis.ml.siting.score        # 候选网格机会打分（含弃队叠加）
python -m pytest data_analysis/ml/siting/tests -q     # 12 项纯单元测试
```

产物落在 gitignored 的 `data_analysis/outputs/ml_siting/`（`backtest.{json,md}`、`score_summary.json`、
`candidate_opportunities.csv`；`O_EXCL` 独占写，入口 `require_empty_run_dir` 拒绝就地改写）。

## 文件

| 文件 | 职责 |
| --- | --- |
| `common.py` | 批次绑定、clean 装载、站级需求强度（会话/充电分钟/弃队）、`write_new_*`/`require_empty_run_dir` |
| `field.py` | 纯几何 + 需求场：haversine、距离衰减核、吸收/覆盖/机会打分、`loso_predict` |
| `backtest.py` | 留一站回测 + cityMean/globalMean/random 基线 + 零和机制诊断（本线主事件） |
| `score.py` | 候选网格机会打分（叠加弃队未满足需求），产物标注未验证 |
| `tests/test_siting.py` | 几何核 + 打分恒等式 + zero-sum/gradient 双向回归 + 强度聚合 |

## 已知不足

- **无外生需求面**：仓库没有人流/POI/交通量任何一张表，需求只能从被服务点反推；这是选址的先天缺陷，
  本线如实认账，不假装解决。真实选址需接入外部需求面。
- **LOSO 为负**：当前数据不支持"用邻居外推预测新站需求"，因此 `score.py` 的排序不可用于决策。
- **需求口径不唯一**：会话数 vs 充电分钟秩相关仅 0.47；换口径（如按时段/用户画像）可能改变排序，
  本线只钉了主口径并披露分歧。
- 契约边界：本线只读发布批 clean 表、**不新增数据、不进 `delivery/`、不动 `contracts/`**；若日后接入
  外生数据需另立数据源批次，届时本线按 `verify_batch` 换绑重跑。
