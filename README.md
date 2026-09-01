# Qt Charging Platform

基于 Ubuntu 22.04、Qt Framework 6.2.4、C++17 和 SQLite 的电动汽车充电桩应用管理平台。

> 当前状态：阶段 0/1 候选开发基线。Client/Server 可构建启动，公共模型、Socket 协议和
> SQLite 候选 v1 已建立；登录与后续业务功能尚未实现，不应将占位界面视为已完成功能。
> 候选契约需经五人确认，并通过 Ubuntu 22.04 + Qt 6.2.4 严格 CI 与最小登录闭环后，
> 才放行全面接入真实接口。

## 第一阶段范围

```text
Qt 充电用户端
        ↓ TCP + JSON
Qt PC Server / 运营管理端
        ↓ Service / Repository
SQLite
```

首阶段暂不开发 Web 大数据大屏和机器学习子系统。必须先完成用户端、管理端、数据库与
“预约 → 充电 → 计费 → 结算”闭环。

## 已建立的基线

- CMake + C++17 的 `charging_client`、`charging_server`、`charging_common` target。
- Qt 6.2.4 最低兼容线；macOS 可用较新 Qt 做向前构建，Ubuntu CI 严格限定 6.2.4。
- 公共 User/Station/Charger/Reservation/Order 等候选模型、状态和 JSON 转换。
- TCP 4 字节大端长度帧、JSON v1 envelope、稳定动作名与错误码。
- SQLite schema v1：8 张表、外键、CHECK、索引、活动业务唯一约束和可重复 seed。
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
  qt6-base-dev qt6-base-dev-tools libqt6sql6-sqlite

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCHARGING_PLATFORM_STRICT_QT_VERSION=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
bash scripts/verify_database.sh
```

开发到图表和地图功能时，再按对应 target 安装并接入 Qt Charts / Qt WebEngine，不要在未使用前
就将它们变成全员的强制依赖。

## macOS / Apple Silicon 开发

Qt Online Installer 安装 Qt 6.2.4 macOS Kit 后，用 Qt Creator 选择该 Kit，或在命令行指定本机 Qt 路径：

```bash
cmake -S . -B build-macos \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.2.4/macos \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCHARGING_PLATFORM_STRICT_QT_VERSION=ON
cmake --build build-macos --parallel
ctest --test-dir build-macos --output-on-failure
```

如本机暂时只有较新 Qt，可不传严格开关进行日常构建；代码仍必须只使用 Qt 6.2.4
已有 API，最终以严格 CI 为准。不得提交本机 `CMAKE_PREFIX_PATH`、Qt Creator `.user`
文件或 `/Users/...` 绝对路径。

## 启动骨架应用

Ubuntu 普通 CMake 构建下：

```bash
./build/server/charging-server --address 127.0.0.1 --port 9527
./build/client/charging-client
```

macOS 可直接在 Qt Creator 中运行 `charging_server` / `charging_client` target。当前窗口用于验证
项目装配与模块边界，尚未接入真实登录业务。

## 目录和依赖方向

```text
client/       用户端 app/network/services/pages/widgets 独立 CMake 边界
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

## 下一个集成目标

组长先在独立 feature 分支打通真实手机号登录闭环：

```text
Client -> TCP frame -> Server -> UserService -> UserRepository -> SQLite -> Client
```

在候选契约经五人确认、严格 CI 通过且该闭环验证 UI、Socket、协议、Service、Repository
和 SQLite 前，成员只并行做 mock UI、数据库准备和基于候选接口的隔离开发，不得各自接入
真实 Socket/SQL。组长记录放行结论后，再让五名成员全面并行接入真实接口。
