# QML station 域迁移 · 状态/信号→绑定映射草稿

> **历史草稿，不是当前运行契约。** 下文保留早期迁移记录，其中 Canvas 网格、
> 模拟路线、车辆准入、时段预约和桥缺位演示回退等描述已过时。
> 当前交付以代码及 [地图与双端验收说明](../development/delivery_ui_validation.md)
> 为准：首页使用 `StationMapView` 真实底图，首页/导航统一走 `MapBridge`；
> 显式手动地址定位，失败不伪造结果；预约准入统一由服务端未完成订单规则决定。

> 基线：develop `3ae4add`（PR #33/#34 已合入，QML 栈在 `client/qml/`）。
> 领地：`client/qml/pages/station/**`（成员2）。组件/服务名逐字取自 `client/qml/CONTRACT.md`。
> 本稿两用途：① 每页迁移的对账单；② **今晚桥接口需求清单**（§桥缺口）——station/reservation 等桥按此形状补即可。

## 通用直译口径（映射速查的代码化）

| widgets 原语 | QML 直译 |
|---|---|
| QStackedWidget 页栈/返回栈 | Shell `StackView`（已存在，login gate 已做）；页内跳转一律 `App.navigate(route, arg)`，页根 `property var arg` 收参 |
| 页面构造里 setObjectName | 根 `Item { objectName: "…Page" }`（锚点值见下表，勿改） |
| QTimer 倒计时（订单页） | `Timer { repeat: true; interval: 1000 }` + `readonly property int remainingSeconds` 绑定 |
| connect(service, signal, page, slot) | `Connections { target: <contract名>; function on<Signal原驼峰>() }` |
| setProperty+polish 动态 QSS 态 | 直接 `color: xxx === "fault" ? Style.danger : …` 绑定（缺陷3类问题天然消失） |
| 列表重建式刷新（refreshFilteredCards） | `ListModel` + `property var raw`，投影函数只重排 model |
| QDialog 弹层 | `Popup`（非模态）——FilterDialog 用 |
| 页面级 setStyleSheet | 全部 `P.Style.*` token；禁止字面量（契约 §2） |
| 状态门（缺陷4模式） | `property bool loaded/failed` 门 + `viewState` 串（"loading"/"empty"/"error"/"list"）绑定 NoticePanel.visible |

每页根节点必须自绘背景 `Rectangle { anchors.fill: parent; color: P.Style.bg }`（offscreen 截图已知问题）；绑定里 `App &&` 守 teardown。枚举比较一律 `String(x).toLowerCase()`（桥口径小写串，双保险）。

## P0 五页

### HomeShell（壳）——已完成，无需我写
成员3 的 `Shell.qml` 即旧 HomeShell 双生（objectName "homeShell"，登录门、四 tab、toast 都在）。**缺口**：路由表只有 station/login/profile 三行指向我；需他追加：
`station_detail→StationDetailPage.qml`、`reservation_confirm→ReservationConfirmPage.qml`、`reservation_module→ReservationModulePage.qml`、`navigation→NavigationPage.qml`、`favorites→FavoritesPage.qml`、`notifications→NotificationPage.qml`、`settings→SettingsPage.qml`、`coupon→CouponPage.qml`（功能增加批新增），并把这 8 条 + station/login/profile 翻进 `migrated`。
**顶栏漏斗一行接线**：`StationHomePage`/`FavoritesPage` 已暴露 `openAdvancedFilter()`，Shell `onFilterRequested` 空桩改为
`if (stack.currentItem && stack.currentItem.openAdvancedFilter) stack.currentItem.openAdvancedFilter()` 即活。

