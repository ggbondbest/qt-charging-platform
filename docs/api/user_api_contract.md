# 用户端接口契约 v1

基于 `develop` 的 `034ada2`（已含 PR #18），面向五人并行接入。
本文与 [Socket 协议](socket_protocol.md) 共同构成约定；已有登录及预约—充电—支付
七个动作不变。后续实现 PR 必须同步更新本文实现状态，不能把 Mock 当作服务端实现。

## 1. 本 PR 实际交付与边界

交付八个动作的请求/成功响应/错误语义、公共动作及错误常量、可复用的请求校验与
规范化函数 `charging::protocol::user_api::normalizeRequestData`、可执行 JSON 示例及测试。
预约客户端改用公共 `GET_RESERVATIONS` 常量，不再上传 `userId`。

**本 PR 不增加 Dispatcher 路由、Service 数据库业务、数据库迁移或真实网络适配器；
八个动作目前仍不能通过 Server 完成业务。** 新校验函数尚未接入 Dispatcher，
它只处理字段，不代替登录鉴权、用户隔离、头像白名单或充值事务。

| 动作 | 本 PR | 当前真实 Server | 客户端接入前需补齐 |
| --- | --- | --- | --- |
| `GET_STATIONS` | 契约 + 请求校验 + 示例测试 | 未实现 | 分页获取，不把当前页当全部站点 |
| `GET_CHARGERS` | 同上 | 未实现 | 分页获取，不用单页条数算站点总桩数 |
| `GET_RESERVATIONS` | 同上 + 公共常量替换 | 未实现 | 分页、显示扩展字段、关联订单恢复 |
| `GET_USER_INFO` | 契约 + 请求校验 + 示例测试 | 未实现 | `IRequestTransport` 真实网络适配 |
| `UPDATE_USER_INFO` | 同上 | 未实现 | 同上；头像使用双方约定的内置 key |
| `RECHARGE` | 同上 + 幂等语义 | 未实现 | 持久保存流水号、重试、读取 `balanceCents` |
| `GET_RECHARGE_RECORDS` | 契约 + 请求校验 + 示例测试 | 未实现 | 真实传输、严格解析分页响应 |
| `GET_ORDERS` | 同上 | 未实现 | 真实传输、关联名称、恢复正在充电订单 |

仍已实现的七个动作：`USER_LOGIN`、`RESERVE_CHARGER`、`CANCEL_RESERVATION`、
`START_CHARGING`、`GET_CHARGING_STATUS`、`STOP_CHARGING`、`PAY_ORDER`。
PR #19 是数据层工作，不自动使以上八个动作上线；合入前须解决其审查问题。

## 2. 通用规则

- 请求/响应继续使用 v1 envelope 与长度前缀 TCP 帧，`requestId` 用于对应请求和响应。
  下文及示例文件展示的是 envelope 中的 `data`，不是可直接写入 Socket 的完整报文。
- 八个动作均要求登录用户 Session。未登录返回 `UNAUTHORIZED`；用户被冻结返回
  `USER_FROZEN`。Service 每次操作检查当前用户状态。不能信任客户端 `userId`，即使
  传入也忽略。断线后重新登录，不能把旧连接 Session 当成仍有效。
- ID 为 `[1-9][0-9]*` 的十进制字符串，范围不超过正 `qint64`。ID 不允许 JSON number、
  前导零、符号或空白。可空关联 ID 用 `null`，不使用 `"0"`。
- 金额为整数分，电价为分/kWh，电量 Wh，功率 W，时长秒；时间为 UTC ISO-8601。
  金额计算及 SQL LIMIT/OFFSET 计算用 `qint64`，禁止以浮点数计算钱。
- 字符串长度除 ASCII 限定字段外按 Qt `QString::size()` 的 UTF-16 code unit 计。
  `nickname`、`keyword` 使用 `trimmed()`；不擅自修剪 ID、状态、流水号。
- 未知额外字段忽略；已知字段类型错误（包括不允许的 `null`）返回 `INVALID_ARGUMENT`。
  envelope 自身错误仍按主协议返回 `INVALID_ENVELOPE` 等。
