# 最终交付与验收

交付是一个 Vue 网页、一个 FastAPI 同源入口，以及相互隔离的只读统计 MySQL 库和演示业务 MySQL 库。
保留已有功能与数据，新增分析和 AI 参谋都在这套入口内使用。Python 3.11/3.12、Node 24、MySQL 8.4，
模型只用 CPU；重跑 Spark 时另外需要 Java 17 和 `requirements-spark.txt`。不需要 GPU。

## 交付范围

| 能力 | 最终入口与证据 |
| --- | --- |
| 经营统计和地图 | 运营总览；原发布批次、城市/日期筛选、只读 MySQL 查询 |
| 多维运营分析 | 总览内「时空与效率」「用户与服务」；七张 Spark 聚合表、执行计划、哈希和守恒检查 |
| 清洗挑战 | 独立 `datasets/cleaning_challenge_v1`；20 类质量问题经正式清洗器处理后的修复/隔离证据 |
| 已有机器学习 | 智能分析中的负荷/空闲桩预测、用户流失风险、结束会话异常复核；报告来自当前已验证工件 |
| 智能找站 | 起点、目标电量、到站/等待模型、推荐理由与路线预览；后端保留原业务兼容能力 |
| AI参谋 | 智能分析内的子页，`POST /api/v1/intelligence/advisor`；默认解释本地实际聚合与模型报告 |
| 模拟控制台 | 回放时钟与配对实验；独立业务库，不改统计批次 |

AI参谋不另起服务、不新增模型训练、不替代原有图表或预测页。离线模式没有模型供应商调用，也不需要Key。
原始业务数据是模拟的；“实际聚合/实际模型报告”是指由这些文件计算得到的证据，不是预填问答或现场运营实测。

以下两项不纳入本次交付，研究分支保留：

- **PR #74：未来七天报修预测。** 工单由固定概率随机生成，没有设备健康驱动的可学机制。
  分支报告在新种子批次的 TEST AUC 从 0.6219 降至 0.4366；发布批冻结阈值也未达到精确率目标。
  排程未按阈值过滤、每日预算只检查全期总额、观察截止使用最后工单日期等问题仍需修正。
  现阶段不能据此发布维修建议或“故障概率”。
- **PR #78：变压器扩容与选址。** 本批变压器容量 360 kW，复核最大负荷约 150.852 kW，无实际超限。
  新负荷模型的 MAE 和尖峰表现未胜过持续值基线；逐时静态分配尚未验证连续会话影响。
  空间留一验证没有正向收益；原审 `2d07340` 修正场模型置换配对后的独立复算 p 值约 0.17–0.26，
  不支持原显著性表述。追加源码复核 `6ff5613` 已修最近邻机制检验措辞及若干输入门禁，
  但场模型置换仍将置换输入与原标签比较；两种检验不能混用，本次未重训该研究分支。
  它适合保留为规划研究，不足以新增扩容决策入口。

## 数据和结论边界

业务数据来自 `analytics_full_180d_v1` 中的 180 天模拟发布批次，**不是 Caltech ACN-Data 实测充电会话**。
气象来源按原始清单单独说明；真实历史气象不使模拟支付、用户、维修或充电事件变成实测数据。
本地文件、MySQL 发布、分析包和模型工件必须绑定同一 `datasetId`、`publishedBatchId` 及来源哈希。

清洗挑战是主动构造的质量测试，不是全量污染率调查；挑战通过率不等于未知真实数据上的清洗准确率。
相关矩阵和瓶颈联动用于发现待复核问题，不证明因果或策略收益。异常筛查是已结束会话的辅助复核，
主要识别热异常且召回有限，不是实时电池保护、安全诊断或设备故障预测。流失分数不是校准概率。
不宣传“模型已经上线验证安全”“能够保证到站有桩”或“扩容一定提高收益”。

## 从已有交付包启动

