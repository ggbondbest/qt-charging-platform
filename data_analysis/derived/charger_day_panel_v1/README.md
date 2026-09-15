# charger_day_panel_v1（第七线新增派生数据集）

本目录是**派生数据**，不是第二阶段发布批次的一部分：全部列都是对发布批次
`analytics-298aa3ee1401461fb06ea2bb96930dcf`（run `spark-6ed381b125034f9e95d726a74befb121`）真实记录的确定性聚合
（groupby + sum/mean/max/mode），**没有编造标签、没有新增事件、没有修改任何原始行**。
`datasets/analytics_full_180d_v1/` 一个字节都没动。

重新生成（会拒绝覆盖已有目录）：

```
python -m data_analysis.ml.reliability.build_panel
```

## 文件

- `panel_charger_day.csv`：13,500 行，sha256 `6521aba654881d75…`
- `day_context.csv`：900 行，sha256 `c1c641ba68548141…`

`derivation_manifest.json` 记录源批次、源表行数、生成器代码哈希与全部口径；
`data_analysis/ml/reliability/common.py:verify_derived()` 在每次读取时复查行数与 sha256，
对不上就拒用。

## panel_charger_day.csv

每台桩 × 每个北京日历日的稠密面板（75 台 × 180 天 = 13,500 行）。
当日 0 次尝试的日子**显式记 0**，不缺行。样本域与第六线逐字相同：
`非排队关联 ∧ charger_id 非空 ∧ outcome ∈ {STARTED, FAILED}（与第六线逐字相同）`。

对账：面板总尝试 102,801、总技术失败 3,676
（与第六线样本域完全相等）。当日 ≥1 次尝试的桩日 13,444 行，
其中 0.2173 至少出现一次技术启动失败；0 次尝试的 56 行
失败率恒为 0.0。

### ⚠ 暴露度：这份数据最容易误用的一点

当日失败日占比随当日尝试次数单调上升：

| 当日尝试数 | 失败日占比 |
|---|---|
| (0, 2] | 0.0186 |
| (2, 5] | 0.0698 |
| (5, 10] | 0.2908 |
| (10, 20] | 0.4106 |
| (20, 1000000000] | 0.5286 |

因此 `attempts_on_day` / `users_on_day` / `started_on_day` 是**标签窗口内**的量，训练时点（昨天为止）
不可得，已写进 `NON_FEATURE_COLUMNS` 禁入名单。本线只允许"截至昨天的日均尝试量"这类因果量，
并在报告里单列一条 oracle 基线（真用明天的真实尝试次数）标注为不可部署，用来读出"排序收益里
有多少是暴露度"。

## day_context.csv

每城 × 每日的日历事实（周末/场景事件/计划系数）与天气日聚合（气温均值/极值、湿度、雨量、
降雨小时数、主导天气）。注意：本文件存的是**当天事实**，不含因果判断；特征层只用"严格早于
当日起点"的量（天气取前一日聚合，日历是计划量可取当日），见
`data_analysis/ml/reliability/features.py`。电价表**不产出特征**（日级聚合后是城市级常量，
等于把禁入的 city_id 换马甲带进来）。

## 明确不读的表/列

- `anomaly_labels`：第四线的事后人工标签，对'明天会不会启动失败'是答案侧信息，本线整表不读
- `maintenance_tickets.status`：全表终态，'现在已解决'不等于'当时已解决'；工单只用 reported_at/restored_at
- `maintenance_tickets.accepted_at/work_started_at`：调度侧时间戳，真实系统里夜间可否读到未定，保守不用
- `maintenance_tickets.labor_cost_cents/parts_cost_cents`：钱是修完才知道的量，对次日风险无前置信息
- `tariffs`：电价在 (城, 小时) 上是常数，日级聚合后成为**城市级常量**——等价于把 ban 掉的 city_id 换个马甲带进特征（第六线同样以'纯身份列'禁掉 city_id），本线不读
- `charging_sessions`：会话级电量/时长是'这趟充得怎么样'（第一/四线），本线的暴露度已由 attempts_on_day 表达，且会话表里没有次日可得的前置信息
- `payments/reviews/operating_costs/corruption_log/battery_samples/vehicle_energy_intervals/queue_entries/reservations/campaigns`：与'次日这台桩会不会启动失败'无因果关系，或需事后才可得

## 口径声明

全部指标为模拟数据测试结果（第二阶段发布批次 analytics-298aa3ee1401461fb06ea2bb96930dcf），不代表真实运营数据表现。本线特征来自新增派生数据集 charger_day_panel_v1（data_analysis/derived/charger_day_panel_v1/）：对发布批次真实记录的确定性聚合（按桩×北京日历日、城市×日历日 groupby），无编造标签、无新增事件。
