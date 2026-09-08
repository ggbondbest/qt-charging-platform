# 用户端接口契约 v1

基于 `develop` 的 `034ada2`（已含 PR #18），面向五人并行接入。
本文与 [Socket 协议](socket_protocol.md) 共同构成约定；已有登录及预约—充电—支付
七个动作不变。后续实现 PR 必须同步更新本文实现状态，不能把 Mock 当作服务端实现。

## 1. 实现状态（用户业务 PR 更新）

交付八个动作的请求/成功响应/错误语义、公共动作及错误常量、可复用的请求校验与
规范化函数 `charging::protocol::user_api::normalizeRequestData`、可执行 JSON 示例及测试。
预约客户端改用公共 `GET_RESERVATIONS` 常量，不再上传 `userId`。

PR #20 交付契约；后续用户业务分支已增加八个 Dispatcher 路由、`UserApiService`、
`UserApiRepository` 和 `NetworkRequestTransport`，八个动作可通过 TCP 查询/修改真实 SQLite。
2026-09-08 迭代追加数据域三个只读动作 `GET_USER_STATS`/`GET_COUPONS`/`GET_NOTIFICATIONS`
（冻结只新增），配套 `notifications`/`coupons` 两张新表与三个写挂点
（`STOP_CHARGING` 落充电结束通知、`PAY_ORDER` 落支付成功通知、预约到期清扫落超时提醒；
`RECHARGE` 达标同事务发券）。同日再加签到积分域 `CHECK_IN`/`GET_POINTS`
（`user_checkins`/`points_ledger` 两表，日粒度幂等）与评价域
`SUBMIT_CHARGER_RATING`/`GET_MY_RATINGS`（`charger_ratings` 表，`order_id UNIQUE`
一单一评幂等），动作总数达十五个。
本次不依赖 PR #19，不引入其管理查询或备份恢复代码，不修改 schema v1。
运行、验证及剩余 UI 工作见 [用户业务接入说明](../development/user_api_runtime.md)。

| 动作 | 当前服务端功能 | 当前真实 Server | 客户端状态 |
| --- | --- | --- | --- |
| `GET_STATIONS` | ACTIVE 站点关键词/分页及全站桩数 | 已实现 | 自动获取全部分页后展示 |
| `GET_CHARGERS` | 可见站点下全部状态电桩分页 | 已实现 | 自动获取全部分页后展示 |
| `GET_RESERVATIONS` | 本人预约/状态/关联名称和 orderId | 已实现 | 真实分页、扩展字段与独立 live 缓存 |
| `GET_USER_INFO` | 本人资料和余额 | 已实现 | 真实适配器 |
| `UPDATE_USER_INFO` | 昵称/头像原子更新与头像清单校验 | 已实现 | 真实适配器 |
| `RECHARGE` | 流水幂等、余额和流水同事务 | 已实现 | 持久保存流水号、同金额重试、读取当前余额 |
| `GET_RECHARGE_RECORDS` | 本人充值流水分页 | 已实现 | 真实传输、分页校验 |
| `GET_ORDERS` | 本人订单/状态/关联名称/分页 | 已实现 | 真实传输；点击 CHARGING 订单进入实时页面 |
| `GET_USER_STATS` | 本人已完成订单按月聚合 + 碳排换算 | 已实现 | 月报页 StatsPage（statsService 桥），mock 同形 |
| `GET_COUPONS` | 本人券状态过滤 + 分页 | 已实现 | 券页 CouponPage（couponService 桥），接线即拉缓存 |
| `GET_NOTIFICATIONS` | 本人站内通知分页（充电结束/支付成功/超时提醒） | 已实现 | NotificationPage（NotificationService 服务端通道） |
| `CHECK_IN` | 每日签到幂等入账 + 积分余额 | 已实现 | PointsPage 签到按钮（pointsService 桥），mock 同形 |
| `GET_POINTS` | 本人积分总分 + 流水分页 | 已实现 | PointsPage 流水卡（pointsService 桥） |
| `SUBMIT_CHARGER_RATING` | 完成单一次评价（order_id UNIQUE 幂等重放） | 已实现 | OrderDetailPage 完成态评价卡（ratingsService 桥） |
| `GET_MY_RATINGS` | 本人评价分页（JOIN 桩/站展示字段） | 已实现 | RatingsPage 我的评价（ratingsService 桥） |

仍已实现的七个动作：`USER_LOGIN`、`RESERVE_CHARGER`、`CANCEL_RESERVATION`、
`START_CHARGING`、`GET_CHARGING_STATUS`、`STOP_CHARGING`、`PAY_ORDER`。
PR #19 是数据层工作，不自动使以上八个动作上线；合入前须解决其审查问题。

