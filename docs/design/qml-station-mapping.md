# QML station 域迁移 · 状态/信号→绑定映射草稿

> 基线：develop `0ab34da`（PR #33 已合入，QML 栈在 `client/qml/`）。
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
`station_detail→StationDetailPage.qml`、`reservation_confirm→ReservationConfirmPage.qml`、`reservation_module→ReservationModulePage.qml`、`navigation→NavigationPage.qml`、`favorites→FavoritesPage.qml`、`notifications→NotificationPage.qml`、`settings→SettingsPage.qml`，并把这 7 条 + station/login 翻进 `migrated`。

### LoginPage.qml（"loginPage"）
| widgets | QML |
|---|---|
| QLineEdit 校验器 `1[0-9]{0,10}`/11 位 | `TextField { maxLength: 11; acceptableInput: /^1[0-9]*$/.test(text) }` |
| 点登录 → `authService_->login(phone)` | `authService.login(phone)`（契约名） |
| handleLoginStarted：遮罩+按钮"登录中…" | `Connections onLoginStarted` → `overlay.running=true; busy=true`（桥若无此信号则本地置位，TODO(contract)） |
| handleLoginSucceeded(user, created) | `onLoginSucceeded(user, created)` → 成功文案（含"（已自动注册）"）；壳收 loginStateChanged 自动翻页 |
| handleLoginFailed(message) | `onLoginFailed(message)` → resultLabel 红字 |
| resetState() | `function resetState()`：清 busy/文案回"请输入11位手机号" |

注：AuthService 是裸服务（非桥），`login()` 非 slot——调用即失败，属预期（今晚补桥；桥方法名 `login` 不改）。

### StationHomePage.qml（"stationHomePage"）
| widgets | QML |
|---|---|
| 构造即 `search()`；QSignalBlocker 防触发循环 | `Component.onCompleted: stationQueryService.search(keyword)`；投影纯函数无循环 |
| queryStarted/Succeeded/Failed | `Connections` 三分支 → `loading/loaded/failed` + `setRefreshing(false)` |
| 结果缓存 lastResults_ → 本地投影（排序/电价/筛选三源） | `property var raw: []`；`function project()`：priceMax→`priceCentsPerKwh<=priceMax`，sort=`availableChargers desc`/`distanceMeters asc`（-1 排后），criteria 组内 OR 组间 AND |
| StationFilterCriteria 8 组 | `property var criteria`（JS 对象同字段名：maxDistanceKm/statuses/operators/accessTypes/parkingFees/features/chargerTypes/voltageBands），与 applyStationFilter 同语义 |
| 卡片：名称/电价/空闲/距离/☆ | delegate `ClickableCard` + `App.navigate("station_detail", {id,name,…})`；星星 `MouseArea` eat 事件 → `favoritesService.toggle(id)`；`text: favoritesService.contains(id) ? "★" : "☆"`（`favoritesChanged()` 重算绑定） |
| 空态「重置」不误清关键词（缺陷2 口径） | `hasActiveFilters()`：criteria 非空或 priceMax>0；重置只回退这两源 |
| 搜索框在壳顶栏 | `arg` 作为关键词入口（route 参数），TODO(contract)：Shell 接线 onSearchSubmitted→navigate("station", kw) |
| 地图分栏（WebEngine） | `StationMapItem.qml` Canvas 示意（降级态可视化 + 选卡联动高亮）；真图=明天 QtWebEngineQuick 决策 |
| PullToRefresh | `P.PullToRefreshArea` 包 ListView，`onRefreshRequested: search(keyword)` |

### StationDetailPage.qml（"stationDetailPage"）
| widgets | QML |
|---|---|
| 入参 station+distance → `fetchDetail(station, distanceMeters)` | 结构化参数过不了桥 → 桥需 `fetchDetailById(int stationId, int distanceMeters)`，见 §桥缺口；页面 `arg` 带 {id,name,distanceMeters} |
| detailStarted/Succeeded/Failed | Connections → viewState 门（含"站点正常但无桩"= chargers 空 + hasChargerData） |
| 离线横幅 | `status.toLowerCase()!=="active"` → 红条（`warningSoft` 底） |
| 故障桩红卡（原属性选择器） | delegate `Card { border.color: st==="fault" ? Style.danger : Style.line; border.width: st==="fault" ? 2 : 1 }` |
| 桩状态彩签 | `StatusTag { tone: {available:"success",reserved:"warning",charging:"info",fault:"danger",offline:"neutral"}[st] ?? "neutral" }` |
| 预约按钮三重准入 + reservationBlocked | `App.loggedIn` 判 + 车辆数/在途预约判（依赖桥，未补前 `App.showToast("…","warning")` 兜底）→ `App.navigate("reservation_confirm", arg)` |

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

## P1 五页

### StationFilterDialog.qml（"stationFilterDialog"）
`Popup { modal: false }`；8 组 = Column{Repeater{ActionButton variant:"chip"}}；距离单选（再点取消）、其余多选 toggle；「重置」只清勾选、「确定」`applied(criteria)`→父页 project()。旧组选项字面量（5/10/30/50 km、营业中等）从 widgets 常量 JS 化，TODO(contract)：改由服务层暴露选项。

### NavigationPage.qml（"navigationPage"）
arg=record map；进页先模拟口径（distance/eta 兜底公式），`mapGeoService.requestDrivingRoute({lat,lng},{lat,lng})`（桥：参数改 double×4 或 map）→ `onRouteSucceeded(route)` 原地替换 + caption"真实导航路线·腾讯地图"；`onRouteFailed` Toast+保持模拟；`requestDistanceMatrix`/逆地理同桥形状。`StationMapItem` 画 route polyline；代际号 `property int gen` 保留（QML 也要防过期回调）。

