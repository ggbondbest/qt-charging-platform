# 贡献指南

本项目由五人协作开发。所有成员在开始编码前都应阅读：

- [角色与协作流程](docs/team/roles_and_workflow.md)
- [Git 协作指令](docs/team/git_commands.md)
- [命名与代码风格](docs/design/naming_and_code_style.md)
- [Qt 6.2.4 兼容规范](docs/development/qt_6_2_4_compatibility.md)

当前第一阶段只实现 Qt 用户端、Qt PC 服务端和 SQLite 数据库。Web 大屏与机器学习模块不在当前开发范围内。

当前公共模型、协议与 Schema 是候选 v1。五人确认、Ubuntu 22.04 + Qt 6.2.4 严格 CI
和手机号登录最小闭环完成前，只允许 mock UI、数据库准备及基于候选接口的隔离开发；
不得各自创建并接入真实网络层或数据库访问层。

## 1. 开发基线

- 最终验收系统：Ubuntu 22.04。
- Qt Framework：6.2.4。
- 语言标准：C++17。
- 构建工具：CMake。
- 数据库驱动：QSQLITE。
- 提交必须保持 Ubuntu 22.04 可编译，不得写个人机器路径。
- Qt Creator 的版本可以不同，但 Qt Framework Kit 必须为 6.2.4；
  Ubuntu 严格 CI 必须精确使用 6.2.4。

首次构建前，检查 CMake 输出的 Qt 版本。使用 Qt 6.2.4 时建议开启严格检查：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCHARGING_PLATFORM_STRICT_QT_VERSION=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

不得提交 `build/`、Qt Creator 用户配置、运行时数据库、日志、缓存或本地密钥。

## 2. 分支模型

仓库使用以下三层分支：

```text
master          稳定、可演示、可验收
  ↑
develop         日常集成
  ↑
feature/...     短期任务分支
```

普通功能开发必须从最新 `develop` 创建分支：

```bash
git switch develop
git pull --ff-only origin develop
git switch -c feature/client-station-list
```

分支名使用小写英文和连字符：

```text
feature/client-station-list
feature/client-profile
feature/client-charging
feature/server-dashboard
feature/server-user-management
feature/database-user-repository
feature/network-login
```

规则如下：

- 禁止直接向 `master` 或 `develop` 提交代码。
- 一个 feature 分支只处理一个清晰任务，不作为某位成员的永久个人分支。
- 普通 PR 的目标分支是 `develop`。
- 只有阶段版本已经在 `develop` 集成验证完成后，才由组长创建 `develop` 到 `master` 的发布 PR。
- 分支落后时先同步 `develop` 并解决冲突；不熟悉变基或强制推送时先联系组长。
- PR 合并后删除对应 feature 分支。

## 3. 提交规范

Commit 格式为：

```text
type(scope): concise description
```

允许的 `type`：

- `feat`：新增功能。
- `fix`：修复缺陷。
- `ui`：仅涉及界面或样式。
- `refactor`：不改变外部行为的重构。
- `test`：新增或调整测试。
- `docs`：文档变更。
- `build`：CMake 或构建配置。
- `ci`：持续集成配置。
- `chore`：其他维护工作。

常用 `scope` 包括 `client`、`server`、`network`、`protocol`、`database`、`common`、`docs` 和 `build`。

示例：

```text
feat(network): add login request codec
fix(database): prevent duplicate settlement
ui(server): add dashboard summary cards
docs(protocol): document charging response
```

不要使用“修改”“update”“111”“最终版”等无法表达目的的提交说明。每个提交应只包含一个逻辑变更；提交前先运行 `git status`，按文件选择暂存范围。

## 4. 共享契约变更

以下内容属于高冲突、跨模块契约：

- 根 CMake 与公共构建选项。
- `common/` 中的数据模型、状态、错误码和协议结构。
- Socket 帧格式、请求类型和字段。
- 数据库 Schema、Seed 和 Repository 接口。
- 全局 QSS、共享资源路径和配置键。

功能需要修改这些契约时，应先创建一个小型、独立的契约 PR，说明受影响模块并更新对应文档和测试。契约 PR 合并后，相关功能分支再同步 `develop`，不得在多个 feature 分支中各自定义不同版本。

## 5. Pull Request

推送分支后创建 PR，并完整填写模板。PR 应做到：

- 标题符合 Commit 格式并能概括结果。
- 关联需求清单或 Issue；没有 Issue 时写明对应功能。
- 列出实际执行过的构建和测试命令，而不是只写“已测试”。
- UI 变更附截图或短视频，并说明验证平台。
- 协议、Schema、配置或公共模型变更明确列出兼容影响。
- 不混入无关格式化、重命名或其他成员的修改。
- 请求至少一名非作者成员 Review；核心架构和跨模块 PR 必须由组长 Review。

Review 意见应针对正确性、线程安全、数据一致性、兼容性、可测试性和模块边界。作者解决所有阻塞意见后才能合并。

## 6. 合并前检查

提交 PR 前至少确认：

- 使用 Qt 6.2.4 和 C++17 完成构建。
- 相关测试通过，且没有用跳过测试来掩盖失败。
- 没有引入 Qt 6.3 及以上才提供的 API 或 CMake 命令。
- Client 没有直接访问 SQLite，UI 页面没有散落 SQL。
- Socket 代码能处理半包、粘包、断开、超长消息和非法 JSON。
- 数据库写操作对失败和事务回滚有明确处理。
- 未提交密钥、手机号等敏感数据、绝对路径、数据库文件或构建产物。
- 文档、协议、Schema 和代码保持一致。

满足以上条件并获得批准后，PR 方可合并到 `develop`。
