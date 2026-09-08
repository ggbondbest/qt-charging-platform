# QML station 域迁移 · 状态/信号→绑定映射草稿

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
| **设计轮（2026-09-08 用户指定"ui 风格相称+按钮布局设计感"）** | 全部 widgets 锚点保留（真点击回归依赖），版式重排：① 出血 hero 绿渐变带（`stationHeroBand`，ProfilePage 同款 x:-margin 出血+Gradient.Horizontal heroFrom→heroTo）承载标题/关键词态+三统计大字（N 座电站/N 枪空闲/均价）；② **map⇄list 分段切换**（`viewModeListButton`/`viewModeMapButton` 双段胶囊，EV 充电 app 通例）——地图态 `stationMapPanel` 高 128⇄300 动画展开+选中 **peek 浮卡**（`stationPeekCard`：名称/地址/空闲/价格大字+`stationPeekOpenButton`"详情"）；③ 筛选整合：原 Flow 六件散排→**单行胶囊工具栏** `stationFilterBar`（排序三 chip〔文案精简"空闲/最近"〕｜分隔线｜电价 combo）+ 漏斗 `advancedFilterButton` 迁到分段行右端固定席位（原 Flow 溢出裁钮问题的布局级根治）带**激活筛选计数徽标** `advancedFilterBadge`（有筛选时按钮自动 primary 化）；关键词非空现 `clearKeywordButton`"✕ kw"；④ 站点卡：价格大字右挂（¥/kWh 上下排 fontLg2 bold brandDeep）+ **空闲比例条**（可用性色彩：brand 充足/warning<34%/danger 0）；状态 Tag+星星锚点不动。锚点行为（selectedMarker 双向联动/pull/四态门）零改动。
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
arg=record map。**四轮地图 APP 化（2026-09-08 用户指定）**：进页自动 `requestIpLocation()` IP 定位起点→成功 `setUserLocationLatLng`+起点态 `located`，失败明示"定位失败·请手动输入"（`failed` 态，兜底演示坐标保页面永不空）；起点行 `originRow`（状态提示 `originHintLabel` + 输入框 `originField`〔地址或"纬度,经度"，前者走 `requestAddressGeocode`，后者直用〕+ `originGoButton`"路线" + `originLocateButton`"🎯"重定位）。真路线：`requestDrivingRoute(4 double)`→`qmlRouteReady(requestId, routeMap)`（polyline=[[lat,lng],…]/steps=[{instruction,distanceMeters},…]/distanceMeters/durationMinutes——direction duration 分钟口径；requestId 代际过滤）；距离覆盖 record.distanceMeters 重算 eta。地图=**腾讯静态图优先**（route 到达后 `requestStaticMap(中点, 距离→zoom 阶梯, 真 polyline, 起/终 markers)`→`qmlStaticMapReady(filePath)` 落 TempLocation PNG→`Image.source=Url.fileUrl(…)`；任何失败静默回落 `StationMapItem` Canvas 真折线——QtLocation/QtWebEngine 本环境均无，此为本机可行的"真地图"上限）。**跳转腾讯地图导航**=`navigationExternalButton`→`Qt.openUrlExternally(navigationUriUrl(...))`（URI API routeplan，URL 内嵌 referer=key：绝不打印/入库；无 key `usable()==false` 置灰）。消费面全为 `qml*` 转发信号（原 struct 载荷信号 QML 读不了，见 §桥缺口定形）。无 key 保持模拟先行（与原 widgets 口径一致，不发请求）。

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
- **ProfilePage.qml**（"profilePage"，Shell 路由表指向本目录）：渐变头卡（锚点 uiProfileHeroButton；余额=App.currentUser 的 balanceLabel/nicknameLabel，含 wallet/recharge 两入口与 profile_edit 编辑位）+ 入口列表（openOrdersButton / openReservationsButton / openFavoritesButton / **openNotificationsButton / openCouponButton（三/四轮批补——"消息页面进不去"修复的入口位）** / openSettingsButton）+ 退出登录（`authService.logout()` 缺位期以 `App.logout()` mock 兜底）。

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

## 三/四轮用户实测修复批（四合一请求 b/c 项，2026-09-07/08；d 项地图 APP 化见 NavigationPage 节）