### LoginPage.qml（"loginPage"）
| widgets | QML |
|---|---|
| QLineEdit 校验器 `1[0-9]{0,10}`/11 位 | `TextField { maxLength: 11; acceptableInput: /^1[0-9]*$/.test(text) }` |
| 点登录 → `authService_->login(phone)` | `authService.login(phone)`（契约名） |
| handleLoginStarted：遮罩+按钮"登录中…" | `Connections onLoginStarted` → `overlay.running=true; busy=true`（桥若无此信号则本地置位，TODO(contract)） |
| handleLoginSucceeded(user, created) | `onLoginSucceeded(user, created)` → 成功文案（含"（已自动注册）"）；壳收 loginStateChanged 自动翻页 |
| handleLoginFailed(message) | `onLoginFailed(message)` → resultLabel 红字 |
| resetState() | `function resetState()`：清 busy/文案回"请输入11位手机号"（含 secondField） |
| **二级保护密码（登录环节，用户二轮指定口径）** | `secondVisible: phoneOk && secondRequired(phoneField.text)`——命中"设过密码且开启保护"的手机号才现 `secondPasswordEdit`（objectName 同）行；submit() 前置门：空→"该账号已开启二级保护密码，请输入密码后登录"；错→清框+"二级保护密码错误，请重新输入"。校验服务 `verifyProtectionPassword` 优先（`hasProtectionPassword()+protectionEnabled()` 皆真=任意号需密码）、`StationState.needsSecondPassword(phone)` 库兜底；`noteLoginPhone(phone)` 记最近登录号供设置页绑定 |
| **整页滚动（二轮修复批）** | 页根套 Flickable（`loginCol` x/y=spaceXl，contentHeight=col.height+2*spaceXl），小屏可上下拖拽 |

注：AuthService 是裸服务（非桥），`login()` 非 slot——调用即失败，属预期（今晚补桥；桥方法名 `login` 不改）。mock 通道实为 `QmlApp::authService()==nullptr`（app_bridge.cpp:99）——空调用不抛异常，页面以 `App.login()`（C++ Q_INVOKABLE mock 直登，app_bridge.h:83）兜底，桥落地后走 `if (authService)` 正路。

### StationHomePage.qml（"stationHomePage"）
| widgets | QML |
|---|---|
| 构造即 `search()`；QSignalBlocker 防触发循环 | `Component.onCompleted: stationQueryService.search(keyword)`；投影纯函数无循环 |
| queryStarted/Succeeded/Failed | `Connections` 三分支 → `loading/loaded/failed` + `setRefreshing(false)` |
| 结果缓存 lastResults_ → 本地投影（排序/电价/筛选三源） | `property var raw: []`；`function project()`：priceMax→`priceCentsPerKwh<=priceMax`，排序三档=综合（默认，服务端顺序透传）/`availableChargers desc`/`distanceMeters asc`（-1 排后），criteria 组内 OR 组间 AND |
| StationFilterCriteria 8 组 | `property var criteria`（JS 对象同字段名：maxDistanceKm/statuses/operators/accessTypes/parkingFees/features/chargerTypes/voltageBands），与 applyStationFilter 同语义 |
| 卡片：名称/电价/空闲/距离/☆ | delegate `ClickableCard` + `App.navigate("station_detail", {id,name,…})`；星星 `MouseArea` eat 事件 → `favoritesService.toggle(id)`；`text: favoritesService.contains(id) ? "★" : "☆"`（`favoritesChanged()` 重算绑定） |
| 空态「重置」不误清关键词（缺陷2 口径） | `hasActiveFilters()`：criteria 非空或 priceMax>0；重置只回退这两源 |
| 搜索框在壳顶栏 | `arg` 作为关键词入口（route 参数）；Shell 接线已完成（用户点名修复）：`onSearchSubmitted→App.navigate("station", keyword)`，页端 `onArgChanged` 收词重查；搜索框/铃铛显隐改绑 `stack.currentItem.route`（原绑 `shell.route` 仅启动求值一次） |
| 地图分栏（WebEngine） | `StationMapItem.qml` Canvas 示意（降级态可视化 + 选卡联动高亮）；真图=明天 QtWebEngineQuick 决策 |
| PullToRefresh | `P.PullToRefreshArea` 包 ListView，`onRefreshRequested: search(keyword)` |
| 演示数据通道（桥缺位，二轮修复批） | `refresh()`：服务缺位或调用抛 → `loadDemo()`（demo=true，`homeDemoCaption` 标"演示数据…接入后自动替换"）；4 站带经纬度→地图自动布点、关键词按名称/地址过滤；`onQuerySucceeded` 置 demo=false 自动退位；**真实失败信号 onQueryFailed 仍保留失败 UI+重试钮**（用户要求异常页不撤） |
| 筛选栏溢出 | 二轮修复批曾用 `stationFilterBarFlick` 横向 Flickable（原固定宽 Row 把「⛏ 筛选」钮裁成半截）；**变基后按"机制按成员3"口径换成 develop 的 `Flow` 自动换行**（同缺陷同修，六控件 420 宽放不下时折行，无横向滚动残留） |
| ListView 尺寸 | PullToRefreshArea 内容是 Column（平台禁垂直/fill 锚）：显式 `width: parent.width+2*spaceSm / x: -spaceSm / height: pull.height` 等价还原 -spaceSm 出血 |

