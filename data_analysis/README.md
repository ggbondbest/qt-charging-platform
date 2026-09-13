# 第二阶段：充电运营大数据分析基础

本目录独立于原 Qt 客户端和服务端，不修改一期业务数据库、协议或 Qt 构建。
按第二阶段计划，先准备**可复现的模拟数据 → Spark 清洗统计 → HDFS 存储验证**，
再由小组开发 Vue 大屏和预测模型。

本次已完成数据层：数据生成、独立脏数据注入、关联校验、真实 Spark 清洗、经营统计、
历史特征/未来标签、数据包发布、只读 HTTP 查询 API 及公共接口契约。**还没有实现完整网页或训练模型**。
HDFS 接入代码已预留实际路径参数，但老师提供的 Linux/Hadoop 环境仍需单独验证。

网页/模型组请先看 [数据层交付指南](DATA_LAYER.md) 和 [公共契约](contracts/README.md)。
按老师流程演示数据清洗，请看 [数据清洗与准备验收指南](docs/cleaning_acceptance.md)：
补充逐字段质量探查、清洗前后对比、规则与血缘、11 个分组维度和 3 组双维对比；继续使用 FastAPI。
仓库附 `analytics_full_180d_v1` / `analytics_sample_7d_v1` 两个可直接使用的统计包；
不需要每位成员都重跑 Spark。发布查询库后即可开发 Vue，两张独立 ML 表可直接用于训练。
180 天交付包已按 2026-09-13 验收批次更新，并附 23 张清洗明细 Parquet、隔离记录、规则清单、质量审计、11 维分析和 3 组双维对比。
直接查看 [180 天数据包说明](datasets/analytics_full_180d_v1/README.md)。目录名保留以兼容组员命令，实际批次以清单中的 ID 为准；7 天交付包没有改动，不与全量混用。

## 一、可以直接使用的内容

- 5 个城市：大连、沈阳、北京、上海、深圳；每城 5 个模拟站，每站 3 个桩，共 25 站、75 桩。
- 180 天正式数据：2025-12-01 至 2026-05-29（上海业务日），6,000 个虚拟用户及车辆。
- 23 张关联业务表：订单之外，还包括请求失败、支付退款、排队、预约、维修、成本、天气、电池、车辆网外能量账等。
- 5 分钟连续桩状态和电量，包含空闲、充电、预约占用、充后占位、维护、离线。
- 7 天小样本，适合组员快速开发；**与全量属于不同批次，不要合并统计**。
- 压缩 CSV、字段结构、文件 SHA256、预览 CSV；代码只依赖 Python 标准库即可生成和校验。
- PySpark：枚举清洗、无效会话隔离、去重、Parquet 输出、质量报告、10 张经营统计和 ML 交接表。
- 只读 API：5 城/25 站目录、经营概览、七类日/小时图表（含负荷）、日期筛选排行榜、清洗质量、模型能力声明。
- 公共契约：OpenAPI、TypeScript、数据表精确结构、JSON Schema、严格批次/单位/错误约定及真实响应示例。

业务记录由程序独立模拟，不含真实姓名、手机、车牌或原参考文件的记录。
站名、品牌、展会、资费为模拟；坐标为城市周边示意坐标，并非真实电站地址。
天气背景使用带署名的 ERA5 再分析，节假日日期引用官方安排；这不把模拟充电变成真实运营记录。

v2 正式版已生成 **5,832,840 行原始记录**（含已标注脏副本），其中 121,539 条有效充电会话、
3,888,000 条桩采样、1,217,829 条电池采样。完整数据目录约 71 MB（十进制），已随仓库提供。
各表行数、五城市对比及实际统计示例见 [数据统计快照](docs/dataset_profile.md)。
自动测试覆盖生成、篡改、行为方向、官方日历、参考数据校准和真实 Spark 对账；
全量小时/日报对账的具体批次与结果见 [全量 Spark 验证](spark_jobs/full_validation_summary.json)。

### v2 为什么比直接随机更合理

