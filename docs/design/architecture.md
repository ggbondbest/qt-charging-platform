# 充电平台架构基线

状态：`candidate-v1`（登录和核心充电闭环已通过 Ubuntu 22.04 / Qt 6.2.4 严格 CI，
待五人确认后冻结）

## 1. 范围与约束

首阶段只交付以下三部分：

1. Qt 充电用户端；
2. Qt PC 服务器及运营管理端；
3. SQLite 数据库。

Web 大屏和机器学习子系统不在首阶段范围内。原要求书写的是“Ubuntu 22.04 及以上、Qt Creator 6.2 及以上”，本组按更严格的验收约束统一为：

- Ubuntu 22.04；
- Qt 6.2.4；
- C++17；
- CMake；
- SQLite（Qt SQL 的 `QSQLITE` 驱动）；
- TCP Socket + JSON；
- Qt 多线程对象模型。

在较新 Qt 上开发只能证明源代码可向前构建，不能替代 Ubuntu + Qt 6.2.4 的最终 clean build。

## 2. 模块与依赖方向

```text
charging_client
  UI -> client service -> TCP connection
                         |
                         v
                  charging_common
                         ^
                         |
charging_server
  admin UI -> application service <- request dispatcher <- client session
                    |
                    v
             repository interface
                    |
                    v
              SQLite repository
                    |
                    v
     ChargingPlatform::DatabaseResources
```

依赖规则：

- `common/` 只依赖 `Qt6::Core`，禁止依赖 Widgets、Network、Sql 或任一应用目录；
- Client 不得链接 QtSql、不得直接打开平台数据库；
- 页面不得包含 SQL，不得直接读写 `QTcpSocket`；
- Dispatcher 只做协议校验、鉴权和路由，不承载业务事务；
- Service 负责业务用例、状态规则和对外错误映射；
- Repository 负责查询、持久化和 SQLite 原子事务，不弹窗、不拼协议响应；
- Server 管理页面也必须经过 Service/Repository，不能绕过业务规则；
- `database/schema.sql` 和 `database/seed.sql` 通过资源 target 编译进 Server，不能依赖启动时的当前目录。

现有公共 CMake target：

- `charging_common`，别名 `ChargingPlatform::Common`；
- `charging_database_resources`，别名 `ChargingPlatform::DatabaseResources`。

Client 与 Server 还为 `network`、`services`、`pages`、`widgets`、`repositories`、
`database` 建立了模块内 target。成员只修改负责目录的 `CMakeLists.txt`；应用装配文件由
组长维护，避免多人同时编辑根 target 的源文件列表。Client 的 `pages` 与 `services`
又预先拆成成员 2 的 `station` 和成员 3 的 `profile_charging` 子模块。

数据库资源路径固定为：

- `:/database/schema.sql`；
- `:/database/seed.sql`。

## 3. 推荐目录职责

```text
client/
  app/                 应用启动、配置、页面装配
  network/             TCP 连接、重连、请求关联
  services/            面向 UI 的异步用例接口
  pages/               页面
  widgets/             可复用控件

server/
  app/                 服务端启动、管理窗口装配
  network/             listener、session、dispatcher
  services/            登录、预约、充电、订单、计费等事务
  repositories/        接口及 SQLite 实现
  database/            连接生命周期、脚本执行、迁移
  pages/               PC 管理端页面

common/
  model/               共享 DTO、enum、JSON 映射
  protocol/            envelope、错误码、TCP 帧编解码

database/              schema、seed、qrc
docs/                  需求、架构、协议、数据字典、测试记录
tests/                 单元与集成测试
```

建议类名使用职责名，例如 `ClientConnection`、`ServerListener`、`ClientSession`、`RequestDispatcher`、`UserService`、`UserRepository`。避免无边界的 `Manager`、`Utils` 或 `Data`。

## 4. 命名规范

- 文件和目录：`lower_snake_case`；
- 类、结构体、枚举：`PascalCase`；
- 函数和变量：`lowerCamelCase`；
- 编译期常量：`kPascalCase`；
- C++ namespace：`charging::model`、`charging::protocol`、`charging::client`、`charging::server`；
- JSON 字段：`lowerCamelCase`；
- SQLite 表和列：`lower_snake_case`；
- Socket 动作和持久化状态：`UPPER_SNAKE_CASE`；
- Git 分支：`feature/<area>-<topic>`；
- Commit 前缀：`feat:`、`fix:`、`refactor:`、`test:`、`docs:`、`chore:`、`ui:`。

