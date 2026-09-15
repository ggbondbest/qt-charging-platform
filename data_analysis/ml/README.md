# 已整合的 CPU 机器学习模块

统一安装/训练：`python -m pip install -r data_analysis/requirements-delivery.txt`、`python -m data_analysis.delivery.cli train`。
完整服务使用 [delivery](../delivery/README.md)，不在 HTTP 请求中训练模型。

| 能力 | 实现 | 在网页/推荐中的用途 |
| --- | --- | --- |
| 小时平均负荷 | [load](load/EVALUATION.md)，PR66，过去24完整小时→未来1/6/24小时kW | 智能分析曲线；找桩内部辅助5%的均衡项 |
| 小时末空闲桩 | [availability](availability/DELIVERY.md)，PR70，容量分布、校准区间 | 运营规划；目标为hh:55采样，不替代分钟到站预测 |
| 分钟到站/等待 | [ChargePilot ML](../chargepilot/ml/README.md)，5分钟状态分布、条件等待、服务成功率 | 起点→ETA→模型→排序→预约/排队/充电 |
| 用户流失风险 | [churn](churn/README.md)，截止时刻聚合、用户留出 | 历史观察日14天未回访风险；分数不是校准概率 |
| 会话异常筛查 | [anomaly](anomaly/README.md)，固定训练参照、完成会话推理 | TEST会话辅助复核；主要识别热异常，召回有限 |
| 历史站点偏好 | [recommend](recommend/README.md) | 离线研究；不将体验账户冒充已有历史用户 |

数据来自项目 `analytics_full_180d_v1` CLEAN，**全部为模拟数据，不是ACN实测**。预测与统计必须使用同一datasetId、publishedBatchId、来源哈希。
工件只接受自己本地训练的可信文件；先验证来源、版本与哈希再反序列化。`outputs/` 不提交 Git。

## 评估与使用边界

- 小时模型服从manifest时间切分及跨度purge；到站模型使用5分钟完整历史。不将未来标签、未来天气或结束金额当作当时已知特征。
- 流失聚合截至观察时刻、排除窗口后注册用户，收入只取已完成会话。用户切分互斥，但不是跨新月份部署验证。
- 异常阈值及语境参照只在TRAIN/VALIDATION确定；异常标签只用于评价，不参与无监督特征。移除v2/v3/v4并列运行入口，Git保留历史。
- 网页从当前训练工件读取指标；旧PR报告是历史研究记录，不是当前部署模型的结果。模型不存在或校验失败明确NOT_READY。
- 不以深度网络数量代替验证，不宣传模拟集结果已达到现实运营或安全诊断能力。

管理分析适配器：`InsightsService.status/report/list_churn/predict_user/list_anomalies/inspect_session`。
小时模型注册入口：`/api/v1/intelligence/models`；选择注册modelId再调用`/api/v1/intelligence/forecast`。
