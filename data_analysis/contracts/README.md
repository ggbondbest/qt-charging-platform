# 第二阶段公共契约 v1.0.0

这份契约连接数据处理、Vue 大屏和 Python 模型。它是第二阶段 HTTP/文件契约，**不修改一期 Qt TCP 协议**。
以本目录的机器可读文件及自动测试为准，不让各成员自行命名相同指标。

## 1. 从哪里开始

| 文件 | 给谁使用 | 内容 |
| --- | --- | --- |
| `openapi.json` | Vue、接口开发、验收 | 10 个真实 HTTP 路由、参数、DTO、分页、错误及预测成功格式约定 |
| `types.ts` | Vue + TypeScript | 从实际 OpenAPI 生成，可导入类型；不是 HTTP 请求实现 |
| `table_schemas.json` | 数据层、机器学习 | 10 张导出表的完整字段顺序、类型、单位、可空性、主键 |
| `serving_manifest.schema.json` | 数据发布 | 批次、来源、CSV 分片、校验值与质量报告格式 |
| `model_metadata.schema.json` | 两名模型开发者 | 模型标识、数据来源、特征版本、时间切分、依赖、模型文件哈希与评价 |
| `prediction_result.schema.json`、`model.py` | 模型与 API | 预测输出格式及可执行的时间、容量、单位校验 |
| `examples/` | 全组 | 从随附样本查询库实际请求生成的响应，不是假造大屏统计 |

启动方式见 [数据层交付指南](../DATA_LAYER.md)，详细 HTTP 行为见 [API 说明](../backend/README.md)。
运行服务后 `/docs` 与 `openapi.json` 一致；CI 检查两者及 TypeScript 不发生漂移。

## 2. 公共字段，不得各自变更

- HTTP/JSON 字段为 `camelCase`；CSV/Parquet/SQLite 列为 `snake_case`。例如 `energy_wh → energyWh`，由 API 转换，数据库不直接返回页面。
- `schemaVersion=1.0.0`：第二阶段发布/API 版本；`featureVersion=history24-v1`：模型特征版本。原始数据 `schema_version=1.1.0` 是另一层格式，不要混用。
- `datasetId` 标识原始模拟数据集；`pipelineRunId` 标识一次清洗；`publishedBatchId` 标识一次统计发布。它们不相等。相同 raw 重跑统计也会得到新发布 ID。
- `sourceManifestSha256` 绑定原始数据清单。全量与 7 天样本是独立模拟批次，不能拼起来累计，也不能拿不同批次的特征和标签配对。
- 金额为人民币**整数分**、电量为 **Wh**、功率为 **kW**、耗时为**秒**、比率为 **0–1**。Vue 展示元、kWh、百分数时分别除以 100、除以 1000、乘以 100。
- 时间点严格为 UTC ISO8601 `Z`；业务日期按 `Asia/Shanghai`。所有日期范围都是 `[startDate, endDate)`，不包含 `endDate`。
- 没有观测/没有分母用 `null`，已观测到没有事件用 `0`。CSV 中空字段是 NULL；禁止 NaN、Infinity、字符串数字混入 JSON。
- `source=SIMULATED`。大屏应标明“模拟运营数据 / 数据截至时间”，不能显示为真实、实时经营数据。

## 3. HTTP 响应、分页和错误

所有成功和错误均为：

```text
{ code, message, data, meta: {
    requestId, datasetId, schemaVersion, pipelineRunId, publishedBatchId, generatedAt
} }
```

成功 `code=OK`；错误仍遵循同一外壳，`data` 可为 null 或具名错误数据。`requestId` 同时出现在 `X-Request-ID`。
`generatedAt` 是批次生成时间，不是网页刷新时间；数据库未就绪时未知身份字段为 null。

