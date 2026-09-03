# Socket JSON 协议 v1

状态：`candidate-v1`（登录和核心充电闭环已通过 Ubuntu 22.04 / Qt 6.2.4 严格 CI，
待五人确认后冻结）

实现入口：

- `common/include/charging/common/protocol/protocol.h`：envelope、动作名、错误码和 JSON codec；
- `common/include/charging/common/protocol/frame_codec.h`：TCP 长度帧 codec。

## 1. 传输层

协议运行在 TCP 之上。TCP 是字节流，单次 `readyRead` 不等于一条消息。每条消息使用以下 frame：

```text
+----------------------+--------------------------+
| payload length       | payload                  |
| 4 bytes, uint32, BE  | N bytes, UTF-8 JSON     |
+----------------------+--------------------------+
```

也就是：

```text
uint32_big_endian(N) || compact_utf8_json
```

规则：

- 长度只包含 JSON payload，不包含 4 字节头；
- payload 长度必须为 `1..1,048,576` 字节；
- JSON 根必须是 object，不使用换行分帧、不使用 `QDataStream << QString`；
- decoder 必须缓存半包，并循环取出一次读取中的多个完整 frame；
- 4 字节头、JSON 正文都可能被任意拆分；
- 零长度或超大 frame 属于不可恢复的 framing error，Server 记录安全日志后关闭该连接；
- 有效 frame 中的非法 JSON 可以返回协议错误；无法取得可靠 envelope 的错误不得猜测 requestId。

`FrameDecoder::append()` 已覆盖半包和粘包。调用方在它返回 `false` 后应关闭 socket。

## 2. 公共约定

- 编码：UTF-8；
- 字段名：`lowerCamelCase`，大小写敏感；
- 动作和状态：`UPPER_SNAKE_CASE`，大小写敏感；
- 所有 envelope 都携带整数 `protocolVersion: 1`；
- 数据库主键和业务 ID 使用十进制字符串，必须匹配 `[1-9][0-9]*` 且位于正
  `qint64` 范围内，例如 `"userId": "42"`；`requestId` 是请求关联标识，不受此规则限制；
- 金额使用整数分，例如 `"balanceCents": 10000` 表示 ¥100.00；
- 所有非 ID 整数必须在 JSON 可精确表示的区间
  `[-9007199254740991, 9007199254740991]` 内；业务字段的非负/正数约束另行适用；
- 电价使用分/千瓦时，功率使用瓦，电量使用瓦时，时长使用秒；
- 时间必须是带 `Z` 或明确 UTC offset 的 ISO-8601；接收后统一转 UTC，
  发送时使用 `2026-09-01T08:30:00.000Z` 形式；
- 可空时间使用 JSON `null`；
- `data` 和 `error.details` 始终是 object；
- 未知额外字段应忽略，缺少或类型错误的必填字段必须拒绝；
- 业务逻辑只依赖 `error.code`，不能解析可翻译的 `error.message`。

## 3. Request envelope

必填字段：

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `protocolVersion` | integer | 当前必须为 `1` |
| `kind` | string | 必须为 `REQUEST` |
| `type` | string | 非空，最多 64 字符，值见动作表 |
| `requestId` | string | 非空，最多 64 字符；同一连接未完成请求内唯一 |
| `data` | object | 即使无参数也必须传 `{}` |

Client 建议用 `QUuid::createUuid().toString(QUuid::WithoutBraces)` 生成 requestId。

```json
{
  "protocolVersion": 1,
  "kind": "REQUEST",
  "type": "USER_LOGIN",
  "requestId": "8f985c62-4d62-47c9-bdca-51b9ef933ed7",
  "data": {
    "phone": "13800138000"
  }
}
```

## 4. Response envelope

每个可解析 Request 恰好返回一个 Response。`type` 和 `requestId` 必须原样回显，Client 不能靠响应顺序匹配请求。

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `protocolVersion` | integer | 当前为 `1` |
| `kind` | string | 必须为 `RESPONSE` |
| `type` | string | 与请求相同 |
| `requestId` | string | 与请求相同 |
| `success` | boolean | 业务是否成功 |
| `data` | object | 成功数据；失败时通常为 `{}` |
| `error` | object/null | 成功必须为 `null`；失败必须为 object |

错误 object：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `code` | string | 稳定机器码，非空 |
| `message` | string | 面向日志或 UI 的可读说明 |
| `details` | object | 非敏感结构化上下文，无内容时 `{}` |

成功示例：