- **"收藏按钮点不了"根因（成员3 文件 `ClickableCard.qml` 功能性代修）**：卡内容器盖层 `MouseArea{anchors.fill}` 声明在内容 Column **之后**→Qt 命中栈前→后遍历，卡内子按钮永远被盖层先吃→点击被劫持去详情页。修复=盖层 MA 移到内容之前（附根因注释）；文本区（无 handler）经命中栈下沉仍由盖层接住，卡导航不回归。**曾先试 `z:1` 提权——无效已撤回**（z 仅作用于兄弟节点间，子树出不去父级堆叠）。
- **"消息页面进不去"定性**：真点击测试证明 HEAD 铃铛链（TopNavBar Text"🔔"→`onNotificationsRequested`→`App.navigate("notifications")`→Shell 翻页）**本就正常**——用户复现的是**入口可发现性**：ProfilePage"账号与服务"缺消息行（PR#41 重写时又丢了优惠券行）。修复=补 `openNotificationsButton`"🔔 消息通知"+`openCouponButton`"🎟 优惠券"两行入口（配合 Shell coupon 翻位，见 §桥缺口 Shell 行）。
- **回归钉（新测试目标 `test_qml_station_interactions`，tests/CMakeLists.txt append）**：与 qml_client_pages（信号发射驱动）分工——此套专测**事件投递本身**：bootShell 与 preview 同构装配 + `QTest::mouseClick(window,…,QPoint)` 真鼠标穿完整命中栈。① 铃铛真点→notificationPage 上屏；② 卡片 `favoriteStarButton` 真点→星字形翻转+**不得**劫持进详情页，再点站点名长文本→卡导航仍活（盖层下沉链保护）。Qt6.2.4 注意：`QTest::mouseClick` 的 QWindow 重载只收 QPoint；offscreen delegate 惰性→先 `grabToImage` 强制场景图渲染。
- **MapGeoService QML 面 + 静态图/URI 方案**：本环境 QtLocation 与 QtWebEngine 的 QML 模块均不存在（两处 qml 目录已核）→导航"真地图"= 静态图 API（C++ 拉 PNG 落 temp，Image.source 直挂）+ Canvas 真折线回落 + URI API 跳转外部腾讯地图接力导航；详见 §桥缺口 MapGeoService 行与 NavigationPage 节。
- **SettingsBridge 实名差异（待成员3 核对）**：页面对二级密码按 CONTRACT.md 盲写的 `hasProtectionPassword/setProtectionPassword/verifyProtectionPassword/clearProtectionPassword` 与桥实际交付的 `hasSecondPassword/setSecondPassword/verifySecondPassword(+protectionEnabled/setSecondProtectionEnabled)` **不同名**——QML try/catch 落到 StationState 会话库（功能仍闭环），但服务通道永不命中。桥/页面二选一改名即可翻正（页面调用点带 TODO(contract) 注释）。
- **b 项设计轮落地（StationHomePage 重写，全部 widgets 锚点保留）**：出血 hero 绿渐变带（站点/空闲枪/均价三统计）→ map⇄list 分段胶囊（EV 充电 app 通例）→ 单行筛选胶囊条（综合/空闲/最近三 chip + 电价下拉吃满行尾 + 行尾漏斗计数徽标）→ 站点卡价格大字右挂+空闲比例条（danger/warning/brand 三档色）；地图态浮 peek 卡（站点名/地址/空闲/价格+详情按钮）。**运行期守卫**：peek 卡绑定在 `visible=false` 时仍求值，`selectedMarker=-1` 曾穿透 `count > selectedMarker` 上界守卫触发 `get(-1)` TypeError×4——五处绑定统一补 `selectedMarker >= 0`，并新增回归钉（下条）钉死。
- **截图验收轮（2026-09-08，三图零运行期报错）**：`11-home-redesign-list.png`（重设计列表态全貌）；`12-nav-nostream-mock.png`（导航页无 key 态=诚实降级口径：演示位置提示、🎯置灰、Canvas 两点示意线、跳转腾讯地图按钮置灰——key 仅环境变量注入，本机 shell 未导出即为此态，真链路以 21 例假 HTTP 单测覆盖）；`13-profile-entries.png`（"账号与服务"四行入口含新增消息通知/优惠券）。**ctest 环境口径**：qml 两套必须与 preview 同法导出 `QML2_IMPORT_PATH`，缺失时报 `QtQuick.Controls.Basic is not installed`→page 空指针假失败（非代码回归）；本批复跑 **36/36 全绿**。回归钉扩至 4 例 6/6：profile 消息行真点→消息页、map⇄list 分段真点→peek 浮现（-1 守卫不浮现/选中才浮现）→peek 详情导航链。

