# QML 迁移契约 CONTRACT —— 两个 agent 的唯一 API 依据

**基线**：`feature/client-qml-migration-sprint`（原 client-qml-base 已并入，冲刺全部内容走这一个 PR）。改本文件=会签 PR，谁都不许单方面改。以下名字**逐字使用**，不许改名、不许发明；后端没定的字段写 `TODO(contract)`。

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

> **排雷实录（2026-09-06 补）**：C++ 服务方法是普通成员函数（非 slot）且信号载荷是裸 struct——QML 两头都不可见。故 `walletService`/`orderService`/`chargingService` 注入的是**同名转发桥**（`service_bridges.h`）：方法名/信号名逐字不变，仅载荷改为 map/list、枚举参数改为小写字符串。**`fetchOrders` 的 filter 取 `"all" | "charging" | "waiting_payment" | "completed"`**。其余服务（station/reservation/…）今晚按同样模式补桥，补前直接调用会失败——页面照常按契约名盲写。

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

### 排雷实录（已验证的坑，别再踩）
- offscreen 下 `QQuickWindow::grabWindow()` **永远返回 null**（GL /software 后端都救不了）；截图走 `contentItem()->grabToImage()`。
- 推论：**每个页面根节点必须自绘背景**（`Rectangle { anchors.fill: parent; color: Style.bg }`），否则截图区是黑色。
- 进程退出时 context property 变 null 会触发绑定重算——绑定里用 `App && App.xxx` 守一下，避免 teardown 噪音。
- Controls 只有 `Basic` 样式可用（`QQuickStyle::setStyle("Basic")` 已在 main.cpp 设好，勿改）。
