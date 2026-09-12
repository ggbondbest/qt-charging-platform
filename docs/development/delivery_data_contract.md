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
`ServerThread::run()` 在数据库打开后、开始监听前执行一次清理，并在同一工作线程
中每秒调用一次维护入口；定时器、Repository 与 SQLite 连接具有相同线程归属。
后台清理不依赖用户请求或管理页面刷新，不能从界面线程使用该连接。
启动时清理失败会终止启动，定时清理失败记录安全错误并由下一次周期重试。
管理请求执行前也会补做清理；维护失败返回 `DATABASE_ERROR`，且维护结束后再次
检查请求截止时间，避免数据库等待导致已经超时的请求继续修改数据。
退出/关闭管理会话不触发这一步数据库维护，因此仍可正常注销。
重复调用安全；返回 false 时 diagnostic 仅供内部诊断，不应直接暴露 SQL 到页面。

`server_runtime` 的 `reservationsExpireWithoutClientTraffic` 用例在预约成功后关闭
TCP 客户端，直接观察数据库，验证后台任务把预约、订单和电桩更新为
EXPIRED / CANCELLED / AVAILABLE，再验证管理端读到同一状态；
`delivery_data_contract` 同时覆盖三表事务失败回滚和重复清理安全性。
该运行时调度与回归已归入本数据契约 PR，使无请求清理无需依赖后续业务 PR；
头像上传和未完成订单导航等业务通信补充仍属于后续 PR 的范围。

本次没有修改表结构、索引或数据库版本；旧库迁移和备份版本兼容由独立数据库 PR 处理。
