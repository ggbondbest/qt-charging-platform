# Python 查询与预测服务：后续开发入口

本次仅定义边界，尚未实现 HTTP 服务。建议采用 FastAPI，统一读取已完成 Spark 批次。
不要把每次大屏刷新变成一次全量 Spark 作业，也不要直接将数百万原始行传给浏览器。

建议接口：

- `GET /api/v1/datasets/current`：dataset_id、SIMULATED 来源、数据范围、最后批次状态。
- `GET /api/v1/dashboard/overview?city_id=&start_date=&end_date=`：总体经营指标。
- `GET /api/v1/dashboard/stations/{station_id}/hourly`：趋势、六状态、覆盖率。
- `GET /api/v1/dashboard/quality`：输入、规范化、隔离、去重数量与原因。
- `POST /api/v1/predictions/load`：站点、预测起点和 1/6/24 小时跨度，输出负荷预测与模型版本。

统一返回 `dataset_id, source, as_of, data`；金额单位、时间、空分母规则遵守 `docs/data_dictionary.md`。
模型未训练、批次未完成、日期越界时返回明确错误，不能用随机数字冒充预测结果。
预测日志只保存模拟输入与版本，不引入真实用户隐私。API 不复用 Qt 的 TCP JSON 契约。