## 六组批量指令轮（2026-09-08 用户批量指令①—⑥，station 域全量落地）

- **① 车辆强制校验整套撤除（仅 QML+preview 通道，widgets 孪生未动=在案欠账）**：详情页 `vehicleRequiredPrompt`/`unfinishedReservationPrompt` 两弹层连同 `StationState` 车辆读全删；新唯一保留闸=**充电中拦截**（`chargingBusyPrompt`："您有车辆正在充电中，暂无法发起新预约"+"去查看"→charging 页；判定=`orderService.fetchOrders("charging",1)`+`onOrdersLoaded` 非空即拦，桥缺位/失败默认放行=与 activeCount -1 同口径）。装配面一行定音：preview `app_bridge.cpp` **撤 `reservationService_->setSettingsService()` 注入**→`finishMockSubmit` 的 0车拒绝/每车唯一自然失效、名额闸回退"至多 1 条有效预约"——ReservationService 本体与 widgets HomeShell（自带注入路径）零改动，`tst_home_shell`/`tst_reservation_service` 车辆断言全数保绿。确认页车辆下拉首项"（不绑定车辆）"默认选中（vehicleIndex=-1→payload vehicleId=0/plate=""），Slider 去车辆 enabled 条件；设置页两文案改中性（"暂无车辆（预约可不绑定车辆）"）。
- **② 顶栏漏斗唯一入口 + 星星**：`⛛`(U+26DB 字体缺字→豆腐块) 换 **Canvas 标准漏斗**（fillStyle muted/pressed brand）紧邻 🔔，右上挂 danger 计数徽标（`filterBadgeCount`←Shell 绑 `stack.currentItem.activeFilterBadge`，两页提供缺位=0）；页面右上"⛏ 筛选"与收藏页 `favoritesFilterButton` 删除——**全局高级筛选入口仅 1 处**。收藏星 34×24/f18→**42×30/f26 黄色填充 `★`**未收藏灰 `☆` 同尺寸（新 token `Style.starGold:"#FBBF24"`）。
- **③ 地图 key 入库 + 地点检索 + 导航页双输入**：`client/config/map_services.json` 随 git 入库（**2026-09-08 用户知情拍板，推翻上轮"绝不入库"**；安全口径入档：仓库可见=key 可见→控制台必须配签名校验+域名/IP 白名单；日志/信号面零打印 key 与完整 URL 不变）。`MapGeoService` 三级解析：env 两名字（`TENCENT_MAP_API_KEY`/旧 `CHARGING_TENCENT_MAP_KEY`）**任一已定义（含空值）即 env 权威**→原链；两者皆未定义→读 config 文件（编译宏 `CHARGING_MAP_CONFIG_FILE` 注路径，QFile+QJsonDocument 失败静默）→无 key。此"定义即权威"=测试隔离锚：ctest 四套件（home_shell/qml_client_pages/qml_station_interactions/station_map_panel 族）ENVIRONMENT 注空变量即与真实网络绝缘，map 单测 `init()` qputenv("") 天然覆盖。找站页：keyword 非空且 `usable()`→`requestAddressGeocode` 得中心点→**全量拉站+haversine 重算 distanceMeters**→"📍X 周边" hero 标题+地图兜底中心移过去；geocode 失败→Toast+退回现关键词口径；无 key 行为逐字节不变。导航页：目的地行 `destinationRow`（只读输入框显示选中站+【更换】）→`navigationPickPopup` 站列表（search("") 全量/失败兜演示清单，行=名+价+距起点直线距离）→选中重算路线/静态图；外部跳转按钮文案改**"点击导航"**。
- **④ 电价自定义区间**：弹窗距离组后新增"电价区间（¥/度）"组（`priceMinField`/`priceMaxField`+`priceRangeErrorLabel` 红字：格式非法/min>max 均拒提交）；applied 载荷 `priceMinCents/priceMaxCents`（**-1=不限；0 是合法边界，全链禁 `||` 短路**）；`project()` 与预设 combo **AND 叠加**（胶囊条行为不变）；漏斗徽标计入区间。
- **⑤ 站卡八维标签行**：`stationTagsFor(id)` 从 `page.raw` 回查（不动 ListModel 形状）——operator/accessType/chargerTypes[]/features[]/parkingFee/电压双档逐维渲染 `P.StatusTag`；**命中当前 criteria 对应组→tone success+brand 描边**，未命中族基色（免费系/桩型 info、其余 neutral）；`chargerLabel` 演示枚举 fast/slow→快充/慢充、真桥枚举逐字即匹配键；演示通道字段与弹窗枚举错位处="有值即渲染、匹配不上只是不高亮"零裁撤。
- **⑦ 实测追加修复（同日用户桌面实测发现）：导航【更换】弹窗打开即冻结**。根因=弹层 `Column` 只给宽不给高（Column 默认高=implicitHeight 由子项反推），其子 `ListView.height: parent.height - y` 的 parent 正是该 Column → 自我循环，Qt 每帧刷 `QQuickItem::polish() loop`（现场 4575 行后 UI 卡死被杀）。修复=`anchors.fill: parent` 显式定高切断回边（页面主 Column 本就是这个写法所以无恙）。教训入册：**Popup 内布局 Column 必须显式定高/anchors.fill，凡"子项读 parent.height 且 parent 是自动高度容器"即循环**；截图/无头测试不点开弹层就测不出来——已补真点击钉 `navigationPickPopupOpensWithoutPolishLoop`（message handler 计数零 loop 行+弹层有候选行；反证：注回旧写法事件循环刷死、ctest TIMEOUT 红）。
- **⑥ 账与验证**：ctest **36/36 全绿**（新 4 例：map config 回退/env 遮蔽 2 例 + 0车直达确认页/充电中真点拦截 2 例；⑦修复后追加第 5 例循环钉，36 套仍全绿 37 例）；改动 8 文件 qmllint 零 Error；截图 4 张：`21-station-tags-funnel.png`（标签行+顶栏漏斗+黄星一张全含）、`22-confirm-optional-vehicle.png`（"（不绑定车辆）"默认可提交态）、`23-nav-dual-input.png`（双输入+更换+点击导航+190 降级 Toast 实证）、`24-filter-price-range.png`（区间回填 1.00~1.40）。**桌面实机链路（同日⑦轮）**：charging-server --demo-seed 起 9527 + preview 直跑，弹窗候选 6 行渲染截图核验通过。**活体探测在案：两 env key 当日均 `status 190 无效的key`**（config 兜底路真发请求拿到 190→全链诚实降级为模拟路线，UI 与请求链已验证正确）——真地图数据待控制台开通/换发有效 key 后自动激活，QML 零改动。**环境口径增补**：本机 qml6-module-* 运行包缺失（apt 漂移），QML 测试与 preview 需 `QML2_IMPORT_PATH` 指向解包目录（本仓零改动的仓外 workaround：`/home/bit/.qt-qml-local` dpkg -x）；`admin_login` 补 `QT_QPA_PLATFORM=offscreen` ENVIRONMENT（无 DISPLAY 机器裸跑 xcb abort，预存缺口本轮修入）。

