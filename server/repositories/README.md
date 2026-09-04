# Server Repositories

放置 Repository 接口和 SQLite 实现。该模块只负责查询与持久化，不弹窗、不构造协议
响应；Schema 变更走独立契约 PR。

当前已实现：

- `UserRepository`：按手机号查询和并发安全的首次创建；
- `StationRepository`：站点关键字/状态筛选、分页和电桩数量聚合；
- `ChargerRepository`：按站点、状态、类型筛选并分页查询电桩；
- `ChargingRepository`：预约、过期、取消、开始、状态和停止的多表事务；
- `OrderRepository`：余额扣减与订单完成的单事务支付。
- `RechargeRepository`：幂等充值、余额与充值记录原子写入、分页查询。
- `ReservationQueryRepository`：按用户/状态分页读取预约及站点、电桩、订单上下文；
- `OperationLogRepository`：记录并分页检索管理员操作日志。

写流程使用 `BEGIN IMMEDIATE`，每次状态更新都带旧状态条件并检查影响行数。Repository
只返回领域错误和仅供日志使用的诊断文本，Service 不会把 SQLite 错误直接返回客户端。
