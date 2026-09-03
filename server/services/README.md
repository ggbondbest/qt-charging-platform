# Server Services

放置登录、预约、充电、计费、订单和后台管理用例。Service 负责业务校验、状态规则和对外错误映射，
Repository 负责实际的 SQLite 原子事务。Service 不引用 Widgets 或直接处理 Socket 字节流。

当前已实现：

- `UserService`：手机号登录/自动注册和冻结用户拦截；
- `ChargingService`：15 分钟预约、取消、开始、实时状态和幂等停止；
- `BillingService`：瓦、秒、瓦时、分之间的纯整数安全计费；
- `OrderService`：余额条件扣款和幂等支付；
- `ChargingStateMachine`：预约、订单和电桩的显式合法迁移表。

`ChargingService` 和 `OrderService` 支持注入 UTC 时钟，测试不依赖等待真实时间。
SQLite 受控诊断只写 Server 日志，对外响应使用稳定错误码和通用消息，不泄露 SQL 或
数据库文件信息。
