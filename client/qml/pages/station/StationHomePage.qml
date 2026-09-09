import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls as Controls // Overlay is not exported by Basic in Qt 6.2.
import QtQuick.Layouts
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// One scrollable page: explicit origin, real map, filters and station cards.
// Station data comes from the selected transport; errors never load fake rows.
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
    readonly property string originCity: mapBridge.browsingCity
    property string originAddressValue: ""
    property int priceTierIndex: 0

    function money(cents) { return (cents / 100).toFixed(2) }
    function originQuery() {
        const selected = page.originCity.trim()
        const region = selected === "选择地区 / 输入完整地址" ? "" : selected
        const address = page.originAddressValue.trim()
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
        // Load the complete paginated station catalogue; city/keyword are a
        // local projection, so rapid city changes cannot mix stale responses.
        try { stationQueryService.search("") }
        catch (e) { failQuery("无法提交站点查询，请检查服务连接") }
    }
    function failQuery(message) {
        loading = false; loaded = false; failed = true; failMessage = message
        raw = []; stationModel.clear(); mapMarkers = []
    }
    property string failMessage: ""

    // ---- 三源投影（与 StationQueryService.applyStationFilter 同语义，
    //      距离/电价/排序为纯客户端投影，不重发请求） ----
    function project() {
        const c = page.criteria
        const rows = []
        for (const source of (page.raw || [])) {
            const s = Object.assign({}, source)
            if (String(s.address || "").indexOf(page.originCity) !== 0) continue
            const search = page.keyword.trim().toLowerCase()
            if (search.length && (String(s.name) + " " + String(s.address)).toLowerCase().indexOf(search) < 0) continue
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
        const markers = []
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
            if (typeof s.latitude === "number" && typeof s.longitude === "number"
                    && isFinite(s.latitude) && isFinite(s.longitude))
                markers.push({id: String(s.id), lat: s.latitude, lng: s.longitude, label: s.name})
        }
        if (JSON.stringify(mapMarkers) !== JSON.stringify(markers)) mapMarkers = markers
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
        page.priceTierIndex = 0
        page.criteria = ({ maxDistanceKm: 0, statuses: [], operators: [], accessTypes: [],
                           parkingFees: [], features: [], chargerTypes: [], voltageBands: [] })
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
            page.raw = stations || []
            page.project()
        }
        function onQueryFailed(message) {
            page.failQuery(message)
            if (App) App.showToast("站点查询失败：" + message, "danger")
        }
    }
    Connections {
        target: typeof mapBridge !== "undefined" ? mapBridge : null
        function onLocationChanged() { page.project() }
        function onBrowsingCityChanged() {
            page.originAddressValue = ""
            stationPopup.close()
            page.selectedStation = null
            page.resetFilters()
        }
    }
    Connections {
        target: favoritesService
        function onFavoritesChanged() { page.project() }   // 重算星星绑定
    }
    // 壳顶栏搜索→路由参数（Shell 接线待成员3 改 navigate("station", kw)，
    // 现按 arg 变更响应）。
    onArgChanged: { const k = typeof arg === "string" ? arg : ""; if (k !== keyword) { keyword = k; refresh() } }

    Component.onCompleted: refresh()

    property var mapMarkers: []
    property var selectedStation: null
    function stationArg(s) {
        return {id: s.stationId, name: s.name, address: s.address,
                priceCentsPerKwh: s.priceCentsPerKwh, distanceMeters: s.distanceMeters,
                status: s.status, latitude: s.lat, longitude: s.lng}
    }
    function navigateStation(s) {
        if (!s || !isFinite(s.lat) || !isFinite(s.lng)) {
            App.showToast("该电站暂无有效坐标，无法导航", "warning"); return
        }
        App.navigate("navigation", {stationId: s.stationId, stationName: s.name,
                     stationLatitude: s.lat, stationLongitude: s.lng, hasStationLocation: true})
    }
    function selectMapStation(id) {
        // Never trust a URL from web content as a booking payload.
        for (let i = 0; i < stationModel.count; ++i) {
            const s = stationModel.get(i)
            if (s.stationId === id) {
                selectedStation = Object.assign({}, s)
                stationPopup.open(); return
            }
        }
    }
    function locateOrigin() {
        if (mapBridge.busy || !page.originAddressValue.trim().length) return
        mapBridge.geocodeAddress(page.originQuery())
    }

    ListModel { id: stationModel }
    ListView {
        id: stationList
        objectName: "stationList"
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: stationModel
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        header: Column {
            width: stationList.width
            spacing: P.Style.spaceMd
            RowLayout {
                width: parent.width
                Text {
                    Layout.fillWidth: true
                    text: page.originCity + "充电站"; font.pixelSize: P.Style.fontXl
                    font.bold: true; color: P.Style.ink
                }
                P.ActionButton {
                    text: page.loading ? "查询中" : "刷新"; variant: "ghost"
                    enabled: !page.loading; onClicked: page.refresh()
                }
            }
            P.Card {
                width: parent.width
                Column {
                    width: parent.width
                    spacing: P.Style.spaceSm
                    RowLayout {
                        width: parent.width
                        P.ComboBox {
                            id: originRegion
                            objectName: "originRegionComboBox"
                            Layout.preferredWidth: 104
                            model: mapBridge.availableCities
                            currentIndex: mapBridge.availableCities.indexOf(mapBridge.browsingCity)
                            onActivated: function(index) { mapBridge.setBrowsingCity(model[index]) }
                        }
                        P.TextField {
                            id: originAddress
                            objectName: "originAddressField"
                            Layout.fillWidth: true
                            placeholderText: "输入起始地址，例如软件园路"
                            text: page.originAddressValue
                            onTextChanged: page.originAddressValue = text
                            onAccepted: page.locateOrigin()
                        }
                    }
                    RowLayout {
                        width: parent.width
                        Text {
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            text: mapBridge.error.length ? mapBridge.error
                                  : mapBridge.busy ? "正在查询腾讯地图…"
                                  : mapBridge.hasLocation ? "起点：" + mapBridge.locationLabel
                                  : "浏览" + page.originCity + "；输入地址设置红色起点"
                            color: mapBridge.error.length ? P.Style.danger : P.Style.muted
                            font.pixelSize: P.Style.fontSm
                        }
                        P.ActionButton {
                            objectName: "locateAddressButton"; text: "定位"
                            enabled: originAddress.text.trim().length > 0 && !mapBridge.busy
                            onClicked: page.locateOrigin()
                        }
                    }
                }
            }
            StationMapView {
                objectName: "stationMapPanel"
                width: parent.width; height: Math.max(200, Math.min(240, page.height * 0.32))
                html: {
                    const originRevision = mapBridge.locationLabel
                    const cityRevision = mapBridge.browsingCity
                    return mapBridge.mapHtml(page.mapMarkers)
                }
                cityName: page.originCity
                onStationSelected: function(id) { page.selectMapStation(id) }
            }
            Text {
                width: parent.width; wrapMode: Text.Wrap
                text: "红色为起点 · 点击电站标记预约或导航 · 列表距离为直线距离"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
            Flow {
                objectName: "stationFilterBar"
                width: parent.width; spacing: P.Style.spaceSm
                P.ActionButton {
                    objectName: "sortRecommendedButton"; variant: "chip"; text: "综合"
                    selected: page.sortMode === 2
                    onClicked: { page.sortMode = 2; page.project() }
                }
                P.ActionButton {
                    objectName: "sortAvailableButton"; variant: "chip"; text: "空闲优先"
                    selected: page.sortMode === 0
                    onClicked: { page.sortMode = 0; page.project() }
                }
                P.ActionButton {
                    objectName: "sortDistanceButton"; variant: "chip"; text: "距离最近"
                    selected: page.sortMode === 1
                    onClicked: { page.sortMode = 1; page.project() }
                }
                P.ComboBox {
                    id: priceCombo
                    objectName: "priceFilterComboBox"
                    width: 126; model: ["全部电价", "≤ ¥1.00", "≤ ¥1.20", "≤ ¥1.50"]
                    currentIndex: page.priceTierIndex
                    onActivated: function(idx) { page.priceTierIndex = idx; page.priceMax = page.priceTiers[idx]; page.project() }
                }
                P.ActionButton {
                    objectName: "advancedFilterButton"; variant: "ghost"; text: "更多筛选"
                    onClicked: page.openAdvancedFilter()
                }
            }
            Text {
                text: page.originCity + " · 找到 " + stationModel.count + " 座电站"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
            Item { width: 1; height: 2 }
        }
        delegate: P.ClickableCard {
            id: stationCard
            objectName: "stationCard"
            width: stationList.width
            onClicked: App.navigate("station_detail", page.stationArg(model))
            Column {
                width: parent.width
                spacing: P.Style.spaceSm
                RowLayout {
                    width: parent.width
                    Column {
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        spacing: P.Style.spaceXs
                        Text {
                            width: parent.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
                            text: model.name; font.bold: true
                            font.pixelSize: P.Style.fontLg; color: P.Style.ink
                        }
                        Text {
                            width: parent.width; wrapMode: Text.Wrap
                            text: model.address; textFormat: Text.PlainText
                            font.pixelSize: P.Style.fontSm; color: P.Style.muted
                        }
                    }
                    Column {
                        Layout.alignment: Qt.AlignTop
                        spacing: 2
                        Text {
                            objectName: "stationPrice"
                            text: "¥" + page.money(model.priceCentsPerKwh)
                            font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.brandDeep
                        }
                        Text { text: "/kWh"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                    }
                }
                Text {
                    width: parent.width; wrapMode: Text.Wrap
                    text: "空闲 " + model.availableChargers + "/" + model.totalChargers
                          + " · " + (model.distanceMeters < 0 ? "待设置起点" : "直线 " + page.distText(model.distanceMeters))
                    font.pixelSize: P.Style.fontSm; color: P.Style.muted
                }
                RowLayout {
                    width: parent.width; spacing: P.Style.spaceXs
                    P.StatusTag {
                        tone: model.status === "active" ? "success" : "neutral"
                        text: model.status === "active" ? "营业中" : "暂停运营"
                    }
                    Item { Layout.fillWidth: true }
                    P.ActionButton {
                        objectName: "favoriteStarButton"; variant: "ghost"
                        glyph: page.isFav(model.stationId) ? "star-filled" : "star"
                        glyphColor: P.Style.warning
                        text: ""
                        onClicked: favoritesService.toggle(model.stationId)
                    }
                    P.ActionButton {
                        objectName: "stationNavigateButton"; text: "导航"; variant: "ghost"
                        onClicked: page.navigateStation(model)
                    }
                    P.ActionButton {
                        objectName: "stationReserveButton"; text: "预约"
                        enabled: model.status === "active" && model.availableChargers > 0
                        onClicked: App.navigate("station_detail", page.stationArg(model))
                    }
                }
            }
        }
        footer: P.NoticePanel {
            objectName: "stationNotice"
            width: stationList.width
            height: visible ? 220 : 0
            visible: page.viewState() !== "list"
            title: page.viewState() === "loading" ? "正在加载站点…"
                 : page.viewState() === "error" ? "站点加载失败" : "没有找到匹配的充电站"
            description: page.failed ? page.failMessage : "调整关键词或筛选条件后重试"
            actionText: page.viewState() === "loading" ? "" : page.failed ? "重试"
                        : page.anyFilterActive() ? "重置筛选" : "清除搜索"
            onActionTriggered: {
                if (page.failed) page.refresh()
                else if (page.anyFilterActive()) page.resetFilters()
                else page.clearKeywordAndSearch()
            }
        }
    }

    Popup {
        id: stationPopup
        objectName: "mapStationPopup"
        parent: Controls.Overlay.overlay
        width: Math.min(390, parent ? parent.width - 32 : 390)
        x: parent ? (parent.width - width) / 2 : 0
        y: parent ? Math.max(12, (parent.height - height) / 2) : 0
        modal: true; padding: P.Style.spaceLg
        background: Rectangle { color: P.Style.surface; radius: P.Style.radiusLg; border.color: P.Style.line }
        contentItem: Column {
            spacing: P.Style.spaceMd
            Text {
                width: parent.width; wrapMode: Text.Wrap
                text: page.selectedStation ? page.selectedStation.name : ""
                font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink
            }
            Text {
                width: parent.width; wrapMode: Text.Wrap
                text: page.selectedStation ? page.selectedStation.address + "\n¥"
                    + page.money(page.selectedStation.priceCentsPerKwh) + "/kWh · 空闲 "
                    + page.selectedStation.availableChargers + "/" + page.selectedStation.totalChargers : ""
                font.pixelSize: P.Style.fontMd; color: P.Style.muted
            }
            RowLayout {
                width: parent.width
                P.ActionButton {
                    objectName: "mapStationNavigateButton"
                    text: "导航"; variant: "secondary"; Layout.fillWidth: true
                    onClicked: { stationPopup.close(); page.navigateStation(page.selectedStation) }
                }
                P.ActionButton {
                    objectName: "mapStationReserveButton"
                    text: "选桩预约"; Layout.fillWidth: true
                    enabled: page.selectedStation !== null && page.selectedStation.status === "active"
                             && page.selectedStation.availableChargers > 0
                    onClicked: {
                        const selection = page.stationArg(page.selectedStation)
                        stationPopup.close(); App.navigate("station_detail", selection)
                    }
                }
            }
            P.ActionButton {
                text: "关闭"; variant: "ghost"; width: parent.width
                onClicked: stationPopup.close()
            }
        }
    }
    StationFilterDialog { id: filterDialog }
    Connections {
        target: filterDialog
        function onApplied(criteria) { page.criteria = criteria; page.project() }
    }
}
