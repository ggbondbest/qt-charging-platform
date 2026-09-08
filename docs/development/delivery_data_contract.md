# 交付数据契约补充

## 管理用户注册时间

`users.list`、`users.get` 的用户 DTO 新增 `createdAt` 与 `createdAtUtc`。
两个字段均来自 `users.created_at`，采用 UTC ISO 8601 时间字符串；
`createdAt` 与用户端 common DTO 的命名一致，`createdAtUtc` 供管理页明确识别时区。
注册时间与 `updatedAt`（资料、余额或状态最后修改时间）不能互相替代。
管理端继续返回脱敏手机号，不增加原始手机号或认证数据。

## 单站电桩在线率

`stations.list`、`stations.get` 和电站写操作返回的站点 DTO 共用以下口径：

| 字段 | 类型/单位 | 口径 |
| --- | --- | --- |
| totalChargers | 整数，台 | 所有状态的电桩总数 |
| availableChargers | 整数，台 | 仅 AVAILABLE，可立即使用 |
| onlineChargerCount | 整数，台 | 非 OFFLINE：AVAILABLE、RESERVED、CHARGING、FAULT |
| onlineRatePercent | 数值，0–100 | onlineChargerCount / totalChargers × 100；无桩为 0 |

这是连接状态在线率，不是空闲率或健康率；故障桩仍计入在线。
该口径与 `chargers.summary.onlineChargers`、`stations.summary.onlineChargers`
和 `dashboard.get.onlineRatio` 一致，但 `onlineRatePercent` 是百分数，
`onlineRatio` 是 0–1 比率，页面不得再次混用倍数。
分页仅限制站点条数，站点内部电桩计数始终覆盖该站全部电桩。

## 无客户端请求时的预约清理

`ChargingRepository::expireReservations(nowUtc, diagnostic)` 为工作线程提供维护入口，
复用既有过期事务，同时把到期 ACTIVE 预约改为 EXPIRED、其 RESERVED 订单改为
CANCELLED、对应 RESERVED 电桩释放为 AVAILABLE。任一步失败整体回滚。
未到期预约、正在充电、待支付和已完成订单不因维护调用改变。
应在持有数据库的服务工作线程启动时及定时调用，不能从界面线程使用该连接。
重复调用安全；返回 false 时 diagnostic 仅供内部诊断，不应直接暴露 SQL 到页面。

本次没有修改表结构、索引或数据库版本；旧库迁移和备份版本兼容由独立数据库 PR 处理。