## merge 对账轮（2026-09-08 晚：origin/develop 7f0e38b 并入 8c7eacd + 全绿批 671ebe5）

- **merge 逐块对账（12 冲突文件，`git show :2:` 逐 hunk 审计，UU 清零，仓内标记全扫净）**：
  `sendRequest` 定型为刻意 hybrid——查询串保留本方手工 `QUrl::toPercentEncoding(keep=",-.:;|~*")`
  （静态图测试锚 `paths=6,0x00B578,255:22.5…` 要求逐字节 `:`，上游 QUrlQuery 必编 `%3A` 破锚；
  逗号两式皆活），sig 路径采上游口径 `QUrl(endpointBase).path()+path`（官方完整 URI path 规则）。
  `Kind` 枚举取并集（上游 WalkingRoute/ForwardGeocoder + 本方 IpLocation/GeocodeAddress/StaticMap）。
  Shell/TopNavBar/FilterDialog 采纳上游登录闸/动态标题/真模式组并逐处挂 typeof 守卫与注释；
  接受两处舍弃：详情 loadDemo bridge-absent 桩让位上游 `catch→detailFailed`（home 演示列表与 mock
  提交通道俱在）、上游 origin 面板让位本方 hero 统计带。
- **预约双闸序**：本方 `chargingBusyPrompt`（指令①"有充电中不可约"，UI 态可注入）先跑 →
  上游 TCP 前置闸 `checkUnfinished`（getOrders×3 并发、fail-closed：任一失败只 Toast 不放行）后跑，
  语义叠加不冲突；`navigate("reservation_confirm")` 在 QmlApp 层统一拦截，页面侧无感。
