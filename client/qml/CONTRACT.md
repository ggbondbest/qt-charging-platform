# QML 迁移契约 —— widgets/QML 双轨期的唯一 API 依据

**基线**：`feature/client-ui-modernization`。改本文件须走会签 PR。以下名字**逐字使用**，不许改名、不许发明；后端没定的字段写 `TODO(contract)`。

## 0. 目录所有权（铁律）
| 路径 | 属主 | 对方 |
|---|---|---|
| `client/qml/platform/**`、`client/qml/*.qml`、`main.cpp`、`CMakeLists.txt` | 成员3 | 只读 |
| `client/qml/pages/profile_charging/**` | 成员3 | 只读 |
| `client/qml/pages/station/**` | **成员2** | 只读 |
| `tests/qml/**`（各自 smoke 放各自子目录） | 各管各 | — |

页面新增文件=只往**自己**目录的 `CMakeLists.txt` 里追一行，父级/CMake 挂载点已预建，永远不用碰共享文件。旧 `client/widgets`、`client/pages`(widgets 版)、`resources/qss`、server/DB/协议 = 全体冻结，迁移期**只新增不修改**。

## 1. 服务注入（context properties，不包壳不改名）
C++ 服务对象直接以 context property 注入根作用域，名字=类名首字母小写；其 **全部 public slot / signal / Q_PROPERTY 原样透传**，QML 里 `on<Signal名>` 直接接：

`authService` `walletService` `orderService` `chargingService` `reservationService` `stationQueryService` `mapGeoService` `settingsService` `favoritesService` `notificationService`

另有 `App` 对象：`App.currentUser`（登录态，`loginStateChanged()` 信号）、`App.navigate(route[, arg])` / `App.back()`（`arg` 可选路由参数，如 order_detail 的订单 map，页面用 `property var arg` 接收）、`App.showToast(text, tone)`（tone 同 §2 StatusTag）。通道选择 `CHARGING_CHANNEL=mock|tcp`，默认 mock。

> **说明（2026-09-06 立，2026-09-07 补全）**：C++ 服务方法是普通成员函数（非 slot）且信号载荷是裸 struct——QML 两头都不可见。故上下文注入的全部是**同名转发桥**（`service_bridges.h`）：方法名/信号名逐字不变，仅载荷改为 map/list、枚举参数改为小写字符串。**`fetchOrders` 的 filter 取 `"all" | "charging" | "waiting_payment" | "completed"`**。station/reservation/settings/favorites/notification 五域已于 09-07 按同模式补桥落地。三个桥侧口径特例：
> - **`fetchDetailById(stationId, distanceMeters)`**（新增桥方法）：裸服务 `fetchDetail(Station,int)` 的 struct 参数 QML 传不动，桥按 id 从上次 `querySucceeded` 缓存重建 Station 再转发；缓存缺失时以占位 Station（仅 id）转发，mock 服务按 id 查自有数据。
> - **`reservationService.submit(map)`**：`startMinutes`/`endMinutes` 为**当日分钟位**（本地，与推荐时段同基准），桥据今日重建 QDateTime 再转 UTC；`end < start` 视作跨零点顺延一天（与 widgets `QDateTimeEdit` 口径一致）。
> - **`settingsService` 的 `second*` 别名**（`hasSecondPassword`/`setSecondPassword`/`verifySecondPassword`）→ 裸服务 `protection*`，语义同一（二级保护密码）；`notificationEnabled(key)` 的 key ∈ `"expiry" | "success" | "cancel"`。
>
> **OrderBridge 增补（2026-09-06 PR #33 两轮评审后）**：①补 `operationFailed(type, code, message)` 信号（与 wallet/charging 桥同型）——查询失败恢复必须接它，且**按 type 过滤**（计数类失败别动列表在途状态）。②补 `isFetchingOrders()`：服务层对在途重复提交**静默丢弃且无回执**，响应也不携带请求参数——因此**页面必须单飞并记住在途身份** `(filter, page, first)`：在途时新指令只登记意图（切筛选→落定判过期丢弃+重查；他页占用通道→queuedReload），**绝不允许清掉在途请求的状态或应用过期响应**。③分页口径：`loadedPage`（已成功页）与 `reqPage`（在途页）分离，加载更多永远发 `loadedPage+1`，失败只置重试态、页码不漂移；接 `ordersLoaded` 第三参 `hasMore`。④头像键双源治理：`ProfileEditPage.avatarChoices` 是 widgets `AvatarLibrary::all()` 的 QML 镜像，`test_qml_client_pages` 逐键对拍（同三方字面量+测试钉死模式）；展示 glyph 与提交 key 分离，`""`=默认昵称首字头像。⑤余额同步矩阵：改 `balanceCents` 的三事件 `profileLoaded` / `rechargeCompleted` / `paymentCompleted` 都必须在 `QmlApp` 回写并 `userChanged`，顶栏才不滞后。