不得在头文件写 `using namespace`，也不得使用平台绝对路径。

## 5. 公共数据契约

公共结构定义在 `common/include/charging/common/model/models.h`。JSON 映射集中在 `model_json.h`，成员不得为同一实体另建一套字段名。

关键表示规则：

- 主键和外键：C++ 为 `qint64`；JSON 为十进制字符串；SQLite 为 `INTEGER`；
- 金额：整数“分”，字段后缀 `Cents` / `_cents`；禁止用 `double` 存金额；
- 电价：整数“分/千瓦时”，`priceCentsPerKwh`；
- 功率：整数瓦，`powerWatts`；
- 电量：整数瓦时，`energyWh`；
- 时长：整数秒，`durationSeconds`；
- 除 ID 外的 `qint64` JSON number 限制在 `±(2^53 - 1)` 内；当前业务量均为非负；
- 坐标：`double` / SQLite `REAL`；
- 时间：C++ `QDateTime`，网络输入必须带 `Z` 或明确 UTC offset；序列化与
  数据库统一为 UTC ISO-8601 文本，例如 `2026-09-01T08:30:00.000Z`；
- 可空时间：无值时 JSON 为 `null`、数据库为 `NULL`、C++ 为无效 `QDateTime`；
- 头像：传递不含本机绝对路径的 `avatarKey`。后续上传接口根据 key 获取内容；
- 站点的总桩数、空闲桩数和在线率均从 `chargers` 聚合，不重复持久化。

枚举序号只允许在进程内使用。协议和数据库必须使用 `enums.h` / `enums.cpp` 定义的大写字符串；未知字符串必须报错，不能默认为 `AVAILABLE` 或 `ACTIVE`。

## 6. 状态机

### 6.1 预约与订单正常路径

```text
Reservation: ACTIVE -> FULFILLED
Order:       RESERVED -> CHARGING -> WAITING_PAYMENT -> COMPLETED
Charger:     AVAILABLE -> RESERVED -> CHARGING -> AVAILABLE
```

取消或超时路径：

```text
Reservation: ACTIVE -> CANCELLED | EXPIRED
Order:       RESERVED -> CANCELLED
Charger:     RESERVED -> AVAILABLE
```

`WAITING_PAYMENT` 时电桩已经释放，但用户仍有未结订单，因此不能创建新预约。

### 6.2 合法迁移

| 实体 | 起始状态 | 目标状态 | 触发 |
| --- | --- | --- | --- |
| Reservation | `ACTIVE` | `FULFILLED` | 开始充电 |
| Reservation | `ACTIVE` | `CANCELLED` | 用户取消 |
| Reservation | `ACTIVE` | `EXPIRED` | 服务端超时 |
| Order | `RESERVED` | `CHARGING` | 开始充电 |
| Order | `RESERVED` | `CANCELLED` | 取消/超时 |
| Order | `CHARGING` | `WAITING_PAYMENT` | 停止或异常中断 |
| Order | `WAITING_PAYMENT` | `COMPLETED` | 扣款成功 |
| Charger | `AVAILABLE` | `RESERVED` | 预约成功 |
| Charger | `RESERVED` | `CHARGING` | 开始充电 |
| Charger | `RESERVED` | `AVAILABLE` | 取消/超时 |
| Charger | `CHARGING` | `AVAILABLE` | 停止充电 |
| Charger | `AVAILABLE` | `FAULT` / `OFFLINE` | 管理操作或心跳检测 |
| Charger | `FAULT` / `OFFLINE` | `AVAILABLE` | 故障解除且自检成功 |

对正在预约或充电的桩执行故障/离线，必须由 ChargingService 先结束相应业务，不能只改一列状态。

### 6.3 事务不变量

以下动作分别必须在一个 SQLite 事务内完成，建议写事务用 `BEGIN IMMEDIATE`：

