# SQLite 数据字典 v1

实现文件：`database/schema.sql`、`database/seed.sql`

状态：`candidate-v1`（待五人确认与最小登录闭环验证）

当前 `PRAGMA user_version = 1`。字段、单位、状态值或约束变化必须先评审；已交付数据库后不得直接覆盖旧表，应增加迁移。

## 1. 全局约定

| 项目 | 约定 |
| --- | --- |
| 主键 | `INTEGER PRIMARY KEY AUTOINCREMENT`，C++ 使用 `qint64` |
| 外键 | `INTEGER`，每个 connection 都必须执行 `PRAGMA foreign_keys=ON` |
| 金额 | 整数分，列名后缀 `_cents`，禁止 `REAL` |
| 电价 | 整数分/千瓦时，`*_cents_per_kwh` |
| 功率 | 整数瓦，`*_watts` |
| 电量 | 整数瓦时，`*_wh` |
| 时长 | 整数秒，`*_seconds` |
| JSON 整数范围 | 除 ID 外的 `qint64` 数值限制为 `0..9007199254740991`，`CHECK typeof(...) = 'integer'` 防止 SQLite 溢出转为 `REAL` |
| 时间 | UTC ISO-8601 `TEXT`，序列化为 `2026-09-01T08:30:00.000Z`；网络输入必须带 `Z` 或 offset |
| 状态 | `UPPER_SNAKE_CASE TEXT` + `CHECK`；不得存 enum 序号 |
| 软状态 | 用户冻结、站点停用、桩离线均改状态，不级联删除业务历史 |
| JSON 文本 | `operation_logs.details_json` 只存合法 object 的 compact JSON，由 Repository 校验 |

初始化 connection 使用：

```sql
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA busy_timeout = 5000;
```

`journal_mode` 是数据库级设置，其余连接仍需单独执行适用的 PRAGMA。测试若使用临时数据库，应使用 `QTemporaryDir` 内的文件，而不是在线程/连接之间不可共享的 `:memory:`。

## 2. users

用户账户及钱包当前余额。

| 列 | 类型 | Null | 默认/约束 | 含义 |
| --- | --- | --- | --- | --- |
| `id` | INTEGER | 否 | PK | 用户 ID |
| `phone` | TEXT | 否 | UNIQUE；11 位数字、以 1 开头 | 登录手机号，不能存整数 |
| `nickname` | TEXT | 否 | trim 后 1..32 字符 | 昵称 |
| `avatar_key` | TEXT | 否 | `''` | 头像资源 key，不是客户端绝对路径 |
| `balance_cents` | INTEGER | 否 | `0`，>= 0 | 钱包余额（分） |
| `status` | TEXT | 否 | `ACTIVE` | `ACTIVE`, `FROZEN` |
| `created_at` | TEXT | 否 | UTC now | 注册时间 |
| `updated_at` | TEXT | 否 | UTC now | 最后修改时间，由 Repository 更新 |

首次自动注册默认昵称为 `用户` + 手机号后四位，余额为 0。`phone` 的 UNIQUE 约束是并发自动注册的最终保护。

## 3. admins

PC 管理端账户。任何查询 DTO 都不得返回 hash 或 salt。

| 列 | 类型 | Null | 默认/约束 | 含义 |
| --- | --- | --- | --- | --- |
| `id` | INTEGER | 否 | PK | 管理员 ID |
| `username` | TEXT | 否 | NOCASE UNIQUE；3..32 | 登录名 |
| `display_name` | TEXT | 否 | 1..32 | 显示名称 |
| `password_algorithm` | TEXT | 否 | `SHA256_SALTED` | 演示版算法标识 |
| `password_salt` | TEXT | 否 |  | 盐 |
| `password_hash` | TEXT | 否 | 64 字符 | 小写十六进制 SHA-256 |
| `status` | TEXT | 否 | `ACTIVE` | `ACTIVE`, `DISABLED` |
| `last_login_at` | TEXT | 是 | NULL | 最近登录时间 |
| `created_at` | TEXT | 否 | UTC now | 创建时间 |
| `updated_at` | TEXT | 否 | UTC now | 修改时间 |

课程要求的默认账号是 `admin / 123456`。Seed 存储 `SHA-256(salt + ":" + password)` 的结果而非明文。该方案仅用于本机实训演示；生产系统必须改用经过审计的慢密码 KDF 和 TLS。

## 4. stations

充电站主数据。

