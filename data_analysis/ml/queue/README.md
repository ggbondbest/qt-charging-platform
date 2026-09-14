# `ml/queue/` · 排队结果与等待时长预测（第五线）

用户点下"加入排队"的那一刻，App 要不要说"这一队大概率排不上，附近有别的站"，以及"大约还要等 N 分钟"。
本线把这两句话变成可复核的模型：**白排风险**（二分类）与**等待分钟**（回归）。

与已有四条线**互不共用标签**：`availability` 看有没有空闲桩、`load` 看用多少电、`recommend` 看去哪个站、
`anomaly`/`churn` 看这趟充得健不健康 / 这个人还回不回来。`recommend` 明确把 ABANDONED/FAILED 排除在它的
标签之外（只当站点侧统计原料），本线接住的正是它放弃的那个标签——样本单位是**排队事件**，决策时点唯一：
`queue_entries.joined_at` 那一刻。

## 数据与标签

| 项 | 口径 |
| --- | --- |
| 数据层 | `datasets/analytics_full_180d_v1/clean/`（只读），批次 `analytics-298aa3ee1401461fb06ea2bb96930dcf` · run `spark-6ed381b125034f9e95d726a74befb121` |
| 样本 | `queue_entries` 全量 **36,594** 行，一行一次排队；`joined_at` 覆盖 2025-11-30 16:30 → 2026-05-29 13:55（UTC） |
| 结局 | `SERVED` 22,414 / `ABANDONED` 10,143 / `CALL_EXPIRED` 4,037，三值互斥无缺失（结构断言见 `build_data.load_queue`） |
| 分类标签 | `y_waste = outcome != SERVED`，全表基础率 **0.3875** |
| 回归标签 | `wait_min = (called_at - joined_at)` 分钟（p50 15 / p90 30 / max 55）。SERVED 与 CALL_EXPIRED 都观测得到"叫号前等待"，**ABANDONED 自己退队、从未被叫，属删失**；回归只在 SERVED 上拟合与评价，不拿 0 或截断值冒充 |
| 切分 | 按 `serving_manifest.mlSplits` 换到 UTC（北京时间日历日 − 8h）：TRAIN 24,852 / VALIDATION 6,038 / TEST 5,267，窗口外 437 行 EXCLUDED |
| 特征 | 37 数值 + 8 类别 = 45 列；另有 10 列因零方差在建表阶段剔除（下节） |

**没有新增、修改或删除任何数据集文件**；产物一律落 `data_analysis/outputs/ml_queue/`（该目录 gitignore，
不入仓），且全部 `O_CREAT|O_EXCL` 独占创建——重跑必须先换新目录，脚本入口 `require_empty_run_dir` 会拒。

## 泄漏纪律（本线的主要工程量）

1. `called_at` / `resolved_at` / `session_id` / `status` 都是**事后字段**，只进标签与分组，绝不进特征
   （`NON_FEATURE_COLUMNS` + `FORBIDDEN_FEATURES` 双名单，`pick_features` 再断言一次）。
2. `charging_attempts` 只借 `vehicle_id` / `reservation_id` 两个"排队前就存在"的外键，`outcome`、
   `failure_reason`、`session_id` 一律不读——它们与本线标签同源。
3. 所有 as-of 聚合走 `StationAsOf`（排序数组 + `searchsorted(side="left")`），语义是**严格早于 t**；
   结局类聚合的知识时刻是 `resolved_at` 而不是 `joined_at`。同刻并列按保守处理：`resolved_at == t`
   的离开视作"这一刻还不知道"，仍算在队内。
4. 比率特征用**与数据无关的常数先验**（0.5）收缩，不从全表标签率取，避免 TEST 分布漏进 TRAIN。
5. `verify_no_future_leak` 随机抽 300 行、按定义暴力重算 `open_now`/`arrivals_60m`/`wait_mean_3h`/
   `waste_rate_prior` 四项，要求**逐条 0 偏差**，否则整个构建失败。本次实测最大偏差 `0.0e+00`。
   这条断言在开发中真的抓到过一个 bug：最初的暴力版把"t 时刻刚好离开"算作已离开，与向量版差 1 人
   （48/300 行），已按保守口径统一。

## 结果（TEST 盲测，一次算完）

> 全部指标为模拟数据测试结果（第二阶段发布批次 `analytics-298aa3ee1401461fb06ea2bb96930dcf`），不代表真实运营数据表现。

白排风险，n = 5,267，基础白排率 0.3782：

| 指标 | 本模型 | 最强基线（三级平滑经验率） | 只看排位 |
| --- | --- | --- | --- |
| AUC | **0.6721** | 0.6626（+1.44%） | 0.6203（+8.34%） |
| PR-AUC | 0.5748 | — | — |
| Brier / LogLoss | 0.2117 / 0.6137 | — | — |
| lift@10% / @20% | **1.94×** / 1.73× | — | — |

冻结阈值 0.3265（在 VALIDATION 上按"精确率 ≥ 0.50 取召回最大"选定，TEST 只验不改）：
告警率 48.11%，精确率 **0.5059**，召回 **0.6436**，F1 **0.5665**。

等待时长（仅在 SERVED ∩ TEST 上评，n = 3,275；标签 p50 15 / p90 30 分钟）：