### StationDetailPage.qml（"stationDetailPage"）
| widgets | QML |
|---|---|
| 入参 station+distance → `fetchDetail(station, distanceMeters)` | 结构化参数过不了桥 → 桥需 `fetchDetailById(int stationId, int distanceMeters)`，见 §桥缺口；页面 `arg` 带 {id,name,distanceMeters} |
| detailStarted/Succeeded/Failed | Connections → viewState 门（含"站点正常但无桩"= chargers 空 + hasChargerData） |
| 离线横幅 | `status.toLowerCase()!=="active"` → 红条（`warningSoft` 底） |
| 故障桩红卡（原属性选择器） | delegate `Card { border.color: st==="fault" ? Style.danger : Style.line; border.width: st==="fault" ? 2 : 1 }` |
| 桩状态彩签 | `StatusTag { tone: {available:"success",reserved:"warning",charging:"info",fault:"danger",offline:"neutral"}[st] ?? "neutral" }` |
| 预约按钮三重准入 + reservationBlocked | `App.loggedIn` 判 + 车辆数判（功能增加批：桥缺位读 StationState 车辆通道，0 辆弹 `vehicleRequiredPrompt`〔去添加车辆→设置〕；在途名额判 `reservationService.activeReservationCount()` 缺位放行，占满弹 `unfinishedReservationPrompt`〔去查看→预约模块〕——旧 HomeShell 弹层同文案同钮直译）→ `App.navigate("reservation_confirm", arg)` |
| 演示数据通道（桥缺位，二轮修复批） | fetch() 抛 → `loadDemo()`：6 桩五态全展（available×2/reserved/charging/fault〔红框〕/offline），detailLoaded=true 让预约链路完整可走；头卡标注"演示数据（详情桥未就绪…）"；`onDetailSucceeded` 自动退位。delegate 引 `chargerList.width` → ListView 须真 `id: chargerList`（objectName 不可作标识符解析） |

### ReservationConfirmPage.qml（"reservationConfirmPage"）
| widgets | QML |
|---|---|
| 车辆下拉（SettingsService.vehicles + 默认车） | `ComboBox` model=桥 vehicles()（缺 → TODO(contract) + 空列表占位） |
| QDateTimeEdit 起止 | 无对应控件：`startMinutes/endMinutes` 双 `Slider`（0~1439，步进 15）+ 文本显示 HH:mm（样式简陋先不管，逻辑位齐） |
| ✨推荐时段（recommendSlot 静态） | 桥需 invokable `recommendSlotFor(distanceMeters)`→{startMinutes,endMinutes}；未补前页内 JS 同公式兜底（模拟分钟=distance/500，+5min 缓冲） |
| 手改不覆盖（人优先） | `property bool userEdited`：任一 Slider 动了即 true；推荐只更新 caption 不改值 |
| 预估费用联动 | `readonly property int feeCents: priceCentsPerKwh * hours`，Text 绑定 |
| ≤45min 约束 | `readonly property bool tooLong: end-start>45`；红行内提示 + 提交 enabled: !tooLong && … |
| submit → submitStarted/Succeeded/Failed | `reservationService.submit(...)` TODO(contract)（载荷改 map）；成功→`Dialog`"是否现在前往充电？"→`App.navigate("navigation", record)` / `App.navigate("reservation_module")`；失败 Toast |
| 整页滚动（二轮修复批） | 主 Column 套 Flickable（`confirmCol` x/y=spaceLg，contentHeight=col.height+2*spaceLg），小屏长表单可上下拖拽 |

## P1 五页

### StationFilterDialog.qml（"stationFilterDialog"）
`Popup { modal: false }`；8 组 = Column{Repeater{ActionButton variant:"chip"}}；距离单选（再点取消）、其余多选 toggle；「重置」只清勾选、「确定」`applied(criteria)`→父页 project()。旧组选项字面量（5/10/30/50 km、营业中等）从 widgets 常量 JS 化，TODO(contract)：改由服务层暴露选项。

### NavigationPage.qml（"navigationPage"）
arg=record map；进页先模拟口径（distance/eta 兜底公式），`mapGeoService.requestDrivingRoute({lat,lng},{lat,lng})`（桥：参数改 double×4 或 map）→ `onRouteSucceeded(route)` 原地替换 + caption"真实导航路线·腾讯地图"；`onRouteFailed` Toast+保持模拟；`requestDistanceMatrix`/逆地理同桥形状。`StationMapItem` 画 route polyline；代际号 `property int gen` 保留（QML 也要防过期回调）。

