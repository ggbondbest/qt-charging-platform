import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets StationHomePage (objectName "stationHomePage" kept).
// 自上而下：手动地址定位、真实站点坐标示意、筛选操作栏
// （排序 chips + 电价下拉 + 高级筛选入口）→ 站点卡片 ListView（星星收藏）。
// 三源投影（关键词/电价/8 组条件）在 QML 侧复现 applyStationFilter 语义；
// 正式运行使用 TCP 服务数据；地址编码失败时保留明确错误，不生成演示站点。
Item {
    id: page
    objectName: "stationHomePage"
    property string route: "station"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    // ---- 页面状态（= widgets 成员变量） ----
    property string keyword: typeof arg === "string" ? arg : ""
    property var priceTiers: [-1, 100, 120, 150]
    property int priceMax: -1
    property int sortMode: 1            // 默认按当前起点的直线距离由近到远
    property var criteria: ({ maxDistanceKm: 0, statuses: [], operators: [],
                              accessTypes: [], parkingFees: [], features: [],
                              chargerTypes: [], voltageBands: [] })
    property var raw: []
    property bool loading: false
    property bool loaded: false         // 状态门（缺陷4 模式）：未落定不进"空"
    property bool failed: false
    property int selectedMarker: -1

    function money(cents) { return (cents / 100).toFixed(2) }
    function originQuery() {
        const selected = originRegion.editText.trim()
        const region = selected === "选择地区 / 输入完整地址" ? "" : selected
        const address = originAddress.text.trim()
        return !region || address.indexOf(region) === 0 ? address : (region + " " + address).trim()
    }
    function distText(m) { return (m === undefined || m < 0) ? "--" : (m / 1000).toFixed(1) + "km" }
    function isFav(id) { // 桥未补时容错为未收藏
        try { return favoritesService ? favoritesService.contains(id) : false }
        catch (e) { return false }
    }
    function viewState() {
        if (!loaded) return failed ? "error" : "loading"
        return stationModel.count > 0 ? "list" : "empty"
    }

    function refresh() {
        if (!stationQueryService) { failQuery("站点服务未连接，请重新登录"); return }
        loading = true; failed = false
        try { stationQueryService.search(keyword) }
        catch (e) { failQuery("无法提交站点查询，请检查服务连接") }
    }
    function failQuery(message) {
        loading = false; loaded = false; failed = true; failMessage = message
        raw = []; stationModel.clear()
    }
    property string failMessage: ""
    property bool demo: false

    // ---- 三源投影（与 StationQueryService.applyStationFilter 同语义，
    //      距离/电价/排序为纯客户端投影，不重发请求） ----
    function project() {
        const c = page.criteria
        const rows = []
        for (const source of (page.raw || [])) {
            const s = Object.assign({}, source)
            // Only real station coordinates and the explicitly selected origin
            // participate. Missing coordinates must not become a 0,0 location.
            s.distanceMeters = (typeof mapBridge !== "undefined" && mapBridge.hasLocation
                                && typeof s.latitude === "number" && typeof s.longitude === "number")
                ? mapBridge.distanceMeters(s.latitude, s.longitude) : -1
            if (page.priceMax > 0 && s.priceCentsPerKwh > page.priceMax) continue
            if (c.maxDistanceKm > 0 && !(s.distanceMeters >= 0
                                         && s.distanceMeters <= c.maxDistanceKm * 1000)) continue
            if (c.statuses.length > 0) {
                const zh = String(s.status).toLowerCase() === "active" ? "营业中" : "暂停运营"
                if (c.statuses.indexOf(zh) < 0) continue
            }
            if (c.operators.length > 0 && c.operators.indexOf(s.operatorName) < 0) continue
            if (c.accessTypes.length > 0 && c.accessTypes.indexOf(s.accessType) < 0) continue
            if (c.parkingFees.length > 0 && c.parkingFees.indexOf(s.parkingFee) < 0) continue
            if (c.features.length > 0 && !(s.features || []).some(f => c.features.indexOf(f) >= 0)) continue
            if (c.chargerTypes.length > 0
                && !(s.chargerTypes || []).some(t => c.chargerTypes.indexOf(t) >= 0)) continue
            if (c.voltageBands.length > 0) {
                const ok = (c.voltageBands.indexOf("低于700V") >= 0 && s.hasVoltageBelow700)
                           || (c.voltageBands.indexOf("700V及以上") >= 0 && s.hasVoltageAtLeast700)
                if (!ok) continue
            }
            rows.push(s)
        }
        if (page.sortMode !== 2)   // 综合=保持服务端返回顺序（widgets 同语义）
            rows.sort(page.sortMode === 1
                      ? (a, b) => {
                            const da = a.distanceMeters < 0 ? 1e12 : a.distanceMeters
                            const db = b.distanceMeters < 0 ? 1e12 : b.distanceMeters
                            return da - db
                        }
                      : (a, b) => b.availableChargers - a.availableChargers)
        stationModel.clear()
        for (const s of rows) {
            stationModel.append({
                stationId: String(s.id), name: s.name, address: s.address,
                priceCentsPerKwh: s.priceCentsPerKwh, availableChargers: s.availableChargers,
                totalChargers: s.totalChargers, distanceMeters: s.distanceMeters,
                status: String(s.status).toLowerCase(),
                // 桥平铺口径未定（TODO(contract)）：station.latitude 或拍平 latitude 双形状兜底
                lat: s.latitude !== undefined ? s.latitude
                   : (s.station && s.station.latitude !== undefined ? s.station.latitude : NaN),
                lng: s.longitude !== undefined ? s.longitude
                   : (s.station && s.station.longitude !== undefined ? s.station.longitude : NaN),
                operatorName: s.operatorName || "", features: (s.features || []).join("·")
            })
        }
    }
    function anyFilterActive() {
        const c = page.criteria
        return page.priceMax > 0 || c.maxDistanceKm > 0 || c.statuses.length > 0
               || c.operators.length > 0 || c.accessTypes.length > 0
               || c.parkingFees.length > 0 || c.features.length > 0
               || c.chargerTypes.length > 0 || c.voltageBands.length > 0
    }
    function resetFilters() {          // 缺陷2 口径：只回退筛选两源，不误清关键词
        page.priceMax = -1
        priceCombo.currentIndex = 0
        page.criteria = ({ maxDistanceKm: 0, statuses: [], operators: [], accessTypes: [],
                           parkingFees: [], features: [], chargerTypes: [], voltageBands: [] })
        if (!anyFilterActive()) { clearKeywordAndSearch(); return }
        project()
    }
    function clearKeywordAndSearch() { page.keyword = ""; refresh() }
    // 供壳顶栏漏斗接线（Shell.qml onFilterRequested 空桩注释点名 member 2 域）：
    // 成员3 一行接通 `stack.currentItem.openAdvancedFilter && stack.currentItem.openAdvancedFilter()` 即活。
    function openAdvancedFilter() { filterDialog.openDialog(page.criteria) }

    Connections {
        target: stationQueryService
        function onQueryStarted() { page.loading = true; page.failed = false }
        function onQuerySucceeded(stations) {
            page.loading = false; page.loaded = true; page.failed = false
            page.demo = false                      // 真数据到位，演示通道退位
            page.raw = stations || []
            page.project()
            pull.setRefreshing(false)
        }
        function onQueryFailed(message) {
            page.failQuery(message)
            pull.setRefreshing(false)
            if (App) App.showToast("站点查询失败：" + message, "danger")
        }
    }
    Connections {
        target: typeof mapBridge !== "undefined" ? mapBridge : null
        function onLocationChanged() { page.project() }
    }
    Connections {
        target: favoritesService
        function onFavoritesChanged() { page.project() }   // 重算星星绑定
    }
    // 壳顶栏搜索→路由参数（Shell 接线待成员3 改 navigate("station", kw)，
    // 现按 arg 变更响应）。
    onArgChanged: { const k = typeof arg === "string" ? arg : ""; if (k !== keyword) { keyword = k; refresh() } }

    Component.onCompleted: refresh()

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd

        Column {
            width: parent.width
            spacing: P.Style.spaceXs
            ComboBox {
                id: originRegion
                objectName: "originRegionComboBox"
                width: parent.width
                editable: true
                model: ["选择地区 / 输入完整地址", "大连市", "沈阳市", "北京市", "上海市", "深圳市"]
                // Selecting a city only edits the query; it is not GPS and does
                // not change the origin until Tencent geocoding succeeds.
            }
            Row {
                width: parent.width
                spacing: P.Style.spaceSm
                TextField {
                    id: originAddress
                    objectName: "originAddressField"
                    width: parent.width - locateButton.width - parent.spacing
                    placeholderText: "输入详细地址或当前位置完整地址"
                    onAccepted: mapBridge.geocodeAddress(page.originQuery())
                }
                P.ActionButton {
                    id: locateButton
                    objectName: "locateAddressButton"
                    text: "定位"
                    enabled: page.originQuery().length > 0 && !mapBridge.busy
                    onClicked: mapBridge.geocodeAddress(page.originQuery())
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: mapBridge.error.length ? mapBridge.error
                      : mapBridge.busy ? "正在查询腾讯地图…"
                      : mapBridge.hasLocation ? "起点：" + mapBridge.locationLabel + " · 列表显示直线距离"
                      : "请先输入地址定位；未定位时不展示虚构距离"
                color: mapBridge.error.length ? P.Style.danger : P.Style.muted
                font.pixelSize: P.Style.fontSm
            }
        }

        // 真实站点坐标投影；点击“导航”打开腾讯 WebEngine 路线页面。
        StationMapItem {
            objectName: "stationMapPanel"
            width: parent.width
            height: 130
            markers: {
                const out = []
                for (let i = 0; i < stationModel.count; ++i) {
                    const r = stationModel.get(i)
                    out.push({ lat: r.lat, lng: r.lng, label: r.name, selected: i === page.selectedMarker })
                }
                return out
            }
            onMarkerClicked: index => {
                page.selectedMarker = index
                stationList.positionViewAtIndex(index, ListView.Center)
            }
        }

        // 筛选操作栏（顺序对齐 widgets：三个排序 chip → 电价 caption → 组合框；
        // ⛏筛选 是 QML 版弹层入口，widgets 走顶栏 filterRequested，Shell 无此入口故页内补位。
        // Flow 自动换行：420 宽单行放不下六个控件，⛏ 曾被 Row 溢出裁掉）
        Flow {
            objectName: "stationFilterBar"
            width: parent.width
            spacing: P.Style.spaceSm
            P.ActionButton {
                objectName: "sortRecommendedButton"
                variant: "chip"
                selected: page.sortMode === 2
                text: "综合"
                onClicked: { page.sortMode = 2; page.project() }
            }
            P.ActionButton {
                objectName: "sortAvailableButton"
                variant: "chip"
                selected: page.sortMode === 0
                text: "空闲优先"
                onClicked: { page.sortMode = 0; page.project() }
            }
            P.ActionButton {
                objectName: "sortDistanceButton"
                variant: "chip"
                selected: page.sortMode === 1
                text: "距离最近"
                onClicked: { page.sortMode = 1; page.project() }
            }
            Text {
                text: "电价"; font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
            ComboBox {
                id: priceCombo
                objectName: "priceFilterComboBox"
                width: 116
                model: ["全部电价", "≤ ¥1.00", "≤ ¥1.20", "≤ ¥1.50"]
                onActivated: idx => { page.priceMax = page.priceTiers[idx]; page.project() }
            }
            P.ActionButton {
                objectName: "advancedFilterButton"
                variant: "ghost"
                text: "⛏ 筛选"
                onClicked: filterDialog.openDialog(page.criteria)
            }
        }

        // 演示数据标注（同优惠券页口径：不冒充真实查询结果；NoWrap 会溢出裁字，补换行）
        Text {
            objectName: "homeDemoCaption"
            visible: page.demo
            width: parent.width
            wrapMode: Text.WordWrap
            text: "当前为演示数据（站点查询桥未就绪，接入后自动替换）；点卡片可进详情预约"
            font.pixelSize: P.Style.fontSm; color: P.Style.faint
        }

        // 列表四态（Column 内余高：parent.height - y）
        Item {
            id: stationListArea
            objectName: "stationListArea"
            width: parent.width
            height: parent.height - y
            clip: true

            P.PullToRefreshArea {
                id: pull
                objectName: "stationPullToRefresh"
                anchors.fill: parent
                pullEnabled: page.loaded || page.failed
                onRefreshRequested: page.refresh()

                ListView {
                    id: stationList
                    objectName: "stationList"
                    // PullToRefreshArea 内容是 Column：禁垂直/fill 锚（平台会告警且不生效），
                    // 显式尺寸 + x 负偏移等价还原原 -spaceSm 出血。
                    // 显式尺寸（PullToRefreshArea 内容是 Column，禁垂直/fill 锚）；
                    // pull 与列表区同高（anchors.fill），取 pull.height 免再引无 id 容器。
                    width: parent.width + P.Style.spaceSm * 2
                    x: -P.Style.spaceSm
                    height: pull.height
                    spacing: P.Style.spaceSm
                    clip: true
                    visible: viewState() === "list"
                    model: ListModel { id: stationModel }

                    delegate: P.ClickableCard {
                        objectName: "stationCard"
                        width: stationList.width - P.Style.spaceSm * 2
                        onClicked: {
                            page.selectedMarker = index
                            if (App) App.navigate("station_detail", {
                                id: stationId, name: name, address: address,
                                priceCentsPerKwh: priceCentsPerKwh,
                                distanceMeters: distanceMeters, status: status,
                                latitude: lat, longitude: lng })
                        }
                        Row {
                            width: parent.width        // Column 内容器：anchors.fill 被忽略且告警
                            spacing: P.Style.spaceMd
                            Column {
                                width: parent.width - 110
                                spacing: 2
                                Text {
                                    width: parent.width; elide: Text.ElideRight
                                    text: name; font.pixelSize: P.Style.fontLg
                                    font.bold: true; color: P.Style.ink
                                }
                                Text {
                                    width: parent.width; elide: Text.ElideRight
                                    text: address; font.pixelSize: P.Style.fontSm; color: P.Style.muted
                                }
                                Text {
                                    text: "¥" + page.money(priceCentsPerKwh) + "/kWh · 空闲 "
                                          + availableChargers + "/" + totalChargers
                                          + " · 直线 " + page.distText(distanceMeters)
                                          + (status !== "active" ? " · 暂停运营" : "")
                                    font.pixelSize: P.Style.fontSm; color: P.Style.brandDeep
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: App.navigate("navigation", {
                                            stationName: name, stationLatitude: lat,
                                            stationLongitude: lng, hasStationLocation: true })
                                    }
                                }
                            }
                            Item {
                                width: 96
                                height: cardActions.implicitHeight
                                Column {
                                    id: cardActions
                                    anchors.centerIn: parent
                                    spacing: P.Style.spaceXs
                                    P.StatusTag {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        tone: status === "active" ? "success" : "neutral"
                                        text: status === "active" ? "营业中" : "暂停运营"
                                    }
                                    P.ActionButton {
                                        objectName: "stationNavigateButton"
                                        text: "导航"
                                        variant: "ghost"
                                        onClicked: App.navigate("navigation", {
                                            stationName: name, stationLatitude: lat,
                                            stationLongitude: lng, hasStationLocation: true })
                                    }
                                    MouseArea {
                                        objectName: "favoriteStarButton"
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        width: 34
                                        height: 28
                                        Text {
                                            anchors.centerIn: parent
                                            text: page.isFav(stationId) ? "★" : "☆"
                                            font.pixelSize: 20
                                            color: page.isFav(stationId) ? P.Style.warning : P.Style.faint
                                        }
                                        onClicked: {
                                            // toggle 返回操作后状态；桥未补时静默（TODO(contract)）
                                            try { favoritesService.toggle(stationId) } catch (e) {}
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // 错峰入场（契约 §4：40ms × ≤8）
                    add: Transition {
                        NumberAnimation { property: "opacity"; from: 0; to: 1
                            duration: P.Style.motionEnabled ? P.Style.durEnter : 0 }
                    }
                }
            }

            P.NoticePanel {
                objectName: "stationNotice"
                anchors.fill: parent
                visible: viewState() !== "list"
                glyph: viewState() === "loading" ? "⏳" : viewState() === "error" ? "⚠️" : "🔍"
                title: viewState() === "loading" ? "正在加载站点…"
                     : viewState() === "error" ? "站点加载失败" : "没有找到匹配的充电站"
                description: viewState() === "error"
                             ? (failMessage.length > 0 ? failMessage
                               : "请检查网络或服务端通道（CHARGING_CHANNEL）后重试")
                             : anyFilterActive() ? "放宽筛选条件试试" : "换个关键词试试"
                actionText: viewState() === "loading" ? ""
                            : viewState() === "error" ? "重试"
                            : anyFilterActive() ? "重置筛选" : "清除搜索"
                onActionTriggered: {
                    if (viewState() === "error") page.refresh()
                    else if (anyFilterActive()) page.resetFilters()
                    else page.clearKeywordAndSearch()
                }
            }
        }
    }

    // 高级筛选弹窗（P1 同目录交付；onApplied 回条件对象）
    StationFilterDialog { id: filterDialog }
    Connections {
        target: filterDialog
        function onApplied(criteria) {
            page.criteria = criteria
            page.project()
        }
    }
}
