# 充能智析赛事整合启动与验收

本轮把 Qt 业务和 Vue 运营中心接到同一条可验证的数据链路。用户在 Qt 完成预约、充电、支付后，Vue 从同一 SQLite 读取状态和汇总；历史分析、模型和 ChargePilot 回放继续使用各自的数据。所有充电、支付、站点与用户均为模拟。

本地修改位于 `codex/competition-integration-20261017`，基于 `67c2907b777749c73b5690568bdd6a89d7db4356`。项目负责人检查后已授权将最新整合版本提交 PR；源码与材料入口见 `docs/competition_submission/`。实际运行证据与环境限制见 [本轮验证记录](competition_validation.md)。

## 数据与智能能力的分工

| 入口 | 数据与能力 | 业务边界 |
| --- | --- | --- |
| Qt 用户端 | 既有找站、预约、充电、支付及原生地图配置 | 保留原有 TCP 业务流程，默认读取统一腾讯地图配置 |
| Qt 管理端运营中心 → 当前模拟业务 | 同一 SQLite 的只读事务快照 | 仅 COMPLETED 订单进入已付金额与电量，不含充值；保留整数分/Wh |
| 历史运营分析与预测 | 180 天模拟批次、MySQL、真实训练的 CPU 模型 | 1/6/24 小时预测、风险和异常复核只对原批次适用 |
| 历史策略演示 | ChargePilot 的独立模拟目录、回放时钟及到站模型 | 不将 ST-XX 映射为 Qt STA-XX，不创建 Qt 预约 |
| 历史 AI 运营参谋 | 原历史聚合、模型报告和知识引用 | 不接收 Qt 账户信息或当前业务行，不代表在线安全诊断 |

管理端仅保留一个“运营中心”入口：当前模拟业务展示 Qt 业务库的概览、趋势与明细；历史运营分析使用独立发布批次。两者在同一界面切换，金额、时间和站点身份不相加。模拟控制台直接进入，不需要额外管理员令牌；Qt 管理员登录和找站用户会话保持原有行为。

Qt 客户端业务流程沿用 master，随后按授权接入统一地图配置。本轮没有把历史 ML 预测绑定到 Qt 当前预约；后续接入需先核验站点身份、补齐同口径特征并验证模型，最终预约仍由 Qt 服务端执行。

## 构建与首次运行

命令均在仓库根目录运行。Python 使用 3.11/3.12，Node 使用 24；Qt 基线为 6.2.4，可在新版 Qt 本地构建，正式演示系统需重新运行验收。

```bash
python -m venv data_analysis/.venv
# Linux/macOS
source data_analysis/.venv/bin/activate
# Windows PowerShell: .\data_analysis\.venv\Scripts\Activate.ps1
python -m pip install -r data_analysis/requirements-api.txt
cd data_analysis/frontend
npm ci
npm test
npm run build
cd ../..
cmake -S . -B build-delivery -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCHARGING_BUILD_QML=ON -DCHARGING_EMBED_WEB_DASHBOARD=ON
cmake --build build-delivery --parallel 2
```

Qt 需 Core、Gui、Network、Sql、Widgets、Charts、Quick、QuickControls2、WebEngineQuick；嵌入管理端额外需 WebEngineWidgets。完整 Qt 依赖安装见根 README。上述续行使用 Bash；PowerShell 请合为一行或以反引号续行。

### 日常使用：只启动 Qt 管理端

完成首次安装和构建后，直接在 Qt Creator 运行 `charging_server`，或者打开编译好的管理端。Qt 在业务数据库就绪后自动启动 Python 网页服务，使用本机空闲端口并将实际地址交给“运营中心”。仅启动或失败时显示状态；就绪后网页铺满管理端内容区域，不再显示地址栏、外层标题和调试按钮。当前业务和历史分析均可进入大屏，按 Esc 恢复普通界面。退出管理端时只清理它自己启动的服务。

`http://127.0.0.1:<动态端口>/?embedded=operator` 是 Qt 内部加载网页的本机地址：`127.0.0.1` 指当前电脑，动态端口避免冲突，`embedded=operator` 启用嵌入布局。它不需要手动填写；正常页面不显示该地址。

Node.js 只负责 `npm run build` 生成 `data_analysis/frontend/dist`；日常运行不需要 Node.js、Vite 或手动打开浏览器。Python 负责提供这些静态文件和数据接口，Qt 已代为启动。直接打开静态HTML无法代替实时SQLite查询和机器学习API。

默认仅启动当前Qt业务监控，不要求MySQL。Python依次按 `CHARGING_WEB_PYTHON`、本机配置的 `pythonExecutable`、仓库 `data_analysis/.venv` 和系统PATH查找。可通过 `CHARGING_DASHBOARD_CONFIG` 指定本机JSON配置；默认读取仓库内已被Git忽略的 `runtime/dashboard-service.json`。

启用已准备好的历史分析与模型时，本机配置可为：