列表 `data={items,page,pageSize,total,hasNext}`，页码从 1 开始，`pageSize` 默认 20、最大 100。
图表 `data.items` 按时间升序，`limit` 最大 1000，超过时 `truncated=true`，应缩短查询日期，不默默丢数据。
`chart=load&granularity=hour` 返回同小时各站平均功率的总和 `meanPowerKw`，而不是对站点再次求平均；任一站缺观测则为 null，并附完整站点数与采样覆盖。

网页首次读取 `/api/v1/datasets`，缓存响应中的数据集和发布 ID；后续请求都传这两个 ID。
切换城市/日期后重新请求 overview、stations、charts；取消或忽略旧选择的过期响应。
收到 409 时重新读取批次，再刷新整屏，不能把不同发布时间的数据拼在一起。

| HTTP 状态 | `code` 示例 | 页面处理 |
| --- | --- | --- |
| 400 | `FILTER_MISMATCH` | 城市与站点不匹配，调整筛选 |
| 404 | `DATASET_NOT_FOUND` / `CITY_NOT_FOUND` / `STATION_NOT_FOUND` | 重新读取目录 |
| 409 | `BATCH_MISMATCH` | 重新选择当前发布批次 |
| 422 | `INVALID_ARGUMENT` / `INVALID_DATE_RANGE` / `DATE_OUT_OF_RANGE` / `INVALID_SORT` | 提示输入错误，不自动重试 |
| 503 | `DATA_UNAVAILABLE` | 查询库尚未发布或不可读 |
| 503 | `MODEL_NOT_READY` | 模型尚未接入，展示未就绪，不绘制假预测 |
| 500 | `INTERNAL_ERROR` | 展示请求编号；不展示 SQL、文件路径或堆栈 |

完整错误码集合见 `model.py:ERROR_CODES`；还保留 `HISTORY_TOO_SHORT`、`MODEL_INCOMPATIBLE` 等供真实模型接入使用。

## 4. 数据层交给网页什么

| 导出表 | 用途 | 主键 |
| --- | --- | --- |
| `cities` | 5 城目录和中心坐标 | city_id |
| `station_snapshot` | 电站坐标、桩数、批次末状态 | station_id |
| `station_hourly_metrics` | 小时电量、负荷、六类状态、覆盖情况 | station_id + recorded_at |
| `station_metrics_daily` | 电量、付款退款、购电/运维成本、利用率分子分母 | station_id + business_date |
| `city_daily` | 对站点日报的可加指标进行城市汇总 | city_id + business_date |
| `user_activity_daily` | 跨日期/跨站精确去重活跃用户和复访人数 | station_id + business_date + user_id |
| `station_cohorts_daily` | 按预约创建/排队加入日期分组的最终结局 | station_id + business_date |
| `station_service_daily` | 排队处理、维修恢复、评分及各自分母 | station_id + business_date |
| `ml_features_hourly` | 截至预测起点已知的 24 小时特征 | station_id + reference_time |
| `ml_targets_hourly` | 未来标签和各预测跨度的时间切分；**不进入查询库** | station_id + reference_time |

重要口径：

- 利用率 = 完整小时充电采样数 / 完整小时全部桩采样数，维护桩仍在分母；聚合城市和日期时先加分子分母，不能平均百分比。
- 净收款 = 成功收款 − 已发生退款，按交易日期计算；它不是会话估算费用，也不是利润。
- `cashContributionCents` 进一步扣购电、日常运营和维修成本；不含税、折旧、资本开支等，不称净利润。
- 活跃/复访用户由 API 在整个筛选集合重新去重；不得把每日去重人数加起来。
- 预约/排队 cohort 是本批次最终结果，不是历史那一天已经知道的状态，禁止当未来预测特征。
- 排队平均等待以成功叫号或未叫号的退出/超时为等待终点；队列总停留另有指标。维修解决时长和实际维修时长分开。
- `station_snapshot` 始终是批次末快照；`snapshotSemantics=LATEST_IN_BATCH`，日期筛选只影响 `periodMetrics`。缺少桩数据使用 unknown，不用旧状态填成最新。
- 清洗报告的 `normalizedRows` 指发生枚举规范化的行，包括正常空值规范化；不要把它直接叫作“异常数据条数”。拒绝原因、干净会话数单独展示。

