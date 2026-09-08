# Qt Charging Platform

基于 Ubuntu 22.04、Qt Framework 6.2.4、C++17 和 SQLite 的电动汽车充电桩应用管理平台。

> 交付入口：QML 用户端 `charging-client` + PC 运营管理端 `charging-server`。
> 默认使用真实 TCP 和服务端唯一 SQLite 数据源。Widgets 用户端不再默认构建。
> 验收步骤、未合入 PR 边界和外部地图配置见 [交付与验收说明](docs/development/delivery_acceptance.md)。

## 第一阶段范围

```text
QML 充电用户端
        ↓ TCP + JSON
Qt PC Server / 运营管理端
        ↓ Service / Repository
SQLite
```

首阶段暂不开发 Web 大数据大屏和机器学习子系统。必须先完成用户端、管理端、数据库与
“预约 → 充电 → 计费 → 结算”闭环。

## 已建立的基线

- CMake + C++17 的 `charging_client`、`charging_server`、`charging_common` target。
- Qt 6.2.4 兼容基线；Ubuntu CI 严格限定 Framework 版本为 6.2.4。
- 公共 User/Station/Charger/Reservation/Order 等候选模型、状态和 JSON 转换。
- TCP 4 字节大端长度帧、JSON v1 envelope、稳定动作名与错误码。
- SQLite schema v1：8 张表、外键、CHECK、索引、活动业务唯一约束和可重复 seed。
- 手机号登录/自动注册：Client 异步请求、Server 二次校验、SQLite 查询/创建和冻结拦截。
- `NetworkManager` 和 `TcpServer` 统一入口，以及 `ClientSession` 会话身份和
  `RequestDispatcher` 业务路由。
- `ChargingService` / `OrderService` / `BillingService` 核心业务，显式充电状态机，
  15 分钟预约过期和整数安全计费。
- 预约、取消、开始、停止和支付的 SQLite 多表事务；重复停止/支付不会
  重复累计或扣款。
- QtTest、数据库完整性验证、Ubuntu 22.04 / Qt 6.2.4 GitHub Actions。
- 五人分工、分支、Commit、PR、Code Review、命名和 Qt 兼容规范。

## 环境基线

| 项目 | 统一要求 |
| --- | --- |
| 最终验收 | Ubuntu 22.04 |
| Qt Framework | 6.2.4 |
| 语言 | C++17 |
| 构建 | CMake 3.16+ |
| 数据库 | SQLite / QSQLITE |
| 网络 | QTcpSocket / QTcpServer |

Qt Creator 是 IDE，版本号不等于 Qt Framework 版本号。验收时 CMake 找到的 Qt 必须是 6.2.4。

## Ubuntu 22.04 从零构建

Ubuntu 22.04 官方仓库提供 Qt 6.2.4：

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build sqlite3 \
  qt6-base-dev qt6-base-dev-tools libqt6sql6-sqlite \
  qt6-declarative-dev qt6-declarative-dev-tools qt6-webengine-dev qt6-charts-dev \
  qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtquick-layouts \
  qml6-module-qtquick-controls qml6-module-qtquick-templates \
  qml6-module-qtquick-dialogs qml6-module-qtqml qml6-module-qtqml-models \
  qml6-module-qtqml-workerscript qml6-module-qtwebengine

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DCHARGING_PLATFORM_STRICT_QT_VERSION=ON \
  -DCHARGING_BUILD_QML=ON -DCHARGING_BUILD_WIDGETS_CLIENT=OFF
cmake --build build --parallel
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ctest --test-dir build --output-on-failure
bash scripts/verify_database.sh
```

QML、Charts、WebEngine 已属于交付模块，缺少开发包或 QML 运行模块不能作为完整验收通过。
不得提交个人 `CMAKE_PREFIX_PATH`、Qt Creator
`.user` 文件或任何本机绝对路径。

## 启动应用

Ubuntu 普通 CMake 构建下：

```bash
./build/server/charging-server --address 127.0.0.1 --port 9527 \
  --database ./runtime/demo.sqlite3 --demo-seed
./build/client/charging-client --host 127.0.0.1 --port 9527
```

先启动 Server，再启动 Client。Client 输入 11 位手机号后会通过真实 TCP 请求 Server；
已有用户直接返回，新手机号自动注册，冻结用户会被拒绝。找站、预约、充电、停止、
充值与支付均走同一真实会话；管理端使用 `admin / 123456` 登录演示库后查看这些变化。Server 可使用
`--database <path>` 指定 SQLite 文件；只有显式传入 `--demo-seed` 才加载演示数据。

## 目录和依赖方向

```text
client/       QML 用户端 qml/network/services；pages/widgets 保留旧界面回归代码
server/       PC 端 app/network/services/repositories/database/pages 独立 CMake 边界
common/       Client/Server 共享的模型和协议
database/     schema.sql、seed.sql 与 Qt resource
config/       可提交的配置示例；真实 config.ini 已忽略
resources/    图标、图片、QSS 约定
tests/        QtTest 与资源验证
docs/         需求、架构、协议、数据库、协作文档
```

依赖必须单向：Client 不访问 SQLite，UI 不写 SQL，页面不直接解 TCP 帧，Server 业务通过
Service 和 Repository 访问数据库。

## 数据库演示数据

`database/seed.sql` 仅供 demo/测试：

- 管理员：`admin / 123456`（数据库存 salt + hash，不存明文）；
- 演示用户：`13800138000`，余额 100.00 元；
- 3 个演示站点、7 个电桩。

新手机号自动注册时的默认余额是 0，不是 seed 用户的 100.00 元。

## 五人开发流程

仓库保留已有 `master` 作为稳定分支，`develop` 作为日常集成分支：

```text
master  <- develop <- feature/<area>-<topic>
```

```bash
git switch develop
git pull --ff-only origin develop
git switch -c feature/client-station-list
```

普通 PR 合入 `develop`；阶段稳定后由组长发起 `develop -> master` 发布 PR。禁止五人
同时直接推送 `master` / `develop`。

## 必读文档

- [需求与验收 Checklist](docs/requirements/requirements_checklist.md)
- [阶段 0/1 架构基线](docs/design/architecture.md)
- [Socket JSON 协议 v1](docs/api/socket_protocol.md)
- [SQLite 数据字典 v1](docs/database/data_dictionary.md)
- [命名与代码风格](docs/design/naming_and_code_style.md)
- [Qt 6.2.4 兼容规范](docs/development/qt_6_2_4_compatibility.md)
- [五人角色与协作流程](docs/team/roles_and_workflow.md)
- [贡献指南](CONTRIBUTING.md)

## 已打通的两条闭环

手机号登录：

```text
Client -> TCP frame -> Server -> UserService -> UserRepository -> SQLite -> Client
```

充电业务：

```text
NetworkManager -> TcpServer -> ClientSession -> RequestDispatcher
               -> ChargingService / OrderService / BillingService
               -> ChargingRepository / OrderRepository -> SQLite
```

交付验收使用 QML 页面及真实服务，不能用 `CHARGING_CHANNEL=mock` 的演示结果代替。
跨模块变更仍须通过 Review 和严格 CI；`common/`、
`database/schema.sql` 和跨模块接口属于高冲突区域，修改前必须确认契约并通过 PR Review。
