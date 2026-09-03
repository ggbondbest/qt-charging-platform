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
  4. **我的**（账户卡片：昵称/手机号/余额透传 + “退出登录”）。
  另含索引 4 的详情路由页（非 Tab）。
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
  `HomeShell` 显示、返回列表收起），不重复开发导航。**预约入口仅为 UI 占位**
  （空闲桩卡片“预约”按钮 → 页内占位提示 + `reservationRequested` 信号），
  正式预约流程属任务 #17。
- `StationMapPanel`：腾讯地图入口（需求 #22）。Key 缺失或加载失败时降级为
  友好提示 + 重试，**不向用户展示原始环境变量字符串**，下方列表照常浏览。

## 站点数据：模拟 → 真实接口无缝切换

`StationQueryService`（`client/services/station/`）双通道：

- 模拟通道（当前默认）：6 条演示站点 + 关键字过滤（含 1 个离线站 id4、1 个
  无桩站 id6，专用于驱动详情页边界状态演示），站点详情通道返回与空位数自洽
  的充电桩列表（覆盖空闲/占用/故障/离线），带约 400ms 模拟延迟驱动加载状态；
- 真实通道：服务端 `GET_STATIONS` / `GET_CHARGERS` 就绪后，`MainWindow` 已
  注入 `ClientConnection`，仅需 `service->setLiveMode(true)` 即切换，结果解析
  为同一 `StationList` / `StationDetail` 信号，**页面 UI 逻辑零改动**。
- 真实通道的失败路径已有测试覆盖（`station_query_service`：服务端未实现
  列表/详情命令时页面得到友好错误而非异常）。

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
