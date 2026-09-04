# Settings Service（成员 2）

个人中心“⚙️ 设置”独立页面的数据层（任务 #17 二次迭代）。纯本地服务，
三大模块对应设置页三张卡片：

- **账号安全（二级保护密码）**：`setProtectionPassword`（长度 ≥ 4，仅存
  SHA-256 哈希，**明文不落盘**）、`verifyProtectionPassword`、
  `clearProtectionPassword`；`setProtectionEnabled(true)` 在未设置密码时返回
  false 且不改状态——设置页开关据此置灰并引导先设密码。
- **车辆管理**：`Vehicle { id, plate, brandModel, batteryKwh, connectorType,
  isDefault }` 多车增删改（`addVehicle`/`updateVehicle`/`removeVehicle`/
  `setDefaultVehicle`，`setMockVehicles` 供演示/测试整体覆盖）。不变式：
  **至多一台默认车**（新默认顶掉旧的；删除/取消唯一默认后剩余首台自动
  接任；首台自动默认；无车时 `defaultVehicle()` 为 nullptr）。车辆数量决定
  预约名额上限——`ReservationService::setSettingsService(this)` 注入后，
  `unfinishedSlotLimit()` = `vehicleCount()`（未注入回退 1）。
- **通知与提醒**：到期提醒 / 成功通知 / 取消通知三个开关，默认全开，
  切换即写 `QSettings`（组 `settings/notifications/…`），重进页面回读。
  组织/应用名在 `client/app/main.cpp` 统一设置。

车辆列表为**内存模拟数据**：初始为空（无车 → 预约入口被拦截并引导去
设置-车辆管理添加），`setMockVehicles` 供演示/测试整体装配；密码哈希 /
二级保护开关 / 通知开关经 `QSettings` 本地持久化。后端 `SETTINGS` /
`VEHICLE` 命令与数据表尚未定义（protocol/common 属成员 1/3 领域）；接口
就绪后可按 `ReservationService` 同款双通道模式扩展，信号形状不变、页面
代码零改动。

信号：`vehiclesChanged` / `protectionStateChanged` / `notificationsChanged`。
`resetForTesting()` 清除本服务全部本地持久化（测试隔离）。

## 测试

`tst_settings_service`（9 用例，独立 QSettings 域
`ChargingPlatformTeam/SettingsServiceTest`，不污染真实用户配置）：车辆 CRUD
与默认车不变式、默认标记互斥、`setMockVehicles` 规整、密码哈希存储
（64 位十六进制、无明文）、开关前置条件与跨实例持久化、通知开关默认值
与回读。UI 端设置页交互（置灰开关、车辆弹窗校验、名额联动 caption）见
`tst_home_shell`。
