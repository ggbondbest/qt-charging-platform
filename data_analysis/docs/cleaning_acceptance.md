# 第二阶段数据清洗与准备验收指南

本次按老师提供的《数据清洗基本流程》补齐“探查、评估、规则、执行、校验、报告”，并把分析维度和对比结果单独导出。原始数据不覆盖，原 FastAPI 和查询表契约继续使用。按组长最新确认，忽略截图中的 Flask 要求，不增加第二套 Web 框架。

## 验收要求对应什么

| 要求 | 本项目实现或交接边界 |
| --- | --- |
| Python 3.11 或 3.12 | CI 的 Linux 使用 3.11，Windows 使用 3.12；本地实际版本写入运行报告 |
| Spark 清洗和分析 | `spark_jobs/pipeline.py` 执行清洗；`acceptance_analysis.py` 计算维度和对比；不是把生成器参考结果改名当 Spark 输出 |
| 分析维度不少于 8 个 | 提供 11 个真实分组维度，见下表；不把电量、订单数、金额三个指标算成三个维度 |
| 至少两个维度对比 | 提供三组双维分析；统计口径、日期基准、分母和字段单位随产物保存 |
| Hadoop 3.x 存储 | 支持 HDFS 输入输出，提供真实 HDFS 验证脚本；须在老师环境实跑后才能标记通过 |
| Web 请求与响应 | 保留 FastAPI；读不可覆盖的 SQLite 发布快照，不在每次网页请求时重跑 Spark |
| Node.js 23 及以上、Vue 3 | 网页组的开发与验收要求，本次不宣称网页已经实现 |
| DataV 和丰富图表 | 网页组可用 DataV 做大屏布局，配合 ECharts；现有接口及本次 CSV/清单提供真实计算数据，不硬编码截图数字 |

截图只说明 MySQL 版本不限制，未明确要求必须用 MySQL。本次不擅自迁移已完成的 SQLite 查询层。

## 一次运行可以得到什么

在仓库根目录、Python 3.11/3.12 虚拟环境和 Java 17 就绪后运行。以下命令在 PowerShell 和 Linux shell 都可逐条执行；第一次用小样本。

```text
python -m pip install -r data_analysis/requirements-spark.txt -r data_analysis/requirements-api.txt
python -m data_analysis.scripts.run_acceptance --input data_analysis/datasets/charging_sample_7d_v2 --output data_analysis/outputs/acceptance_sample_run1 --driver-memory 3g
```

源数据已有标注的脏副本，无需额外注入即可展示清洗。需要现场演示注入时，换一个新的输出目录并加 `--inject-rate 0.05 --seed 42`。这个 5% 是对未被标注的规范会话抽样的概率，不是全部百万行的 5%，也不保证最终恰好抽中 5%。注入结果另存 `dirty_input/`，原始数据不会被改动。

正式批次可将输入改为 `charging_full_180d_v2`、输出改为新目录，并使用 `--driver-memory 4g`。不要在同一台电脑同时运行多个全量 Spark 作业。

```text
<本轮新输出目录>/
  processed/
    clean/<23张表>/                实际清洗后的 Parquet
    rejected/charging_sessions/    隔离/去重明细、原因、原始值与来源
    statistics/                   小时和日统计
    reports/cleaning_rules.json    稳定规则编号、版本、处理方式和边界
    reports/cleaning_audit.json    字段探查、六维质量、动作、样例和前后对比
    reports/quality_report/        兼容已有导出流程的质量摘要
  analysis/                       11个分组维度、3组双维对比，CSV/预览/字段说明
  export/                         现有经营统计、ML特征/独立标签、契约与哈希
  analytics.sqlite3               FastAPI 可查询的只读快照
  acceptance_report.json          本轮版本、数量、处理状态
  acceptance_report.md            答辩时按顺序展示的简要记录
  _SUCCESS                        所有本地步骤完成后才生成
```

未完成的目录保留 `_RUNNING` 和失败信息，不应作为成功结果展示。重试须用新目录，不手工补 `_SUCCESS`。运行报告明确区分本地成功、HDFS 尚未验证、网页尚未验收和模型尚未训练。

数据准备成功后，还可实际核对 FastAPI 返回与 SQLite 汇总是否一致，保存独立报告：

```text
python -m data_analysis.scripts.verify_serving --database data_analysis/outputs/acceptance_sample_run1/analytics.sqlite3 --bundle data_analysis/outputs/acceptance_sample_run1/export --output data_analysis/outputs/acceptance_sample_run1/api_verification.json
```

这是本地接口验证，不是 Vue 页面验收。正常启动 FastAPI 的命令和跨域配置仍按 [后端说明](../backend/README.md) 执行。

## 本次已实际跑通的结果