| 方案 | MAE（分钟） |
| --- | --- |
| **本模型 GBDT(L1)** | **7.0972** |
| 排位中位数（最优基线） | 7.4229 |
| 站点×小时中位数 | 7.9847 |
| 全局中位数 | 8.1145 |

RMSE 9.3921 分钟，p90 绝对误差 15.133 分钟，±2 分钟命中率 19.60%，输出区间合法（0–120 分钟）100%。
相对最优基线 MAE 降低 **4.39%**。

校准是这条线最结实的一块：TEST 十分位的预测均值与实测频率最大偏差 **0.0604**（唯一偏高的桶是
0.448–0.525 段，预测 0.4787 / 实测 0.4183），最低桶 0.2017→0.2353、最高桶 0.7273→0.7324，单调不交叉
——所以"风险 20% 的人"和"风险 73% 的人"确实被区别对待了，可以直接拿去定运营话术。

消融（同一 TEST，缩减模型只用于归因、不参与发布）：

| 特征组 | 特征数 | TEST AUC |
| --- | --- | --- |
| 只有排位 | 1 | 0.6203 |
| 只有站点/用户/时间静态 | 23 | 0.6244 |
| 只有队列/桩动态 as-of | 21 | **0.6728** |
| 全量（发布的主模型） | 45 | 0.6721 |

## 已知不足（如实呈报）

- **站点静态属性在这批数据里零方差**：25 个站每站都是 3 桩、变压器 360 kW、服务费 30 分，`dc_share`
  恒为 0；`from_reservation` 恒 0、`reservation_lead_min` 100% 缺失；`home_city_matches_station` 恒 1。
  这些列在建表阶段剔除并逐个留痕（`build_summary.droppedConstant`）。**后果**：本线的可学信号只剩
  队列动态与时间/天气，站点侧个性化的空间被数据本身封住了。
- 相对最强基线（三级平滑经验率）AUC 只 +1.44%，且"只有动态"消融 0.6728 略高于全量 0.6721——
  静态块是死重。**不据此换模型**：TEST 只用一次，换特征集必须在 VALIDATION 上定，那是 v0.2 的活。
- 等待 MAE 7.10 分钟对 p50=15 分钟的标签是约 47% 的相对误差，±2 分钟命中率仅 19.60%；
  排队事件只有 5 分钟粒度（遥测 as-of 最大陈旧 5.0 分钟），分钟级"还要等多久"的天花板不高。
  对外话术应当是"大约 15 分钟 / 排上的可能性偏低"，不是精确倒计时。
- **回归存在选择偏差**：等待模型只在"最终排上了"的人身上训练与评测，中途放弃的人（占 38.75%）没有等待
  终值。所以 `预计等待分钟` 的语义是"如果排上，大约等多久"，不是"这次排队体验如何"。话术上必须区分，
  否则白排的人会被报以一个根本不存在的等待值。
- **TEST 内部有漂移**：前半月 AUC 0.6908 / 后半月 0.6535，基础白排率 0.3879 → 0.3693。30 天窗口里已经
  能看到衰减，说明站点历史类特征需要滚动重训，不能一次冻结用一季。
- `CALL_EXPIRED`（叫号没赶上）与 `ABANDONED`（自己退队）在本版合并成一个"白排"标签；分三分类是自然的
  下一步，但两者在 joined_at 时刻的可分特征本来就不多，收益待验证。
- 无对外接口：`contracts/` 归负责人，本线**不擅自加端点**，只在下面提案。

## 契约提案（只提案，不改 `contracts/`）

`GET /predict/queue-risk?stationId=&positionAtJoin=&joinedAt=` 返回
`{riskProbability, alertThreshold, expectedWaitMinutes, modelVersion, publishedBatchId}`；
或先按离线口径交付：把本表 `test_predictions.csv` 的字段并进站点运营周报，不动 API。
两者都需要负责人点头；SQLite 服务库里**不放**本线产物，也不伪造看板数字。

## 复现

```bash
# 环境：Python 3.13.9 / numpy 2.4.4 / pandas 2.3.3 / scikit-learn 1.7.2 / joblib 1.5.2 / pyarrow 21.0.0
python -m data_analysis.ml.queue.build_data   # 建矩阵 + 泄漏审计（写 queue_matrix.pkl / build_summary.json）
python -m data_analysis.ml.queue.train        # 只吃 TRAIN/VALIDATION（写 gbdt-queue-abandon-v1.joblib / train_metrics.json）
python -m data_analysis.ml.queue.evaluate     # TEST 盲测一次（写 evaluation_report.json / .md）
python -m data_analysis.ml.queue.predict --self-check
python -m data_analysis.ml.queue.predict --dump-test-table 200
python -m unittest discover -s data_analysis/ml/queue/tests -t .
```

产物：`queue_matrix.pkl`、`build_summary.json`、`gbdt-queue-abandon-v1.joblib`（含两个模型 + 基线表 +
阈值 + 批次/矩阵哈希）、`train_metrics.json`、`evaluation_report.json`、`evaluation_report.md`、
`test_predictions.csv`。`data_analysis/outputs/` 在 `.gitignore` 内，所以以上文件不入仓，入仓的是本 README
与上面的数字。
