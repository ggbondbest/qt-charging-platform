# Reservation Services（成员 2）

充电桩预约交互的服务层（任务 #17 / #17 迭代），仅前端对接逻辑，不包含后端
业务。当前提供：

- `ReservationService`：预约列表 / 提交预约 / 取消预约 / 到期流转四类操作，
  与站点查询服务相同的双通道设计，页面 UI 对二者无感知。

## 双通道

- **模拟通道（当前默认）**：约 400ms 延迟驱动加载态。内置 4 条演示记录覆盖
  预约中 / 已完成 / 已取消 / 已过期（`setMockRecords` 可清空以演示空态）；
  提交在本地仓库追加“预约中”记录，支持失败分支演示——
  `setSimulateNextSubmitConflict(true)` 模拟“桩被其他用户抢占”（并发边界），
  时长 ≤ 0 回“参数非法”；**单预约约束**：存在未结束预约时任何提交回业务
  错误“您当前尚有未结束的预约，请结束当前预约后再发起新预约”（UI 入口与
  Service 双层校验，防绕过）；取消仅对“预约中”记录生效，其余回“已结束
  无法取消”；`expireReservation(id)` 供倒计时归零流转“已过期”（幂等，仅
  转换仍为“预约中”的记录并发 `reservationExpired` 信号驱动页面刷新）。
- **真实通道**：服务端已实现 `RESERVE_CHARGER` / `CANCEL_RESERVATION`
  （需登录会话，桩/预约 ID 为十进制字符串）。注入 `ClientConnection` 并
  `setLiveMode(true)` 后提交/取消直发真实请求，成功响应解析
  `data["reservation"]`（`model::fromJson`）并与请求侧展示上下文
  （站点名 / 桩编号 / 充电规格 / 时长 / 预估费用 / 导航距离）合并为同一
  `ReservationRecord` 信号；单预约约束以服务端裁决为准。预约列表查询命令
  协议尚未定义（暂以 `GET_RESERVATIONS` 字面量占位），live 模式下列表走
  友好失败路径；命令就绪后仅需替换协议常量，**UI 零改动**。

两条通道输出完全一致的信号形状：

```
listStarted / listSucceeded(ReservationList) / listFailed(reason)
submitStarted(chargerId) / submitSucceeded(ReservationRecord) / submitFailed(reason)
cancelStarted(reservationId) / cancelSucceeded(reservationId) / cancelFailed(reason)
reservationExpired(reservationId)
```

`ReservationRecord = model::Reservation`（common 层已有核心模型）
`+ stationName + chargerCode + chargerSpec + durationMinutes + estimatedFeeCents
+ distanceMeters`（金额单位：分；预估费用 = 电价(分/度) × 时长(分钟) / 60；
`chargerSpec` 如“直流快充 · 120kW”由 `chargerSpecText` 生成；
`distanceMeters` 为虚拟导航距离占位，预留对接后续导航模块，缺省 -1）。

倒计时数据源：`model::Reservation.expiresAtUtc`（预约订单页每秒
`secsTo` 刷新，归零调用 `expireReservation` 自动流转状态）。

## 测试

`tst_reservation_service`：模拟通道全分支（四状态列表排序、提交成功/唯一性
约束拒绝/抢占/参数非法、取消规则、`hasUnfinishedReservation` 三态跟踪、
`expireReservation` 幂等流转、`chargerSpec`/距离上下文、失败开关一次性消耗、
空记录覆盖）+ 真实通道接缝（未实现列表命令友好失败、未登录被服务端拒绝、
登录会话下端到端真实预约→取消→再预约→被占桩拒绝，复用服务端组件装配真实
TCP 服务）。UI 端约束与倒计时/页面路由覆盖见 `tst_home_shell`。