### StationMapItem.qml（组件，非页）
Canvas：`property var markers`（{lat,lng,label,selected}）、`property var route`（[[lat,lng],…]）；自动 fit 边界 + 10px 内边距；示意底（网格+对角线河）纯装饰；`onPaint` 里画点/折线；无 WebEngine 依赖、offscreen 可截图。**决策记录**：任务书建议 QQuickPaintedItem C++ 包壳——但类型注册要动 `client/qml/main.cpp`（成员3 禁区），CMake 挂载点只透 .qml；故 Sprint 用 Canvas 等价实现，明天真地图走 QtWebEngineQuick `WebEngineView`（独立进程无 Widgets 互斥问题）时一并定夺。

### ReservationModulePage.qml（"reservationModulePage"）
二级 Tab 两 ActionButton（variant 切换 chip/primary）；`reservationService.fetchList()` → `onListStarted/Succeeded(records)/Failed`：按 status.toLowerCase() 分发 active(=="reserved")/done(其余)；取消 `onCancelSucceeded` → 切 Tab + 重拉；`reservationExpired` 信号（桥名以 C++ 为准 TODO(contract)）→ 重拉。二轮修复批：fetchList 桥缺位 → `loadDemo()` 3 条记录（active 进行中/fulfilled/cancelled），`moduleDemoCaption` 标注，`onListSucceeded` 自动退位——修复用户点名的"预约列表加载失败"；母页套 `moduleFlick` Flickable 整页可上下拖拽。

### ReservationOrderPage.qml（"reservationOrderPage"）
三栏 Row：左距离 Column、中信息+倒计时、右电量占位。`Timer` 每秒 `remainingSeconds--`；颜色 `{>`}三档绑定 Style.brand/warning/danger；归零→`reservationService.expireReservation(id)`+`fetchList()`。取消按钮 ActionBar variant:"danger"。二轮修复批：三栏套 `orderFlick` Flickable；**删除撑位 `Item{height: parent.height-200}`**——Column 子项绑 parent.height 触发 polish 死循环（demo 记录渲染后暴露，冒烟 timeout 根因）；窄卡 caption 补 width+WordWrap（NoWrap 裁字）。

### ReservationCompletedPage.qml（"reservationCompletedPage"）
ListView delegate ClickableCard（站点/桩号/时长/费用/状态 StatusTag）；点击→`Dialog` 详情全字段；空态 NoticePanel。

## P2 三页 + ProfilePage

- **SettingsPage.qml**（"settingsPage"）：三模块卡（锚点=settingsSecurityCard/settingsVehicleCard/settingsNotificationCard）；密码弹窗（改密验旧 + ≥4 位 + 两次一致；字段=currentPasswordEdit/newPasswordEdit/confirmPasswordEdit，钮=passwordCancelButton/passwordSaveButton）；**进页密码门已撤销（用户二轮指定口径倒转）**：二级密码作用点在登录环节（见 LoginPage 表），本页只做设置/开关，`protectionSwitchHint` 三态文案同步为登录口径（"已开启：该账号下次在登录页输入手机号时，将要求输入二级保护密码"）；保存时 `StationState.setSecondPassword(plain, 最近登录号)` 把密码绑定到登录用手机号；车辆 CRUD（行钮 vehicleSetDefaultButton/vehicleEditButton/vehicleDeleteButton；弹窗含 vehicleDefaultCheck + 接口双 RadioButton〔对齐旧版，替原 ComboBox〕 + 取消/保存钮组）；通知三开关 `Switch` ↔ `settingsService`；整页 `Flickable`（二轮修复批）。**双通道**：服务 invokable 可读以服务为准，否则落 StationState 会话库——密码哈希/保护开关/车辆跨页往返不丢（真持久化归桥）。
- **FavoritesPage.qml**（"favoritesPage"）：同构 StationHome 列表源=favoritesService.favoriteIds()∩stationQueryService 结果；状态门三 flag 直译（queryLoaded/queryFailed/viewState）；星星可取消；筛选弹窗复用；暴露 `openAdvancedFilter()`。
- **NotificationPage.qml**（"notificationPage"）：`notificationService.notifications()` 桥→ListView；`onNotificationsChanged` 重算；空态引导。
- **ProfilePage.qml**（"profilePage"，Shell 路由表指向本目录）：渐变头卡（锚点 uiProfileHeroButton；余额=App.currentUser 的 balanceLabel/nicknameLabel，含 wallet/recharge 两入口与 profile_edit 编辑位）+ 入口列表五项（openOrdersButton / openReservationsButton / openFavoritesButton / **openCouponsButton（功能增加批）** / openSettingsButton）+ 退出登录（`authService.logout()` 缺位期以 `App.logout()` mock 兜底）。