| 列 | 类型 | Null | 默认/约束 | 含义 |
| --- | --- | --- | --- | --- |
| `id` | INTEGER | 否 | PK | 电站 ID |
| `code` | TEXT | 否 | UNIQUE；1..32 | 稳定业务编号 |
| `name` | TEXT | 否 | 1..64 | 站名 |
| `address` | TEXT | 否 | 1..255 | 地址 |
| `latitude` | REAL | 否 | -90..90 | 纬度 |
| `longitude` | REAL | 否 | -180..180 | 经度 |
| `price_cents_per_kwh` | INTEGER | 否 | >= 0 | 当前电价（分/kWh） |
| `status` | TEXT | 否 | `ACTIVE` | `ACTIVE`, `INACTIVE` |
| `created_at` | TEXT | 否 | UTC now | 创建时间 |
| `updated_at` | TEXT | 否 | UTC now | 修改时间 |

总桩数、空闲数、在线率从 `chargers` 聚合，禁止在本表维护易失真的计数副本。附近站点距离在 Service 查询/计算层产生，不持久化。

## 5. chargers

电桩主数据及累计统计。

| 列 | 类型 | Null | 默认/约束 | 含义 |
| --- | --- | --- | --- | --- |
| `id` | INTEGER | 否 | PK | 电桩 ID |
| `station_id` | INTEGER | 否 | FK -> stations；RESTRICT delete | 所属电站 |
| `code` | TEXT | 否 | UNIQUE；1..32 | 电桩编号 |
| `type` | TEXT | 否 | `FAST`, `SLOW` | 快充/慢充 |
| `power_watts` | INTEGER | 否 | > 0 | 额定功率（W） |
| `status` | TEXT | 否 | `AVAILABLE` | `AVAILABLE`, `RESERVED`, `CHARGING`, `FAULT`, `OFFLINE` |
| `total_charge_count` | INTEGER | 否 | 0；>= 0 | 已完成/停止的充电次数 |
| `total_charge_seconds` | INTEGER | 否 | 0；>= 0 | 累计充电秒数 |
| `created_at` | TEXT | 否 | UTC now | 创建时间 |
| `updated_at` | TEXT | 否 | UTC now | 修改时间 |

`idx_chargers_station_status` 支持按站点和状态汇总。状态只能由 Service 在业务事务中更新。

## 6. reservations

预约记录。每次预约同时创建一个 `RESERVED` 订单。

| 列 | 类型 | Null | 默认/约束 | 含义 |
| --- | --- | --- | --- | --- |
| `id` | INTEGER | 否 | PK | 预约 ID |
| `user_id` | INTEGER | 否 | FK -> users | 用户 |
| `charger_id` | INTEGER | 否 | FK -> chargers | 电桩 |
| `status` | TEXT | 否 | `ACTIVE` | `ACTIVE`, `FULFILLED`, `CANCELLED`, `EXPIRED` |
| `reserved_at` | TEXT | 否 | UTC now | 预约时间 |
| `expires_at` | TEXT | 否 | > reserved_at | 服务端权威过期时间 |
| `ended_at` | TEXT | 是 | ACTIVE 时必须 NULL，其他状态必须非 NULL | 完成/取消/过期时间 |
| `created_at` | TEXT | 否 | UTC now | 创建时间 |
| `updated_at` | TEXT | 否 | UTC now | 修改时间 |

Partial unique indexes 保证每个用户、每个电桩最多一个 `ACTIVE` 预约。15 分钟预约是扩展策略，可由 Service 设置 `expires_at = reserved_at + 15 minutes`；数据库不通过本机 UI 计时器决定过期。

## 7. orders

预约、充电、计费和结算的主业务记录。

| 列 | 类型 | Null | 默认/约束 | 含义 |
| --- | --- | --- | --- | --- |
| `id` | INTEGER | 否 | PK | 订单 ID |
| `order_no` | TEXT | 否 | UNIQUE；1..40 | 对外订单号 |
| `user_id` | INTEGER | 否 | FK -> users | 用户 |
| `charger_id` | INTEGER | 否 | FK -> chargers | 电桩 |
| `reservation_id` | INTEGER | 是 | UNIQUE；与 `user_id`/`charger_id` 组成复合 FK -> reservations | 来源预约；C++ 以 0 表示无值 |
| `status` | TEXT | 否 | `RESERVED` | `RESERVED`, `CHARGING`, `WAITING_PAYMENT`, `COMPLETED`, `CANCELLED` |
| `unit_price_cents_per_kwh` | INTEGER | 否 | >= 0 | 开单时电价快照 |
| `energy_wh` | INTEGER | 否 | 0；>= 0 | 充电量（Wh） |
| `duration_seconds` | INTEGER | 否 | 0；>= 0 | 充电时长（秒） |
| `amount_cents` | INTEGER | 否 | 0；>= 0 | 应付金额（分） |
| `created_at` | TEXT | 否 | UTC now | 创建时间 |
| `started_at` | TEXT | 是 | CHARGING 以后必须非 NULL | 开始时间 |
| `stopped_at` | TEXT | 是 | WAITING_PAYMENT 以后必须非 NULL | 停止时间 |
| `paid_at` | TEXT | 是 | COMPLETED 必须非 NULL | 支付时间 |
| `updated_at` | TEXT | 否 | UTC now | 修改时间 |