- 预约：确认桩为 `AVAILABLE`，创建预约和 `RESERVED` 订单，再把桩改为 `RESERVED`；
- 开始：预约 `ACTIVE -> FULFILLED`、订单 `RESERVED -> CHARGING`、桩 `RESERVED -> CHARGING`；
- 停止：订单 `CHARGING -> WAITING_PAYMENT`、写入计量结果、桩 `CHARGING -> AVAILABLE`、累计桩统计；
- 支付：条件检查订单仍是 `WAITING_PAYMENT`，余额足够后扣款并把订单改为 `COMPLETED`；
- 取消/超时：同步更新预约、订单和电桩。

更新语句必须带旧状态条件，并检查受影响行数。例如支付不能先 `SELECT` 再无条件 `UPDATE`。重复支付可返回既有完成结果，但绝不能重复扣款。

数据库的 partial unique index 还会保证：

- 同一用户最多一个 `ACTIVE` 预约；
- 同一电桩最多一个 `ACTIVE` 预约；
- 同一用户最多一个未完成订单；
- 同一电桩最多一个 `RESERVED` / `CHARGING` 订单。

### 6.4 当前实现入口

- `ChargingStateMachine` 提供预约、订单和电桩的显式迁移表；Repository 的旧状态
  `WHERE` 条件和影响行数检查是并发写入时的最终约束；
- `ChargingService` 提供预约、取消、开始、实时状态和停止；
- `ChargingService` 根据服务端 UTC 时间计算充电时长，`BillingService` 使用额定功率、
  时长和订单电价快照执行纯整数计费；
- `OrderService` 提供待支付订单结算；
- `ChargingRepository` 和 `OrderRepository` 以 `BEGIN IMMEDIATE` 执行多表事务和旧状态条件更新。

实时状态是派生快照，只在停止时固化计量值。重复停止和重复支付返回已有结果，
不重复累计或扣款。预约有效期为 15 分钟；当前采用服务端惰性扫描，下一次预约、取消、
开始或状态查询会原子将到期业务改为 `EXPIRED` / `CANCELLED` / `AVAILABLE`，尚无定时推送。

## 7. 线程模型

生产入口现使用 `ServerRuntime` 分离界面和服务线程：

- GUI 线程拥有管理页面与 ServerRuntime 门面，只接收监听状态、连接数量和安全错误；
- 专用 QThread 在 `run()` 内创建完整服务对象图，然后进入 `exec()` 事件循环；
- 工作线程拥有 QTcpServer、QTcpSocket、ClientSession、Dispatcher、Service、Repository 和 SQLite；
- 网络收发、Session 身份更新、业务及 SQL 在同一工作线程执行，保持原有请求顺序和事务接口；
- 工作线程通过显式 queued signal 通知 GUI，不向 GUI 传递 Socket、Repository 或 SQL handle；
- QThread 对象本身仍属于 GUI，不在其槽中处理业务；`run()` 仅负责装配、事件循环和栈式释放；
- 关闭时请求 interruption/quit，让当前操作结束，再依次销毁网络、业务、Repository、数据库；不调用 terminate；
- SQLite 连接的创建、查询、关闭、removeDatabase 都在服务工作线程，且晚于 Repository 销毁。

这取代原先“Socket 留在 GUI、仅移动数据库”的候选方案，避免引入跨线程 Session 请求排队和
身份竞争。当前是 GUI + 单服务线程，不是线程池；慢 SQL 不阻塞界面，但会延迟其他客户端请求。
既有 ChargingServer 仍可在同线程测试夹具中使用；生产程序统一使用 ServerRuntime。
具体生命周期、测试及后续管理端接入边界见 `docs/development/server_threading.md`。

## 8. SQLite 生命周期

- Server 默认数据库路径使用 `QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)`，并允许 `--database <path>` 覆盖；
- 测试使用 `QTemporaryDir` 下的真实文件，不使用跨连接不可共享的 `:memory:`；
- 每个新 connection 执行 `foreign_keys=ON` 和 `busy_timeout=5000`；
- 新数据库执行 `schema.sql`；仅 demo/测试模式执行 `seed.sql`；
- 通过 `PRAGMA user_version` 管理迁移，当前版本是 1；
- `QSqlQuery::exec()` 不应直接传入含多个语句的整个文件。脚本执行器需逐条执行受控 SQL，并在任一失败时回滚；
- 所有动态值使用 prepared query + bind，不拼接 SQL；
- Repository 对外返回领域错误，不向 Client 暴露原始 SQLite 错误或文件路径。