```json
{
  "pythonExecutable": "/absolute/path/to/venv/bin/python",
  "mode": "full",
  "environmentFile": "/absolute/path/to/private-environment.json",
  "mysql": {
    "executable": "/absolute/path/to/mysqld",
    "baseDir": "/absolute/path/to/mysql",
    "dataDir": "/absolute/path/to/existing-mysql-data",
    "port": 33717
  }
}
```

`environmentFile` 是已有服务环境变量的私有JSON字典；凭据不应提交。`mysql` 可省略，表示使用已运行的数据库服务；设置后也只允许启动已有数据目录，不会初始化数据库、发布历史数据或训练模型。已经运行的MySQL由原启动者管理，Qt退出不会关闭它。没有本机配置时使用 `qt-live-only` 模式；完整模式依赖失败会显示失败原因，不将当前业务冒充历史分析。

需要连接独立部署的网页服务时，设置 `CHARGING_DASHBOARD_URL`，Qt将使用该地址且不管理外部进程。`--no-dashboard-autostart` 可显式关闭自动启动，用于手动联调或测试。

### 可选：同时启动客户端的演示启动器

AI与腾讯地图Key统一填写 `config/services.local.json`：用户客户端启动时直接读取 `tencentMap`，Qt托管AI服务只读取 `ai`；下面的启动器也按两端职责分别传递。模板、字段和客户端直接启动用法见 [统一密钥配置](services_configuration.md)。本次客户端更新需重新构建一次，以后填写Key后重启即可，不需要重新构建Vue。

```bash
python scripts/launch_demo.py --build-dir build-delivery --demo-seed
```

启动器先检查端口、前端构建和 Qt 二进制，启动 Qt Server 并等待就绪，再检查只读数据库和启动 FastAPI，最后启动用户端。它显式关闭Qt内部自动启动，避免产生重复API。默认数据库为 `runtime/competition.sqlite3`、TCP 9527、HTTP 8000，日志在 `runtime/logs/`。首次演示可用 `--demo-seed`；它仅应用于演示库，不应指向生产库。Qt 管理端的既有演示账户为 `admin / 123456`。

打开 `http://127.0.0.1:8000/?embedded=operator`，或在 Qt 管理端进入运营中心。启动器同时设置 `QT_SQLITE_PATH` 与 Qt 的 `--database` 为同一绝对路径，并设置 `CHARGING_DASHBOARD_URL` 为当前 HTTP 地址。按 Ctrl+C 结束它启动的进程；关闭一个组件也会收回其余子进程。已有进程和数据库不会被自动删除或接管。

可选参数：`--database /absolute/path/demo.sqlite3`、`--tcp-port 9528`、`--http-port 8001`、`--no-client`、`--headless`。无界面模式用于联调，不作为实际界面兼容性证据。

### 启用完整历史分析与模型

按 [历史交付指南](../data_analysis/delivery/README.md) 安装 `requirements-delivery.txt`、运行 `delivery.cli train`、发布 MySQL 统计库，并配置独立 ChargePilot 业务库和凭据。准备完成后：

```bash
python scripts/launch_demo.py --build-dir build-delivery --demo-seed --full
```

完整模式会核验两库、批次和四类模型，失败则停止启动并提供日志。训练和发布保持显式步骤，不在启动时重训模型或覆盖数据库。历史演示不需要 GPU。

### 分别启动或排查

```bash
# Qt 使用同一文件；Windows/macOS 的具体二进制位置取决于构建器
./build-delivery/server/charging-server --database /absolute/path/demo.sqlite3 --demo-seed --no-dashboard-autostart
python -m data_analysis.delivery.cli check --qt-live-only --qt-database /absolute/path/demo.sqlite3
python -m data_analysis.delivery.cli serve --qt-live-only --qt-database /absolute/path/demo.sqlite3
```

完整服务去掉 `--qt-live-only`。也可设置 `QT_SQLITE_PATH`；命令行 `--qt-database` 优先。
实时模式对历史接口返回 `HISTORICAL_SERVICES_DISABLED`，页面显示不可用原因，不编造模型结果。

## 接口与失效处理

| GET 接口 | 返回 |
| --- | --- |
| `/api/v1/qt-live/health` | 验证数据库可读及所需表/列存在，返回UTC快照时刻、只读与模拟标签 |
| `/api/v1/qt-live/snapshot` | 单事务内的汇总、运营趋势、异常设备、电站及最近订单；Vue 的轮询入口，`trendDays=7/30` |
| `/api/v1/qt-live/summary` | 用户/电站/电桩/订单计数、在线率、今日/本月营收、累计已付分与完成 Wh；业务日按北京时间 |
| `/api/v1/qt-live/stations` | Qt 站点身份、价格、营业状态及各电桩状态；支持有界分页 |
| `/api/v1/qt-live/orders` | 有界最近订单、状态与费用电量；不含手机号、姓名、余额、支付凭据 |