- 当前余额统一为 `balanceCents`；充值记录的 `balanceAfterCents` 是该次入账后的
  历史快照，不应改名为当前余额。

### 分页

五个列表动作统一接收可选整数 `page`（默认 1，范围 1..2147483647）、`pageSize`
（默认 20，范围 1..100）。不合法时拒绝，不静默截断或修改。响应必须包含
`page`、`pageSize`、`total` 和对应数组。`total` 是登录权限与筛选条件生效后、分页前
的总条数（0..2147483647），不是当前页长度；超过支持范围返回 `INTERNAL_ERROR`。
超出末页成功返回空数组，并保留真实 `total`。无结果为 `total: 0`、数组 `[]`，不返回 `null`。

`offset = (qint64(page) - 1) * pageSize`，Repository 可继续用 `limit/offset/totalCount`，
Service 负责映射，无须让数据库结构照搬 JSON 名称。计数与列表应使用同一读事务快照。
列表不是跨请求的冻结快照，数据变化后客户端可以刷新第一页。

### 共同错误

| code | 含义 |
| --- | --- |
| `UNAUTHORIZED` | 无登录用户 Session |
| `USER_FROZEN` | 当前用户不可操作 |
| `INVALID_ARGUMENT` | data 参数类型、范围或枚举错误；`details.field` 指出字段 |
| `NOT_FOUND` | 指定资源不存在或对该用户不可见，不泄漏他人资源是否存在 |
| `DATABASE_ERROR` | 数据库操作失败；响应不得包含 SQL、文件路径或原始数据库错误 |
| `INTERNAL_ERROR` | 非预期内部失败；返回安全提示 |

上述是接口实现后的业务错误；当前未注册动作仍返回 `UNKNOWN_REQUEST_TYPE`。
失败 `data` 为 `{}`，不返回部分业务结果；`success: false` 与主协议一致。

## 3. 八个接口

完整成功示例位于 [user_api_examples.json](user_api_examples.json)，由测试读取。
表中 User/Station/Charger/Reservation/Order/RechargeRecord 均指现有
`common/include/charging/common/model/models.h` 与 `model_json.cpp` 的 JSON 格式，
不是数据库列名，也不是另加一层 `item.model` 对象。示例给出全部基础字段。

### GET_STATIONS

- 请求：`keyword?: string`（修剪后 0..64，默认空），`page?`、`pageSize?`。
- 返回：`{stations: StationSummary[], page, pageSize, total}`。
- StationSummary = **平铺的完整 Station** + 必填 `distanceMeters: integer`。
  第一版无客户端坐标参数，`distanceMeters` 固定返回 `-1` 表示未知；客户端可通过地图
  服务另行计算，不得显示成 0 米。经纬度来自 Station，用于地图展示。
- 只返回 `ACTIVE` 站点；关键词按站点名称或地址的字面包含匹配，使用 SQLite 默认
  LIKE 大小写语义，并转义 `%`、`_` 及转义符；不把用户输入解释为通配符或 SQL。
  默认 `id ASC`。空关键词匹配全部可见站点。
- R1 最小字段沿用 `id/code/name/address/latitude/longitude/priceCentsPerKwh/status/
  totalChargers/availableChargers/distanceMeters`。`totalChargers` 为全站桩数；
  `availableChargers` 仅计 `AVAILABLE`，不能由当前分页计算。
- 距离/价格排序、快慢充筛选暂为客户端展示能力；分页结果上进行的排序不能宣称是
  全站全量排序。服务端排序、区域筛选和坐标参数需后续契约 PR，不能默默上线自定义字段。

### GET_CHARGERS

- 请求：必填 `stationId: string ID`，`page?`、`pageSize?`。
- 返回：`{chargers: Charger[], page, pageSize, total}`，`id ASC`。
- 必须验证站点存在且 `ACTIVE`，否则 `NOT_FOUND`；返回该站全部状态的桩供 UI 展示，
  只有 `AVAILABLE` 可尝试预约，是否成功由预约事务决定。

