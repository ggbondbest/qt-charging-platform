# Reservation Services（成员 2）

充电桩预约交互的服务层（任务 #17 / #17 二次迭代），仅前端对接逻辑，不包含后端
业务。当前提供：

- `ReservationService`：预约列表 / 提交预约 / 取消预约 / 到期流转 / 迟到取消
  五类操作，与站点查询服务相同的双通道设计，页面 UI 对二者无感知。

## 时间段预约（二次迭代，替代“选时长”）

- `submit(charger, station, startUtc, endUtc, vehicleId, vehiclePlate,
  distanceMeters)` 按**时间段**提交：时长 = end − start，Service 兜底校验
  end>start 且 ≤ **45 分钟**（“预约时间段不能超过 45 分钟”）。
- `ReservationRecord = model::Reservation`（common 层核心模型，**未改动**）
  `+ stationName + chargerCode + chargerSpec + startAtUtc + vehicleId +
  vehiclePlate + lateCancelled + durationMinutes + estimatedFeeCents +
  distanceMeters`（金额单位：分；预估费用 = 电价(分/度) × 时长(分钟) / 60；
  `chargerSpec` 如“直流快充 · 160kW”由 `chargerSpecText` 生成；
  `distanceMeters` 为虚拟导航距离占位，缺省 -1）。
- 静态 `recommendSlot(distanceMeters, nowUtc)`：系统推荐时段——模拟行驶
  时长 = 5min + ⌈距离/500m⌉min（真实地图就绪后仅替换此处），start = 现在 +
  行驶时长后**向上对齐 15 分钟刻度**（本地时区），end = start + 45 分钟。
- 迟到自动取消：`cancelLateReservations()` 将“已过开始 + 15 分钟仍未开始
  且时段仍在有效期”的“预约中”记录流转为“已取消”并打 `lateCancelled`
  标记（历史页展示“已取消·迟到”），逐条发 `reservationExpired` 驱动刷新；
  由预约订单页每秒 tick 调用，幂等。

## 名额制（二次迭代，替换“全局仅一条”）

- 上限 = **车辆数**（注入 `setSettingsService()` 后从 SettingsService 实时
  读取；未注入时回退 1，兼容旧装配）：`unfinishedSlotLimit()`；
  每车至多 1 条未结束预约：`activeCountForVehicle(vehicleId)`。
- 三层拒绝文案（Service 兜底，UI 入口先行拦截）：无车辆 →“请先在设置-车辆
  管理中添加车辆”；该车已有未结束预约 →“该车辆已有未结束的预约…”；
  总数达上限 →“可预约名额已全部占用（名额 = 车辆数）…”。

## 双通道

- **模拟通道（当前默认）**：约 400ms 延迟驱动加载态。内置 4 条演示记录覆盖
  预约中 / 已完成 / 已取消 / 已过期（`setMockRecords` 可清空以演示空态）；
  提交在本地仓库追加“预约中”记录（`reservedAtUtc = start`、
  `expiresAtUtc = end`），支持失败分支演示——
  `setSimulateNextSubmitConflict(true)` 模拟“桩被其他用户抢占”（并发边界）；
  取消仅对“预约中”记录生效，其余回“已结束无法取消”；`expireReservation(id)`
  供倒计时归零流转“已过期”（幂等，仅转换仍为“预约中”的记录并发
  `reservationExpired` 信号驱动页面刷新）。
- **真实通道**：服务端已实现 `RESERVE_CHARGER` / `CANCEL_RESERVATION`
  （需登录会话，桩/预约 ID 为十进制字符串）。注入 `ClientConnection` 并
  `setLiveMode(true)` 后提交/取消直发真实请求，成功响应解析
  `data["reservation"]`（`model::fromJson`）并与请求侧展示上下文
  （站点名 / 桩编号 / 充电规格 / 起止时间段 / 车辆 / 预估费用 / 导航距离）
  合并为同一 `ReservationRecord` 信号。**口径差异（已知，README 明示）**：
  服务端唯一索引 `ux_reservations_active_user` 仍强制“每用户一条有效预约”，
  故 live 通道的名额制以**服务端裁决为准**（第 2 条提交会被服务端拒绝）；
  车辆数 = N 名额规则当前仅在模拟通道生效，待协议扩展（携带 vehicle_id、
  放开唯一索引）后随服务端迁移，UI 零改动。时间段与车辆上下文经
  `RESERVE_CHARGER` 的 `duration_minutes` + 请求侧记录承载，服务端暂不回报
  `startAtUtc`（以提交时刻为准）。预约列表查询命令协议尚未定义（暂以
  `GET_RESERVATIONS` 字面量占位），live 模式下列表走友好失败路径；命令
  就绪后仅需替换协议常量，**UI 零改动**。

两条通道输出完全一致的信号形状：

```
listStarted / listSucceeded(ReservationList) / listFailed(reason)
submitStarted(chargerId) / submitSucceeded(ReservationRecord) / submitFailed(reason)
cancelStarted(reservationId) / cancelSucceeded(reservationId) / cancelFailed(reason)
reservationExpired(reservationId)
```

倒计时数据源：`model::Reservation.expiresAtUtc`（预约订单页每秒
`secsTo` 刷新，归零调用 `expireReservation` 自动流转状态；开始前
`startAtUtc` 驱动“距开始 mm:ss”阶段）。

## 测试

`tst_reservation_service`（19 用例）：模拟通道全分支（四状态列表排序、时间段
提交成功/超 45min 拒绝/无效时段、名额制（按车辆唯一 + 名额上限 + 取消释放
+ 抢占一次性开关）、`activeReservationCount`/`unfinishedSlotLimit` 随车辆数
联动、`expireReservation` 幂等、`cancelLateReservations` 迟到窗口判定与幂等、
`recommendSlot` 对齐与上限、`chargerSpec`/距离上下文、空记录/无车辆兜底）+
真实通道接缝（未实现列表命令友好失败、未登录被服务端拒绝、登录会话下端到端
真实预约→取消→再预约→被占桩拒绝，复用服务端组件装配真实 TCP 服务）。
UI 端名额/倒计时/迟到展示与页面路由覆盖见 `tst_home_shell`。