- **mock 通道迁移**：上游把 mock 判定移到进程 env `CHARGING_CHANNEL`（QML context 属性到不了 C++
  构造期）——preview 演示真图走默认 TCP→9527，要 mock 须显式带 env；测试 bootShell 补 `qputenv`。
  mock 种子活动单×上游 TCP 闸死锁以命名测试缝解开：`MockRequestTransport::cancelActiveOrders()` +
  `QmlApp::clearUnfinishedOrdersForTesting()`（dynamic_cast——IRequestTransport 无 QObject 血统），
  spin 450→900ms 对齐 `kMockLatencyMs=450`。
- **WebEngineView 惰性化**：裸建 WebEngineView 在无 `QtWebEngineQuick::initialize()` 的测试进程
  段错误 → NavigationPage 改 `Loader{active:webRouteReady}`+mapBridge 缺位四 accessor 守卫；
  导航页 WebEngine 消费 JS key（`TENCENT_MAP_JS_KEY` 运行时 env，**永不入库**——git 配置文件只含
  WebServiceAPI key，两把 key 用途/配额独立）。
- **QPA/env 钉**：delivery_admin_pages（上游 Qt Charts 建 QChartView 真 QPA 面）/station_query_service/
  reservation_service 补 `QT_QPA_PLATFORM=offscreen`；qml_tcp_delivery（写于 key 入库前）补空 env
  密钥隔离防真实外网；test_qml_station_interactions 随上游 app_bridge 构造补链 map_bridge.cpp；
  构建机需 `libqt6charts6-dev`（CMake CONFIG 包，运行时 .so 不算数）。
- **上游自暴露竞态根除（671ebe5，含 server/database 一行面）**：上游 `concurrentDatabaseConnections`
  要求同库双写者皆 None，本 VM 稳定撞 Database 错。探针定罪不在仓储层 BEGIN IMMEDIATE（schema.sql
  自带 busy_timeout=5000 已覆盖该窗口），在 `DatabaseConnection::open` 的 migrateManagedIndexes——
  Qt `transaction()` 发 deferred BEGIN，读 sqlite_master 后 DROP/CREATE 属**读→写锁升级**，SQLite
  对 pending-upgrade BUSY 按防死锁规则**不触发 busy handler**（timeout 救不了）。改显式 BEGIN
  IMMEDIATE（COMMIT/ROLLBACK 直发，绕开驱动 transact 状态机）+ `QSQLITE_BUSY_TIMEOUT=5000`
  connect option 提前到 open 时刻（覆盖 schema.sql 首段 journal_mode=WAL 先于脚本内 PRAGMA 的
  拿锁窗口）。修复前单跑 ~1/4 挂、修复后 **30/30 绿**。
- **活体现状改口**：控制台开通 WebServiceAPI 后复核——ip 定位 `status 0 Success`、静态图回真 PNG、
  驾车路线 `status 0`，**真链路已激活**（本 ledger 上条"当日均 190"为首轮在案记录，保留不改写；
  降级链彼时已实证正确）。
- **账**：全量串行 ctest **41/41 全绿**（QML 三套需 `QML2_IMPORT_PATH` 指向 rootless overlay，
  本 VM 环境专属；标准 apt 全量机系统路径自带）；改动 **12 个 QML 文件 qmllint 零 Error**。

## 桥缺口（今晚补桥的形状建议，成员2→成员3）