### GET_RESERVATIONS

- 请求：`status?: string`，`page?`、`pageSize?`。状态缺省或 `""` 表示全部；其余仅允许
  `ACTIVE/FULFILLED/CANCELLED/EXPIRED`，不接受 `ALL` 或订单状态。
- 返回：`{reservations: ReservationSummary[], page, pageSize, total}`，
  按 `reservedAt DESC, id DESC`。
- ReservationSummary = 平铺完整 Reservation + 必填 `stationName: string`、
  `chargerCode: string`、`orderId: string ID | null`。有对应订单就返回真实 ID；
  只有确实不存在关联时返回 `null`，不可拿 `orderNo` 代替 ID。
- 仅查 Session 用户的预约；状态和过期时间以服务器为准，客户端倒计时不能写数据库状态。
  `FULFILLED` 只表示预约已用于启动充电，不代表订单已经支付完成。
- 恢复流程：`ACTIVE` 预约可用 `reservation.id` 调 `START_CHARGING`；
  通过 `GET_ORDERS(status=CHARGING)` 找正在充电的订单，再用 `order.id` 查实时快照。

### GET_USER_INFO

- 请求：`{}`。
- 返回：`{user: User}`，完整资料来自 Session 用户，包括 `balanceCents`。
- 不返回管理信息或其他用户资料。

### UPDATE_USER_INFO

- 请求：`nickname?: string`、`avatarKey?: string`，至少出现一项。
- `nickname` 修剪后长度 1..32；`avatarKey` 匹配 `[A-Za-z0-9_-]{0,64}`，空串恢复默认。
  非空 key 还必须属于当前内置头像清单：`bolt/plug/car/leaf/cat/panda/moon/rocket`
  （与 `AvatarLibrary::all()` 一致），未知 key 返回 `INVALID_ARGUMENT`。
  公共校验仅检查格式，资源清单检查由 Service 完成；不支持 URL、路径或图片上传。
- 不提供的字段保持原值；两个字段一起提交时原子更新，不能只更新一半。
  `phone/balanceCents/status/userId` 等不可借此修改。
- 返回：`{user: User}`，包含最终完整资料和更新后的 `updatedAt`。

### RECHARGE（模拟充值，不接真实支付）

- 请求：必填 `amountCents: integer`（1..10000000，含上限，即 100000 元）、
  `transactionNo: string`（`[A-Za-z0-9_-]{1,40}`）。
- 客户端每次**新充值意图**生成唯一流水号，例如不带花括号的 UUID；发送前持久保存
  流水号和金额。超时/断线后相同意图复用流水号和金额；不能每次重试生成新号。
  `requestId` 只是传输关联号，不是防重复充值凭据。跨用户冲突也不能复用流水号。
- 返回：`{record: RechargeRecord, balanceCents: integer, idempotent: boolean}`。
  `record.status` 必须为 `SUCCESS`；首次成功 `idempotent=false`。
- 扣除其他操作影响后的当前余额为 `balanceCents`；重放时 `record` 保留原入账记录，
  `record.balanceAfterCents` 不随当前余额变化，两者可以不同。
- 同一流水号、同一 Session 用户、相同金额，且旧记录为 `SUCCESS`：返回原记录及当前余额，
  `idempotent=true`，不再入账。用户或金额不同：`IDEMPOTENCY_CONFLICT`，不泄漏原记录。
- 同用户同金额但旧记录为 `FAILED`：`RECHARGE_FAILED`，不转成成功，也不再次入账。
  只有收到明确失败结果后，用户主动发起新的充值意图才使用新流水号；超时并非明确失败。
- 余额增加和成功流水落库必须在同一事务中，余额不能超过 JSON 安全整数上限；
  超限返回 `INVALID_ARGUMENT`（`details.field=amountCents`），全部回滚。
  并发相同流水号必须靠数据库唯一约束与事务保证只加一次钱，不靠客户端按钮防抖。

### GET_RECHARGE_RECORDS

- 请求：`page?`、`pageSize?`。
- 返回：`{records: RechargeRecord[], page, pageSize, total}`，
  `createdAt DESC, id DESC`，仅查 Session 用户，含 `SUCCESS/FAILED` 两种已落库记录。