## 功能增加批（2076518，用户实测反馈驱动——注意与纯迁移批次的口径区别：本节是新增功能，无 widgets 对账源）

- **CouponPage.qml**（"couponPage"，route "coupon"）：可用/已使用/已过期三态 Tab + 面额（cash ¥）/折扣（discount 折）双券型卡 + 「去使用」占位（TODO(contract): redeem）+ 空态。旧版仅在 README 留"同款式敬请期待"槽从未实装页面；券服务不在 CONTRACT.md → 桥缺位期渲染页内**演示数据**（与 mock 直登同口径，页内标明"演示数据·券服务桥未就绪"），`typeof couponService` 守卫接入后自动替换。
- **StationState.js**（`.pragma library` 跨页会话库）：cyrb53 双 32 位混合散列存二级密码（明文即散即用，UI 不落）、protectionOn 开关、车辆数组本地通道（CRUD + 默认车迁移）。进程退出即清空——真持久化归 SettingsService（TODO(contract)）。SettingsPage/StationDetailPage/ReservationConfirmPage 三处消费。
- **二级密码拦截位决策（已被二轮口径倒转，留档）**：功能增加批曾按当时用户指定把拦截放"我的→设置"入口（进页锁屏门）；二轮复测用户明确"逻辑完全颠倒"——现行口径为**登录环节按手机号拦截**（见 LoginPage 表 + 二轮修复批节），设置页锁屏门已整体撤销。预约/取消位如需拦截，服务桥落地后可在此基础上二次开启。
- **widgets 锚点全对账**（本轮 method：widgets 页 `setObjectName` 全量 ∖ QML `objectName` 全量）：交互锚点 40+ 处逐字对齐（改名/补名，明细见 commit message）；**未采用同名的剩余项=三类**：①布局容器（Scroll/Stack/Splitter/Pane——QML 声明式列表无此物）；②成员3 域（ui*/recharge*/wallet/homeShell/appRoot）；③被更好形态吸收（订单页 loading/error 归母页 moduleNotice、导航 map 占位=StationMapItem、详情 loading 态并入 detailNotice 文案）——不算缺失。

## 二轮修复批（98c9b6b→e4fd0b9 变基后，用户二轮复测——三截图+六条指令，全部增量修改，配色/圆角/绿色主按钮未动）

- **二级密码倒转**：设置页进页锁屏门整体撤销（`secondPasswordGate` 删除，原位置留口径注释）；LoginPage 新增密码行 `secondPasswordEdit`（仅"设过密码+开启保护"的手机号命中出现）+ submit() 前置门（空/错两提示）；StationState 增加 `passPhone` 绑定与 `noteLoginPhone/accountPhone`——设置页存密码时绑最近登录号。服务通道优先（hasProtectionPassword+protectionEnabled 皆真=任意号需密码），库通道兜底。
- **整页滚动**：登录/预约确认/预约模块/订单四页根容器套 Flickable（用户点名三页全覆盖+订单页连带）；找站/详情列表本在 ListView 内滚动，无需外层。
- **顶栏修复（成员3 文件两处最小改动，用户点名"搜索框/通知图标/返回按钮"）**：① `Shell.searchVisible` 改绑 `stack.currentItem.route`（原绑 `shell.route` 仅启动求值一次→登录后搜索框/铃铛永不再显示）；② `onSearchSubmitted` 改 `App.navigate("station", keyword)`（原丢关键词）；③ TopNavBar 返回钮容器 1×1 Item→`backText.implicitWidth × nav.implicitHeight`（原 verticalCenter 致文字上半截越出导航条被窗口顶缘裁切）。均仅改绑定/布局，样式 token 不动。
- **演示数据通道（找站/详情/预约模块三页）**：桥缺位 catch→`loadDemo()`，页内标"演示数据（…桥未就绪，接入后自动替换）"，真数据信号到达 demo=false 自动退位；**真实失败信号（onQueryFailed/onDetailFailed/onListFailed）保留失败 UI+重试**（用户要求异常页不撤）。找站 4 站带经纬度→地图自动布点（用户点名"地图渲染排查"结论：地图组件无 bug，全红因列表无数据可布点）；关键词搜索在演示通道按名称/地址过滤（搜索链路完整可验）。
- **订单页 polish 死循环**：三栏 Column 内撑位 `Item{height: parent.height-200}` 绑 parent.height→布局循环求值（冒烟 06 timeout 根因，demo 记录渲染后暴露），删除。
- **文字裁切族**：Text 默认 NoWrap——homeDemoCaption/detailChargerSummaryLabel/moduleDemoCaption/orderModuleCaption×2 补 width+WordWrap；找站筛选栏套横向 Flickable（"⛏ 筛选"钮原被裁半截，与返回钮同类缺陷）。
- 冒烟：11 路由 offscreen rc=0 日志门零告警（截图成批同尺寸=克隆 Shell 补丁被仓库版覆盖丢过，重跑 setup-smoke.sh 复原——QML 热加载、翻位表/arg 注入/自动登录三补丁必须在位）；密码流转断言 10/10（StationState.js node 直验）；qmllint 9 页零 Error；截图 12=登录页密码行形态（仅冒烟克隆临时强制 visible 拍摄，仓库绑定未动）。