| 服务 | 需要的桥方法/信号（名字=C++ 原名，载荷改 map/list） |
|---|---|
| StationQueryService | `search(keyword)`；`queryStarted/querySucceeded(stations[])/queryFailed(msg)`；**新增** `fetchDetailById(int stationId, int distanceMeters)`（替代 struct 参数版）+ `detailStarted/detailSucceeded(detail)/detailFailed`；list map 字段=StationListItem 拍平（含 station 的 id/name/address/priceCentsPerKwh/status/totalChargers/availableChargers + distanceMeters/operatorName/accessType/parkingFee/features/chargerTypes/hasVoltageBelow700/hasVoltageAtLeast700），枚举串小写 |
| AuthService | `login(phone)` / `logout()`；`loginSucceeded(userMap, createdBool)/loginFailed(msg)`（或页面只依赖 App.loginStateChanged + currentUser；mock 通道 App.login/logout 已可用，页面已接兜底） |
| ReservationService | `fetchList/cancel(id)/expireReservation(id)/submit(map)`；`listStarted/listSucceeded(records)/listFailed/submitStarted(chargerId)/submitSucceeded(record)/submitFailed/cancelStarted/cancelSucceeded/cancelExpired(…)`；record map=ReservationRecord 拍平；`recommendSlotFromTravelMinutes` 以 `Q_INVOKABLE` 暴露；**新增** `activeReservationCount()` / `activeCountForVehicle(qint64)`（详情页预约三重准入的名额判，缺位返回 -1 放行） |
| SettingsService | 车辆 `vehicles()`（Q_INVOKABLE，map 列表）/`addVehicle(Vehicle)/updateVehicle(Vehicle)/removeVehicle(qint64)/setDefaultVehicle(qint64)/defaultVehicle()` + `vehiclesChanged`；二级密码四件 `hasProtectionPassword()/setProtectionPassword(plain)/verifyProtectionPassword(pw)/clearProtectionPassword()` + 开关 `protectionEnabled()/setProtectionEnabled(bool)` + `protectionStateChanged`（设置页与详情页调用点已按此原名接线，桥一落地即自动翻正）；通知开关按位 `notificationEnabled(key)` + 对应 setter + `settingsChanged` |
| CouponService（**全新服务，CONTRACT.md 尚无**） | `coupons()` invokable（map 列表：id/title/kind(cash|discount)/valueCents/discountTenths/thresholdCents/condition/expiresAtUtc/status(available|used|expired)/source）+ `redeem(id)`；`couponsChanged`。缺位期 CouponPage 以页内演示数据渲染三态并标"演示数据"，接入后自动替换 |
| FavoritesService | **已交付**（成员3 service_bridges.cpp）：`contains(qint64)/toggle(qint64)→bool/favoriteIds()` 全 `Q_INVOKABLE`；`favoritesChanged`。导航页/找站页消费面已翻正 |
| NotificationService | **已交付**（成员3 service_bridges.cpp）：`notifications()` invokable（新→旧 map 列表）；`notificationsChanged` |
| MapGeoService | 上下文属性=裸 `MapGeoService*`（非 bridge 类）。成员2 于本类直接扩 **QML 面**（`Q_INVOKABLE` 方法族 + `qml*` QVariant 形转发信号，widgets 消费方仍走原 struct 信号，两不遮蔽）：`requestDrivingRoute(4×double)`、`requestReverseGeocodeLatLng(2×double)`、`requestIpLocation()`、`requestAddressGeocode(address)`、`requestStaticMap(center,zoom,w,h,routePairs,markerPairs)`、`navigationUriUrl(from,to)→QString`（含 referer=key，调用方勿打印）、`setUserLocationLatLng`、`userLocationMap()`、`usable()`。转发信号：`qmlRouteReady(id,map)`/`qmlRouteError`、`qmlIpLocationReady/Error`、`qmlGeocodeReady/Error`、`qmlStaticMapReady(id,filePath)/Error`。静态图成功落 TempLocation（轮换删上一张）。query 值统一 `QUrl::toPercentEncoding(keep=",-.:;|~*")`——坐标串逐字节不变、中文/markers 正确编码；sig 仍对编码前原文计算。无 key 一律异步 `qml*Error(NoApiKey)` 且零网络触达。单测 `test_map_geo_service` 23 例全绿（假 HTTP+QTemporaryDir 配置文件两例，永不触真实网络）；key 解析 2026-09-08 起三级：env 任一已定义（含空=权威无 key）→env 链；皆未定义→`client/config/map_services.json`（演示 key 已入库，用户拍板；baseUrl 同源、尾斜杠 trim） |
| Shell（非桥，路由表） | station 域 10 条路由已由成员3 翻位落地（develop `0f65111`：migrated+10、pageSource+7）；**coupon 两行翻位 + 顶栏漏斗接线已由成员2 代修落地**（`migrated` +"coupon"、`pageSource` +"pages/station/CouponPage.qml"、`onFilterRequested`=`stack.currentItem.openAdvancedFilter && …()` 空桩守卫调用——找站/收藏两页均已暴露该方法）。`searchVisible` 绑当前页 route、`onSearchSubmitted→App.navigate("station", keyword)`、TopNavBar 返回钮容器尺寸三处=用户点名代修（各带根因注释），PR#41 变基后仍在位（已核） |