- `balanceAfterCents` 是历史快照；不得用某条记录覆盖当前钱包余额。

### GET_ORDERS

- 请求：`status?: string`，`page?`、`pageSize?`。状态缺省或 `""` 为全部；其余仅允许
  `RESERVED/CHARGING/WAITING_PAYMENT/COMPLETED/CANCELLED`。
- 返回：`{orders: OrderSummary[], page, pageSize, total}`，`createdAt DESC, id DESC`。
- OrderSummary = 平铺完整 Order + 必填 `stationName: string`、`chargerCode: string`。
  只查 Session 用户订单，不能将管理端全量查询直接返回。徽标计数可以用
  `status + page=1 + pageSize=1` 查询 `total`。
- 当前详情可用列表完整 Order；充电中的实时金额仍调用 `GET_CHARGING_STATUS`，
  不把旧列表快照当最终结算金额。支付以 `PAY_ORDER` 返回结果为准。

## 4. 本次不冻结为必填的扩展

- R1 的照片、营业时间、停车费/占位费、快充计数及嵌套 `price/chargers` 尚未冻结，
  不能用新嵌套对象替换已有平铺 Station 字段。
- R2 `feeDetail` 尚未冻结/实现；现有 `amountCents` 与固定单价计费仍是唯一结算依据。
  拆分电费/服务费/优惠前先明确规则、订单快照和数据库迁移，不伪造费用明细。
- R3 `estimatedMinutesRemaining`、`soc` 尚未冻结/实现，缺失显示“暂无估算”，不当作 0。
- PR #18 的未来时间段、车辆绑定及预约时长是客户端演示，不改变已有
  `RESERVE_CHARGER {chargerId}` 的服务器即时预约规则。真实时段预约须独立设计冲突检测。
- 扫码新动作、故障上报、资金消费/退款流水、管理端接口不在本 PR 范围。

## 5. 并行接入分工与验收

| 负责人员 | 后续实现内容 | 必测项目 |
| --- | --- | --- |
| 组长 | 将公共校验接入八个 Dispatcher 路由；Service 鉴权/映射；真实 `IRequestTransport` 适配 | 未登录、冻结、伪造 userId、超时/重连、安全错误 |
| 组员 2（站点/预约） | 分页站点与桩列表、预约扩展字段、开始充电入口；真实数据替换 Mock | 末页/空页、不可用桩、关联订单恢复、真实预约规则提示 |
| 组员 3（个人/订单/充电） | 资料、订单、钱包真实传输；充值流水号持久化与结果不明重试；订单页由旧显示时间排序对齐 createdAt 分页顺序 | 相同充值不重复入账、两种余额、状态/错误展示 |
| 组员 4（管理端） | 按原分工继续管理端，复用公共基础模型；不自行定义用户接口 | 不混用管理员权限和用户 Session |
| 组员 5（数据层） | 修复 PR #19；增加用户限定订单查询、预约 orderId；分页统计和充值事务 | 用户隔离、唯一流水并发、失败流水、余额溢出、事务回滚 |

接入者可复用 `normalizeRequestData(type, data, &normalized, &error)`，只使用其规范化
输出作为已知请求参数；身份另从 Session 获取。列表保留现有基础模型，扩展字段与模型平铺，
使用 `model::toJson()` 输出基础部分后补充字段，不修改数据库基础模型来塞 UI 字段。

各模块可以按示例并行写代码/Mock，但上线要在对应实现 PR 增加真实 TCP+SQLite 测试。
本次 `user_api_contract` 测试覆盖示例与字段校验，**不证明八个接口已端到端可用**。
现有 `MockRequestTransport` 尚非完整契约实现（如充值响应、默认分页、错误码和头像校验）；
本 PR 保留演示行为，后续接入 PR 必须同步调整 Mock 与客户端测试，不可直接当作合规 Server。
新增或改变必填字段、枚举、金额/状态语义时，先改本文、公共定义和测试，再改双方实现。