## 9. 最小登录闭环

首个集成目标只做手机号登录，但必须实际经过全部层：

```text
Login page
  -> client auth service
  -> ClientConnection
  -> v1 frame + USER_LOGIN JSON
  -> ClientSession / RequestDispatcher
  -> UserService::loginOrRegister
  -> UserRepository
  -> SQLite
  -> response frame
  -> client auth service
  -> home page
```

行为：

1. Client 使用正则 `^1[0-9]{10}$` 做即时校验；Server 必须再次校验；
2. Repository 按手机号查询；
3. 不存在时插入昵称 `用户` + 手机号后四位，余额为 0；
4. 并发首次登录使用唯一索引及 `INSERT OR IGNORE`（或捕获唯一冲突后重查）保证只创建一行；
5. 冻结用户返回 `USER_FROZEN`；
6. 成功响应包含 `created` 和安全的 `user` DTO，不返回任何管理员密码字段；
7. Session 绑定已登录 `userId` 和 `USER` 角色；后续需要登录的动作不能信任 Client 自报的 userId；
8. Client 以 `requestId` 关联响应并设置超时，不能假定响应顺序；
9. 新用户默认余额是 0。Seed 中测试用户 `13800138000` 为便于演示预充 100.00 元，两者不可混淆。

## 10. 五人并行边界

候选公共契约形成后，五人分支边界为：

- 组长：`feature/socket-core` / 核心状态机与集成；
- 成员 2：`feature/client-station` / 登录、找站、找桩、地图入口；
- 成员 3：`feature/client-profile-charging` / 个人中心、充值、订单与充电 UI；
- 成员 4：`feature/server-dashboard-management` / 管理登录、Dashboard、用户/站/桩管理；
- 成员 5：`feature/database-repositories` / 初始化、Repository、事务和 DB 测试。

登录和核心充电闭环已经提供可复用的 Socket、Session、Service、Repository 和数据库运行
时边界。成员可以在各自目录基于候选接口并行开发，但在五人确认前，不宣布公共
契约冻结，也不各自重复创建 Socket/SQL 基础层或复制公共 Model。跨模块 PR 先更新相应
设计文档，经组长 review 并通过严格 CI 后再改公共接口，五人确认后记录冻结结论。

## 11. Qt 6.2.4 兼容红线

- 只使用 C++17，不使用 designated initializer、`std::format`、ranges 等 C++20 功能；
- 不使用 `QPromise`、QtFuture 的较新 API、`QChronoTimer`、`QHttpHeaders`、`QNetworkInformation`；
- 不使用较新版本才提供的 `_qs` 字面量和容器便利函数；
- 使用 `QStringLiteral`、`QByteArray`、`QJsonDocument`、`QJsonObject`、signals/slots；
- CMake 显式开启 `AUTOMOC`、`AUTOUIC`、`AUTORCC`，不依赖较新 Qt 的项目辅助命令；
- `find_package(Qt6 6.2.4 ...)` 表示最低兼容版本；最终必须在 6.2.4 构建；
- Socket 长度头手动按大端编码，不使用可能受 `QDataStream::Version` 影响的对象序列化；
- 不得提交操作系统元数据、构建目录、用户 Kit 文件或本机绝对路径。

## 12. 真实接口并行放行门槛

- `charging_common` 能在 Qt 6.2.4/C++17 编译；
- enum 的字符串往返及未知值拒绝测试通过；
- v1 请求/响应 envelope 往返测试通过；
- 帧头拆分、正文拆分、粘包、多帧、零长度、超大帧测试通过；
- schema + seed 能在空数据库执行，seed 可重复执行；
- `foreign_key_check` 无输出，`integrity_check` 为 `ok`；
- 已有手机号登录、首次自动注册、非法手机号、冻结用户、重复首次登录均有集成测试；
- 从干净 clone 按 README 在 Ubuntu 22.04 + Qt 6.2.4 构建并运行 Server/Client；
- 五人确认候选契约并留下记录；
- 完成以上门槛后由组长记录放行结论，再允许五人同时接真实接口。
