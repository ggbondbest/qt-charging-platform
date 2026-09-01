# Qt 6.2.4 兼容规范

## 1. 兼容目标

本项目的唯一 Qt Framework 兼容基线是 **Qt 6.2.4**，语言标准是 **C++17**，最终验收环境是 **Ubuntu 22.04**。不得使用 Qt 6.3 及以上才新增的 API 或 CMake 功能。

Qt Creator 是 IDE，不决定程序兼容版本。成员可以使用不同版本的 Qt Creator，
但必须选择 Qt 6.2.4 Kit，且合并前必须通过 Ubuntu 22.04 + Qt 6.2.4 的严格 CI。

查询 Qt API 时使用 [Qt 6.2 文档](https://doc.qt.io/qt-6.2/)，不要默认使用搜索引擎打开的最新 Qt 文档。

## 2. CMake 要求

根项目将 Qt 6.2.4 设为最低兼容版本，同时提供 CI 严格开关：

```cmake
set(CHARGING_PLATFORM_QT_BASELINE_VERSION "6.2.4")
option(CHARGING_PLATFORM_STRICT_QT_VERSION "Require exactly the baseline Qt" OFF)

find_package(Qt6 ${CHARGING_PLATFORM_QT_BASELINE_VERSION} REQUIRED
    COMPONENTS Core Gui Widgets Network Sql)

if(CHARGING_PLATFORM_STRICT_QT_VERSION
   AND NOT "${Qt6_VERSION}" VERSION_EQUAL "${CHARGING_PLATFORM_QT_BASELINE_VERSION}")
    message(FATAL_ERROR "The strict build requires Qt 6.2.4")
endif()
```

CI 必须传入 `-DCHARGING_PLATFORM_STRICT_QT_VERSION=ON`。`Charts`、
`WebEngineWidgets` 等较大模块只在实际功能接入时按 target 查找并链接，避免让骨架项目
在未使用这些模块时就强制所有成员安装。

Qt 6.2.4 项目应手动启用 CMake 的 Qt 自动处理能力：

```cmake
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
```

禁止使用 `qt_standard_project_setup()`。该命令从 Qt 6.3 才引入，在 Qt 6.2.4 中不存在。标准 `add_executable()` 可以使用；`qt_add_executable()` 从 Qt 6.0 起可用，但任何 Qt CMake helper 在加入前仍须确认 6.2 文档中存在。

不得把个人机器上的 Qt 路径、`CMAKE_PREFIX_PATH` 或 `Qt6_DIR` 写入版本控制。每位成员通过自己的 Qt Kit、`qt-cmake` 或本地命令行环境定位 Qt。

## 3. C++ 与 Qt API 规则

- 只使用 C++17，不使用要求 C++20/23 的项目代码语法或标准库类型。
- 禁止引用 Qt 私有头文件，如带 `_p.h` 后缀的文件。
- 使用某个不熟悉的 Qt 类、方法、枚举或 CMake 命令前，检查 6.2 文档；若页面标注 “since Qt 6.3” 或更高版本，则不得使用。
- 不通过 `#if QT_VERSION` 只实现高版本路径。确需条件编译时，必须同时提供 Qt 6.2.4 可测试的完整实现。
- 不依赖未记录的内部行为或平台专属实现。
- 新代码不得使用 Qt 5 已移除的 API。

常见错误示例：

| 不兼容或不推荐 | Qt 6.2.4 项目做法 |
|---|---|
| `qt_standard_project_setup()` | 手动设置 `AUTOMOC`、`AUTOUIC`、`AUTORCC` |
| `QRegExp` | `QRegularExpression` |
| `QTextStream::setCodec()` | `QTextStream::setEncoding()` |
| `QDesktopWidget` | `QGuiApplication::screens()` / `QScreen` |
| Qt 私有头文件 | 使用对应公开 API |
| 最新文档中的 `QRestAccessManager` | 使用 Qt 6.2 的 `QNetworkAccessManager` |
| 最新文档中的 `QHttpHeaders` | 使用 Qt 6.2 支持的请求头 API |
| 最新文档中的 `QChronoTimer` | 使用 `QTimer` |

上述表格不是完整清单；在 Qt 6.2.4 上实际编译才是最终判断。

## 4. 本项目允许的 Qt 模块

第一阶段预期使用：

| 模块 | 用途 |
|---|---|
| `Qt6::Core` | 核心类型、JSON、时间、文件和线程基础 |
| `Qt6::Gui` | 图像、图标和窗口基础 |
| `Qt6::Widgets` | Client 与 PC 管理端界面 |
| `Qt6::Network` | TCP Socket 与 HTTP 请求 |
| `Qt6::Sql` | QSQLITE 数据访问 |
| `Qt6::Charts` | 后台营收与状态图表 |
| `Qt6::WebEngineWidgets` | 腾讯地图和路线页面 |
| `Qt6::Test` | 单元测试和集成测试 |

新增 Qt 模块前必须说明用途、Qt 6.2.4 可用性、Ubuntu 安装方式和许可证/部署影响。不得仅为一个简单工具函数引入大型第三方依赖。

## 5. Ubuntu 22.04 目标环境规则

目标环境代码必须遵守：

- 使用 `QDir`、`QFileInfo`、`QStandardPaths` 处理路径。
- 运行时数据库使用 `QStandardPaths::AppDataLocation` 等可写位置。
- 不硬编码个人主目录、开发者机器路径或启动时工作目录。
- 只读资源优先放入 `.qrc` 并使用 `:/` 别名访问。
- 源码和文本资源使用 UTF-8 与 LF。
- QSS 使用验收环境可用字体或系统默认字体。
- UI 必须在 Ubuntu 22.04 的不同 DPI、字体度量和窗口尺寸下正常布局。

开发完成后至少在 Ubuntu 22.04 做一次全新 clone 和从零构建；其他环境的构建成功不能替代 Ubuntu 验收。

## 6. Socket 与数据库兼容注意事项

Qt 版本兼容不只指“能编译”，还包括相同业务行为：

- TCP 是字节流，必须缓存并解析半包和粘包，不能假设一次 `readyRead()` 对应一条 JSON。
- 帧头和整数编码明确字节序，不依赖本机 CPU 字节序。
- JSON 使用 `QJsonDocument`、`QJsonObject` 和 `QJsonArray` 的 Qt 6.2 公共 API。
- `QTcpSocket` 只能在其所属线程操作。
- 每个数据库线程创建独立、唯一命名的 `QSqlDatabase` 连接。
- SQL 使用绑定参数；余额扣除、订单状态和电桩状态等相关写入使用事务。
- SQLite 文件位置不能依赖应用启动时的当前工作目录。

## 7. WebEngine 与地图

- Qt 6.2.4 安装时需要额外选择 Qt WebEngine 模块。
- 腾讯地图 Key 不得提交到仓库；通过本地配置或环境变量注入。
- Web 页面加载失败、无网络、Key 无效时必须给出可理解的降级提示，不能让主业务流程崩溃。
- 自动化测试不依赖真实腾讯地图服务；地图网络调用通过接口隔离或使用可控测试替身。
- 最终 Ubuntu 验收要实际检查 WebEngine 运行时依赖和页面显示，不只验证编译。

## 8. 提交前兼容检查

每个 PR 作者应确认：

- 本机 CMake 找到的 Qt Framework 不低于 6.2.4，并且清楚记录实际版本。
- 严格 CI 找到的 Qt Framework 精确为 6.2.4，而不是 Qt Creator 版本。
- 项目以 C++17 配置并成功编译。
- 新使用的 Qt API 在 Qt 6.2 文档中存在。
- 没有 `qt_standard_project_setup()` 或其他 Qt 6.3+ CMake helper。
- 没有私有 Qt 头文件、机器绝对路径或平台专属二进制依赖。
- 与路径、数据库、Socket、编码和 UI 布局相关的变更已在 Ubuntu 22.04 验证。
- 测试在 Qt 6.2.4 上通过；最终判断以精确版本构建结果为准。
