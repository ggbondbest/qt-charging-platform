# 会话异常复核（交付版 v5）

用于管理端筛查**已经结束的充电会话**，不是实时设备保护或自动故障诊断。
与智能找桩、流失风险没有直接的因果依赖，不强行合成一个模型。

## 使用

在仓库根目录安装统一机器学习依赖后运行：

```bash
python -m data_analysis.ml.insights train
python -m data_analysis.ml.insights report
python -m unittest data_analysis.tests.test_ml_insights_delivery
```

统一产物位于 `outputs/ml_insights_delivery/`，包括模型、历史留出样本、评估报告、文件与源数据指纹。
同一个已冻结目录再次运行只验证并复用，不覆盖评估；协议变更应换输出目录重新训练。
`InsightsService.list_anomalies()` 列出 TEST 会话中的待复核项，`inspect_session(id)` 提供分数、阈值、观测特征及说明。
HTTP 请求不会现场训练模型，也不读取 `anomaly_labels` 作为模型输入。

## 固定参照与正确边界

- 清洗后的时间戳是 UTC，业务边界按上海时区的 05-01 / 05-15 / 05-30 零点转换。
- 会话必须整体结束于所属时间窗；跨边界的 37 条会话不进入任何训练/验证/测试侧。
- 充电器×电流档、充电器×SOC 档的温度、电流以及电芯压差参照只来自 TRAIN。
- 未见充电器/档位退回 TRAIN 充电器或全局中位数，并明确返回 `contextFallback`，不默认判正常。
- IsolationForest 与参照信号的组合、阈值仅在 VALIDATION 选择；新数据使用固定 TRAIN 经验分布。
  添加另一条测试会话不会改变原会话的分数。
- 输出是异常排序分，不是“故障概率”。文字解释是观测事实，不伪称因果解释。

## 实际回顾性留出结果

模型 `fixed-reference-session-anomaly-v5`，TEST 10,002 条会话，415 条有模拟异常标签：

| 指标 | 数值 |
| --- | ---: |
| 提交复核 | 93 |
| Precision | 1.0000 |
| Recall | 0.2241 |
| F1 | 0.3661 |
| 热应力召回 | 93/93 |
| 提前结束／功率降额召回 | 均为 0 |

当前验证选择了温度语境信号，因此主要擅长**热异常**，绝不能宣传为全类型准确检测。
数据是本项目合成数据，并非 ACN 实测或真实车辆现场验证。PR67 的旧测试集曾用于研究比较，
这里如实称“修正协议后的回顾性留出评估”，不声称从未见过的全新测试。

## 精简说明

原 v2/v3/v4 连续试验入口已从交付树移除，可从 PR67 Git 历史恢复。
特别是 v3/v4 的“每个 split 单独排名”只能批筛，不能独立在线判断；交付版不复用其分数和报告。
`detect.py` 仅保留兼容入口，实际实现统一在 `delivery.py` 与 `ml/insights.py`。
