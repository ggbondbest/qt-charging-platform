# 命名与代码风格

本规范用于减少五人并行开发时的命名冲突和无意义格式差异。若仓库中的自动格式化配置与本文在纯排版细节上冲突，以自动格式化配置为准；架构、类型和命名规则仍以本文为准。

## 1. 基础规则

- 使用 C++17 和 Qt 6.2.4。
- 所有源码、CMake、SQL、QSS、JSON 和 Markdown 使用 UTF-8 与 LF 换行。
- 源码目录、文件名和资源名只使用英文、数字和下划线，不使用空格或中文。
- 缩进使用 4 个空格，不使用 Tab。
- 一行原则上不超过 100 个字符；长函数调用按参数换行。
- 头文件使用 `#pragma once`。
- 使用 `nullptr`，不使用 `NULL` 或整数 `0` 表示空指针。
- 不为无关文件做整文件格式化；格式调整应与功能修改保持最小范围。

## 2. 文件和目录

文件名使用 `lower_snake_case`：

```text
station_service.h
station_service.cpp
charging_page.ui
socket_protocol_test.cpp
```

推荐的模块内目录名称：

```text
pages/
widgets/
services/
network/
models/
protocol/
repositories/
utils/
```

一个主要类通常对应一个同名 `.h/.cpp` 文件。Qt Designer 的 `.ui` 文件与页面类使用相同词根。生成的 `ui_*.h` 文件不得手工修改或提交。

## 3. C++ 标识符

| 对象 | 规则 | 示例 |
|---|---|---|
| 类、结构体、枚举类型 | `PascalCase` | `ChargingService`, `OrderStatus` |
| 函数、方法 | `lowerCamelCase` | `startCharging()`, `loadStations()` |
| 局部变量、参数、成员变量 | `lowerCamelCase` | `stationId`, `requestData` |
| 私有成员变量 | `lowerCamelCase_` | `socket_`, `currentUserId_` |
| 常量 | `kPascalCase` | `kDefaultServerPort` |
| 枚举值 | `PascalCase` | `OrderStatus::WaitingPayment` |
| 命名空间 | 小写英文 | `charging::protocol` |
| 宏 | `UPPER_SNAKE_CASE` | `CHARGING_ENABLE_WEBENGINE` |
| CMake 目标 | `lower_snake_case` | `charging_client`, `charging_common` |

布尔值使用能直接读出真假含义的名称：

```cpp
bool isConnected;
bool hasUnpaidOrder;
bool canStartCharging;
```

避免 `flag`、`data1`、`tmp`、`obj`、`manager2` 等缺少业务含义的名称。

## 4. Qt 对象命名

Qt Designer `objectName` 使用“业务含义 + 控件类型”的 `lowerCamelCase`：

```text
phoneLineEdit
loginButton
stationTableView
revenueChartView
statusLabel
pageStackedWidget
```

禁止保留 `pushButton_2`、`label_7`、`widget_3` 等自动名称。Signal 和 slot 使用动作或事件语义：

```cpp
signals:
    void loginSucceeded(qint64 userId);
    void chargerStatusChanged(qint64 chargerId, ChargerStatus status);

private slots:
    void handleLoginClicked();
    void handleSocketDisconnected();
```

优先使用函数指针形式的 `connect`：

```cpp
connect(loginButton, &QPushButton::clicked,
        this, &LoginPage::handleLoginClicked);
```

不使用旧式 `SIGNAL()` / `SLOT()` 字符串语法，除非 Qt 6.2.4 没有可用的类型安全替代方案，并在代码旁解释原因。

## 5. 类型、单位和状态

跨模块字段必须统一类型和单位：

- 数据库主键和业务 ID 使用 `qint64`。
- 金额使用 `qint64` 表示“分”，如 `balanceCents`、`feeCents`；不得用 `double` 存储余额或结算金额。
- ID 在 JSON 中用十进制字符串；其他 `qint64` JSON number 必须位于
  `[-9007199254740991, 9007199254740991]` 的精确整数区间。
- C++ 时间点使用 UTC `QDateTime`，成员名以 `AtUtc` 结尾；JSON 与 SQLite
  使用 UTC ISO-8601 文本，字段以 `At` / `_at` 结尾；网络输入必须带
  `Z` 或明确 UTC offset。
- 距离在模型中明确单位，例如 `distanceMeters`；仅在 UI 层格式化为公里。
- 功率使用整数瓦，例如 `powerWatts`；只在 UI 层格式化为 kW。
- 手机号使用 `QString`，不得转为整数。
- 状态使用 `enum class`，不得在业务代码中散落魔法整数或任意字符串。

示例：

```cpp
enum class ChargerStatus {
    Available,
    Reserved,
    Charging,
    Fault,
    Offline
};

enum class OrderStatus {
    Reserved,
    Charging,
    WaitingPayment,
    Completed,
    Cancelled
};
```

