# Station Pages（成员 2）

登录页与首页导航外壳放在本目录。按终审规格「首页导航外壳（核心）」实现：
导航壳抽为**全局公共组件**（`client/widgets/` 的 `TopNavBar` + `BottomTabBar`），
所有用户端页面复用同一套导航、样式与交互。视觉统一使用成员 3 维护的全局主题
`resources/qss/client_platform.qss`（电动绿 token），组件/页面专属样式以对象名
限定在各自内部生效，不改动全局 QSS。

## 组件

- `LoginPage`：手机号登录页。品牌区 + 居中登录卡片 + 手机号输入（11 位、`1`
  开头校验）、主按钮、结果提示；`loginStarted` 显示 `LoadingOverlay` 遮罩并禁用
  表单，`loginSucceeded` 发信号供 `MainWindow` 跳转，`loginFailed` 红字提示可重试；
  `resetState()` 供退出登录后复位。
- `HomeShell`：用户端根页面 = **TopNavBar（顶部）+ QStackedWidget（内容）+
  BottomTabBar（底部固定）**。底部四个 Tab：
  1. **找站**（`StationHomePage`，默认激活）；
  2. **订单**（占位，成员 3 OrderHistoryPage 联调后替换）；
  3. **充值**（占位，成员 3 WalletPage 充值通道接入后替换）；
  4. **我的**（任务 #17 迭代重构：`QScrollArea` 滚轮滚动——用户信息卡片居上
     （昵称/手机号/余额）→ 功能容器垂直排布（“📒 我的预约（预约订单 /
     已完成的预约）”入口 + 充电订单/优惠券/设置占位槽，仅入口不实现业务）
     → **退出登录红色字体置于最底部**（`isDangerText` 属性局部样式）。
  另含三个路由页（均非 Tab）：索引 4 站点详情、索引 5 **预约确认页面**、
  索引 6 **我的预约模块**（二级 Tab：预约订单 / 已完成的预约）。顶部导航
  “‹ 返回”按路由回上一级（确认 → 详情、详情 → 找站、模块 → 我的），由
  `HomeShell` 统一维护显示/收起。
- `StationHomePage`（任务 #7）：找站业务页。自上而下 = 地图容器（固定高度）→
  筛选栏（综合/空闲优先/距离最近 排序芯片 + 电价筛选下拉，变更即时刷新本地
  投影，不重复请求）→ 可滚动站点卡片列表（名称、地址、电价、空闲桩数、距离）。
  地址搜索复用顶部导航搜索框（HomeShell 接线）。列表区四态：加载中 / 空数据
  （可清空搜索）/ 异常（友好提示 + 重试）/ 正常列表。数据经
  `StationQueryService` 获取。
- `StationDetailPage`（任务 #12）：站点详情业务页。结构 = 站点信息卡
  （名称/地址/状态）→ 电价与距离 → 离线横幅（站点 `Inactive` 时醒目提示）
  → 充电桩卡片列表（编号/类型/功率/工作状态：空闲、占用·充电中、占用·已
  预约、故障、离线）。状态全覆盖：加载中 / 站点 ID 非法或接口、网络异常
  （友好提示 + “返回首页”回找站列表）/ 正常列表 / 空数据（“该站点暂无充电
  桩”）/ 故障桩红色视觉标记（页面局部属性样式）。桩数据经
  `StationQueryService` 详情通道异步获取，与列表页共用同一服务实例，模拟 ↔
  真实切换 UI 零改动。返回按钮复用全局 `TopNavBar` 的“‹ 返回”（进入详情由
  `HomeShell` 显示、返回列表收起），不重复开发导航。
- `ReservationConfirmPage`（任务 #17 迭代，替代原 `ReservationDialog` 弹窗）：
  独立预约确认路由页（索引 5），由详情页预约按钮在**满足预约条件**（已登录
  且无未结束预约）时经 `reservationConfirmRequested` 信号进入，不再弹窗。
  页面内容 = 站点名称 / 充电桩编号 / 充电类型与功率 / 预约时长下拉（默认
  60 分钟）/ 预估费用（电价 × 时长即时联动）。【关闭】→ 返回站点详情页；
  【确认预约】提交：提交中按钮禁用显示“提交中…”（loading），成功发
  `confirmed` 由壳刷新桩状态并**自动跳转【预约订单】页**，失败红色展示原因
  （桩被抢占 / 参数非法 / 网络错误）且停留在本页可修改重试。长内容置于
  `QScrollArea`（鼠标滚轮滚动）。
- 单预约约束（任务 #17 迭代，入口拦截）：同一用户同一时刻仅允许一条未结束
  预约。详情页点“预约”时若存在未结束预约，发 `reservationBlocked` 交
  `HomeShell` 弹提示“您当前尚有未结束的预约，请结束当前预约后再发起新预约”
  且**不跳转**；“去查看”直达【预约订单】页结束当前预约。Service 层兜底二次
  校验，即使绕过 UI 直接 `submit` 也返回同一业务错误。