```json
{
  "protocolVersion": 1,
  "kind": "RESPONSE",
  "type": "USER_LOGIN",
  "requestId": "8f985c62-4d62-47c9-bdca-51b9ef933ed7",
  "success": true,
  "data": {
    "created": false,
    "user": {
      "id": "1",
      "phone": "13800138000",
      "nickname": "用户8000",
      "avatarKey": "",
      "balanceCents": 10000,
      "status": "ACTIVE",
      "createdAt": "2026-09-01T00:00:00.000Z",
      "updatedAt": "2026-09-01T00:00:00.000Z"
    }
  },
  "error": null
}
```

失败示例：

```json
{
  "protocolVersion": 1,
  "kind": "RESPONSE",
  "type": "USER_LOGIN",
  "requestId": "8f985c62-4d62-47c9-bdca-51b9ef933ed7",
  "success": false,
  "data": {},
  "error": {
    "code": "INVALID_PHONE",
    "message": "手机号必须为 11 位数字且以 1 开头",
    "details": {
      "field": "phone"
    }
  }
}
```

## 5. Event envelope

`EVENT` 已在公共 enum 中预留，用于后续服务端推送充电进度或电桩状态。v1 首版不发送 Event，也尚未冻结 Event 字段；任何成员不得自行定义并上线。需要时必须先更新本文档、公共 codec 和测试。

## 6. v1 动作注册表

### 6.1 用户端

| type | 用途 | 登录要求 | 首版主要 data |
| --- | --- | --- | --- |
| `USER_LOGIN` | 手机号免密登录/自动注册 | 匿名 | `phone` |
| `GET_STATIONS` | 查询附近/指定区域电站 | 用户 | 位置、筛选、分页 |
| `GET_CHARGERS` | 查询站内电桩 | 用户 | `stationId` |
| `RESERVE_CHARGER` | 预约空闲电桩 | 用户 | `chargerId` |
| `CANCEL_RESERVATION` | 取消本人有效预约 | 用户 | `reservationId` |
| `START_CHARGING` | 从有效预约开始充电 | 用户 | `reservationId` |
| `GET_CHARGING_STATUS` | 获取实时充电快照 | 用户 | `orderId` |
| `STOP_CHARGING` | 停止本人充电订单 | 用户 | `orderId` |
| `PAY_ORDER` | 支付待结算订单 | 用户 | `orderId` |
| `GET_USER_INFO` | 刷新本人资料 | 用户 | `{}` |
| `UPDATE_USER_INFO` | 修改昵称/头像 key | 用户 | 待详细接口冻结 |
| `RECHARGE` | 钱包模拟充值 | 用户 | `amountCents` |
| `GET_RECHARGE_RECORDS` | 查询本人充值记录 | 用户 | 分页参数 |
| `GET_ORDERS` | 查询本人订单 | 用户 | 状态、分页参数 |

### 6.2 管理端

| type | 用途 | 登录要求 | 首版主要 data |
| --- | --- | --- | --- |
| `ADMIN_LOGIN` | 管理员账号密码登录 | 匿名 | `username`, `password` |
| `GET_DASHBOARD` | 营收和设备概览 | 管理员 | 时间维度 |
| `GET_USERS` | 用户搜索/分页 | 管理员 | 关键词、分页 |
| `FREEZE_USER` | 冻结用户 | 管理员 | `userId` |
| `UNFREEZE_USER` | 解冻用户 | 管理员 | `userId` |
| `RESTART_CHARGER` | 模拟远程重启 | 管理员 | `chargerId` |
| `CREATE_STATION` | 新增电站 | 管理员 | 站点字段；另行冻结完整校验 |

动作名集中在 `protocol.h` 的 `request_type` namespace。当前 Dispatcher 已实现
`USER_LOGIN`、`RESERVE_CHARGER`、`CANCEL_RESERVATION`、`START_CHARGING`、
`GET_CHARGING_STATUS`、`STOP_CHARGING` 和 `PAY_ORDER`。表中其他动作是已预留的公共名称，
在对应成员实现并注册前会返回 `UNKNOWN_REQUEST_TYPE`。

## 7. USER_LOGIN 详细契约

请求 `data`：

| 字段 | 类型 | 规则 |
| --- | --- | --- |
| `phone` | string | `^1[0-9]{10}$` |

成功 `data`：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `created` | boolean | 本次是否自动创建用户 |
| `user` | object | `model_json.h` 定义的 User DTO |

服务端行为：

1. 再次校验手机号，不能只信 Client；
2. 已有 `ACTIVE` 用户直接返回；
3. 已有 `FROZEN` 用户返回 `USER_FROZEN`；
4. 不存在时创建昵称 `用户` + 手机号后四位，`balanceCents = 0`；
5. 唯一冲突后重查，确保并发首次登录不产生两个用户；
6. 成功后把连接 Session 设为 `USER` 并绑定数据库 user ID。