## 经验等级批（2026-09-09，用户指令："我的页面昵称下加等级进度条（上等级下经验、可点击进详情）+ 与设置并列的每日任务，完成加经验"——无 widgets 对账源的新增功能）

- **ProgressService**（`client/services/profile_charging/src/progress_service.cpp`，纯客户端 C++ QObject，QSettings 组 `progress/<phone>` 持久化，零服务端交互）：五档累计 XP 门槛 青铜0/白银60/黄金150/铂金350/黑金700（🥉🥈💎👑，礼包 0/100/150/200/300 + 权益文案）；每日任务 签到30/搜索20/详情20/路线20/月报20 + 全勤30，**当日幂等**（done 表 taskId→日期，跨日自然重置，QSettings 键 `task_<id>` 不含 '/'——含 '/' 会被读成子组致 childKeys 不可见）；`setTodayForTesting` 日期缝供多日推进单测。
- **XP 事件漏斗=单点 `QmlApp::navigate()`**：station_detail→detail、navigation→route、stats→stats、station+非空关键词→search（顶栏搜索经 `App.navigate("station", keyword)` 天然入漏斗）；reservation_confirm 早退不进漏斗；签到由 TasksPage/PointsPage 在真实 `pointsService.checkIn()` 回执后 `reportEvent("checkin")`。
- **双账本诚实口径**：服务端 points_ledger 仅 CHECK_IN(+10/日)/RECHARGE 两路写入，`UserApiAction` 无 grant 动作、CouponService 只读 → 签到任务给**真积分+经验双份**，其余任务经验与升级礼包"积分"记在等级体系自己的账目（`ProgressService.gifts`），TasksPage/LevelPage 页脚均如实标注分账。若要礼包真入账需新增服务端 wire 动作（TODO(contract)，二期）。
- **暴露面**：`QmlApp.progressService` Q_PROPERTY（NOTIFY servicesChanged，与会话级桥同形态随登录重建）；页面 `App.progressService` + null 守卫，main.cpp/context 零新增；升级 toast 在 app_bridge 的 levelUp connect 里发。
- **UI 三处**：ProfilePage hero 昵称下 徽章「🥉 Lv.1 青铜会员 ›」/进度条（starGold 填充+Behavior 动画）/经验行「当前 X/SPAN XP · 距 NEXT 还需 N」，点徽章→level；行列表与设置并列新增「🗓️ 每日任务 / 🏅 会员等级」；**TasksPage.qml**（"tasksPage"，route "tasks"：等级 hero 卡+五任务卡+签到直发钮+全勤行+分账脚注）；**LevelPage.qml**（"levelPage"，route "level"：档位色 hero+五档阶梯权益 StatusTag（已达成/当前/未解锁）+升级礼包记录+分账脚注）。
- **测试**：新增 `tst_progress_service.cpp` 10 例（QTemporaryDir 隔离域；起点/当日幂等/未知事件/全勤只发一次/多日升档礼包逐档/跨日重置/落盘续读/手机号互不串）；client_pages +2（任务页×真引擎签到→经验→升级 toast 全链，**用例开头 remove("progress") 清零**——PointsPage 用例经新签到钩子合法写入会污染基线；等级页×引擎状态镜像，断言全取动态值）；station_interactions +1 真壳端到端（等级条渲染/点徽章进页/tasks 路由/stats 漏斗+幂等）。Shell.qml migrated/pageSource 各 +2 路由。

