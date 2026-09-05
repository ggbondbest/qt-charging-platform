# 八个用户接口：业务实现与运行

PR #20 已合入 develop（`a136d10`），本业务分支已同步该基线，PR 直接面向 develop。
不必等待 PR #19；本分支没有引入其尚待修复的备份/恢复功能，也没有修改数据库 schema。

## 实际代码链路

```text
个人/订单/钱包页面 → 客户端 Service → NetworkRequestTransport
站点/预约页面 → StationQueryService / ReservationService
                              ↓ 共用登录后的 ClientConnection
TCP → ClientSession → RequestDispatcher → UserApiService
                                         ↓
                                  UserApiRepository → SQLite
```

- `server/services/user_api_service.*` 是这八个用户接口的统一业务入口，并非八个新类。
  负责动作映射、Session ID 检查、契约校验、头像清单、错误和公共模型 JSON 映射。
- `server/repositories/user_api_repository.*` 是用户场景专用查询/写入仓库，返回 SQL 行数据，
  不拼 TCP 报文。与 PR #19 的管理仓库隔离，避免把管理查询直接暴露给用户。
- 每次业务操作在 SQLite `BEGIN IMMEDIATE` 事务内重新检查用户是否存在且 ACTIVE；
  个人查询始终使用 Session ID。分页 count/list 使用同一事务快照。
- 充值通过全局唯一流水号和写事务保证幂等。成功重放返回原记录及当前余额；FAILED
  记录不转成成功；余额溢出/流水插入失败全部回滚。事务锁适合当前实训规模，尚未做高并发性能优化。
- 站点/预约/订单查询复用已有预约过期更新，将预约 EXPIRED、订单 CANCELLED、桩释放一并提交。
- `NetworkRequestTransport` 复用现有 requestId 关联、10 秒超时，不自动重发写操作。
  使用 `sendFor(QObject*, ...)` 防止已销毁 Service 被旧回调访问。
- 真实登录用 `HomeShell(user, connection, parent)`；不传连接的独立预览继续用 Mock。
  断线/退出登录会销毁旧壳并回登录页；重新登录建立新的 Session。登录失败也清除服务器旧身份。

## Ubuntu / Qt 6.2.4 验证

在仓库根目录执行（Qt Creator 也可直接打开根 `CMakeLists.txt`）：

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCHARGING_PLATFORM_STRICT_QT_VERSION=ON
cmake --build build --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

分别在两个终端启动；`--demo-seed` 仅用于演示库，提供 3 个站点及电桩：

```bash
./build/server/charging-server --database ./demo-user-api.sqlite3 --demo-seed
```

```bash
./build/client/charging-client
```

两个程序默认使用 `127.0.0.1:9527`。不加 demo-seed 的新库站点列表为空是正常的，
真实模式不会再用 Mock 站点掩盖空数据库。不要把演示库提交到 Git。

可手动检查：登录 → 找站/站内电桩 → 我的预约 → 我的资料修改 → 充值 → 充值流水 → 订单。
改昵称、充值后重启服务端再登录，数据应仍然存在。换另一个手机号登录，不应看到前一用户的
预约、订单和充值流水。

## 充值结果不明时

真实钱包发送前将 `{amountCents, transactionNo}` 保存到 QSettings，按服务端地址/端口与
用户 ID 的 SHA-256 值隔离。超时、断线、数据库错误或响应损坏时保留原意图。
重新登录后用**原金额**重试，沿用流水号，不会再入账。不同金额会被拦截，不能绕过未确认交易。
收到有效成功结果或明确 INVALID_ARGUMENT/IDEMPOTENCY_CONFLICT/RECHARGE_FAILED 后才清理。
不要删除应用设置来“修复”超时；它可能保存着已入账但尚未确认的流水号。

## 测试覆盖和明确边界

- `user_api_integration`：八接口、充电/支付旧闭环、伪造 userId、冻结用户、非法字段、
  站点隐藏、分页/字面关键词、跨页真实列表、过期释放、充值冲突/FAILED/余额溢出、
  SQL 触发器注入失败回滚、两个 TCP Session 与独立 SQLite 连接并发、文件持久化、
  适配器回调一次性/生命周期、10 秒超时、重连身份、持久流水重试。
- `client_navigation`：真实登录后的页面使用 NetworkRequestTransport，展示 SQLite 种子站点。
- 真实预约仍是即时生效、保留 15 分钟；页面禁用未来时间和车辆绑定控件，不伪造这些字段。
- 本 PR 不新增“开始充电”按钮。`START_CHARGING {reservationId}` 已有服务端实现并在集成
  测试覆盖；组员 2 继续补预约页面入口。已有 CHARGING 订单可以从订单列表进入实时充电页，
  停止/支付使用真实通道。不要将“八个接口已接通”理解为全部页面交互均已完工。
- 地图导航、车辆本地设置、未来时段预约、费用拆分、故障上报、退款、管理员登录不在本次业务范围。
- 站点/电桩/预约页面目前自动聚合所有分页后展示，适合实训数据量；大规模数据需后续改成逐页加载。