- 按站型和用途生成：办公/校园日间补能、住宅晚间到访与跨夜停留、商场休息日需求、营运车辆多时段快充。
- 使用提供样本的小时分布及“补能量×连接时长”联合统计，但不复制原记录，也不把未知站点类型当成已证实的办公站。
- 深圳 UrbanEV 公开小时利用时长只作弱参考；**利用率不是到站人数**。北京、大连等城市没有可比实测曲线的部分不编造结论。
- 五城 21,600 条逐小时气象背景来自同一 ERA5 模型；低温影响驾驶耗电和充电功率，具体强度仍明确为模拟假设。
- 官方假期/调休日与普通周末分开；同车 SOC 连续、AC/DC 停留不同、等价桩不再固定挑第一台。
- 网外行驶/补电有单独能量账，避免所有车只在小规模平台补能而耗尽电量；**不计入平台营收**。

证据、参数与限制详见 [充电行为依据](docs/behavior_evidence.md)。每个批次的 `behavior_report.json`
直接从生成后的明细统计，不是预先写好的“目标曲线”；排除批次首尾日，分别核对到访、供电和占位。
合成数据可保证可检查的业务约束，但不保证代表真实五城市场或真实预测精度。

## 二、目录负责什么

```text
data_analysis/
  charging_data/          Python 生成器、统一字段、压缩写入、独立校验器
  config/                生成配置、匿名校准聚合、官方日历、ERA5缓存及来源许可
  datasets/              已生成的完整数据和小样本，拉取后可直接使用
    charging_*_v2/       原始数据包
      raw/               23 张原始表，按月分片的 CSV.gz
      preview/           每张表前 12 行，普通 CSV，便于查看字段
      reference_aggregates/  Python 独立小时/日报对照值，不是 Spark 输出
      manifest.json      版本、来源、记录数、文件校验值、时间范围
      schema.json        每张表的精确字段顺序
      validation_report.json  独立检查结果及状态分布
      behavior_report.json    从实际明细计算的城市/站型/时段与驻留核验
    analytics_full_180d_v1/  最新 180 天交付包
      csv/               原有 10 张统计/特征/标签表，接口契约不变
      clean/             23 张清洗明细，Parquet 格式
      rejected/          被隔离的会话和重复副本，不混入训练输入
      reports/           本批 cleaning_rules.json、cleaning_audit.json
      analysis/          11 维分析、3 组对比的 CSV、预览和字段清单
      serving_manifest.json     查询包版本、批次和 CSV 哈希
      acceptance_manifest.json  清洗明细、报告和分析附件的批次与文件校验清单
    analytics_sample_7d_v1/  原有 7 天统计交付包，独立批次
  spark_jobs/            真正执行清洗和统计的 PySpark 作业
  backend/               已实现的只读统计 HTTP API；模型尚未就绪时明确返回 503
  contracts/             OpenAPI、TS 类型、表结构、预测/模型契约和真实响应示例
  publishing/            CSV校验、便携包、不可覆盖的只读 SQLite 发布
  scripts/               一键运行数据层、生成实际接口示例
  frontend/              后续 Vue + ECharts 页面边界说明
  ml/                    预测任务、训练切分和防泄漏约定
  docs/                  运营指标调研、数据字典、参考资料分析、交接说明
  tests/                 生成/篡改/边界/真实 Spark 集成测试
  outputs/               本机运行结果（忽略，不提交）
```

## 三、先跑哪一步

在**仓库根目录**执行。按验收使用 Python 3.11 或 3.12；数据生成无需安装第三方库。

```sh
# 1. 已附数据，无需重生成；先检查小样本
python -m data_analysis.charging_data.validate --dataset data_analysis/datasets/charging_sample_7d_v2

# 2. 要演示生成过程时，必须给一个新的空输出目录
python -m data_analysis.charging_data.generator --config data_analysis/config/sample.json --output data_analysis/outputs/demo_sample_run1

# 3. 生成全量（耗时/空间更多，不要覆盖已有数据）
python -m data_analysis.charging_data.generator --config data_analysis/config/full.json --output data_analysis/outputs/demo_full_run1

# 4. 不安装 Spark 也能执行基础测试；集成测试会明确跳过
python -m unittest discover -s data_analysis/tests -v
```

同一配置和代码重复生成，CSV.gz 内容和 SHA256 相同。修改参数应更换 `dataset_id`，保留配置与 manifest。
不要手工编辑正式数据；损坏或需改规则时，在新目录重新生成并校验。
本版 `generator_version=2.0.0`、`schema_version=1.1.0`；manifest 还记录行为版本及参考文件 SHA。
旧 v1 批次仍可从 Git 历史恢复，但已从当前交付目录移除，避免组员误混旧数据。