## 桥缺口（今晚补桥的形状建议，成员2→成员3）

| 服务 | 需要的桥方法/信号（名字=C++ 原名，载荷改 map/list） |
|---|---|
| StationQueryService | `search(keyword)`；`queryStarted/querySucceeded(stations[])/queryFailed(msg)`；**新增** `fetchDetailById(int stationId, int distanceMeters)`（替代 struct 参数版）+ `detailStarted/detailSucceeded(detail)/detailFailed`；list map 字段=StationListItem 拍平（含 station 的 id/name/address/priceCentsPerKwh/status/totalChargers/availableChargers + distanceMeters/operatorName/accessType/parkingFee/features/chargerTypes/hasVoltageBelow700/hasVoltageAtLeast700），枚举串小写 |
| AuthService | `login(phone)` / `logout()`；`loginSucceeded(userMap, createdBool)/loginFailed(msg)`（或页面只依赖 App.loginStateChanged + currentUser；mock 通道 App.login/logout 已可用，页面已接兜底） |
| ReservationService | `fetchList/cancel(id)/expireReservation(id)/submit(map)`；`listStarted/listSucceeded(records)/listFailed/submitStarted(chargerId)/submitSucceeded(record)/submitFailed/cancelStarted/cancelSucceeded/cancelExpired(…)`；record map=ReservationRecord 拍平；`recommendSlotFromTravelMinutes` 以 `Q_INVOKABLE` 暴露；**新增** `activeReservationCount()` / `activeCountForVehicle(qint64)`（详情页预约三重准入的名额判，缺位返回 -1 放行） |
| SettingsService | 车辆 `vehicles()`（Q_INVOKABLE，map 列表）/`addVehicle(Vehicle)/updateVehicle(Vehicle)/removeVehicle(qint64)/setDefaultVehicle(qint64)/defaultVehicle()` + `vehiclesChanged`；二级密码四件 `hasProtectionPassword()/setProtectionPassword(plain)/verifyProtectionPassword(pw)/clearProtectionPassword()` + 开关 `protectionEnabled()/setProtectionEnabled(bool)` + `protectionStateChanged`（设置页与详情页调用点已按此原名接线，桥一落地即自动翻正）；通知开关按位 `notificationEnabled(key)` + 对应 setter + `settingsChanged` |
| CouponService（**全新服务，CONTRACT.md 尚无**） | `coupons()` invokable（map 列表：id/title/kind(cash|discount)/valueCents/discountTenths/thresholdCents/condition/expiresAtUtc/status(available|used|expired)/source）+ `redeem(id)`；`couponsChanged`。缺位期 CouponPage 以页内演示数据渲染三态并标"演示数据"，接入后自动替换 |
| FavoritesService | `contains(id)/toggle(id)/favoriteIds()/favoriteCount()` 全部 `Q_INVOKABLE`；`favoritesChanged` |
| NotificationService | `notifications()` invokable（新→旧 map 列表）；`notificationsChanged` |
| MapGeoService | `requestDrivingRoute(fromLat,fromLng,toLat,toLng)` / `requestDistanceMatrix(list)` / `requestGeocode(lat,lng)`；`routeSucceeded(routeMap)/routeFailed(err,msg)`（routeMap={polyline:[[lat,lng],…], distanceMeters, durationMinutes, steps:[{instruction,distanceMeters},…]}——导航页折线/距离/步骤三消费位已齐）、`distanceMatrixSucceeded(elements)/distanceMatrixFailed`、`geocodeSucceeded(address)/geocodeFailed` |
| Shell（非桥，路由表） | station 域 10 条路由已由成员3 翻位落地（develop `0f65111`：migrated+10、pageSource+7）；**仅剩 coupon 两行**（`migrated` +1、`pageSource` +1 行）与顶栏漏斗一行接线 `stack.currentItem.openAdvancedFilter?.()`。`searchVisible` 绑当前页 route、`onSearchSubmitted→App.navigate("station", keyword)`、TopNavBar 返回钮容器尺寸三处=用户点名代修（各带根因注释），已随 rebase 并入本分支 |

