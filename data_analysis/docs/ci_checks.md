# 第二阶段 CI 在检查什么

CI 是提交代码后由 GitHub 自动运行的验收，不是项目运行时的服务。目前有 9 个检查任务：

| GitHub 检查名称 | 检查内容 |
| --- | --- |
| `generator (ubuntu-22.04)` | Linux 下的数据生成、可复现性、文件哈希、关联和业务约束，以及不依赖 Spark/API 的基础回归；另外验证随库 7 天原始样本 |
| `generator (windows-latest)` | 同一套基础检查在 Windows 上运行，发现路径、编码、文件权限等平台差异 |
| `API and contracts / ubuntu-22.04` | 发布只读 SQLite、数据包完整性、接口查询/分页/错误、未来标签隔离、OpenAPI/TS 一致性和 JSON Schema 格式校验 |
| `API and contracts / windows-latest` | 同一套接口与发布检查在 Windows 上运行，确保网页组的开发环境能用 |
| `Spark / cleaning` | 实际启动 Java/PySpark，验证类型转换、错误隔离、去重、Parquet 输出和独立统计对账 |
| `Spark / dirty-parser` | 注入的坏记录能被识别；NaN/Infinity、整数列中的小数文本不能被静默接受 |
| `Spark / dashboard` | 经营统计口径：加权利用率、缺失采样、跨站用户去重、排队/维修的分组日期和分母 |
| `Spark / ml-features` | 历史窗口连续、不得混入未来数据、24 小时标签、训练/验证/测试时间边界 |
| `Spark / data-export` | 小规模真实数据从生成到清洗、导出、便携包、查询库的整条链路；验证错批、坏哈希、未完成批次和覆盖保护 |

基础任务不安装 API、JSON Schema 或 Spark 依赖，对应测试会明确跳过；专门的 API 和 Spark 任务负责真正运行它们。
检查任务不是 9 个应用，也不是每次提交都重复运行 9 遍 180 天全量。自动测试主要使用小规模、确定性数据；全量数据验证另有批次记录。

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

## 本次 Windows 失败原因

发布器原来以只读文件句柄打开临时 SQLite 后调用 `fsync`，Windows 返回 `OSError: [Errno 9] Bad file descriptor`。
这同时导致基础测试和 API 测试中的发布用例失败，不是 Spark 统计算错。
修复采用可读写、不截断的私有临时文件句柄，在正式发布和设置只读前完成刷盘。刷盘失败仍阻止发布；不会跳过 Windows 检查、忽略异常或取消正式查询库的只读保护。

绿色 CI 只表示这些自动检查通过，不代表 Vue 页面、模型训练或老师环境的 HDFS 已完成验收。