Seed 用户余额是演示数据，不是新注册默认值。

## 8. 预约—充电—结算详细契约

以下动作均要求当前 TCP Session 已通过 `USER_LOGIN` 绑定用户。Server 只使用
Session 中的 user ID；即使 Client 在 `data` 中额外传入 `userId`，也必须忽略。

### 8.1 `RESERVE_CHARGER`

请求 `data`：

```json
{ "chargerId": "1" }
```

成功 `data`：

```json
{
  "reservation": { "id": "1", "status": "ACTIVE" },
  "order": { "id": "1", "status": "RESERVED" }
}
```

上例仅展示关键字段，实际响应使用 `model_json.h` 的完整 DTO。Server 在同一个
`BEGIN IMMEDIATE` 事务中确认用户和电站可用、创建 15 分钟有效的预约、按当前电价快照
创建订单，并将电桩从 `AVAILABLE` 改为 `RESERVED`。用户已有未完成订单时不允许
再次预约。当前版本在下一次预约、取消、开始或状态查询时扫描并原子清理已过期预约，
暂不提供准点定时扫描或主动过期推送。

### 8.2 `CANCEL_RESERVATION`

请求 `data`：

```json
{ "reservationId": "1" }
```

成功 `data` 包含完整 `reservation` 和 `order`。一个事务内完成：

```text
Reservation ACTIVE -> CANCELLED
Order       RESERVED -> CANCELLED
Charger     RESERVED -> AVAILABLE
```

对已取消预约重试时返回现有结果，不重复改状态。

### 8.3 `START_CHARGING`

请求 `data`：

```json
{ "reservationId": "1" }
```

成功 `data` 包含完整 `reservation` 和 `order`。仅本人、未过期的 `ACTIVE` 预约可以开始，
一个事务内完成：

```text
Reservation ACTIVE   -> FULFILLED
Order       RESERVED -> CHARGING
Charger     RESERVED -> CHARGING
```

对同一预约重复开始返回 `INVALID_STATE_TRANSITION`。

### 8.4 `GET_CHARGING_STATUS`

请求 `data`：

```json
{ "orderId": "1" }
```

成功 `data`：

```json
{
  "order": {
    "id": "1",
    "status": "CHARGING",
    "durationSeconds": 250,
    "energyWh": 500,
    "amountCents": 60
  },
  "currentPowerWatts": 7200
}
```

充电中的 `order` 是实时派生快照，查询本身不将计量数据写回 SQLite。断线重新登录后可以
用同一 order ID 恢复查询。非充电状态返回已持久化的订单，并使
`currentPowerWatts = 0`。

### 8.5 `STOP_CHARGING`

请求 `data`：

```json
{ "orderId": "1" }
```

成功 `data` 包含完整 `order`。Server 按自己的 UTC 时间和电桩额定功率计算最终费用，
并在一个事务内完成：

```text
Order   CHARGING -> WAITING_PAYMENT
Charger CHARGING -> AVAILABLE
```

同时固化订单的时长、电量、金额和停止时间，并且只累加一次电桩充电次数与时长。
对 `WAITING_PAYMENT` 或 `COMPLETED` 订单重试停止时，返回已保存结果，不重复计费。

### 8.6 `PAY_ORDER`

请求 `data`：

```json
{ "orderId": "1" }
```

成功 `data`：

```json
{
  "order": { "id": "1", "status": "COMPLETED" },
  "balanceCents": 9880
}
```

仅本人的 `WAITING_PAYMENT` 订单可扣款。余额条件扣减与
`Order WAITING_PAYMENT -> COMPLETED` 处于同一事务；余额不足时两者都不改变。对已完成
订单重试支付只返回当前结果，不再扣款。

### 8.7 计费规则

v1 使用固定额定功率模拟，全程采用 `qint64` 整数并进行溢出检查：

```text
durationSeconds = floor(serverNowUtc - startedAtUtc)
energyWh        = floor(powerWatts * durationSeconds / 3600)
amountCents     = floor((energyWh * unitPriceCentsPerKwh + 500) / 1000)
```

最后一式表示金额精确到分并按半分向上取整。电价是创建订单时的快照，充电期间修改电站
当前电价不会改变已有订单。

## 9. 错误码

### 9.1 framing / envelope