C++ 枚举与协议/数据库字符串之间的转换集中放在 `common/`，Wire 值使用固定的 `UPPER_SNAKE_CASE`，例如 `WAITING_PAYMENT`。未知值必须返回明确错误，不能静默映射为默认状态。

## 6. JSON、协议和错误码

JSON key 使用 `lowerCamelCase`：

```json
{
  "protocolVersion": 1,
  "kind": "REQUEST",
  "type": "USER_LOGIN",
  "requestId": "9b2e...",
  "data": {
    "phone": "13800138000"
  }
}
```

请求类型、状态字符串和错误码使用 `UPPER_SNAKE_CASE`，并只能由公共协议模块定义。错误码示例：

```text
INVALID_REQUEST
UNAUTHORIZED
NOT_FOUND
CONFLICT
INSUFFICIENT_BALANCE
DATABASE_ERROR
INTERNAL_ERROR
```

不得使用界面提示文本判断业务错误。Service 返回稳定错误码，UI 再将错误码转换为用户可读提示。

## 7. 分层边界

依赖方向保持单向：

```text
Client UI → Client Service → Client Network → Common Protocol

Server UI / Network → Server Service → Repository → SQLite
                                      ↘ Common Model/Protocol
```

规则如下：

- Client 不直接链接或访问 SQLite。
- UI 页面不拼接 SQL、不直接解析 Socket 帧、不实现计费状态机。
- Repository 只负责数据访问，不弹窗、不引用 UI 类。
- Service 负责业务校验、事务边界和状态转换。
- `common/` 只放真正跨 Client/Server 共享的模型、协议、错误码和无状态工具，不放页面或数据库实现。
- 模块之间通过明确接口、signals/slots 或公共消息交互，不通过全局变量共享可变状态。

## 8. QObject、内存与线程

- QObject 有 parent 时由 Qt 父子对象树管理，不再使用另一个所有者重复释放。
- 非 QObject 的独占资源优先使用值语义或 `std::unique_ptr`。
- 原始指针默认只表示非拥有引用；拥有关系不清楚时必须在接口中说明。
- 不跨线程直接调用 UI 控件。
- `QTcpSocket` 只在其所属线程读写。
- 每个数据库工作线程使用独立且唯一命名的 `QSqlDatabase` 连接。
- 跨线程更新通过 queued signal/slot 或明确的任务队列完成。
- Lambda 捕获应尽量显式，异步 Lambda 不得捕获可能提前销毁的局部引用。

## 9. 数据库代码

SQL 表名和列名使用 `lower_snake_case`，表名使用复数：

```text
users
admins
stations
chargers
orders
reservations
recharge_records
operation_logs
```

数据库规则：

- 所有外部输入使用绑定参数，不拼接 SQL 字符串。
- 余额扣除、订单结算和电桩状态更新等多表写操作必须放在事务中。
- 明确检查 `prepare()`、`exec()`、`commit()` 的结果，并保留可诊断错误信息。
- 不在日志中打印管理员密码、地图 Key 或完整敏感信息。
- 测试使用临时或内存数据库，不依赖开发者本机已有 `.db` 文件。

## 10. 字符串、日志和错误处理

- 固定 Qt 字符串优先使用 `QStringLiteral()`。
- 不把面向用户的中文提示当作协议常量或数据库状态。
- 可恢复错误返回给调用者；不可恢复初始化错误应记录上下文并安全退出。
- `Q_ASSERT` 只用于程序员错误，不能代替运行时输入校验。
- 日志至少包含模块、操作和稳定标识符；避免只打印“失败”。
- 不捕获错误后无动作，不返回无意义的默认成功值。

## 11. UI 与资源

- 全局颜色、字号、圆角和间距由公共 QSS/设计 Token 控制。
- 页面专属 QSS 应限制作用域，避免通过裸类名影响其他页面。
- 资源文件名使用小写英文；通过稳定的 `:/` 资源别名访问。
- 不硬编码 macOS 专属字体；优先使用系统默认字体。
- 不依赖 Qt 默认控件对象名或绝对图片路径。
- UI 线程不得执行长时间 SQL、网络等待或计算任务。

## 12. 测试命名

- 测试源文件使用 `tst_<subject>.cpp`，例如 `tst_protocol_codec.cpp`。
- 测试类使用 `<Subject>Test`，测试方法使用描述行为的 `lowerCamelCase`。
- 一个测试只验证一个清晰行为；失败信息应能定位输入和期望。
- 协议测试覆盖完整帧、半包、粘包、非法长度和非法 JSON。
- 业务测试覆盖正常流程、重复操作、余额不足、冲突、回滚和断线恢复。
