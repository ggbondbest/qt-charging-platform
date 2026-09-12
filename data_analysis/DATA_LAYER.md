# 数据层交付与接入

## 已完成和剩余内容

本次已实现：独立脏数据注入、真实 PySpark 清洗、业务统计、历史特征/未来标签导出、可校验的数据包、只读 SQLite 查询库、10 个 HTTP 路由及公共契约。
Vue 页面、模型训练/评价和真实预测仍由后续开发完成。老师的 Linux/HDFS 环境需最终验收；本地运行通过不等于 HDFS 集群已验收。

```text
已有 v2 原始数据（保留不改）
  ├─ 可选：脏数据注入 → 新 raw 批次 + 注入清单
  ↓
PySpark 清洗 → clean / rejected / quality_report / _SUCCESS
  ↓
PySpark 经营统计 + 24h 历史特征与未来标签
  ↓
Parquet + CSV.gz + serving_manifest + 文件 SHA256
  ├─ 发布只读 SQLite → Python API → Vue + ECharts
  └─ 特征 CSV + 独立标签 CSV → 训练/验证/测试 → 模型接入预测 API
```

`reference_aggregates` 只用作独立对账，不作为 Spark 统计输入。
每次作业写新目录，失败输出留有运行标记，不能发布；不覆盖原始数据或正在使用的查询库。

## A. 网页组：直接启动已有统计接口

在仓库根目录执行，不需要安装 Spark 或 Hadoop：

```text
python -m pip install -r data_analysis/requirements-api.txt
python -m data_analysis.publishing.publish --input data_analysis/datasets/analytics_full_180d_v1 --output data_analysis/outputs/web_run1/analytics.sqlite3
```

Windows PowerShell：

```powershell
$env:ANALYTICS_DB = (Resolve-Path data_analysis/outputs/web_run1/analytics.sqlite3).Path
python -m uvicorn data_analysis.backend.app:app --host 127.0.0.1 --port 8000
```

Linux shell：

```sh
export ANALYTICS_DB="$PWD/data_analysis/outputs/web_run1/analytics.sqlite3"
python -m uvicorn data_analysis.backend.app:app --host 127.0.0.1 --port 8000
```

打开 `http://127.0.0.1:8000/docs` 可直接试接口。Vue 开发端口 5173 的两个本机来源默认允许跨域。
请求顺序建议：datasets → cities → overview/stations/charts。从第一次响应取批次 ID，后续全都带上。
电站坐标/状态用于地图，按城市/日期的概览和排行、小时/日趋势、排队维修等均已由接口计算，不需在 Vue 扫原始 CSV。
预测页先读取 `/api/v1/models` 显示模型未就绪，等模型组交付后接真实结果。

小样本改用 `analytics_sample_7d_v1`；一个 SQLite 只对应一个批次，不把两者导入同一个库。
数据库文件禁止原地覆盖，更新数据时发布新文件并重启 API。默认不含账号鉴权，仅用于本机教学展示；不得直接暴露公网。

## B. 模型组：直接使用已导出的数据

`datasets/analytics_full_180d_v1/csv/ml_features_hourly/` 为输入特征，
`csv/ml_targets_hourly/` 为标签及 split。一个表可能有多个 CSV.gz 分片，应读取清单中所有分片。
所有字段、单位和主键见 [公共契约](contracts/README.md)，不要根据前几行猜类型。

- 负荷：未来 1/6/24 小时曲线，对应 `label_power_kw_h01..h24`。
- 空闲桩数：各预测小时末采样点，对应 `label_available_count_h01..h24`。
- 特征与标签按站点 ID、预测起点一对一关联；只选对应跨度的 TRAIN/VALIDATION/TEST，不用 EXCLUDED。
- 正式批次训练/验证/测试按时间划分；先做基线，再比较树模型或小型 LSTM。不得随机拆分时间序列来抬高指标。
- 导出的历史特征可进入 API 查询库，未来标签不会进入。未来天气/故障真值不属于这份输入特征。
- 异常检测是额外扩展，电池曲线可使用 clean/battery_samples，真值 anomaly_labels 仅评价时关联；当前没有声称已完成异常检测模型。

交付：受信模型文件 + 预处理器 + 公共格式元数据 + 实测评价 + Predictor 适配器。
此后替换 API 的 MODEL_NOT_READY 分支；模型未准备好前不使用随机数冒充预测。

## C. 数据处理展示：独立注入脏数据

正式 raw 已包含可追踪的脏副本。新增工具支持在保留旧污染的前提下，再对未污染会话抽样注入：

```text
python -m data_analysis.charging_data.inject_dirty --input data_analysis/datasets/charging_sample_7d_v2 --output data_analysis/outputs/dirty_demo_run1 --rate 0.05 --seed 42
```

包含重复会话、未知关联 ID、负费用、缺失会话 ID、状态格式错误；抽样以 seed 固定，操作可复现。
原目录不变，新目录包含新 manifest、来源哈希、`injection_plan.json` 和 `_INJECTION_SUCCESS`。
`rate` 作用于尚未污染的规范会话，不是“所有表 5% 的每个字段都随机损坏”；不要误解数量。