## 验收自查（19:00 冲刺门）
- [x] `--view=station`：地图示意+筛选栏渲染；列表区为设计内降级态"站点加载失败·站点查询桥未就绪（等待服务桥今晚补全）"
- [x] `--view=login` 表单+校验渲染通过；`--view=profile` 余额卡（¥100.00 演示户）+五入口全出
- [x] detail（arg 头卡即出+桩区失败态重试）/ confirm（A-07/直流快充 120kW/23:45—00:30 推荐+无车辆引导）/ module（Tab+列表失败态）/ navigation（真 arg：模拟路线折线+2.4km+建议出发时刻+降级 Toast）——桥未补前均按口径渲染降级态，10 路由截图零运行期报错
- [x] 截图通道：仓外克隆 `/tmp/smoke-src` 打 Shell 翻位+`--arg=` 补丁（见 §截图环境），仓库 Shell.qml 未动
- [x] 每页根 objectName == 上表锚点
- [x] 修正批 `55ef843`：onClickFunction、Row polish 环、IntValidator 溢出、hhmm 分钟位、余额卡隐高、null-record 绑定、QQuickPopup 作用域、详情头卡 arg 即出、桥缺位显式降级
- [x] 审计轮修正批：登录/退出在 mock（authService=nullptr）下兜底 `App.login()/logout()`（不再卡遮罩）；收藏页投影补 voltageBands 组；导航页存 `route.steps`（真实转向指引 + >15 段截断文案）；地图 hit-test 与 onPaint 界域同构（markers∪route、NaN 坐标天然跳）；卡内 anchors.fill 告警 9 处清理（ClickableCard/P.Card 同款：内容 default-property 进内部 Column，卡内锚点被忽略且逐实例告警）——ClickableCard 5 处（profile×2 实爆 + favorites/station/completed×3 潜伏）+ P.Card 4 处（notification delegate×1 + 订单页三栏卡×3，均桥落地/记录到达即爆）；改 Column 契约内 width 绑定、卡高交内容自然高，10 路由复跑零告警零报错
- [x] 二轮修复批（98c9b6b→e4fd0b9）：二级密码倒转到登录环节/四页 Flickable/顶栏搜索·铃铛·返回钮修复/三页演示数据通道/订单页 polish 环/NoWrap 裁字族——11 路由 rc=0 零告警 + 密码断言 10/10 + qmllint 零 Error，详见 §二轮修复批
- [x] **经验等级批（2026-09-09）**：ctest 53/53（含新 progress_service 10 例）；qmllint 新改页面零告警；三页 offscreen 截图（我的页 hero 等级三件套/任务页五卡+全勤/等级页阶梯）；Wayland 真机换新二进制运行中
- [x] **变基轮（rebase 至 develop `0fcc44d` 后复测）**：成员3 服务桥（`3db8931`）与路由翻位（`0f65111`）已在基底落地 → station/reservation_module 直渲**真数据**（上条"桥未就绪"降级态自然退位，演示通道 catch 不再触发）；订单三栏=成员3 塌陷修正（colW+自然高）外套本分支整页 Flickable；排序 chip `selected` 机制与"综合=2/空闲=0/距离=1"编号以 develop 为准并入。复跑 11 路由 offscreen rc=0 零报错、qmllint 零 Error；解冲突口径=样式/机制按成员3、widgets 对账锚点 objectName 按本分支（`detailPriceLabel/detailDistanceLabel/reservationOrderTabButton/uiProfileHeroButton` 等实名保留）

## 截图环境（发给成员3 的翻位清单=克隆内已验证补丁）
QML 自 `CHARGING_QML_SOURCE_DIR` 文件系统加载（改 .qml 无需重编）；运行
`QML2_IMPORT_PATH=<解包 qml6-modules> QT_QPA_PLATFORM=offscreen charging-qml-preview --view=X [--arg=JSON] --screenshot=…`。
解包 qml6-modules 实机路径=`/home/bit/qt6qml/usr/lib/x86_64-linux-gnu/qt6/qml`（apt 未装 qml6-module-*，缺该环境变量时
所有 preview 运行报 "module QtQuick.Window is not installed"——排查一次记一次）。
**变基轮起**：develop 基底已含路由翻位与 `--arg=`/`--logged-in` 原生支持（成员3 `0f65111`+`d9b8c11`），
10 路由可**仓库直构直跑、零补丁**；仅 coupon 一条仍需克隆翻位（Shell 两行：migrated +1、pageSource +1）。
截图产物：`~/qml-station-shots/01..10-*.png`。