2026-09-13 已用新流程完成 7 天样本（额外脏数据注入）和 180 天全量批次。全量 23 张表共 5,832,840 行，保留 121,539 条充电会话，隔离及去重 1,917 条，会话及全部表的行数守恒检查通过。11 个分析维度、3 组双维结果和查询快照均已生成。

全量 108,000 条小时统计和 4,500 条日统计已逐键逐字段与独立参考结果核对，无超出约定容差的差异；参考数据只在计算完成后用于核验。样本和全量各完成 59 次 FastAPI 请求核对，接口汇总与 SQLite 一致。

原始运行输出在 `data_analysis/outputs/acceptance_sample_20260913_run1` 和 `data_analysis/outputs/acceptance_full_20260913_run1`。
全量已整理为仓库内的 [180 天交付包](../datasets/analytics_full_180d_v1/README.md)：规则和质量审计在包内 `reports/`，清洗明细在 `clean/`，对比结果在 `analysis/`，原有统计/特征/标签在 `csv/`。没有把查询 SQLite、缓存文件、重复统计 Parquet 或本机输出目录整体上传。
可复核的源码哈希、批次、数量和测试范围保存在 [本次验收记录](cleaning_acceptance_validation.json)。这些记录不是远程 CI、真实 HDFS、网页或模型验收结果。

打包是清洗完成后的独立步骤，原有运行命令仍然写入 `--output`，不会自动覆盖 datasets。要整理下一轮已通过的运行结果，先选择新目录：

```text
python -m data_analysis.publishing.acceptance_bundle --input data_analysis/outputs/acceptance_full_20260913_run1 --output data_analysis/outputs/next_delivery_run1
python -m data_analysis.publishing.acceptance_bundle --verify data_analysis/outputs/next_delivery_run1
```

确认通过后再用独立 Git 提交更新交付目录。打包工具本身拒绝覆盖已存在的目录，避免组员误操作丢失旧数据。

## 清洗具体做了什么

1. 探查。每张表统计行数、字段缺失、类型不合规、主键重复与数值/时间范围；范围来自成功解析并标准化的观测值，清洗前仍可能含后来隔离的负数，不冒充原始字符串分布。保留统计分母，不能把合法可空字段算成业务错误。
2. 评估。六维分别描述完整性、规则范围内的一致性和有效性、唯一性、按历史批次窗口衡量的时效性，以及无法由模拟数据证明的真实准确性。没有外部真值时，不写“准确率 100%”。
3. 规则。只修复有确定含义的格式，如边界空白、结构化字段零宽字符、数值全角数字、枚举大小写。不猜测元与分、不用平均金额填账单、不把未知时区自动当北京时间。
4. 执行。缺失关键 ID、负金额、金额关系不符、时间顺序错误、SOC 越界、关联关系不一致等会话进入隔离层；保留原始信息和原因。其他表出现不可安全处理的格式错误时拒绝整批发布。
5. 校验。先校验再去重，避免坏副本覆盖正常记录；核对输入会话数等于保留会话数加隔离和重复副本数。修正过的记录可能仍保留，不能再加到删除量中。
6. 存档。原始文件 SHA256、规则版本、批次 ID、处理前后统计、有限样例和来源一起存档。清洗与去重都不改原始文件。

一条记录可能同时去空格和转换枚举大小写，因此各项“修正动作行数”允许重叠，不可相加当作修正记录总数。所有重复/隔离明细都保留在 Parquet，JSON 只展示有限样例，避免把百万行搬进报告或网页。

异常高负荷不一定是脏数据，因此不按 IQR/Z-score 一刀切删除。缺失关键金额与目标标签不能编造填充。不可恢复的乱码只能标记或拒绝，不能宣称恢复成原文。对中文评价暂不做分词、停用词或自动删表情，这些是后续文本建模的任务，不能破坏现有业务文本。

ML 表保持历史特征与未来标签分离，训练/验证/测试按时间划分。模型组如果增加均值填充、标准化或 One-Hot 编码，只能在训练集拟合，再应用于验证/测试；不能用全量数据提前计算均值。

## 11 个分析维度和 3 组双维对比

| 单维分组 | 可回答的问题 | 适合的展示 |
| --- | --- | --- |
| 城市 | 哪个城市的起充会话、电量更多 | 柱状图、地图 |
| 电站 | 同一城市哪些站使用更多 | 排行榜 |
| 业务日期 | 每日到访充电变化如何 | 折线图 |
| 起充小时 | 用户通常什么时段开始充电 | 24小时柱状图 |
| 站点类型 | 住宅、办公、商业站的需求差别 | 分组柱状图 |
| 用户类别 | 私家、通勤、营运等用户的使用差别 | 环形图、柱状图 |
| 车型 | 不同车型的会话电量差别 | 分组柱状图 |
| 充电接口类型 | AC/DC 等接口的使用差别 | 环形图 |
| 星期分类 | 周一至五与周末的起充需求差别 | 对比柱状图 |
| 支付渠道 | 成功收款、退款按渠道如何分布 | 堆叠柱状图 |
| 故障类型 | 哪些类型报障较多、恢复耗时如何 | 排行图 |