清洗把非法会话隔离到 rejected、规范化可修字段，再去重；脏副本不计入真实有效会话。
支持文件哈希、行数、主外键、金额/电量和时间约束等校验。注入未完成、源文件篡改或混批会拒绝处理。

## D. 从 raw 一次跑完数据层

需要 Java 17、Python 3.10–3.12、PySpark 3.5.6。Java 路径由环境的 `JAVA_HOME` 配置。
建议全量作业至少预留 4 GB JVM 堆和额外进程/系统内存；不能把 4 GB 堆理解成机器总内存只要 4 GB。
大原始表缓存使用磁盘，样本和全量都使用本地双核作业即可；没有 GPU 需求。

```text
python -m pip install -r data_analysis/requirements-spark.txt
python -m data_analysis.scripts.run_data_layer --input data_analysis/datasets/charging_sample_7d_v2 --output data_analysis/outputs/sample_all_run1 --driver-memory 2g
python -m data_analysis.scripts.run_data_layer --input data_analysis/datasets/charging_full_180d_v2 --output data_analysis/outputs/full_all_run1 --driver-memory 4g
```

该便捷入口用于本地路径，产出 processed/、export/、analytics.sqlite3 和发布报告；全链路成功才写根 `_SUCCESS`。
要展示新注入的清洗过程，把 `--input` 换为 `outputs/dirty_demo_run1`，使用新的独立输出目录。
如果环境中有多个 Python，`PYSPARK_PYTHON` 应指向正在运行的虚拟环境 Python，防止 Spark 子进程选错版本。

分开运行时依次使用：

```text
python -m data_analysis.spark_jobs.pipeline --input 原始批次 --output 新的processed目录 --driver-memory 4g
python -m data_analysis.spark_jobs.export --input 原始批次 --processed 同一批次processed目录 --output 新的export目录
python -m data_analysis.publishing.publish --input 新的export目录 --output 新的查询库.sqlite3
```

单独 export 启动 JVM 前也应配置资源，使用 `PYSPARK_SUBMIT_ARGS=--driver-memory 4g pyspark-shell`，或在 `spark-submit` 中传 `--driver-memory 4g`。
便捷入口会统一配置资源。不要在已启动的 JVM 上临时修改 driver heap 并以为生效。

`processed` 输出视为不可变批次，禁止手工改 Parquet。导出会核对清洗批次身份、原始哈希和统计行数；不是对 processed 所有 Parquet 做防恶意篡改签名。

## E. Linux / HDFS 验收

先按老师的环境说明启动 HDFS，实际地址从 Hadoop 配置读取，**不要盲填别人的端口**。
把一个完整 raw 批次（含 manifest、schema、raw、校验文件）上传到新的 HDFS 批次目录，检查 `hdfs dfs -ls` 和 NameNode 页面。
例如集群已配置 `fs.defaultFS` 后，可以使用 `/user/charging/raw/run1` 作为 HDFS 根路径，或使用老师环境的 `hdfs://主机:端口/...`。

1. pipeline 的 `--input` 指向 HDFS 原始批次，`--output` 指向新的 HDFS processed 批次。
2. export 的 `--input` 仍为同一原始路径，`--processed` 为刚才结果，`--output` 为新的 HDFS export 批次。
3. 保存终端作业日志、HDFS 路径与 `_SUCCESS`、Parquet 文件及质量报告作为验收证据。
4. 用 `hdfs dfs -get` 下载完成的 export 到新的本地目录，再执行 Python publish 和 API；网页不直接请求 HDFS。
5. 同批 raw 可用 `verify_aggregates` 与独立参考统计对账，见 [Spark 说明](spark_jobs/README.md)。

提交代码并不能替代以上真实环境验证；本次验证记录会明确 HDFS 是否已执行。
HDFS 是存储层，Spark 是计算层，SQLite 是网页查询快照；各层职责不同，不需要再让每次网页点击触发 Spark。

## F. 交付目录和完成标准

| 目录 | 是否交给组员 | 用途 |
| --- | --- | --- |
| `datasets/charging_*_v2` | 是 | 原始模拟数据、来源和独立对照，原样保留 |
| `datasets/analytics_*_v1` | 是 | 可直接发布的统计/特征/标签 CSV.gz，体积小于重复附 Parquet |
| `contracts/` | 是 | 公共字段、OpenAPI、TS 类型、JSON Schema、真实接口样例 |
| `outputs/` | 不提交 | 临时 Parquet、SQLite、失败批次、模型产物、运行日志 |
| `docs/data_layer_validation.json` | 是 | 本轮实际运行与校验结果；不是人工编造的指标 |

运行测试：

```text
python -m unittest discover -s data_analysis/tests -v
```

未安装 API 依赖会明确跳过其测试；真实 Spark 测试还需 `RUN_SPARK_TESTS=1`。CI 分别运行基础、API/发布/契约和 Spark 套件，避免“跳过”当成“验证通过”。
9 项检查的具体分工、第一阶段工作流移除及合并规则说明见 [第二阶段 CI](docs/ci_checks.md)。
网页、模型两组可并行使用同一发布批次；整合时依照 source hash、版本和 ID 核对，不需要再等待字段设计。
