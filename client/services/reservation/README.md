# Reservation Services（成员 2）

充电桩预约交互的服务层（任务 #17），仅前端对接逻辑，不包含后端业务。
当前提供：

- `ReservationService`：预约列表 / 提交预约 / 取消预约三通道操作，与站点
  查询服务相同的双通道设计，页面 UI 对二者无感知。

## 双通道

- **模拟通道（当前默认）**：约 400ms 延迟驱动加载态。内置 4 条演示记录覆盖
  预约中 / 已完成 / 已取消 / 已过期（`setMockRecords` 可清空以演示空态）；
  提交在本地仓库追加“预约中”记录，支持三类失败分支演示——
  `setSimulateNextSubmitConflict(true)` 模拟“桩被其他用户抢占”（并发边界），
  同桩重复提交回“预约冲突”，时长 ≤ 0 回“参数非法”；取消仅对“预约中”记录
  生效，其余回“已结束无法取消”。
- **真实通道**：服务端已实现 `RESERVE_CHARGER` / `CANCEL_RESERVATION`
  （需登录会话，桩/预约 ID 为十进制字符串）。注入 `ClientConnection` 并
  `setLiveMode(true)` 后提交/取消直发真实请求，成功响应解析
  `data["reservation"]`（`model::fromJson`）并与请求侧展示上下文
  （站点名 / 桩编号 / 时长 / 预估费用）合并为同一 `ReservationRecord` 信号。
  预约列表查询命令协议尚未定义（暂以 `GET_RESERVATIONS` 字面量占位），
  live 模式下列表走友好失败路径；命令就绪后仅需替换协议常量，**UI 零改动**。

两条通道输出完全一致的信号形状：

```
listStarted / listSucceeded(ReservationList) / listFailed(reason)
submitStarted(chargerId) / submitSucceeded(ReservationRecord) / submitFailed(reason)
cancelStarted(reservationId) / cancelSucceeded(reservationId) / cancelFailed(reason)
```

`ReservationRecord = model::Reservation`（common 层已有核心模型）
`+ stationName + chargerCode + durationMinutes + estimatedFeeCents`
（金额单位：分；预估费用 = 电价(分/度) × 时长(分钟) / 60）。

## 测试

`tst_reservation_service`：模拟通道全分支（四状态列表排序、提交成功/冲突/
抢占/参数非法、取消规则、失败开关一次性消耗、空记录覆盖）+ 真实通道接缝
（未实现列表命令友好失败、未登录被服务端拒绝、登录会话下端到端真实
预约→取消→再预约→被占桩拒绝，复用服务端组件装配真实 TCP 服务）。