| code | 场景 |
| --- | --- |
| `INVALID_FRAME` | 零长度或无效 framing |
| `PAYLOAD_TOO_LARGE` | frame/JSON 超过 1 MiB |
| `INVALID_JSON` | payload 不是合法 JSON |
| `INVALID_ENVELOPE` | 根、字段、类型或 kind 不合法 |
| `UNSUPPORTED_PROTOCOL_VERSION` | 版本不是 1 |
| `UNKNOWN_REQUEST_TYPE` | Dispatcher 未注册该动作 |

### 9.2 鉴权和业务

| code | 场景 |
| --- | --- |
| `INVALID_PHONE` | 手机号格式错误 |
| `USER_FROZEN` | 用户已冻结 |
| `UNAUTHORIZED` | 未登录、角色不符或资源不属于当前用户 |
| `CHARGER_NOT_AVAILABLE` | 预约目标电桩或其所属电站不可用 |
| `INVALID_STATE_TRANSITION` | 业务状态不允许该操作 |
| `INSUFFICIENT_BALANCE` | 支付余额不足 |
| `NOT_FOUND` | 目标不存在；资源不属于当前用户时使用 `UNAUTHORIZED` |

### 9.3 服务端和客户端本地

| code | 场景 |
| --- | --- |
| `DATABASE_ERROR` | 数据库失败；message 不含原始 SQL/路径 |
| `INTERNAL_ERROR` | 未分类服务端错误 |
| `CONNECTION_ERROR` | Client 本地连接失败 |
| `REQUEST_TIMEOUT` | Client 本地请求超时 |

Client 必须为未知 code 提供统一兜底提示。Server 日志可以关联 requestId 和经过控制的
内部错误分类。响应和日志都不得包含密码、password hash、salt、完整 SQL 或本地文件路径；
通用异常边界不得直接输出未脱敏的 `exception.what()`。

## 10. 会话、并发与幂等

- 初始 Session 为 `ANONYMOUS`；登录后只能成为 `USER` 或 `ADMIN` 之一；
- 需要鉴权的动作从 Session 取 identity，不接受 Client 用 `userId` 越权；
- 一个连接可以有多个未完成请求；响应顺序可以不同；
- requestId 在“连接 + 未完成请求”范围内唯一；重复未完成 requestId 返回 `INVALID_ENVELOPE`；
- 断线不表示充电停止。重新登录后用 `GET_CHARGING_STATUS` 恢复状态；
- `PAY_ORDER`、`STOP_CHARGING`、取消预约等必须由数据库旧状态条件保证幂等；
- v1 不提供明文密码以外的传输加密。课程本机演示只允许 loopback/可信局域网；如跨机器或公网必须先增加 TLS，不能把该协议宣称为生产安全。

## 11. 兼容策略

- v1 可新增可选响应字段，旧 Client 忽略未知字段；
- 不能删除/改名字段、改变字段类型/单位、复用状态含义；
- 新增必填请求字段或改变 framing 属于不兼容变化，必须提升 protocolVersion；
- 公共动作、错误码、enum 字符串的变更必须同时修改代码、本文档和测试；
- 服务端不支持收到的版本时返回 `UNSUPPORTED_PROTOCOL_VERSION`；如果 framing 已损坏则直接断开。

## 12. 最低测试集

- Request 和成功/失败 Response 序列化后可严格解析；
- 缺失 `data`、错误 kind、空 requestId、错误版本、数组根、非法 JSON 被拒绝；
- 4 字节头逐字节输入、正文逐字节输入仍只产出一条 payload；
- 两帧拼接一次输入产出两条 payload；
- 一帧结束加下一帧半包时，只产出第一条并保留半包；
- 长度 0 和大于 1 MiB 返回稳定错误并 reset；
- UTF-8 中文昵称往返不损坏；
- ID 保持字符串，金额保持整数；
- 相同 requestId 的响应能够由 Client 正确关联；
- 未知 type、未登录请求和冻结用户返回对应错误码。
- `RESERVE_CHARGER`、`CANCEL_RESERVATION`、`START_CHARGING`、
  `GET_CHARGING_STATUS`、`STOP_CHARGING`、`PAY_ORDER` 均能通过 Dispatcher 到达对应用例；
- 鉴权动作只使用 Session 绑定的 user ID，伪造 `data.userId` 无效，访问他人资源返回
  `UNAUTHORIZED`；
- 计费整数公式、电价快照和余额不足路径有确定性测试；
- 15 分钟预约惰性过期会在后续业务请求中原子同步预约、订单和电桩状态；
- 重复取消、停止和支付不会重复更新统计或扣款；
- 预约、开始、停止和支付在中途失败时完整回滚；
- 真实 TCP 连接覆盖登录、预约、开始、状态查询、停止和支付的完整闭环。