### Spark 清洗和统计

准备 Java 17、Python 环境后：

```sh
python -m pip install -r data_analysis/requirements-spark.txt
python -m data_analysis.spark_jobs.pipeline --input data_analysis/datasets/charging_sample_7d_v2 --output data_analysis/outputs/spark_sample_run1
```

必须选择不存在的输出目录。全量只需改 `--input` 为正式数据目录。
真实 Spark 测试与 HDFS 命令见 [Spark 说明](spark_jobs/README.md)。
Windows 编写和调试 Python/Vue；Spark 运行时依赖 Java/Hadoop 环境，遇到本地 Hadoop 原生依赖问题时，
可先在老师的 Linux 环境运行同一作业，不要把普通文件夹当成 HDFS 截图交付。

## 四、数据如何流动

```text
Python 模拟同一批用户与电站事件
  ↓
raw：压缩 CSV（含有清单可追踪的脏记录）
  ↓ 上传老师的 HDFS；本地调试可先直接读文件
PySpark：明确类型 → 修正规范 → 隔离坏行 → 去重 → 分组统计
  ↓
clean / rejected / statistics：Parquet + 质量报告 + _SUCCESS
  ↓
经营统计 / 历史特征 / 独立未来标签 → CSV.gz + 批次清单
  ├─ 已实现只读 SQLite / Python API → 后续 Vue 智慧大屏
  └─ 后续模型训练与评估 → 按公共契约接入预测 API → 网页输入/输出
```

`reference_aggregates` 仅用于检验 Spark 计算是否正确，**Spark 不读取它来冒充计算结果**。
大屏通过 API 读取有 `_SUCCESS` 且已发布的统计批次，带上 datasetId/publishedBatchId 和数据时间，不能把历史模拟快照叫作实时运营。
HDFS 负责存储，Spark 负责清洗与计算；不需要为了这个数据规模搭多节点集群。

## 五、能分析哪些运营问题

| 问题 | 可用数据与结果 |
| --- | --- |
| 哪个城市/站点收入多，购电与维修花多少？ | 会话费用、成功支付/退款、分时购电、日常/维修成本 |
| 哪些站空闲，哪些时段拥挤？ | 逐桩状态、5 分钟负荷、站点类型、预约与排队 |
| 用户为什么没充上，等待多久？ | 初次到访请求及失败原因、叫号/退出/超时、预约结局 |
| 设备坏了多久，维修进度如何？ | 故障类别、维修四阶段、停用和恢复时间 |
| 什么影响充电负荷与复购？ | 时段、星期、天气、事件日、用户分群和使用历史 |
| 哪些曲线异常，能否训练预测？ | 电压/电流/SOC/温度、电量、降功率/温升/提前停止标签 |

具体指标公式、分母、业务边界和调研来源见 [运营指标](docs/operator_metrics.md)。
字段单位及清洗规则见 [数据字典](docs/data_dictionary.md)。
五人对接建议见 [开发交接](docs/development_handoff.md)。

## 六、展示与使用边界

- CSV 里金额是**分**，电量是 **Wh**；页面分别除以 100、1,000。原始记录用 UTC，报表按上海业务时间。
- 全量遥测超过 Excel 单表行数上限，不能用 Excel 打开一个分片就断言“全数据只有这些”；先看 preview 和 manifest，再用 Spark。
- 合成数据有设计规律，可用于教学验证；模型分数不证明真实城市预测能力，也不是电池安全诊断。
- 期末业务状态以校验报告为准。大屏若展示正在排队或维修，应选历史时间回放并明确标注，不得捏造当前数量。
- 原始会话约 2% 被选中进行一种脏数据操作；有的是修正原行，有的是额外副本，因此不是“所有表恰好 2% 坏行”。
- 全量明细以压缩分片直接入库，最大分片远小于 GitHub 单文件限制；后续模型、Parquet、构建产物不纳入 Git。
- 气象数据署名：Weather data by [Open-Meteo.com](https://open-meteo.com/); ERA5 from ECMWF / C3S，遵循 [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)。网页显示天气时也需保留来源链接。
