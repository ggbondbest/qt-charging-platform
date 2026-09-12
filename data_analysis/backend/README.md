# 只读统计 API

HTTP 服务已实现，直接读取发布完成的 SQLite 快照，不在请求中启动 Spark，也不扫描数百万行 raw CSV。预测仅提供严格的请求结构与能力声明，**尚未训练模型，不返回预测值**。该接口不复用 Qt TCP JSON 契约。

## 启动与测试

需要 Python 3.10 及以上，建议在项目虚拟环境中安装：

```text
python -m pip install -r data_analysis/requirements-api.txt
python -m unittest data_analysis.tests.test_analytics_api -v
```

先完成数据发布，再把环境变量 `ANALYTICS_DB` 设为已发布 SQLite 文件的绝对路径。一个数据库只对应一个数据集与一个已发布批次；切换文件后重启服务，不覆盖正在服务的批次。

```text
python -m uvicorn data_analysis.backend.app:app --host 127.0.0.1 --port 8000
```

浏览器打开 `http://127.0.0.1:8000/docs` 查看请求参数及响应结构。默认仅允许 Vue 常用开发来源 `http://localhost:5173`、`http://127.0.0.1:5173`；其他来源用逗号分隔的 `ANALYTICS_CORS_ORIGINS` 配置。服务不包含账号鉴权，默认绑定本机；不要未经鉴权和网络访问控制直接部署到公网。

## 接口与筛选

| 路由 | 内容 |
| --- | --- |
| `GET /api/v1/health` | 发布数据是否可读；未配置或未发布返回 503 |
| `GET /api/v1/datasets` | 当前文件中的单个数据集及来源哈希、日期范围 |
| `GET /api/v1/cities` | 城市目录与坐标 |
| `GET /api/v1/stations` | 电站目录、最新快照、筛选日期内的经营摘要 |
| `GET /api/v1/pipeline/runs` | 已发布批次、各原始表规模、清洗数量／原因／脱敏样本，不暴露物理目录 |
| `GET /api/v1/dashboard/overview` | 电量、资金、资源、去重活跃用户、排队／维修／评价统计 |
| `GET /api/v1/dashboard/charts` | 按日／小时的统计图表数据 |
| `GET /api/v1/models` | `models=[]`、`implementedPrediction=false` |
| `POST /api/v1/predict/load` | 负荷预测接口声明，合法请求返回 `MODEL_NOT_READY` / 503 |
| `POST /api/v1/predict/availability` | 可用性预测接口声明，同样不返回假预测 |

查询字段统一使用 camelCase：`datasetId`、`publishedBatchId`、`cityId`、`stationId`、`startDate`、`endDate`。批次字段可省略以选当前文件；显式批次不一致返回 409，防止悄悄切到别批数据。城市／站点不存在返回 404，二者不匹配返回 400。无效参数、未知排序、日期越界返回 422。

日期按 `Asia/Shanghai`：`startDate` 包含当天，`endDate` **不包含当天**。省略时使用完整发布范围。日期筛选对经营、事件及用户统计一致生效；城市目录本身没有时间变化。

列表采用 `page=1&pageSize=20`，每页最多 100 条；超出末页返回空数组。电站排序 `sortBy=id/name/capacity/city/energy/netPaid/utilization`，城市 `id/name`，数据集／批次 `id/generatedAt`；方向为 `asc/desc`。这些字段均映射到固定 SQL 白名单。后三种电站排行按筛选日期汇总后、分页前排序，空观测值始终排最后；同分按电站 ID 稳定排序。

图表参数 `chart=energy/revenue/utilization/states/service/cohorts/load`，`granularity=day/hour`；电量、利用率及状态支持小时粒度，**负荷 `load` 仅支持 hour**。同小时 `meanPowerKw` 为筛选集合中各站平均功率之和，不是各站平均值；任一站不完整或缺行时返回 `null`，并附 `completeStationCount/incompleteStationCount/isComplete` 与采样覆盖数量。`capacity` 是筛选集合桩数，不是 kW。`limit` 默认 400、最大 1000，超过范围返回 `truncated=true`，应缩小日期范围继续查询。稀疏事件表的无事件日期补零；缺少遥测的时段电量保留 `null`，不伪造零负荷。

预测请求示例结构：

```json
{
  "datasetId": "已发布的数据集ID",
  "publishedBatchId": "已发布的批次ID",
  "stationId": "已发布的电站ID",
  "referenceTime": "2026-05-30T00:00:00Z",
  "modelId": "待接入的模型ID",
  "horizonHours": 6
}
```

`horizonHours` 只允许 JSON 整数 1、6、24，不把 `true`、`6.0` 或字符串当作整数；`referenceTime` 必须为以 `Z` 结尾的 UTC 整点，代表第一段预测区间的起点；`modelId` 为非空字符串。这个日期不是已知未来天气输入。后续 ML 模块应替换未就绪处理并发布 `modelVersion`，当前任何合法预测请求都明确失败而不是随机返回。OpenAPI 中预先声明了 **未来接入后的 200 成功格式** `PredictionResult`，便于 Vue 编写输入／输出组件；这不是当前已提供模型推理。

## 统计口径与追溯

- 每条响应为 `code/message/data/meta`。`meta` 固定含 `requestId/datasetId/schemaVersion/pipelineRunId/publishedBatchId/generatedAt`，请求编号同时在 `X-Request-ID`；`generatedAt` 指数据发布时间，不是刷新页面时间。数据库不可用时，尚不可得的批次字段为 `null`。
- 数量为整数，金额为人民币分，电量为 Wh；比率为 0–1，不是百分数。没有分母的均值／比率为 `null`。净收款不是利润，模拟现金贡献不含税、折旧、资本开支等。
- 利用率重新用完整小时的分子／分母求比，不平均各站或每日比率。覆盖数量同时返回；不完整时段的已观察电量可以相加，但不能声称全量完整。
- 活跃用户在整个筛选集合中按用户去重；复访用户先按用户汇总会话数再判断至少两次，不把每日去重人数直接相加。
- 预约／排队 cohort 是“创建／加入日期分组、批次最终结果”，不是那天已知的结果。排队处理、修复耗时、评价则按完成／事件日期统计，各有独立分母；事后 cohort 不能当预测特征。
- 电站 `snapshotAt/dataAsOf` 和 `snapshotSemantics=LATEST_IN_BATCH` 表示**本批次末尾最新观察**，不随历史日期筛选回放。只有 `periodMetrics` 随日期改变，缺失桩标记 unknown，不使用旧状态冒充当前状态。
- 质量页 `tables/normalizedByTable/rejectionReasons/rejectionSamples` 使用具名结构；拒绝样本最多 100 条，只保留模拟会话 ID 与原因，不返回原始行或源文件目录。`normalizedRows` 是至少一个枚举字段经过去空格、大写或空串转 NULL 的行数，包含正常的空值规范化，**不是发现了这么多脏数据**；`cleanSessionRows` 单独展示清洗后会话数。

## OpenAPI 导出

```text
python -m data_analysis.backend.app --export-openapi 新的输出文件.json
```

也可调用 `export_openapi(path)`；无需启动数据库或 Spark，输出路径已存在时拒绝覆盖。接口测试使用独立构造的 SQLite 小样本，覆盖分母、跨站去重、日期边界、空结果、只读保护、注入输入、错误脱敏和未就绪模型。