- `ReservationModulePage`（任务 #17 迭代，替代原 `ReservationListPage`）：
  “我的预约”拆分为两个独立页面，模块内**二级 Tab**切换（仅模块内生效，
  不改动全局底部 Tab 外壳）：
  - `ReservationOrderPage`【预约订单】：进行中预约**左-中-右三栏**——左 =
    距预约桩距离（虚拟占位，预留对接后续导航模块）；中 = 预约信息 +
    **倒计时每秒刷新**（>30 分钟绿 / 5~30 分钟黄 / <5 分钟红，属性选择器局部
    样式），归零自动流转“已过期”并刷新，+ 【取消预约】（取消中禁用防重复
    提交；成功自动跳转【已完成的预约】页，失败页内提示可重试）；右 = 汽车
    电量虚拟占位（业务暂不实现）。无进行中预约 → 友好空提示 + “去找桩”回
    找站 Tab；加载中/接口异常态全覆盖（错误态重试）。
  - `ReservationCompletedPage`【已完成的预约】：历史预约卡片列表（已完成 /
    已取消 / 已过期），每张卡片含站点/桩/规格/时长/时间/费用/状态全部信息，
    **点击弹出详情弹窗**展示完整字段；`QScrollArea` 滚轮滚动查看更多记录；
    加载中 / 空记录 / 接口错误（+ 重试）全覆盖。
- 详情页桩卡片预约按钮（任务 #17 改造）：所有桩均展示按钮，**非空闲置灰
  不可点击**（tooltip“仅空闲充电桩可预约”）；未登录点击由 `StationDetailPage`
  发 `reservationLoginRequired`，`HomeShell` 弹非模态登录提示框，“去登录”经
  全局 `loginRequested` 跳登录页——登录逻辑不在页面内重复实现。未登录访问
  “我的预约”入口同样被壳拦截提示登录。
- `StationMapPanel`：腾讯地图入口（需求 #22）。Key 缺失或加载失败时降级为
  友好提示 + 重试，**不向用户展示原始环境变量字符串**，下方列表照常浏览。

## 站点/预约数据：模拟 → 真实接口无缝切换

`StationQueryService`（`client/services/station/`）双通道：

- 模拟通道（当前默认）：6 条演示站点 + 关键字过滤（含 1 个离线站 id4、1 个
  无桩站 id6，专用于驱动详情页边界状态演示），站点详情通道返回与空位数自洽
  的充电桩列表（覆盖空闲/占用/故障/离线），带约 400ms 模拟延迟驱动加载状态；
- 真实通道：服务端 `GET_STATIONS` / `GET_CHARGERS` 就绪后，`MainWindow` 已
  注入 `ClientConnection`，仅需 `service->setLiveMode(true)` 即切换，结果解析
  为同一 `StationList` / `StationDetail` 信号，**页面 UI 逻辑零改动**。
- 真实通道的失败路径已有测试覆盖（`station_query_service`：服务端未实现
  列表/详情命令时页面得到友好错误而非异常）。
- 预约提交/取消/列表/到期流转走 `ReservationService`
  （`client/services/reservation/`），同一双通道模式：服务端已实现
  `RESERVE_CHARGER` / `CANCEL_RESERVATION`（需登录会话），注入连接并
  `setLiveMode(true)` 即无缝切换，端到端真实提交/取消已有测试覆盖
  （`reservation_service`）；预约列表命令协议尚未定义，live 通道按未知命令
  走友好错误，就绪后 UI 零改动。单预约约束在 Service 层兜底（模拟通道直接
  拒绝第二次提交；真实通道以服务端裁决为准）。倒计时归零经
  `expireReservation` 本地流转“已过期”并触发模块刷新。预约成功后模拟通道经
  `StationQueryService::setMockChargerReserved` 反映到详情刷新
  （真实通道以服务端桩状态为准）。

## 登录态传递

- 登录成功后 `MainWindow` 以 `HomeShell(user)` 构造壳，`user` 经
  `TopNavBar::setUser` 与“我的”页透传展示；跨页面保持导航一致。
- 未登录进入首页（`HomeShell` 空用户构造）：右上角显示“登录”按钮，点击发
  `loginRequested` 由 `MainWindow` 切回登录页；“我的”页显示 NoticePanel
  “立即登录”入口。
- 已登录点击顶部头像 → `profileRequested` → 壳内切换到“我的”Tab。
- “退出登录”发 `logoutRequested` → 回登录页并 `resetState()`。

## 状态提示（规格边界）

接口未就绪统一用模拟数据渲染；搜索提交、关键字为空、未登录访问“我的”均有
Toast/状态标签/NoticePanel 基础提示；Tab 切换即时高亮（互斥选中）。

## 腾讯地图接入条件

`StationMapPanel` 只有在**同时满足**以下两条时才渲染真实地图，否则降级为
友好提示 + “重试”，站点列表浏览不受影响（需求 #22；规格禁止向用户展示
原始环境变量字符串）：

1. 构建时找到 Qt6 WebEngine（`find_package(Qt6 QUIET COMPONENTS WebEngineWidgets)`，
   命中才定义 `CHARGING_PLATFORM_HAS_WEBENGINE`）；
2. 运行时注入 Key：环境变量 `CHARGING_TENCENT_MAP_KEY`（**Key 不得提交进仓库**）。

当前开发机缺 Qt6 WebEngine，构建走降级分支；安装方式见
`docs/development/qt_6_2_4_compatibility.md` 第 7 节。