## 2. 通用规则

- 请求/响应继续使用 v1 envelope 与长度前缀 TCP 帧，`requestId` 用于对应请求和响应。
  下文及示例文件展示的是 envelope 中的 `data`，不是可直接写入 Socket 的完整报文。
- 本文档动作均要求登录用户 Session。未登录返回 `UNAUTHORIZED`；用户被冻结返回
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

以上是已接入接口的业务错误；其余未注册动作仍返回 `UNKNOWN_REQUEST_TYPE`。
失败 `data` 为 `{}`，不返回部分业务结果；`success: false` 与主协议一致。

## 3. 十五个接口

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

### GET_USER_STATS

- 请求：`period?` ∈ `"week" | "month" | "year"`（缺省 `"month"` = 冻结前行为）、
  `months?` 整数 `1..12`，缺省 6（= 取最近 N 个周期）。
- 返回：`{months: [{monthKey, orderCount, energyWh, amountCents,
  durationSeconds, co2Grams}]}`，新→旧；**仅 COMPLETED 订单**按 `created_at` 所在
  周期聚合，不分页。`monthKey` 随档位取 `%Y-W%W` / `YYYY-MM` / `YYYY`（周期键）。
- `co2Grams = round(energyWh × 0.5568)`（生态环境部全国电网平均排放因子
  0.5568 tCO₂/MWh）；TODO(contract)：因子与取整口径业务终确认。
- 金额为分、电量 Wh、时长秒；展示单位换算留在页面层。

### GET_COUPONS

- 请求：`status?` ∈ `"available" | "used" | "expired"`（线上小写；缺省为全部）、
  `page?`、`pageSize?`。
- 响应的 `status` 与 `status` 过滤、`total` 计数均按**有效状态**口径：
  `USED` 最优先，其次 `expiresAt` ≤ 服务端当前时间即 `expired`，其余 `available`；
  存储列不因到期改写（纯读侧派生）。mock 通道同口径。
- 返回：`{coupons: [{id, kind: "cash"|"discount", title, valueCents,
  discountTenths|null, thresholdCents, condition, expiresAt, expiresAtUtc, status,
  source, createdAt, updatedAt}], page, pageSize, total}`，`createdAt DESC, id DESC`。
- `expiresAtUtc` 为 epoch 毫秒数字（CouponPage 展示口径）；`condition` 是服务端生成
  的展示文案（"充电满 ¥X 可用" / "无门槛"）。
- 发券规则（一期）：单笔充值 ≥ ¥50 同事务发 ¥5 现金券、30 天有效；
  幂等重放不重发（发券点在充值写库分支内，重放路径提前返回）。TODO(contract)：
  规则业务终确认。核销/抵扣不在本契约（TODO(contract)：PAY_ORDER 抵扣规则二期）。

### GET_NOTIFICATIONS

- 请求：`page?`、`pageSize?`。
- 返回：`{notifications: [{id, type, title, body, createdAtUtc}], page, pageSize,
  total}`，`createdAt DESC, id DESC`；`type` ∈ `"charging_stopped" | "order_paid"`
  （线上小写）。不回显 `userId`/`readAt`。
- 服务端生成挂点：`STOP_CHARGING` 事务落充电结束通知、`PAY_ORDER` 事务落支付成功
  通知；两者幂等重放不重复落库（重放分支在挂点前返回）。
- 已读标记（`read_at`）与推送通道一期不冻结；TODO(contract)。

### CHECK_IN

- 请求：无字段（`{}`）。
- 返回：`{day: "YYYY-MM-DD", points, gained, alreadyCheckedIn}`。`day` 为服务端
  UTC 日期，与 `user_checkins` 落库键同一时刻换算（响应与入库不跨 UTC 午夜漂移）；
  `points` = 签到后总分（`points_ledger` 聚合单一事实源）。
- 幂等语义：`(user_id, day)` 主键，当日重放返回 `alreadyCheckedIn: true`、
  `gained: 0`、总分不变（重放不报错——与 `RECHARGE` 同族）。
- 奖励常量（每日 10 分）定义于 contract.h；TODO(contract)：积分数值规则业务终确认。

### GET_POINTS

- 请求：`page?`、`pageSize?`。
- 返回：`{points, entries: [{id, amount, reason, createdAtUtc}], page, pageSize,
  total}`，新→旧。`reason` 词表一期只映射 `CHECK_IN` → "每日签到"，其余运营文案
  原样透传；TODO(contract)：词表评审。不回显 `userId`。

### SUBMIT_CHARGER_RATING

- 请求：`orderId` 正整数、`rating` 整数 `1..5`、`comment?` 字符串（trim 后
  `≤140`，normalize + DB CHECK 双层）。