## 2. 平台组件（`client/qml/platform/`，import "../../platform"）
| 类型名 | 关键 API（=旧 C++ 类语义） |
|---|---|
| `Style`（单例） | 颜色/字号/圆角/间距/时长/下拉参数令牌，页面里**禁止字面量** |
| `ActionButton` | `variant` ∈ `primary secondary danger ghost chip`；`text`；`onClicked` |
| `Card` / `ClickableCard` | 默认内容项；后者加 `onClicked` |
| `StatusTag` | `tone` ∈ `neutral success warning danger info`；`text` |
| `ActionBar` | `variant` ∈ `primary danger`；`actionText`、`caption`；`onClicked` |
| `NoticePanel` | `glyph` `title` `description` `actionText`；`onActionTriggered` |
| `Toast` | `show(text, tone)`（Popup 型，全局单例 `App.showToast` 转发到这里） |
| `LoadingOverlay` | `running` 属性（=showFor/hideFor） |
| `BottomTabBar` | `tabs: [{id,text},…]`；`currentTab`；`onTabChanged(id)`；`setCurrentTab(id)` |
| `TopNavBar` | `user`、`backVisible`、`searchVisible`、`clearSearch()`；`onSearchSubmitted(keyword)` `onLoginRequested` `onProfileRequested` |
| `PullToRefreshArea` | 默认属性=内容 Item；`pullEnabled`；`onRefreshRequested()`；`setRefreshing(bool)`。语义钉死：8px 激活 / 56px 触发 / 44px 停留（`Style.pull*`），顶部才可用，刷新中不再触发 |

## 3. 路由与锚点
顶栏四 tab id 不变：`station` `order` `charging` `profile`。页面文件=页名（`StationHomePage.qml` 等）。**每页根 Item 的 `objectName` 必须等于旧 widget 版 objectName**（如 `"homeShell"`、`"walletPage"`），明天测试平移靠它。

## 4. 动效
只用 `Behavior`/`NumberAnimation`/`Transition` + `Style.dur*` 令牌（micro 80 / enter 180 / exit 120 / value 140 / breathe 1600，错峰 40×≤8）。无障碍降级：`Style.motionEnabled`（`MOTION_REDUCED=1` 或 offscreen 时 false，动画时长一律 `motionEnabled ? Style.durX : 0`）。

## 5. 验收（冲刺日）
mock 通道下 `charging-qml-preview --view=<路由> --screenshot=<png>` 出非空截图；旧 ctest 34 套保持全绿（证明没碰旧东西）。像素 diff、测试移植、删旧=明天的事。

### 已知问题与解决方式（实测验证，勿再踩）
- offscreen 下 `QQuickWindow::grabWindow()` **永远返回 null**（GL /software 后端都救不了）；截图走 `contentItem()->grabToImage()`。
- 推论：**每个页面根节点必须自绘背景**（`Rectangle { anchors.fill: parent; color: Style.bg }`），否则截图区是黑色。
- 进程退出时 context property 变 null 会触发绑定重算——绑定里用 `App && App.xxx` 守一下，避免 teardown 噪音。
- Controls 只有 `Basic` 样式可用（`QQuickStyle::setStyle("Basic")` 已在 main.cpp 设好，勿改）。
- **offscreen 下 Repeater 的 delegate 对 C++ `findChildren` 不可见**：画面（grabToImage 走场景图）渲染正常，但 delegate 对象何时挂进 QObject 树取决于 delegate 组件的异步编译时序，窗口宿主/等待事件循环都救不了（实测挂窗口 + 2s 轮询仍为空）。交互测试因此**不得** `findChild(delegate objectName)`：改断页面根上的公开状态与函数（`setProperty` / `invokeMethod("load", …)`，与 delegate onClicked 同一代码路径），模型行数走挂过 `objectName` 的 ListModel（如 `uiOrdersModel.count`）。