计费基于订单中的电价快照，不回读站点新价格。非负值采用整数 half-up 到分：

```text
amountCents = (energyWh * unitPriceCentsPerKwh + 500) / 1000
```

计算前应做 `qint64` 溢出保护。后续峰谷价需新增独立明细表，不能在一个订单字段中悄悄改变单位或含义。

Partial unique indexes 保证一个用户最多有一个 `RESERVED` / `CHARGING` / `WAITING_PAYMENT` 订单；一个桩最多有一个 `RESERVED` / `CHARGING` 订单。桩在订单进入 `WAITING_PAYMENT` 时释放。

`(reservation_id, user_id, charger_id)` 复合外键保证订单只能关联同一用户、
同一电桩的预约。预约已进入业务历史后不物理删除，只更新状态。

## 8. recharge_records

充值流水。用户余额和成功流水必须在同一事务内更新。

| 列 | 类型 | Null | 默认/约束 | 含义 |
| --- | --- | --- | --- | --- |
| `id` | INTEGER | 否 | PK | 流水 ID |
| `transaction_no` | TEXT | 否 | UNIQUE；1..40 | 幂等交易号 |
| `user_id` | INTEGER | 否 | FK -> users | 用户 |
| `amount_cents` | INTEGER | 否 | > 0 | 充值额（分） |
| `balance_after_cents` | INTEGER | 否 | >= 0 | 完成本次处理后的余额快照 |
| `status` | TEXT | 否 | `SUCCESS`, `FAILED` | 处理状态 |
| `created_at` | TEXT | 否 | UTC now | 创建时间 |

重复 `transaction_no` 不得重复加余额。

## 9. operation_logs

管理员关键操作审计记录。

| 列 | 类型 | Null | 默认/约束 | 含义 |
| --- | --- | --- | --- | --- |
| `id` | INTEGER | 否 | PK | 日志 ID |
| `admin_id` | INTEGER | 是 | FK -> admins；删除后 SET NULL | 操作者 |
| `action` | TEXT | 否 | 1..64 | 如 `FREEZE_USER` |
| `target_type` | TEXT | 否 | 1..32 | 如 `USER`, `CHARGER` |
| `target_id` | TEXT | 否 | `''` | 通用目标 ID |
| `details_json` | TEXT | 否 | `'{}'` | 非敏感 JSON object 文本 |
| `created_at` | TEXT | 否 | UTC now | 操作时间 |

不得记录管理员明文密码、hash、salt、完整 Socket payload 中的敏感字段或本机路径。

## 10. 索引与一致性检查

主要索引：

- `idx_chargers_station_status`：站内状态表和状态统计；
- `idx_reservations_*`：用户/桩有效预约及过期扫描；
- `idx_orders_user_created_at`：用户订单历史；
- `idx_orders_status_created_at`：后台订单和营收筛选；
- `idx_recharge_records_user_created_at`：充值记录；
- `idx_operation_logs_admin_created_at`：管理员审计。

每次 schema/seed 变更至少运行：

```sql
PRAGMA user_version;
PRAGMA foreign_key_check;
PRAGMA integrity_check;
```

期望分别为 `1`、无行、`ok`。还应断言：

- seed 可重复执行且行数不增长；
- seed 有 1 个管理员、1 个演示用户、3 个站、7 个桩、1 条充值流水；
- 所有桩都有存在的 station；
- 金额/时长/计数无负数；
- 不存在违反 active partial unique index 的业务记录。

## 11. Seed 数据

`seed.sql` 仅用于 demo/测试，不应在普通已有数据库每次启动时执行。

- 管理员：`admin / 123456`，数据库只存 salt + hash；
- 演示用户：`13800138000`，昵称 `用户8000`，余额 10000 分；
- 三个标有“示范”的站点及七个电桩；
- 一条 10000 分成功充值流水，使演示用户余额有账可查；
- 不预置活动预约或订单，避免 seed 后业务状态不一致。

演示坐标和地址只用于 UI/距离算法测试，不应宣称为真实运营站点。