以下命令在仓库根目录运行。MySQL 账号、两库发布、Windows PowerShell 的完整配置见
[统一启动说明](../delivery/README.md#2-首次准备)。不要重建或删除已有数据库。

Linux 首次准备：

```bash
python3.12 -m venv data_analysis/.venv
source data_analysis/.venv/bin/activate
python -m pip install -r data_analysis/requirements-delivery.txt
python -m data_analysis.publishing.acceptance_bundle --verify data_analysis/datasets/analytics_full_180d_v1
python -m unittest data_analysis.tests.test_advanced_bundle -v
python -m data_analysis.delivery.cli train
```

模型训练可能需要数分钟至十几分钟，须在展示前完成。它复用已清洗数据；完整旧工件会校验并复用，
损坏或中断目录应检查或改用新输出位置，不自动覆盖证据。AI参谋没有单独训练步骤。

配置只读 `ANALYTICS_MYSQL_*`、独立业务 `CHARGEPILOT_MYSQL_*` 和管理员令牌后：

```bash
cd data_analysis/frontend
npm ci
npm test
npm run build
cd ../..
python -m data_analysis.delivery.cli check
python -m data_analysis.delivery.cli serve
```

打开 `http://127.0.0.1:8000/`，接口说明在 `/docs`。静态网页与统计、预测、参谋接口共享该端口。
不需要另开参谋网页、独立HTTP服务或另配数据库。`check` 核对已接入模型和两库状态；
仍须在页面实际检查高级分析与参谋响应，不能用一次模型就绪检查替代全部验收。

已有 `datasets/advanced_analytics_v2` 和 `datasets/cleaning_challenge_v1` 可直接复核、展示，无须先重跑 Spark。
重算使用新目录，命令与产物解释分别见 [多维分析](advanced_analytics.md#4-架构与复现)、
[清洗挑战](advanced-cleaning.md#运行方法)。原批次、MySQL 库和训练输入不因挑战测试而改变。

## AI参谋的使用与外部调用

进入「智能分析 → AI参谋」，选择城市和日期，在当前发布批次上提出运营或模型问题；与总览对照时使用相同筛选范围。
响应应给出证据、来源和适用限制；缺少模型或聚合证据时明确说明，不能回填示例指标或编造原因。
切换筛选后再问，确认返回范围与新筛选一致。选择页面提供的下钻操作，应回到现有分析或模型内容。

在线是可选功能，由后端的专属 `ML_ADVISOR_*` 设置启用，并要求用户明确同意该次外部调用。
配置、请求协议和失败处理以 [AI参谋说明](../ml/advisor/README.md) 为准；默认验收不配置任何外部Key。
在线请求只发送固定问题意图、白名单聚合指标的数值/单位及固定提示，**不发送原始问题、自由备注、用户/会话明细**。
外部模型仅选择一个值得优先复核的指标，文字和数字仍由程序依据本地证据生成，不增加新的模型结论。
Key仅留在服务端，不读取其他工具的凭据，不进入前端、仓库或回答来源。

## 四分钟验收路线

前提：模型已经训练，MySQL已发布，网页已构建，服务已启动。四分钟是展示路径，不是从零安装时间。

| 时间 | 操作 | 要核对的结果 |
| --- | --- | --- |
| 0:00–0:35 | 展示批次及模拟数据说明，打开清洗挑战报告 | 20类问题、真实修复/隔离、行数守恒和费用偏差；不把挑战比例当作全量污染率 |
| 0:35–1:35 | 总览选择 `2026-05-23 ≤ 日期 < 2026-05-30`；切城市/站型，点击服务成功率格子 | 热力图、原因分布、等待/占位/配置随筛选变化；展示样本数、不同分母和缺失值 |
| 1:35–2:20 | 智能分析选历史整点，展示负荷或空闲桩预测，再打开异常复核 | 模型ID、当前TEST/基线、kW与桩数单位，以及风险和召回限制；不使用今天日期伪造历史预测 |
| 2:20–3:20 | AI参谋默认离线提出当前范围的运营问题，再询问模型适用限制 | 回答有本地证据和来源、范围一致；无需外部Key；不能将有限证据扩大为故障诊断 |
| 3:20–4:00 | 智能找站设置起点与目标电量，查看推荐和路线 | ETA/等待/推荐理由来自现有流程；路线不可用时有明确说明，不冒充实时库存 |

清洗挑战证据位于 `datasets/cleaning_challenge_v1/report.json`，多维聚合来源位于
`datasets/advanced_analytics_v2/advanced_manifest.json`。详细分析例子见
[本批可复现案例](advanced_analytics.md#7-本轮可直接演示的案例)，页面不能写死该示例数字。
真实腾讯路线和可选在线参谋均不属于无Key基础验收的前置条件。

## 验证与CI覆盖

以下是工作流执行的检查范围，不代表本次修改已在远端运行成功。工作流在所有PR上运行，不按文件路径跳过必需检查。

| CI任务 | 实际检查 |
| --- | --- |
| Python static checks | Ruff语法、未定义名称等错误检查；递归包含`ml`及`advisor`、`delivery`、`chargepilot`、`backend`、Spark脚本与Python测试；不引入整库格式改写 |
| generator / Linux、Windows | 轻量测试发现（Windows额外安装IANA时区数据）；包含真实高级分析/清洗挑战交付包哈希与守恒检查、挑战生成快速测试；另验证样例数据及完整清洗交接包 |
| API and contracts / Linux、Windows | 统计API、高级分析API、发布完整性、JSON Schema、OpenAPI/TypeScript契约漂移；Linux另跑真实MySQL发布/API/校验 |
| Spark / cleaning、dirty-parser、dashboard、ml-features、data-export | Java17与真实PySpark；清洗挑战在dirty-parser，高级分析在data-export；均设置`RUN_SPARK_TESTS=1` |
| Integrated delivery / ML and MySQL | 先实际训练全部既有CPU模型，再跑模型、小时预测、流失/异常、统一HTTP和并发MySQL业务集成；最后计算100用户配对仿真 |
| Advisor专门测试步骤 | 在完整交付依赖和本地工件下运行`test_ml_advisor`、`test_delivery_advisor`；核对本地证据和同源HTTP；在线协议用注入的测试传输，不使用真实Key |
| Integrated delivery / Vue | `npm ci`、Vitest、`vue-tsc`和Vite构建；高级分析筛选/格子联动、参谋操作/导航回归由`npm test`自动发现 |

基础环境的测试发现允许缺少可选运行时的用例跳过；不能将它当作ML、Spark或MySQL的完整测试成绩。
对应集成任务安装所需依赖并显式启用真实运行；完整交付MySQL测试要求模型就绪，缺失会失败。
新参谋不依赖旧`outputs/ml_anomaly`或未纳入交付的报修、扩容模型。

已安装交付依赖后的局部复核：

```bash
python -m unittest data_analysis.tests.test_advanced_bundle data_analysis.tests.test_advanced_api data_analysis.tests.test_cleaning_challenge -v
python -m unittest data_analysis.tests.test_ml_advisor data_analysis.tests.test_delivery_advisor -v
python -m data_analysis.contracts.delivery_schema --check
```

第一条里的Spark用例默认跳过。真实Spark复核在已安装`requirements-spark.txt`的Java17环境运行：

```bash
RUN_SPARK_TESTS=1 PYSPARK_PYTHON="$(command -v python)" python -m unittest data_analysis.tests.test_advanced_analysis data_analysis.tests.test_cleaning_challenge -v
```

完整集成还应运行工作流中的全部ML测试，并按测试说明设置专用`MYSQL_TEST_*`和`RUN_MYSQL_TESTS=1`执行
`data_analysis.tests.test_delivery_integration`。测试账号需具备创建临时测试库/账号的权限；不指向需保留的正式业务库。
前端运行`npm test`和`npm run build`。本地测试不能替代远端CI或最终Linux/HDFS环境复验。
原180天数据的HDFS执行记录见 [HDFS验证记录](hdfs_full_180d_validation.json)；
它不自动证明本轮新增的独立分析/挑战产物也已经在HDFS上运行过。