营收以已完成订单的 `paid_at` 所在北京时间日期入账，趋势展示连续 7/30 天；无订单日期显示 0，有效快照尚未取得则显示“—”。当前业务保留营收/订单/电量趋势、设备状态和异常设备摘要，异常摘要不包含操作员自由文本。

响应的 `data` 标注 `dataSource=qt-sqlite`、`asOf`、`simulated=true`；`meta` 标注 `scope=CURRENT_QT_BUSINESS`、`mergedWithHistory=false` 及请求编号，不带历史发布批次。
数据库地址仅由服务端配置，不接受 HTTP 路径参数；SQLite 使用 `mode=ro`、`query_only` 和只读事务。锁定、缺失、Schema 不匹配及损坏均返回明确错误，不生成文件，不降级为零值。
快照时刻是读取时刻；设备是否持续更新取决于实际 Qt Server。客户端业务提交完成后，下次轮询可见。

Vue 每 7.5 秒刷新，单次请求超时为 6 秒，切页、窗口隐藏和卸载时停止或取消。失败保留并标记最后成功结果；摘要、表格来自同一 snapshot，不把多个时刻拼成一个状态。表格截断时明确提示。

WebEngine 嵌入默认开启；显式关闭 `CHARGING_EMBED_WEB_DASHBOARD` 仍有系统浏览器入口，Qt 原生管理页面不受影响。自动托管服务使用动态本机端口；独立服务可通过 `CHARGING_DASHBOARD_URL` 配置，传统手动模式默认8000。网页加载失败、超时或渲染进程退出时可重试或外部打开，不做定时整页重载。前端构建目标为 Chromium 87；这不能代替 Qt 6.2.4 实机的图表、地图与滚动检查。

### Qt Creator 页面只提示“本次构建使用系统浏览器”

该提示表示正在运行的二进制没有编译 WebEngine 嵌入，不是 Vue 页面加载失败。旧构建目录会保留先前的 `OFF` 缓存，即使源码已改为默认开启，也需在 Qt Creator 的项目 → 构建设置 → CMake 中将 `CHARGING_EMBED_WEB_DASHBOARD` 改为 `ON`，重新配置、构建并停止/重新运行管理端。所用 Kit 需包含同版本的 Qt WebEngineWidgets。

新版管理端会自动启动网页服务并传入实际数据库路径；旧进程需要重新运行才能获得此功能。可在 Qt Creator 运行参数中显式指定 `--database /absolute/path/demo.sqlite3`；省略时程序使用系统应用数据目录下的 `charging-platform.sqlite3`。手动分开启动时仍须保持 `QT_SQLITE_PATH` 与当前Qt库一致。

## 自动验收与演示路线

```bash
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ctest --test-dir build-delivery --output-on-failure
python -m unittest data_analysis.tests.test_qt_live_adapter data_analysis.tests.test_demo_launcher data_analysis.tests.test_managed_web_service -v
python scripts/verify_qt_live.py --build-dir build-delivery --report runtime/verification/qt-live.json
python scripts/verify_dashboard_autostart.py --build-dir build-delivery --report runtime/verification/dashboard-autostart.json
bash scripts/verify_database.sh
```

`verify_qt_live.py` 每次创建新的临时数据库和独立 Qt/HTTP 进程，走真实 TCP 的登录、预约、充电、停止、支付、重复支付，再校验 HTTP 的计数、金额、电量、无 PII 和历史边界。它不会连接或改写已有用户业务库。此测试证明服务与数据联动，实际 QML 点击和 WebEngine 展示另行验收。
`verify_dashboard_autostart.py` 则只启动生产 Qt 服务端，验证它自动启动 API、提供已构建的 Vue、读取同一新建数据库，以及 Qt 退出后 API 自动清理；使用临时配置和数据库，不连接 MySQL。
GitHub Actions 新增 `Qt and Vue integration`，按 Qt 6.2.4 编译生产客户端、嵌入管理端和无 WebEngine 后备，执行只读 API、Qt 测试与跨语言链路。

建议演示顺序：Qt 用户端找站 → 预约、充电、停止、支付 → Qt 管理端订单 → 运营中心当前业务的新增已付金额和电量 → 历史运营分析与 1/6/24 小时预测 → 策略实验与 TEST/基线 → 断开 FastAPI 展示错误及 Qt 业务仍可用。

## 比赛交付还需在演示设备确认

将源码、本轮验证记录、运行环境与实际提交版本保持一致；使用 [数据来源说明](../data_analysis/docs/behavior_evidence.md) 和各模块依赖/许可证记录。运行包应在目标 OS 构建并验证插件与 WebEngine 资源后分发，不能把 macOS 构建当作 Windows 可运行包。

演示电脑须完成 Qt 6.2.4 的实际嵌入或浏览器后备验证、完整模式冷启动、视频录制；地图和在线 LLM 是否启用取决于自己的 Key。团队人数、既有成果复用和赛事截止等规则以团队确认的官方信息为准，本轮代码验证不代替赛事资格审核。