## 5. 数据层交给模型什么

`reference_time=t` 表示已有 `[t−24h,t)` 的完整历史；`lag_power_kw_h01` 是 `[t−1h,t)`，`h24` 是最早一小时。
第一项 `label_power_kw_h01` 对应 `[t,t+1h)` 的平均负荷，`h24` 对应 `[t+23h,t+24h)`。
`label_available_count_h01..h24` 是各个预测小时**最后一次采样时刻**的空闲桩数（当前 5 分钟采样为 hh:55），不是整个小时的平均空闲数。

按 `station_id + reference_time` 一对一关联特征和标签；先选 `split_1h/6h/24h` 的 TRAIN、VALIDATION、TEST，再提取对应标签列。
EXCLUDED 不得进入训练或评价。长预测窗口只要有标签越过训练/验证/测试边界，就会被排除。
切分边界在 `serving_manifest.json.mlSplits`，为上海业务日、右端不包含：正式 180 天排除首尾日后是 118/30/30 天；样本是 3/1/1 天。

默认模型输入是过去 24 小时功率、滚动量、最近可用桩数、桩容量与已知日历。
当前没有 168 小时滞后或未来天气预报；需要周基线时从小时表另建过去窗口，并遵守同一截止时间。
不能输入 `label_*`、`split_*`、未来实际天气、最终订单金额、修复时间、异常真值或脏数据日志。
标准化器、缺失值填补和阈值只在 TRAIN 拟合；测试集仅用于最终评价。

## 6. 两个模型组统一的接入方式

负荷预测和空闲桩数预测都按 `model.py.Predictor` 提供：

```text
predict(history, PredictionContext) -> PredictionResult
```

`history` 是按时间升序的 24 条完整站点小时记录，均早于 referenceTime；不是未来标签。
如模型使用预计算特征，应复用 `spark_jobs/ml_features.py` 的相同转换规则，并用同一时点比对离线/在线特征。
`PredictionContext` 必含数据集、发布批次、站点、模型 ID、UTC 整点、预测跨度 1/6/24 小时。

返回 `schemaVersion/featureVersion/modelId/modelVersion/unit/points`；每小时一个点，必须连续，不能只返回第 6 或第 24 小时。
功率单位 kW，范围 0–站点额定容量；可用桩数量单位 chargers，范围 0–桩总数。若用回归期望数量，允许小数，页面须标“预计空闲桩数”，不能当真实整数库存。
`validate_prediction` 检查字段、版本、模型身份、时间连续性、有限数与容量边界。

交付模型文件时附带 `model_metadata.schema.json` 规定的元数据、训练/验证/测试评价和预处理器。
只加载本项目受信的模型产物，不从网页接受 pickle、文件路径或任意模型下载地址。
当前两个预测 HTTP 路由始终真实返回 503；OpenAPI 中的 200 格式是供后续接入的契约，不代表已训练完成。

## 7. 变更与核对

新增兼容字段先同步 API、JSON Schema、类型与测试；改含义/单位/删除字段必须更换契约版本和导出批次。
更换特征定义必须更换 featureVersion、重新训练，不能复用旧模型。
生成文件使用新路径导出，核对后替换仓库中的对应生成物：

```text
python -m data_analysis.backend.app --export-openapi 新的openapi.json
python -m data_analysis.contracts.generate_types --input 新的openapi.json --output 新的types.ts
python -m data_analysis.contracts.generate_types --input data_analysis/contracts/openapi.json --output data_analysis/contracts/types.ts --check
python -m unittest data_analysis.tests.test_contracts data_analysis.tests.test_contract_drift -v
```

如需完整 JSON Schema 格式验证，使用 Python 3.11–3.12 安装 `requirements-contract-test.txt` 并运行 `test_json_schemas`；这是测试依赖，不是 API 运行依赖。