三组双维结果是“城市 × 起充小时”“站点类型 × 星期分类”“城市 × 支付渠道”。例如城市×小时热力图能同时比较北京和大连的小时分布，而不是只展示两个不相关总数。星期分类仅指日历星期，不冒充经过节假日调休校正的“工作日”。

会话电量按起充日期/小时归入整次会话，是到访群体分析，不是那个小时实际供出的电量；实际小时供电继续使用现有遥测 `station_hourly`。会话账单不等于现金收入；收退款按交易发生日单独计算。跨表聚合不能将会话与多条支付/故障明细直接连接后再求和。

新增 `analysis/` 是独立分析数据包，不改变已承诺给网页/模型组的查询表和 OpenAPI。网页可按清单读取已导出的聚合 CSV；如需全新的动态筛选 API，应另行添加契约和测试，不能假称当前七类 chart 路由已支持全部新维度。

## Linux 和真实 Hadoop 验证

老师允许本地开发、答辩尽量使用 Hadoop。小组只需有人在实际 Hadoop 3.x 环境完成这部分。不要为了本项目重新格式化 NameNode，也不要把本地 `file:///` 目录标为 HDFS。

先在该环境确认 `python --version`、`java -version`、`hadoop version`，并使用老师提供的 `HADOOP_CONF_DIR`。下面的 `namenode:9000` 是占位地址，必须替换成实际地址；所有输出用未存在的新路径。

```text
hdfs dfs -mkdir -p hdfs://namenode:9000/charging/raw
hdfs dfs -put data_analysis/datasets/charging_sample_7d_v2 hdfs://namenode:9000/charging/raw/
python -m data_analysis.spark_jobs.pipeline --input hdfs://namenode:9000/charging/raw/charging_sample_7d_v2 --output hdfs://namenode:9000/charging/processed/acceptance_run1
python -m data_analysis.spark_jobs.acceptance_analysis --input hdfs://namenode:9000/charging/raw/charging_sample_7d_v2 --processed hdfs://namenode:9000/charging/processed/acceptance_run1 --output hdfs://namenode:9000/charging/analysis/acceptance_run1
python -m data_analysis.spark_jobs.export --input hdfs://namenode:9000/charging/raw/charging_sample_7d_v2 --processed hdfs://namenode:9000/charging/processed/acceptance_run1 --output hdfs://namenode:9000/charging/export/acceptance_run1
python -m data_analysis.scripts.verify_hdfs --input hdfs://namenode:9000/charging/raw/charging_sample_7d_v2 --processed hdfs://namenode:9000/charging/processed/acceptance_run1 --export hdfs://namenode:9000/charging/export/acceptance_run1 --report data_analysis/outputs/hdfs_verification_run1.json
```

`verify_hdfs` 只读实际 HDFS：核对文件系统类型、原始分片校验值、批次来源、成功标记、Parquet 行数及导出 CSV 哈希。它不安装 Hadoop，不上传、覆盖、删除或格式化远程数据。连接失败不算通过；报告里的 Hadoop client 版本不是服务端版本，服务端版本需保存老师环境的 `hadoop version` 输出。

验收保存：HDFS 路径及文件列表、Spark 执行日志、清洗报告、分析 CSV、HDFS 验证 JSON。Spark 使用 `local[2]` 同时读写 HDFS 是真实的 HDFS 存储加单机 Spark 计算，不是多节点计算集群。

网页服务仍从本地查询快照读取。把完成的 export 下载到新的本地目录，再发布 SQLite 并启动 FastAPI；浏览器不直接访问 HDFS。

HDFS 命令及 URI 规则依据 [Apache Hadoop FileSystem Shell](https://hadoop.apache.org/docs/r3.4.2/hadoop-project-dist/hadoop-common/FileSystemShell.html)；固定的 Spark/Python/Java 兼容说明见 [PySpark 3.5.6 安装文档](https://spark.apache.org/docs/3.5.6/api/python/getting_started/install.html)。

## 当前不应该声称完成的内容

- 本地单元测试不是老师的 Linux/HDFS 现场验收。
- 历史全量验证记录只对应记录中的旧规则和批次；本次新规则须用新运行报告佐证。
- Vue 3、DataV 大屏与训练后的模型仍由对应成员开发，数据准备完成不等于页面/预测完成。
- 模拟数据能检验工程和统计逻辑，不能据此宣称得到真实城市规律或真实业务预测准确率。
