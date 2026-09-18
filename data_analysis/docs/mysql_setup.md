# 第二阶段 MySQL 8.4 接入与发布

第二阶段正式查询库改为 **MySQL 8.4**，FastAPI 的 URL、参数、返回字段和 OpenAPI 保持不变。第一阶段 Qt 使用的 SQLite 不改。原始数据、Spark 清洗/统计、HDFS 和 ML 特征/标签继续使用原有文件及契约，不需要重新生成 583 万条原始记录。

```text
原始文件 / HDFS → Spark 清洗与统计 → export 数据包
                                      ├─ MySQL 新批次库 → FastAPI → Vue 大屏
                                      └─ 特征 + 独立标签 → 模型训练
```

MySQL 存网页查询所需的统计和目录数据，不替代 HDFS，也不把全部原始表搬入网页查询库。未来标签不进入 MySQL 查询表。一个数据库对应一个完整批次；本项目暂不做多批次自动切换、在线增量更新或自动删旧库。

## 1. 安装与准备

使用 Python 3.11 或 3.12，在仓库根目录安装：

```text
python -m pip install -r data_analysis/requirements-api.txt
```

- **Windows**：安装 MySQL Community Server **8.4** 的 MSI，执行配套 MySQL Configurator，设置管理员密码和 Windows 服务。仅安装 Workbench 不等于安装数据库服务。参见 [MySQL Windows 安装说明](https://dev.mysql.com/doc/refman/8.4/en/windows-installation.html)。
- **Linux**：按发行版安装 MySQL 8.4；Ubuntu/Debian 可使用官方 APT 仓库并选择 8.4 系列，不要把系统默认包一定当作 8.4。安装步骤见 [MySQL APT 仓库说明](https://dev.mysql.com/doc/refman/8.4/en/linux-installation-apt-repo.html)。已有教师环境不要重新初始化数据目录或覆盖原服务。
- 开发环境让 MySQL 仅监听 `127.0.0.1`，默认端口 `3306`；端口被占用时改为实际配置值。不要为省事开放公网 3306。发布器使用 `utf8mb4`，保留中文与完整字符；表使用 InnoDB。
- 用管理员客户端执行 `SELECT VERSION();` 保存真实服务端版本。`mysql --version` 只说明客户端版本，不能代替服务端验证。

本地 MySQL 服务、学校 Linux MySQL 服务和 HDFS 是不同验收项目，代码或 CI 通过不代表学校环境已经安装完成。

## 2. 分开导入账号与查询账号

以下以本轮新库 `charging20260914run1` 为例。数据库名须为小写字母开头、只含小写字母/数字/下划线，最长 64 字符，不能使用 MySQL 系统库。示例采用纯字母数字，避免库级授权中的下划线通配含义。每一轮使用一个新库名。

管理员通过受信 MySQL 客户端创建两个账号；密码在本机客户端填写，不把含密码的 SQL 保存到仓库、截图或录屏里：

```sql
CREATE USER 'charging_import'@'127.0.0.1' IDENTIFIED BY '<本机设置的导入密码>';
CREATE USER 'charging_read'@'127.0.0.1' IDENTIFIED BY '<不同的查询密码>';
GRANT CREATE, CREATE TEMPORARY TABLES, INSERT, SELECT, INDEX, REFERENCES
ON `charging20260914run1`.* TO 'charging_import'@'127.0.0.1';
GRANT SELECT ON `charging20260914run1`.* TO 'charging_read'@'127.0.0.1';
```

这些是占位示例，不是可复用的默认密码。账号已存在时由管理员检查并调整，不要盲目删除账号重建。账号的主机部分须对应 MySQL 实际识别的连接来源；若启用主机名解析使本机连接匹配 `localhost`，请管理员核对账号匹配，不要直接改成 `%`。下一轮只给所需新库授权，不授予全局 `*.*` 权限。

**不要提前创建目标数据库。** 发布器会自行创建空的新库；目标库只要已存在就拒绝，不清空、不覆盖、不自动删除。API 使用 `charging_read`，不要把导入账号或 root 用于正式网页服务。权限背景见 [MySQL 权限说明](https://dev.mysql.com/doc/refman/8.4/en/privileges-provided.html)。

## 3. 配置环境变量

发布器与 API 使用同一组变量，但应分别运行在各自终端，使用各自账号：

| 环境变量 | 示例 / 含义 |
| --- | --- |
| `ANALYTICS_MYSQL_HOST` | `127.0.0.1` |
| `ANALYTICS_MYSQL_PORT` | `3306`，修改过端口则填实际值 |
| `ANALYTICS_MYSQL_DATABASE` | 本轮未存在的新库 `charging20260914run1` |
| `ANALYTICS_MYSQL_USER` | 导入时 `charging_import`，API 时 `charging_read` |
| `ANALYTICS_MYSQL_PASSWORD` | 本机交互输入对应账号密码 |
| `ANALYTICS_MYSQL_SSL_CA` | 可选：受信 CA 文件路径；受控远程连接时启用证书与主机名校验 |

Windows PowerShell（导入终端）：

```powershell
$env:ANALYTICS_MYSQL_HOST = "127.0.0.1"
$env:ANALYTICS_MYSQL_PORT = "3306"
$env:ANALYTICS_MYSQL_DATABASE = "charging20260914run1"
$env:ANALYTICS_MYSQL_USER = "charging_import"
$env:ANALYTICS_MYSQL_PASSWORD = ([System.Net.NetworkCredential]::new("", (Read-Host "MySQL 导入密码" -AsSecureString))).Password
```

Linux Bash（导入终端）：

```bash
export ANALYTICS_MYSQL_HOST=127.0.0.1
export ANALYTICS_MYSQL_PORT=3306
export ANALYTICS_MYSQL_DATABASE=charging20260914run1
export ANALYTICS_MYSQL_USER=charging_import
read -r -s -p "MySQL 导入密码: " ANALYTICS_MYSQL_PASSWORD
export ANALYTICS_MYSQL_PASSWORD
```

不要将真实密码直接写进命令行参数、Git、公共 `.env`、连接 URL 或日志。环境变量仍是当前进程的敏感配置，不要打印整个环境；用完导入终端可关闭。更大范围部署需要额外的密钥管理与受控网络，当前示例限定本机教学使用。

## 4. 发布已有全量数据，不必重跑 Spark

在配置了**导入账号**的终端执行：

```text
python -m data_analysis.publishing.mysql_publish --input data_analysis/datasets/analytics_full_180d_v1 --report data_analysis/outputs/mysql_run1_publish.json
```

输入也可以是当前已完成的 `export/` 目录，或 `analytics_sample_7d_v1` 小样本包。报告使用新的文件路径，不覆盖既有报告。

发布顺序是：校验输入文件/契约/哈希 → 创建新库和表 → 事务内导入、核对行数/关联/统计 → 同一事务提交就绪标志。建表操作与数据导入的边界必须分开，不能假设 MySQL 的 DDL 可以像普通写入一起回滚；参见 [MySQL 隐式提交说明](https://dev.mysql.com/doc/refman/8.4/en/implicit-commit.html)。

出错时不切换 API、不删除旧库。失败的新库可能保留表结构，但不会成为可查询的完整就绪批次；保留诊断，换一个新库名重试。不要手工补就绪标志。库的清理由管理员核对精确目标后另行处理，工具不自动删库。

如果恰在提交时连接中断，或者数据库已提交但本地报告写入失败，客户端不能据此断言数据库未提交。此时先由管理员核对该新库的批次标识，再用只读账号运行核对脚本；不要覆盖同名库、手工补标志或自动切换 API。

### 追加七表高级统计

基本统计库已经完整发布后，可将同一批次的 `advanced_analytics_v2` 七张 Spark 聚合完整安装到**这个已有库**。
这一步使用 `publishing.advanced_mysql`，不是上面的新库发布器；不重新生成原始数据、不改原有查询表、不新建或删除数据库。
目标库必须与成果包的 `datasetId/publishedBatchId/pipelineRunId/sourceManifestSha256` 全部匹配，不能把其他批次补进当前库。

在本机编辑 Git 忽略的 `data_analysis/.env.mysql-import.local`。以下全部是配置格式示例，账号、库名和密码应改为自己已有且获授权的值：

```dotenv
ANALYTICS_MYSQL_HOST=127.0.0.1
ANALYTICS_MYSQL_PORT=3306
ANALYTICS_MYSQL_DATABASE=charging20260914run1
ANALYTICS_MYSQL_USER=charging_import
ANALYTICS_MYSQL_PASSWORD='<本机已有导入账号的密码>'
ANALYTICS_MYSQL_SSL_CA=
```

文件只接受这六个字面量配置项；不会执行 shell、展开变量或加载其他应用凭据。不要打印、提交、截图或共享此文件。
Linux 可执行 `chmod 600 data_analysis/.env.mysql-import.local`；Windows 使用文件权限限制为本人可读写。
进程中显式设置的同名 `ANALYTICS_MYSQL_*` 优先于文件，包括空密码；请在独立导入终端核对配置来源，避免误用正在服务的只读账号。
导入账号只需目标库的 `CREATE/INSERT/SELECT` 等所需权限，不需要 `DROP/DELETE/UPDATE`；不要临时扩权网页账号或混用独立业务库账号。

在仓库根目录执行：

```text
python -m data_analysis.publishing.advanced_mysql --input data_analysis/datasets/advanced_analytics_v2 --config data_analysis/.env.mysql-import.local
```

安装新增 `adv_station_day/adv_station_hour/adv_attempt_flow/adv_session_segments/adv_retention/adv_user_behavior/adv_service_hour`
和 `adv_publication` 就绪清单。每行保留规范化 JSON 及哈希，额外派生站点、城市和日期索引；留存按原 `ALL/CITY/STATION` 范围及 cohort 月处理，不伪造站点。
表结构先建，七表内容与 `READY` 清单在一个事务内写入并读回核验；成功输出 `PUBLISHED`、指纹和各表实际行数。
相同完整发布再运行只核验并返回 `ALREADY_PUBLISHED`，不重复写入；不一致、损坏、半安装或占用 `adv_*` 名称的其他表均拒绝覆盖和自动修复。

MySQL DDL 不能随数据事务回滚，失败可能留下空的 `adv_*` 表；原有表仍不被覆盖，部分高级统计会被 reader 明确拒绝。
不要手工补 `READY`、删表重试或声称所有建表均已撤销；由管理员核对精确目标后另行处理。
提交回执丢失也不能证明未提交：先新连接只读核验，或用相同输入重新运行上述命令，完整已提交状态会返回 `ALREADY_PUBLISHED`，半安装状态仍拒绝继续。

API 继续使用只读账号，须拥有**该库 `.* SELECT`**，确保能看见新表；仅旧表逐表授权不足以识别新安装。
新请求在同一个只读批次快照中优先完整校验数据库副本；只有整个 `adv_*` 扩展完全不存在时，才使用原来的已校验文件包。
任何半安装、坏哈希、错误索引或错批都报错，不转回文件掩盖问题。安装后用网页账号复核 `/api/v1/dashboard/advanced` 及参谋的范围、数值、来源和延迟。
这些完整聚合留在本地 MySQL；在线参谋只按问题选出有界分析证据及知识片段，不把七表全量发给模型。

## 5. 使用只读账号启动 API，并验证

在新的 API 终端配置同一主机/端口/数据库，把 `ANALYTICS_MYSQL_USER` 改为 `charging_read`，交互输入查询密码，再执行：

```text
python -m uvicorn data_analysis.backend.app:app --host 127.0.0.1 --port 8000
```

打开 `http://127.0.0.1:8000/docs`，先检查 `/api/v1/health`，再查城市、概览和图表。默认 API 使用 MySQL；缺配置、连接失败或批次未发布完成返回安全的 503，不自动回退到旧 SQLite，不在响应中暴露数据库凭据。

使用查询账号核对真实 MySQL 数据和 FastAPI 响应：

验证器只接受直接授予 `SELECT`（及无额外权限的 `USAGE`）的账号，不接受角色继承、写权限或 `GRANT OPTION`。它先检查当前角色与授权清单，再安全验证零行写入被拒绝；不能仅凭一次 UPDATE 失败就断言没有 DELETE 等其他权限。

```text
python -m data_analysis.scripts.verify_serving --database-backend mysql --bundle data_analysis/datasets/analytics_full_180d_v1 --output data_analysis/outputs/mysql_run1_api_verification.json
```

核对器在只读账号上尝试一次影响零行的 `UPDATE`，要求数据库权限拒绝该操作；它不修改业务记录，也不会因连接恰好处于只读事务而让高权限账号通过。随后对比接口与 SQL 汇总、批次/未来标签隔离，并以排序后的逻辑行内容哈希核对前后数据未改变，而不是访问 MySQL 的物理数据文件。

需要给网页组生成本批真实响应示例时，同样使用只读账号与新目录；默认读取已配置的 MySQL：

```text
python -m data_analysis.scripts.export_contract_examples --output data_analysis/outputs/mysql_run1_examples
```

该工具不会改写已提交的公共契约；只有显式传入 `--database 文件.sqlite3` 才使用旧 SQLite 离线兼容路径。

默认 CORS 仅允许 `http://localhost:5173` 和 `http://127.0.0.1:5173`；其他来源通过 `ANALYTICS_CORS_ORIGINS` 明确配置。CORS 不是登录鉴权，服务目前不带账号鉴权，不能直接暴露公网。多个组员访问时由负责人配置受限网络和来源，不共享 root 密码。

更新数据时，重复“新库授权 → 新库发布 → 使用新库完成验证”。确认成功后修改 API 的 `ANALYTICS_MYSQL_DATABASE` 并重启；旧库保留，必要时改回旧库并重启回退，不做库内混批覆盖。

## 6. 从原始数据重新运行，以及离线兼容

`run_data_layer` 和 `run_acceptance` 默认发布 MySQL，先配置导入账号及一个尚未创建的新库：

```text
python -m pip install -r data_analysis/requirements-spark.txt -r data_analysis/requirements-api.txt
python -m data_analysis.scripts.run_acceptance --input data_analysis/datasets/charging_sample_7d_v2 --output data_analysis/outputs/acceptance_mysql_run1 --driver-memory 3g
```

所有输出目录仍必须是新目录。没有 MySQL 的离线测试可以显式加 `--database-backend sqlite`；旧的 `publishing.publish --input ... --output ...sqlite3` 和 `verify_serving --database ...sqlite3` 仅保留为离线兼容，不是正式默认部署路径。API 的显式 SQLite 路径构造供这些兼容测试使用，旧 `ANALYTICS_DB` 不作为默认服务入口。

## 7. 验证边界

CI 保留原有九个检查名称。Ubuntu 的 API 检查启动真实 MySQL 8.4，执行发布、查询、隔离和核对测试；Windows 检查执行可移植 API/契约与 SQLite 离线兼容回归，不伪称 Windows CI 已跑 MySQL 服务。CI 临时数据库测试账号仅用于一次性测试容器，不可复制为真实部署密码。

2026-09-13 的清洗/全量核对记录仍有效描述对应 Spark 与 SQLite 批次，但不是这次 MySQL 的实测证明。2026-09-14 已完成真实 MySQL 8.4.11 验证：[本轮验证摘要](mysql_migration_validation.json)。180 天统计包的 9 张查询表共导入 349,056 行，59 次 API 核对通过；另已从 7 天原始样本重新执行 Spark 清洗、统计、MySQL 发布与查询核对。本轮使用样本已有脏记录，未额外执行新一轮脏数据注入。

38 项 MySQL 相关测试全部通过，其中 22 项连接真实数据库；默认测试发现 264 项，执行通过 201 项，跳过 63 项需要单独启用的 Spark/MySQL 测试。远程结果以相应 PR 的 CI 为准。

2026-09-15 已在老师 Linux `master` 的 MySQL 8.4.11 上，将下载的 180 天 HDFS export 发布为新库 `charging20260915hdfsfull1`，再用只读账号完成 59 次 SQL/API 核对，并以 loopback HTTP 实测 `/docs`、health 与运营概览。完整批次、监听范围、导入行数和限制见 [HDFS 全量 MySQL 验证记录](mysql_hdfs_full_180d_validation.json)。该记录不把 Vue 页面、训练后模型或对外网络部署说成已验收。

同日，组长整合后的统一交付已在老师 Linux `master` 完成四类模型训练、独立统计/业务 MySQL、Vue 构建、统一服务和两种网页预测的 loopback 验收。它使用仓库正式交付批次 `analytics-298aa3ee1401461fb06ea2bb96930dcf`，与上段 HDFS export 的 `analytics-57a...` 是不同的发布批次，不能混为同一批验收；完整记录见 [Linux 统一交付验证记录](linux_integrated_delivery_validation_20260915.json)。