### StationMapItem.qml（组件，非页）
Canvas：`property var markers`（{lat,lng,label,selected}）、`property var route`（[[lat,lng],…]）；自动 fit 边界 + 10px 内边距；示意底（网格+对角线河）纯装饰；`onPaint` 里画点/折线；无 WebEngine 依赖、offscreen 可截图。**决策记录**：任务书建议 QQuickPaintedItem C++ 包壳——但类型注册要动 `client/qml/main.cpp`（成员3 禁区），CMake 挂载点只透 .qml；故 Sprint 用 Canvas 等价实现，明天真地图走 QtWebEngineQuick `WebEngineView`（独立进程无 Widgets 互斥问题）时一并定夺。

### ReservationModulePage.qml（"reservationModulePage"）
二级 Tab 两 ActionButton（variant 切换 chip/primary）；`reservationService.fetchList()` → `onListStarted/Succeeded(records)/Failed`：按 status.toLowerCase() 分发 active(=="reserved")/done(其余)；取消 `onCancelSucceeded` → 切 Tab + 重拉；`reservationExpired` 信号（桥名以 C++ 为准 TODO(contract)）→ 重拉。

### ReservationOrderPage.qml（"reservationOrderPage"）
三栏 Row：左距离 Column、中信息+倒计时、右电量占位。`Timer` 每秒 `remainingSeconds--`；颜色 `{>`}三档绑定 Style.brand/warning/danger；归零→`reservationService.expireReservation(id)`+`fetchList()`。取消按钮 ActionBar variant:"danger"。

### ReservationCompletedPage.qml（"reservationCompletedPage"）
ListView delegate ClickableCard（站点/桩号/时长/费用/状态 StatusTag）；点击→`Dialog` 详情全字段；空态 NoticePanel。

## P2 三页 + ProfilePage

- **SettingsPage.qml**（"settingsPage"）：三模块 Column；密码对话框（Popup 两输入+长度校验，哈希存服务，UI 不落任何明文/哈希值）；车辆 CRUD Popup 列表（vehicles 桥）；通知三开关 `Switch` ↔ `settingsService`（桥）。
- **FavoritesPage.qml**（"favoritesPage"）：同构 StationHome 列表源=favoritesService.favoriteIds()∩stationQueryService 结果；状态门三 flag 直译（queryLoaded/queryFailed/viewState）；星星可取消；筛选弹窗复用。
- **NotificationPage.qml**（"notificationPage"）：`notificationService.notifications()` 桥→ListView；`onNotificationsChanged` 重算；空态引导。
- **ProfilePage.qml**（"profilePage"，Shell 路由表指向本目录）：渐变头卡（余额=App.currentUser）+ 入口列表（wallet/order/charging/settings/favorites）。

## 桥缺口（今晚补桥的形状建议，成员2→成员3）

| 服务 | 需要的桥方法/信号（名字=C++ 原名，载荷改 map/list） |
|---|---|
| StationQueryService | `search(keyword)`；`queryStarted/querySucceeded(stations[])/queryFailed(msg)`；**新增** `fetchDetailById(int stationId, int distanceMeters)`（替代 struct 参数版）+ `detailStarted/detailSucceeded(detail)/detailFailed`；list map 字段=StationListItem 拍平（含 station 的 id/name/address/priceCentsPerKwh/status/totalChargers/availableChargers + distanceMeters/operatorName/accessType/parkingFee/features/chargerTypes/hasVoltageBelow700/hasVoltageAtLeast700），枚举串小写 |
| AuthService | `login(phone)`；`loginSucceeded(userMap, createdBool)/loginFailed(msg)`（或页面只依赖 App.loginStateChanged + currentUser） |
| ReservationService | `fetchList/cancel(id)/expireReservation(id)/submit(map)`；`listStarted/listSucceeded(records)/listFailed/submitStarted(chargerId)/submitSucceeded(record)/submitFailed/cancelStarted/cancelSucceeded/cancelExpired(…)`；record map=ReservationRecord 拍平；`recommendSlotFromTravelMinutes` 以 `Q_INVOKABLE` 暴露 |
| SettingsService | `vehicles()`（Q_INVOKABLE，map 列表）/`addVehicle/updateVehicle/removeVehicle/setDefaultVehicle`；三通知开关 getter/setter + `settingsChanged` |
| FavoritesService | `contains(id)/toggle(id)/favoriteIds()/favoriteCount()` 全部 `Q_INVOKABLE`；`favoritesChanged` |
| NotificationService | `notifications()` invokable（新→旧 map 列表）；`notificationsChanged` |
| MapGeoService | `requestDrivingRoute(fromLat,fromLng,toLat,toLng)` / `requestDistanceMatrix(list)` / `requestGeocode(lat,lng)`；`routeSucceeded(routeMap)/routeFailed(err,msg)`、`distanceMatrixSucceeded(elements)/distanceMatrixFailed`、`geocodeSucceeded(address)/geocodeFailed` |
| Shell（非桥，路由表） | 追加 7 条路由 + migrated 翻位（见 HomeShell 节）；`onSearchSubmitted` 改 `App.navigate("station", keyword)` |

## 验收自查（19:00 冲刺门）
- [ ] `charging-qml-preview --view=station` 出列表+星星+地图示意
- [ ] `--view=login` 表单+校验；`--view=profile` 入口列表
- [ ] detail/confirm/module/navigation：路由表落地后可截（桥未补前渲染"加载失败"态也算联通证明）
- [ ] 旧 ctest 34 套保持全绿（我没碰任何旧文件——git status 佐证）
- [ ] 每页根 objectName == 上表锚点