## 验收自查（19:00 冲刺门）
- [x] `--view=station`：地图示意+筛选栏渲染；列表区为设计内降级态"站点加载失败·站点查询桥未就绪（等待服务桥今晚补全）"
- [x] `--view=login` 表单+校验渲染通过；`--view=profile` 余额卡（¥100.00 演示户）+五入口全出
- [x] detail（arg 头卡即出+桩区失败态重试）/ confirm（A-07/直流快充 120kW/23:45—00:30 推荐+无车辆引导）/ module（Tab+列表失败态）/ navigation（真 arg：模拟路线折线+2.4km+建议出发时刻+降级 Toast）——桥未补前均按口径渲染降级态，10 路由截图零运行期报错
- [x] 截图通道：仓外克隆 `/tmp/smoke-src` 打 Shell 翻位+`--arg=` 补丁（见 §截图环境），仓库 Shell.qml 未动
- [x] 每页根 objectName == 上表锚点
- [x] 修正批 `55ef843`：onClickFunction、Row polish 环、IntValidator 溢出、hhmm 分钟位、余额卡隐高、null-record 绑定、QQuickPopup 作用域、详情头卡 arg 即出、桥缺位显式降级
- [x] 审计轮修正批：登录/退出在 mock（authService=nullptr）下兜底 `App.login()/logout()`（不再卡遮罩）；收藏页投影补 voltageBands 组；导航页存 `route.steps`（真实转向指引 + >15 段截断文案）；地图 hit-test 与 onPaint 界域同构（markers∪route、NaN 坐标天然跳）；卡内 anchors.fill 告警 9 处清理（ClickableCard/P.Card 同款：内容 default-property 进内部 Column，卡内锚点被忽略且逐实例告警）——ClickableCard 5 处（profile×2 实爆 + favorites/station/completed×3 潜伏）+ P.Card 4 处（notification delegate×1 + 订单页三栏卡×3，均桥落地/记录到达即爆）；改 Column 契约内 width 绑定、卡高交内容自然高，10 路由复跑零告警零报错
- [x] 二轮修复批（98c9b6b→e4fd0b9）：二级密码倒转到登录环节/四页 Flickable/顶栏搜索·铃铛·返回钮修复/三页演示数据通道/订单页 polish 环/NoWrap 裁字族——11 路由 rc=0 零告警 + 密码断言 10/10 + qmllint 零 Error，详见 §二轮修复批
- [x] **变基轮（rebase 至 develop `0fcc44d` 后复测）**：成员3 服务桥（`3db8931`）与路由翻位（`0f65111`）已在基底落地 → station/reservation_module 直渲**真数据**（上条"桥未就绪"降级态自然退位，演示通道 catch 不再触发）；订单三栏=成员3 塌陷修正（colW+自然高）外套本分支整页 Flickable；排序 chip `selected` 机制与"综合=2/空闲=0/距离=1"编号以 develop 为准并入。复跑 11 路由 offscreen rc=0 零报错、qmllint 零 Error；解冲突口径=样式/机制按成员3、widgets 对账锚点 objectName 按本分支（`detailPriceLabel/detailDistanceLabel/reservationOrderTabButton/uiProfileHeroButton` 等实名保留）

## 截图环境（发给成员3 的翻位清单=克隆内已验证补丁）
QML 自 `CHARGING_QML_SOURCE_DIR` 文件系统加载（改 .qml 无需重编）；运行
`QML2_IMPORT_PATH=<解包 qml6-modules> QT_QPA_PLATFORM=offscreen charging-qml-preview --view=X [--arg=JSON] --screenshot=…`。
解包 qml6-modules 实机路径=`/home/bit/qt6qml/usr/lib/x86_64-linux-gnu/qt6/qml`（apt 未装 qml6-module-*，缺该环境变量时
所有 preview 运行报 "module QtQuick.Window is not installed"——排查一次记一次）。
**变基轮起**：develop 基底已含路由翻位与 `--arg=`/`--logged-in` 原生支持（成员3 `0f65111`+`d9b8c11`），
10 路由可**仓库直构直跑、零补丁**；仅 coupon 一条仍需克隆翻位（Shell 两行：migrated +1、pageSource +1）。
截图产物：`~/qml-station-shots/01..10-*.png`；设计/修复批新增 `11-home-redesign-list / 12-nav-nostream-mock / 13-profile-entries`。

