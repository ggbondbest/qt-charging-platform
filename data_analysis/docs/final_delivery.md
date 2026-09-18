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
| AI参谋 | 全站人物聊天；`POST /api/v1/intelligence/advisor` 在线检索当前聚合、模型报告和项目知识后生成带引用回答，保留本地统计问答 |
| 模拟控制台 | 回放时钟与配对实验；独立业务库，不改统计批次 |

AI参谋不另起服务、不新增模型训练、不替代原有图表或预测页。离线模式没有模型供应商调用，也不需要Key。
网页在有效在线配置存在时默认在线，未配置时默认本地统计问答；网页保留外发说明，在线模式主动发送授权本次外发，不再逐次勾选同意框。
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
七表完整聚合可按 [MySQL追加安装说明](mysql_setup.md#追加七表高级统计) 装入当前匹配统计库，不覆盖原表。
已安装则优先使用数据库副本，只有整个扩展未安装时才用已校验文件；损坏、半安装和错批不会回退。
重算使用新目录，命令与产物解释分别见 [多维分析](advanced_analytics.md#4-架构与复现)、
[清洗挑战](advanced-cleaning.md#运行方法)。原批次、MySQL 库和训练输入不因挑战测试而改变。

## AI参谋的使用与外部调用

点击右下角的鲸鱼娘人物展开对话，在「范围与设置」中选择城市和日期，在当前发布批次上提出运营或模型问题；与总览对照时使用相同筛选范围。
人物形象沿用 PR #79 的 `whale-girl.png`，保留原 MIT 署名。点击人物、收起按钮或按 Esc 可收起；
切换页面和收起面板保留页内最近10组已完成问答，刷新页面清除。在线仅带同批次、同范围最近三组历史，
最多6条，每条800字符、合计4000字符；历史用于理解追问，不能改变统计范围或当作事实来源。
模型上下文会剥离可识别的旧引用标记，保留对话文字；历史数字不作为本期数值。总结此前统计时仍需按当前范围重新取证，不能直接复用旧答复的来源或数字。
关闭时停止接收未完成答复，但不能撤回已经发往供应商的请求。聊天区及展开的设置在面板内滚动，输入区固定在底部。
数据或口径解释应给出证据/知识引用、来源和适用限制；缺少模型或聚合证据时明确说明。
在线问候由模型生成，返回 `status=chat`，不添加统计核验标记或虚构证据。
切换筛选后再问，确认返回范围与新筛选一致。选择页面提供的下钻操作，应回到现有分析或模型内容。

首次在线配置：将 `data_analysis/ml/advisor/.env.example` 复制为同目录 `.env.local`，填入自己的
`ML_ADVISOR_API_KEY`，保留原数据库配置并重启 `python -m data_analysis.delivery.cli serve`。
模板已预设 `https://aiping.cn/api/v1`、`DeepSeek-V4.1-Flash`、启用值 `1` 和供应商名称AIPing。
后端自动读取这个专用文件，也可用进程变量 `ML_ADVISOR_ENV_FILE` 指定路径。进程中的同名变量优先，
包括 `ML_ADVISOR_ONLINE_ENABLED=0` 覆盖文件里的 `1`。已有文件直接编辑，避免覆盖原Key。
GET接口的 `onlineAvailable:true` 只表示配置格式有效，不能证明Key、模型权限、额度和供应商网络已验证。
自动化检查使用注入的响应，不使用真实AIPing Key；当前配置环境另已使用用户自己的Key，实测
`DeepSeek-V4.1-Flash` 的规划、问候及基于MySQL发布聚合的完整RAG回答，并核对返回的指标引用。
这证明这些用例在当前环境可用，不保证每个追问或模型判断正确；其他部署接好自己的Key后仍须在页面另验。

在线模式先由模型从运营概况、瓶颈、站点、补能行为、模型说明五个受控主题中选择最多三个，
再按当前范围构建完整等长期间对比、失败原因/入口、优先关注站与对照、时段重点及补能群体的受控分析证据。
分析证据按主题成组分配，最多48项；加所选模型报告后最多64项。从本地维护的16段口径/方法知识中，中文BM25最多检索4段，每段最多1200字符。
本次问题的实际匹配优先，历史只补剩余空位，避免长旧答复盖过短追问，再由模型生成正文与引用。
原始文件和七表完整聚合留在本地；按问题取证，不把全量聚合或全部知识发给模型。历史不够时不截短前期，缺少观察不编造排名或改进收益。
正常回答通常需要两次模型调用，非流式整条返回。生成阶段明确提供本次允许的引用ID；
安全解析器容忍完整JSON代码块、BOM和字符串内裸换行等格式差异，保留有效转义；不补引号/逗号/缺括号，不截取说明中的对象。
规划和生成各最多两次尝试。JSON格式、字段结构、截断及引用错误共享每个阶段的一次额外纠错，总计最多四次供应商调用，
仍共享原60秒总预算，单次最多45秒且受剩余预算限制。纠错只重用原输入和证据，不回灌损坏草稿，不凭空替换引用或数据。
网络、认证、限流、内容过滤/拒绝、工具调用、外层协议错误或超大响应不自动重试。
精确完整ID的中文括号或组引用可规范化为独立 `[id]`；网页引用列表由规范化后正文实际出现的ID去重生成，
不要求模型正文与冗余数组原样相等，也不补入正文未引用的来源。正文或数组中的未知引用、畸形引用仍拒绝，不猜ID或改写数值。
分析正文必须真正引用本次聚合证据；知识解释可只引用本次项目口径，正文无引用时不能只靠数组放行。纠错后仍按同一规则核验，未通过则报错。
错误诊断日志仅记录阶段、固定原因码、尝试次数及是否可纠错，不记录问答、历史、密钥或供应商异常原文。
这些检查不保证完全消除幻觉，也无法绝对保证外部API持续可用；模型文字、数字解读与建议仍须结合来源复核，不能扩大为真实经营或安全结论。

网页展示外发说明；用户在在线模式点击发送或按Enter，即授权外发本次问题、有限历史、页面范围说明、所选聚合与检索知识。
仅打开面板、切换在线模式或选择示例不会调用模型；HTTP仍要求 `mode:"online", consent:true`，由网页主动发送时设置。
在线不检索或发送原始业务行、用户/会话明细及完整工件；识别出的敏感编号、凭据和本地路径会被拦截或移除。
不要在问题中填写个人信息或凭据。Key只留在后端认证，不读取其他工具的凭据，不进入前端或模型上下文。
收起或停止接收不能撤销已经发往供应商的调用。配置、请求契约和失败处理以 [AI参谋说明](../ml/advisor/README.md) 为准。
请求失败后的“重试发送”由用户主动重发原问题、原范围及原历史，保留已完成聊天和新草稿，不重新载入面板。
范围或模式改变后旧重试失效；只有初始化未载入才提供“重新载入”。主动重试是新的在线请求，可能产生额外调用。

## 四分钟验收路线

前提：模型已经训练，MySQL已发布，网页已构建，服务已启动。四分钟是展示路径，不是从零安装时间。

| 时间 | 操作 | 要核对的结果 |
| --- | --- | --- |
| 0:00–0:35 | 展示批次及模拟数据说明，打开清洗挑战报告 | 20类问题、真实修复/隔离、行数守恒和费用偏差；不把挑战比例当作全量污染率 |
| 0:35–1:35 | 总览选择 `2026-05-23 ≤ 日期 < 2026-05-30`；切城市/站型，点击服务成功率格子 | 热力图、原因分布、等待/占位/配置随筛选变化；展示样本数、不同分母和缺失值 |
| 1:35–2:20 | 智能分析选历史整点，展示负荷或空闲桩预测，再打开异常复核 | 模型ID、当前TEST/基线、kW与桩数单位，以及风险和召回限制；不使用今天日期伪造历史预测 |
| 2:20–3:20 | AI参谋选择本地统计问答，提出当前范围的运营问题，再询问模型适用限制 | 无Key基础路线；回答有本地证据、范围一致，不能将有限证据扩大为故障诊断；在线RAG另行验收 |
| 3:20–4:00 | 智能找站设置起点与目标电量，查看推荐和路线 | ETA/等待/推荐理由来自现有流程；路线不可用时有明确说明，不冒充实时库存 |

清洗挑战证据位于 `datasets/cleaning_challenge_v1/report.json`，多维聚合来源位于
`datasets/advanced_analytics_v2/advanced_manifest.json`。详细分析例子见
[本批可复现案例](advanced_analytics.md#7-本轮可直接演示的案例)，页面不能写死该示例数字。
真实腾讯路线和可选在线参谋均不属于无Key基础验收的前置条件。
在线RAG另验“你好”、当前范围运营问题、口径解释及同范围追问，核对模型答复和聚合/知识引用。
同时检查没有同意框仍可主动在线发送、仅展开/选示例不自动调用、切换范围不引用旧统计、错误配置能明确报错；为模型等待留出额外时间。
模拟一次失败后主动“重试发送”，确认原问题及历史不变、新草稿仍保留，且修改范围后不会重发旧范围请求。

## 验证与CI覆盖

以下是工作流执行的检查范围，不代表本次修改已在远端运行成功。工作流在所有PR上运行，不按文件路径跳过必需检查。

| CI任务 | 实际检查 |
| --- | --- |
| Python static checks | Ruff语法、未定义名称等错误检查；递归包含`ml`及`advisor`、`delivery`、`chargepilot`、`backend`、Spark脚本与Python测试；不引入整库格式改写 |
| generator / Linux、Windows | 轻量测试发现（Windows额外安装IANA时区数据）；包含真实高级分析/清洗挑战交付包哈希与守恒检查、挑战生成快速测试；另验证样例数据及完整清洗交接包 |
| API and contracts / Linux、Windows | 统计API、高级分析API、`test_advanced_store`七表规范化/完整性/非覆盖与提交回执丢失后安全重试、JSON Schema、OpenAPI/TypeScript契约漂移；Linux另跑真实MySQL发布/API/校验 |
| Spark / cleaning、dirty-parser、dashboard、ml-features、data-export | Java17与真实PySpark；清洗挑战在dirty-parser，高级分析在data-export；均设置`RUN_SPARK_TESTS=1` |
| Integrated delivery / ML and MySQL | 先实际训练全部既有CPU模型，再跑模型、小时预测、流失/异常、统一HTTP和并发MySQL业务集成；最后计算100用户配对仿真 |
| Advisor RAG retrieval, generation boundary and same-origin HTTP | 在完整交付依赖下显式运行`test_ml_advisor`、`test_advisor_analysis`、`test_advisor_knowledge`、`test_advisor_output`、`test_advisor_rag`、`test_delivery_advisor`；核对成组分析、16段知识、安全JSON解析、分阶段纠错预算、日志边界、引用和同源HTTP；在线协议用注入的测试传输，不使用真实Key |
| Integrated delivery / Vue | `npm ci`、Vitest、`vue-tsc`和Vite构建；高级分析筛选/格子联动、参谋操作/导航回归由`npm test`自动发现 |

基础环境的测试发现允许缺少可选运行时的用例跳过；不能将它当作ML、Spark或MySQL的完整测试成绩。
对应集成任务安装所需依赖并显式启用真实运行；完整交付MySQL测试要求模型就绪，缺失会失败。
新参谋不依赖旧`outputs/ml_anomaly`或未纳入交付的报修、扩容模型。

已安装交付依赖后的局部复核：

```bash
python -m unittest data_analysis.tests.test_advanced_bundle data_analysis.tests.test_advanced_api data_analysis.tests.test_advanced_store data_analysis.tests.test_cleaning_challenge -v
python -m unittest data_analysis.tests.test_ml_advisor data_analysis.tests.test_advisor_analysis data_analysis.tests.test_advisor_knowledge data_analysis.tests.test_advisor_output data_analysis.tests.test_advisor_rag data_analysis.tests.test_delivery_advisor -v
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