- 返回：`{rating: {id, orderId, chargerId, chargerCode, stationName, rating,
  comment, createdAtUtc}, alreadyRated}`。
- 一单一评：`charger_ratings.order_id UNIQUE` + `INSERT OR IGNORE`——重放返回
  首评原值且 `alreadyRated: true`，不改写（无"改评"动作，TODO(contract) 二期）。
- 安全口径：`chargerId` 取订单快照不信客户端；订单必须属于本人且 `COMPLETED`，
  否则 `NOT_FOUND`（不泄露存在性）。

### GET_MY_RATINGS

- 请求：`page?`、`pageSize?`（`≤100`）。
- 返回：`{ratings: [{id, orderId, chargerId, chargerCode, stationName, rating,
  comment, createdAtUtc}], page, pageSize, total}`，`created_at DESC, id DESC`。
  JOIN chargers/stations 带展示字段，页面无需二次查询；不回显 `userId`。

## 4. 本次不冻结为必填的扩展

- R1 的照片、营业时间、停车费/占位费、快充计数及嵌套 `price/chargers` 尚未冻结，
  不能用新嵌套对象替换已有平铺 Station 字段。
- R2 `feeDetail` 尚未冻结/实现；现有 `amountCents` 与固定单价计费仍是唯一结算依据。
  拆分电费/服务费/优惠前先明确规则、订单快照和数据库迁移，不伪造费用明细。
- R3 `estimatedMinutesRemaining`、`soc` 尚未冻结/实现，缺失显示“暂无估算”，不当作 0。
- PR #18 的未来时间段、车辆绑定及预约时长是客户端演示，不改变已有
  `RESERVE_CHARGER {chargerId}` 的服务器即时预约规则。真实时段预约须独立设计冲突检测。
- 扫码启动通道不在本契约：桩码 payload 格式、摄像头通道与 `SCAN_START` 类动作均
  TODO(contract)。客户端已落 **mock 模拟扫码页**（ScanPage，"模拟通道"演示态：
  速选桩码/手输 `CHG://<站>/<桩>` → 走 `GET_STATIONS`/`GET_CHARGERS` 真查询 →
  引导既有 `RESERVE_CHARGER`），真通道就位时只替换页面数据源接缝（`scanSource`），
  不动本契约动作。
- 峰谷分时电价、电桩停车费/场地情况说明未冻结（TODO(contract)：`GET_STATIONS`/
  `GET_CHARGERS` 扩展字段与计费快照规则一起评审）；客户端按固定单价演示。
- 优惠券核销/抵扣不在本契约（TODO(contract)：`PAY_ORDER` 抵扣规则二期，一期只读
  券列表）；故障上报、资金消费/退款流水、管理端接口不在本 PR 范围。

## 5. 并行接入分工与验收

| 负责人员 | 后续实现内容 | 必测项目 |
| --- | --- | --- |
| 组长 | 维护已接通的路由、业务服务、适配器和集成测试；完成后续合并联调 | 未登录、冻结、伪造 userId、超时/重连、安全错误 |
| 组员 2（站点/预约） | 新增预约页“开始充电”按钮（START_CHARGING 已有）；改善真实列表与即时预约 UI | 末页/空页、不可用桩、关联订单恢复、真实预约规则提示 |
| 组员 3（个人/订单/充电） | 真实页面交互验收；优化未确认充值恢复提示；订单页显示时间分组与 createdAt 分页顺序对齐 | 相同充值不重复入账、两种余额、状态/错误展示 |
| 组员 4（管理端） | 按原分工继续管理端，复用公共基础模型；不自行定义用户接口 | 不混用管理员权限和用户 Session |
| 组员 5（数据层） | 修复 PR #19；管理查询继续独立；复用/重构用户查询时保留本分支已实现的隔离及事务语义 | 用户隔离、唯一流水并发、失败流水、余额溢出、事务回滚 |

接入者可复用 `normalizeRequestData(type, data, &normalized, &error)`，只使用其规范化
输出作为已知请求参数；身份另从 Session 获取。列表保留现有基础模型，扩展字段与模型平铺，
使用 `model::toJson()` 输出基础部分后补充字段，不修改数据库基础模型来塞 UI 字段。

`user_api_contract` 测试覆盖示例与字段校验；`user_api_integration` 使用真实 TCP+SQLite
验证十五个接口、已有充电闭环、用户隔离、充值事务、独立数据库连接并发、重连和超时。
独立页面预览仍使用 Mock，保留部分旧字段兼容预览；不能把 Mock 作为鉴权或事务验收依据。
新增或改变必填字段、枚举、金额/状态语义时，先改本文、公共定义和测试，再改双方实现。
