# 第二阶段 CI 在检查什么

CI 是提交代码后由 GitHub 自动运行的验收，不是项目运行时的服务。原9项检查和保护名称保持不变，统一交付另增2项ML/MySQL和Vue检查。基础与API覆盖Linux/Python3.11及Windows/Python3.12，完整模型链在Linux/Python3.12实际训练与测试。

| GitHub 检查名称 | 检查内容 |
| --- | --- |
| `generator (ubuntu-22.04)` | Linux 下的数据生成、可复现性、文件哈希、关联和业务约束，以及不依赖 Spark/API 的基础回归；另外验证随库 7 天原始样本，以及最新 180 天交付包的文件完整性和批次一致性 |
| `generator (windows-latest)` | 同一套基础检查在 Windows 上运行，发现路径、编码、文件权限等平台差异 |
| `API and contracts / ubuntu-22.04` | 实际启动 MySQL 8.4 容器，检查新库发布、事务完整性、只读 API 和汇总核对；显式运行`test_advanced_store`验证七表副本完整性、非覆盖与提交回执丢失后安全重试；另跑原有 SQLite 离线兼容、数据包完整性、接口分页/错误、未来标签隔离、OpenAPI/TS 和 JSON Schema 校验 |
| `API and contracts / windows-latest` | 可移植接口/契约、`test_advanced_store`独立内存数据库回归与 SQLite 离线兼容；不启动 MySQL 容器，不以此声称 Windows MySQL 服务已验收 |
| `Spark / cleaning` | 实际启动 Java/PySpark，验证类型转换、Unicode 格式修正、错误隔离、去重、六维质量审计、行数守恒、Parquet 输出和独立统计对账 |
| `Spark / dirty-parser` | 注入的坏记录能被识别；NaN/Infinity、整数列中的小数文本不能被静默接受 |
| `Spark / dashboard` | 经营统计口径：加权利用率、缺失采样、跨站用户去重、排队/维修的分组日期和分母 |
| `Spark / ml-features` | 历史窗口连续、不得混入未来数据、24 小时标签、训练/验证/测试时间边界 |
| `Spark / data-export` | 小规模模拟数据实际跑生成到清洗、导出、便携包及 SQLite 离线兼容链路；验证错批、坏哈希、未完成批次和覆盖保护；另验证 11 维分析、3 组双维对比和空分母。正式 MySQL 导入单独在 API 任务检查 |
| `Integrated delivery / ML and MySQL` | 从完整CLEAN实际训练到站、负荷、空闲桩、流失/异常模型；验证无未来泄漏、批次/工件校验、真实MySQL统一HTTP、行程状态机及并发；显式运行参谋的`test_advisor_analysis`、`test_advisor_knowledge`、`test_advisor_output`、RAG与HTTP测试，最后运行有界配对仿真 |
| `Integrated delivery / Vue` | Node24安装锁文件、运行前端回归、TypeScript检查、生产构建；覆盖筛选/错批/过期响应与预测字段边界 |

基础任务不安装 API、JSON Schema 或 Spark 依赖，也不启动 MySQL，对应测试会明确跳过；专门的 API 和 Spark 任务负责真正运行它们。真实 MySQL 测试由 `RUN_MYSQL_TESTS=1` 启用，使用一次性测试服务和独立测试库，不连接项目正式数据库。
检查任务不是 9 个应用，也不是每次提交都重复运行 9 遍 180 天全量。自动测试主要使用小规模、确定性数据；全量数据验证另有批次记录。
新增的随库全量包检查读取文件清单并计算 SHA256，不重跑 180 天 Spark 作业，也不把文件哈希检查当作重新计算业务统计或真实 HDFS 验收。
参谋测试核对完整等长期间、成组证据预算、16段知识的实际BM25检索、来源引用及主动在线发送的授权字段。
引用回归检查精确中文/组引用的安全规范化、从正文形成UI列表、未知/畸形/无正文引用拒绝；历史回归检查旧标记移除、本题知识优先及总结时按当前范围重新取证。
输出解析回归核对JSON包装和裸换行的安全容错、有效转义保真、重复键/非有限数/控制字符拒绝、字节与深度上限；
协议测试核对规划/生成每阶段共享一次纠错、总预算、不可重试错误和仅含安全元数据的诊断日志，前端另测原问题主动重试及草稿保护。
在线模型响应全部注入，不读取真实Key、不产生供应商请求；七表安装单测也不接触现有数据库，真实部署仍需单独验收。
自动化通过不保证外部供应商始终可用，也不意味着模型每条解读均正确。

MySQL 服务仅在 Ubuntu 使用 `mysql:8.4`，Windows 的服务镜像为空所以不启动容器；宿主端口由 GitHub 动态分配，防止与已有服务抢占 3306。这个条件服务配置遵循 [GitHub Actions 服务容器语法](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#jobsjob_idservicesservice_idimage)。CI 内的固定密码仅用于临时测试容器，不是部署密码。

## 为什么第二阶段还有 Ubuntu

Windows 检查面向日常开发；Linux 检查用于服务器兼容性和真实 Spark 执行。
用户要求移除的是第一阶段 `Ubuntu 22.04 / Qt 6.2.4` 检查，对应 `.github/workflows/ci.yml`。
第二阶段 `.github/workflows/data-analysis.yml` 继续保留 Linux 任务，但不安装或编译 Qt。
一期代码、QtTest、QML/数据库验收脚本未删除，仍可手工运行；此 PR 的工作流删除可从 Git 历史恢复。

## 触发与合并规则

- 所有 PR，以及推送到 develop/master 时运行第二阶段检查；不使用整个工作流的路径过滤，避免只修改文档的 PR 因必需检查没有运行而一直等待。
- 同一 PR 再推送时，取消旧版本尚未完成的运行，以最新提交结果为准。
- develop/master 的必需检查从旧 Qt 名称迁移到以上 9 项。更新通过 GitHub 保护规则完成，YAML 本身不会自动修改这些设置。
- 原有审批人数、CODEOWNERS、最后一次推送后的独立审批、讨论解决、管理员限制和分支同步要求保持不变。

## 历史 Windows 失败原因（SQLite 离线发布）

发布器原来以只读文件句柄打开临时 SQLite 后调用 `fsync`，Windows 返回 `OSError: [Errno 9] Bad file descriptor`。
这同时导致基础测试和 API 测试中的发布用例失败，不是 Spark 统计算错。
修复采用可读写、不截断的私有临时文件句柄，在正式发布和设置只读前完成刷盘。刷盘失败仍阻止发布；不会跳过 Windows 检查、忽略异常或取消正式查询库的只读保护。

绿色 CI 只表示这些自动检查通过，不代表 Vue 页面、模型训练或老师环境的 HDFS 已完成验收。
