# 第二阶段：充电运营大数据分析基础

本目录独立于原 Qt 客户端和服务端，不修改一期业务数据库、协议或 Qt 构建。
按第二阶段计划，先准备**可复现的模拟数据 → Spark 清洗统计 → HDFS 存储验证**，
再由小组开发 Vue 大屏、Python 查询接口和预测模型。

本次已实现数据生成、关联校验、真实 Spark 清洗及两种统计；**还没有实现完整网页或训练模型**。
HDFS 接入代码已预留实际路径参数，但老师提供的 Linux/Hadoop 环境仍需单独验证。

## 一、可以直接使用的内容

- 5 个城市：大连、沈阳、北京、上海、深圳；每城 5 个模拟站，每站 3 个桩，共 25 站、75 桩。
- 180 天正式数据：2025-12-01 至 2026-05-29（上海业务日），6,000 个虚拟用户及车辆。
- 22 张关联业务表：订单之外，还包括请求失败、支付退款、排队、预约、维修、成本、天气、电池等。
- 5 分钟连续桩状态和电量，包含空闲、充电、预约占用、充后占位、维护、离线。
- 7 天小样本，适合组员快速开发；**与全量属于不同批次，不要合并统计**。
- 压缩 CSV、字段结构、文件 SHA256、预览 CSV；代码只依赖 Python 标准库即可生成和校验。
- PySpark：枚举清洗、无效会话隔离、去重、Parquet 输出、质量报告、站点小时表和日报。

全部记录是程序独立模拟，不含真实姓名、手机、车牌或原参考文件的记录。
站名、品牌、活动、天气、资费均为模拟；坐标为城市周边示意坐标，并非真实电站地址。

正式版已生成 **6,075,972 行原始记录**（含已标注脏副本），其中 129,768 条有效充电会话、
3,888,000 条桩采样、1,600,654 条电池采样。完整数据目录约 71.4 MB（十进制），已随仓库提供。
各表行数、五城市对比及实际统计示例见 [数据统计快照](docs/dataset_profile.md)。
15 项基础测试、7 项真实 Spark 集成测试通过；全量 Spark 的 108,000 行小时统计和 4,500 行日报
与独立对照值逐字段一致，摘要见 [全量 Spark 验证](spark_jobs/full_validation_summary.json)。

## 二、目录负责什么

```text
data_analysis/
  charging_data/          Python 生成器、统一字段、压缩写入、独立校验器
  config/                正式版/小样本的随机种子、天数和人数
  datasets/              已生成的完整数据和小样本，拉取后可直接使用
    <dataset_id>/
      raw/               22 张原始表，按月分片的 CSV.gz
      preview/           每张表前 12 行，普通 CSV，便于查看字段
      reference_aggregates/  Python 独立小时/日报对照值，不是 Spark 输出
      manifest.json      版本、来源、记录数、文件校验值、时间范围
      schema.json        每张表的精确字段顺序
      validation_report.json  独立检查结果及状态分布
  spark_jobs/            真正执行清洗和统计的 PySpark 作业
  backend/               后续大屏查询/模型服务的接口边界说明
  frontend/              后续 Vue + ECharts 页面边界说明
  ml/                    预测任务、训练切分和防泄漏约定
  docs/                  运营指标调研、数据字典、参考资料分析、交接说明
  tests/                 生成/篡改/边界/真实 Spark 集成测试
  outputs/               本机运行结果（忽略，不提交）
```

## 三、先跑哪一步

在**仓库根目录**执行。建议 Python 3.10–3.12；数据生成无需安装第三方库。

```sh
# 1. 已附数据，无需重生成；先检查小样本
python -m data_analysis.charging_data.validate --dataset data_analysis/datasets/charging_sample_7d_v1

# 2. 要演示生成过程时，必须给一个新的空输出目录
python -m data_analysis.charging_data.generator --config data_analysis/config/sample.json --output data_analysis/outputs/demo_sample_run1

# 3. 生成全量（耗时/空间更多，不要覆盖已有数据）
python -m data_analysis.charging_data.generator --config data_analysis/config/full.json --output data_analysis/outputs/demo_full_run1

# 4. 不安装 Spark 也能执行基础测试；集成测试会明确跳过
python -m unittest discover -s data_analysis/tests -v
```

同一配置和代码重复生成，CSV.gz 内容和 SHA256 相同。修改参数应更换 `dataset_id`，保留配置与 manifest。
不要手工编辑正式数据；损坏或需改规则时，在新目录重新生成并校验。

### Spark 清洗和统计

准备 Java 17、Python 环境后：

```sh
python -m pip install -r data_analysis/requirements-spark.txt
python -m data_analysis.spark_jobs.pipeline --input data_analysis/datasets/charging_sample_7d_v1 --output data_analysis/outputs/spark_sample_run1
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
  ├─ 后续 Python API → Vue 智慧大屏
  └─ 后续训练特征 → 模型评估 → 预测 API → 网页输入/输出
```

`reference_aggregates` 仅用于检验 Spark 计算是否正确，**Spark 不读取它来冒充计算结果**。
大屏将来应读取有 `_SUCCESS` 的统计批次，带上 dataset_id 和数据时间，不能把历史模拟快照叫作实时运营。
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
- 当前全量期末排队已结束、维修均恢复。大屏若展示正在排队或维修，应选历史时间回放并明确标注，不得捏造当前数量。
- 原始会话约 2% 被选中进行一种脏数据操作；有的是修正原行，有的是额外副本，因此不是“所有表恰好 2% 坏行”。
- 全量明细以压缩分片直接入库，最大分片远小于 GitHub 单文件限制；后续模型、Parquet、构建产物不纳入 Git。
