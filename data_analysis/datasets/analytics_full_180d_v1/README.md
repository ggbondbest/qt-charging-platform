# 180 天最新清洗与分析交付包

本目录已更新为 **2026-09-13 清洗验收批次**，包含实际 Spark 清洗明细、隔离记录、质量报告和统计结果，不再只是旧版统计 CSV。
业务数据仍为模拟数据。原始数据保留在相邻的 `charging_full_180d_v2/`，没有覆盖或删除。

目录名保留 `analytics_full_180d_v1`，以兼容组员现有命令；它不是固定批次编号。请以两个 manifest 中的批次 ID 为准，不硬编码分片文件名或旧批次 ID。

## 当前批次

| 内容 | 值 |
| --- | --- |
| 原始数据集 | `charging_full_180d_v2` |
| 业务日期 | 2025-12-01 至 2026-05-29，上海时区，共 180 天 |
| 清洗批次 | `spark-6ed381b125034f9e95d726a74befb121` |
| 发布批次 | `analytics-298aa3ee1401461fb06ea2bb96930dcf` |
| 原始总记录 | 23 表，5,832,840 行 |
| 清洗后明细 | 23 表，5,830,923 行 |
| 有效充电会话 | 121,539 条 |
| 隔离及重复副本 | 1,917 条，其中错误隔离 1,415 条、重复副本 502 条 |
| 分析结果 | 11 个语义维度、3 组双维对比，共 15 张分析结果表 |

## 文件夹怎么用

```text
analytics_full_180d_v1/
  csv/                       原有 10 张统计/特征/标签 CSV.gz，字段契约不变
  clean/<表名>/              23 张清洗明细，分片 Parquet
  rejected/charging_sessions/ 隔离及重复副本，保留原因和原始信息
  reports/
    cleaning_rules.json      清洗规则、版本、规则内容哈希
    cleaning_audit.json      清洗前后质量、行数守恒、样例和来源
  analysis/
    csv/<结果名>/            15 张单维/双维结果的 CSV.gz
    analysis_manifest.json   分组定义、字段单位、统计口径、行数与哈希
    preview.json             每张分析表最多 12 行预览，不是完整数据
  quality_report.json        与原有导出/发布接口兼容的质量摘要
  table_schemas.json         原有 10 张交接表的精确结构
  serving_manifest.json     原有查询/ML CSV 清单和发布批次
  acceptance_manifest.json  清洗明细、报告、分析附件的批次及文件哈希
  _SUCCESS                   已完成交付打包标记
```

每张表可能有多个分片。读取所有分片，不只读第一份；CSV 按 manifest 的 `files` 读取。Parquet 可通过 `spark.read.parquet(".../clean/charging_sessions")` 读取整张表。
`clean/corruption_log` 是污染审计记录，`clean/anomaly_labels` 是模拟评价标签；不能把这些真值信息直接混入模型特征。隔离记录也不属于有效训练样本。

### 网页组

原有 FastAPI 继续使用 `csv/` 的 10 张契约表，不扫描 23 张明细，也不会导入未来 ML 标签。按原方法发布一个新的查询数据库：

```text
python -m pip install -r data_analysis/requirements-api.txt
python -m data_analysis.publishing.publish --input data_analysis/datasets/analytics_full_180d_v1 --output data_analysis/outputs/web_latest_run1/analytics.sqlite3
```

以上命令在仓库根目录执行。将 `ANALYTICS_DB` 指向新文件并重启 FastAPI，完整启动命令见 [数据层交付指南](../../DATA_LAYER.md)。旧 SQLite 不会因 git pull 自动更新；不要覆盖仍在使用的数据库。

新增维度/对比放在 `analysis/csv/`，没有改动原有七类 chart 路由。大屏要使用这些新结果时，按分析清单接入，不能假定旧路由已自动支持所有新维度。

### 模型组

- 原有历史特征：`csv/ml_features_hourly/`。
- 原有独立标签和时间切分：`csv/ml_targets_hourly/`。
- 需要电池/设备明细时：`clean/battery_samples/`、`clean/charger_telemetry/` 等。
- 保留既有的时间切分和防泄漏约定，不能把全量预处理后的未来信息引入训练特征。

### 清洗验收展示

先展示原始数据，再看本包 `reports/` 的规则和质量报告、`rejected/` 的实际隔离记录，最后展示 `analysis/` 的维度与对比结果。清洗后明细和附件保持源文件字节不变，便于按哈希追踪。

## 校验和限制

只校验交付包无需安装 Spark：

```text
python -m data_analysis.publishing.acceptance_bundle --verify data_analysis/datasets/analytics_full_180d_v1
```

该命令核对批次、文件完整性、清单和报告一致性，不重新计算百万行数据。包内 Parquet 行数来自清洗审计；本次另用 Spark 实际核对了 23 表行数及字段结构，源验收和对账记录见 [验收记录](../../docs/cleaning_acceptance_validation.json)。
本次复制交付包后的文件核验、新 SQLite 发布和 59 次 FastAPI 请求核对见 [交付包验证记录](../../docs/dataset_delivery_validation.json)。

本包没有包含本机缓存、重复的统计 Parquet 或查询 SQLite。真实 Hadoop/HDFS、Vue 页面和训练后的模型仍需对应成员验收，不以本次打包成功替代。
旧版数据可从 Git 历史恢复；7 天样本包本次未更新，属于另一批次，不和本包混合统计。
