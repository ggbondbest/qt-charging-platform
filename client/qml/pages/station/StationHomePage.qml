import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets StationHomePage (objectName "stationHomePage" kept).
//
// 2026-09-08 设计轮（用户指定"与其他页面 ui 风格相称 + 按钮布局充满设计感"）：
// ① 出血式 hero 绿渐变带（成员3 #41 语言：heroFrom→heroTo、fontHero、统计大字），
//    关键词/统计一目了然；② map⇄list 分段切换（EV 充电 app 通例）：列表态=紧凑地图
//    +全列表，地图态=大地图+选中站点 peek 浮卡（marker↔卡片双向联动保留）；
// ③ 筛选整合进单行胶囊工具栏：排序三 chip + 电价 + 漏斗（激活筛选计数徽标），
//    替代原 Flow 散排（曾裁掉 ⛏ 的那条）；④ 站点卡：价格大字右挂 + 空闲比例条
//    （可用性色彩），状态/收藏星锚点不变。
// 三源投影（关键词/电价/8 组条件）保持 widgets applyStationFilter 语义；
// 全部 objectName 锚点保留（真点击回归 test_qml_station_interactions 依赖）。
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
    property int sortMode: 2            // 2=综合（服务端顺序，widgets 默认）0=空闲优先 1=距离最近
    property var criteria: ({ maxDistanceKm: 0, statuses: [], operators: [],
                              accessTypes: [], parkingFees: [], features: [],
                              chargerTypes: [], voltageBands: [] })
    property var raw: []
    property bool loading: false
    property bool loaded: false         // 状态门（缺陷4 模式）：未落定不进"空"
    property bool failed: false
    property int selectedMarker: -1
    property string viewMode: "list"    // "list" | "map"（分段切换，默认列表保原口径）

    function money(cents) { return (cents / 100).toFixed(2) }
    function distText(m) { return (m === undefined || m < 0) ? "--" : (m / 1000).toFixed(1) + "km" }
    function isFav(id) { // 桥未补时容错为未收藏
        try { return favoritesService ? favoritesService.contains(id) : false }
        catch (e) { return false }
    }
    function viewState() {
        if (!loaded) return failed ? "error" : "loading"
        return stationModel.count > 0 ? "list" : "empty"
    }
    function activeFilterCount() {   // 漏斗徽标：激活的筛选组数（排序不算）
        const c = page.criteria
        let n = 0
        if (page.priceMax > 0) ++n
        if (c.maxDistanceKm > 0) ++n
        n += (c.statuses.length > 0) + (c.operators.length > 0) + (c.accessTypes.length > 0)
           + (c.parkingFees.length > 0) + (c.features.length > 0)
           + (c.chargerTypes.length > 0) + (c.voltageBands.length > 0)
        return n
    }
    // hero 统计（当前投影结果的实时聚合）
    readonly property int statAvailable: {
        let n = 0
        for (let i = 0; i < stationModel.count; ++i) n += stationModel.get(i).availableChargers
        return viewState() === "list" ? n : 0
    }
    readonly property real statAvgPrice: {
        if (stationModel.count === 0) return 0
        let sum = 0
        for (let i = 0; i < stationModel.count; ++i) sum += stationModel.get(i).priceCentsPerKwh
        return sum / stationModel.count / 100
    }

    function refresh() {
        if (!stationQueryService) { loadDemo(); return }
        loading = true; failed = false
        try { stationQueryService.search(keyword) }   // TODO(contract): 桥补 invokable search
        catch (e) {                                    // 桥缺位：退页内演示数据（标注清楚），
            loadDemo()                                 // 真桥落地后信号即替换，不再走到这
        }
    }
    property string failMessage: ""
    property bool demo: false

    // ---- 演示数据通道（同优惠券页口径：标"演示数据"，不冒充真实查询结果）----
    // 带经纬度点位 → 地图示意自动布点；关键词搜索在演示通道内同样生效。
    function demoStations() {
        const base = [
            { id: 9001, name: "滨海快充站", address: "南山区滨海大道 2012 号",
              priceCentsPerKwh: 128, availableChargers: 6, totalChargers: 12,
              distanceMeters: 2400, status: "active", latitude: 22.5372, longitude: 113.9401,
              operatorName: "国网电动", features: ["雨棚", "卫生间"], chargerTypes: ["fast"],
              parkingFee: "免停车费", accessType: "公共", hasVoltageBelow700: true, hasVoltageAtLeast700: false },
            { id: 9002, name: "科技园慢充站", address: "高新区科苑南路 3188 号",
              priceCentsPerKwh: 98, availableChargers: 4, totalChargers: 8,
              distanceMeters: 1200, status: "active", latitude: 22.5448, longitude: 113.9512,
              operatorName: "特来电", features: ["地下车库"], chargerTypes: ["slow"],
              parkingFee: "首 2 小时免费", accessType: "公共", hasVoltageBelow700: true, hasVoltageAtLeast700: false },
            { id: 9003, name: "深圳湾超充站", address: "东滨路 1008 号",
              priceCentsPerKwh: 145, availableChargers: 2, totalChargers: 6,
              distanceMeters: 3600, status: "active", latitude: 22.5233, longitude: 113.9438,
              operatorName: "华为超充", features: ["雨棚"], chargerTypes: ["fast"],
              parkingFee: "收费", accessType: "公共", hasVoltageBelow700: false, hasVoltageAtLeast700: true },
            { id: 9004, name: "世界之窗充电站", address: "深南大道 9037 号",
              priceCentsPerKwh: 119, availableChargers: 0, totalChargers: 10,
              distanceMeters: 5200, status: "offline", latitude: 22.5391, longitude: 113.9716,
              operatorName: "国网电动", features: [], chargerTypes: ["fast", "slow"],
              parkingFee: "免停车费", accessType: "公共", hasVoltageBelow700: true, hasVoltageAtLeast700: true }
        ]
        const kw = page.keyword.trim()
        return kw.length === 0 ? base
             : base.filter(s => s.name.indexOf(kw) >= 0 || s.address.indexOf(kw) >= 0)
    }
    function loadDemo() {
        demo = true
        raw = demoStations()
        loading = false; loaded = true; failed = false
        project()
        try { pull.setRefreshing(false) } catch (e) {}
    }

    // ---- 三源投影（与 StationQueryService.applyStationFilter 同语义，
    //      距离/电价/排序为纯客户端投影，不重发请求） ----
    function project() {
        const c = page.criteria
        const rows = []
        for (const s of (page.raw || [])) {
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
                stationId: s.id, name: s.name, address: s.address,
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
        // 投影后选中索引越界回退（筛选把选中站滤掉时不打断 peek 显示）
        if (page.selectedMarker >= stationModel.count) page.selectedMarker = -1
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
    // 供壳顶栏漏斗接线（Shell.qml onFilterRequested）：一行接通即活。
    function openAdvancedFilter() { filterDialog.openDialog(page.criteria) }
    function focusStation(index) {     // 列表→marker / marker→列表 双向联动共用
        page.selectedMarker = index
        stationList.positionViewAtIndex(index, ListView.Center)
    }

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
            page.loading = false; page.loaded = false; page.failed = true
            pull.setRefreshing(false)
            if (App) App.showToast("站点查询失败：" + message, "danger")
        }
    }
    Connections {
        target: favoritesService
        function onFavoritesChanged() { page.project() }   // 重算星星绑定
    }
    // 壳顶栏搜索→路由参数（arg 变更响应）。
    onArgChanged: { const k = typeof arg === "string" ? arg : ""; if (k !== keyword) { keyword = k; refresh() } }

    Component.onCompleted: refresh()

    Column {
        id: rootCol
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceSm

        // ---------- ① hero 渐变带（出血到左右缘，同 ProfilePage 口径） ----------
        Rectangle {
            objectName: "stationHeroBand"
            x: -P.Style.spaceLg
            width: rootCol.width + P.Style.spaceLg * 2
            height: 96
            radius: 0
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: P.Style.heroFrom }
                GradientStop { position: 1.0; color: P.Style.heroTo }
            }
            // 右缘大闪电低透明装饰（只此一处的点缀）
            Text {
                anchors.right: parent.right; anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: "⚡"; font.pixelSize: 64; opacity: 0.16
                color: P.Style.surface
            }
            Column {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: P.Style.spaceLg + 4
                anchors.rightMargin: 80
                spacing: 6
                Text {
                    objectName: "stationHeroTitle"
                    text: page.keyword.length > 0 ? "搜索：" + page.keyword : "附近充电站"
                    width: parent.width; elide: Text.ElideRight
                    font.pixelSize: P.Style.fontHero; font.bold: true; color: P.Style.surface
                }
                Row {
                    spacing: P.Style.spaceLg
                    Repeater {
                        model: [
                            { v: page.viewState() === "list" ? String(stationModel.count) : "--",
                              c: "座电站" },
                            { v: page.viewState() === "list" ? String(page.statAvailable) : "--",
                              c: "枪空闲" },
                            { v: page.viewState() === "list" && page.statAvgPrice > 0
                                  ? "¥" + page.statAvgPrice.toFixed(2) : "--",
                              c: "均价/kWh" },
                        ]
                        delegate: Column {
                            spacing: 1
                            Text { text: modelData.v; font.pixelSize: P.Style.fontLg2
                                font.bold: true; color: P.Style.surface }
                            Text { text: modelData.c; font.pixelSize: P.Style.fontSm
                                color: P.Style.heroPhone }
                        }
                    }
                }
            }
        }

        // ---------- ② 分段切换 + 关键词清除（地图⇄列表，EV app 通例） ----------
        Row {
            objectName: "stationViewModeRow"
            width: parent.width
            spacing: P.Style.spaceSm
            Rectangle {
                width: 164; height: 36
                radius: P.Style.radiusSm
                color: P.Style.ghost
                border.width: 1
                border.color: P.Style.line
                Row {
                    anchors.fill: parent
                    anchors.margins: 3
                    spacing: 0
                    Repeater {
                        model: [
                            { obj: "viewModeListButton", text: "☰ 列表", mode: "list" },
                            { obj: "viewModeMapButton",  text: "🗺 地图", mode: "map" },
                        ]
                        delegate: Rectangle {
                            objectName: modelData.obj
                            width: 77; height: 28
                            radius: P.Style.radiusSm - 3
                            color: page.viewMode === modelData.mode ? P.Style.surface : "transparent"
                            border.width: page.viewMode === modelData.mode ? 1 : 0
                            border.color: P.Style.brandEdge
                            Behavior on color { ColorAnimation { duration: P.Style.motionEnabled ? 120 : 0 } }
                            Text {
                                anchors.centerIn: parent
                                text: modelData.text
                                font.pixelSize: P.Style.fontSm
                                font.bold: page.viewMode === modelData.mode
                                color: page.viewMode === modelData.mode ? P.Style.brandDeep : P.Style.muted
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: page.viewMode = modelData.mode
                            }
                        }
                    }
                }
            }
            Item { width: parent.width - 164 - (clearKeywordButton.visible ? clearKeywordButton.width + P.Style.spaceSm : 0) - advancedFilterButton.width - P.Style.spaceSm; height: 1 }
            P.ActionButton {
                id: clearKeywordButton
                objectName: "clearKeywordButton"
                variant: "ghost"
                visible: page.keyword.length > 0
                text: "✕ " + (page.keyword.length > 6 ? page.keyword.slice(0, 6) + "…" : page.keyword)
                height: 36
                onClicked: page.clearKeywordAndSearch()
            }
            // 漏斗（含激活计数徽标）钉行尾——原 Flow 里被裁的 ⛏ 有了固定席位
            P.ActionButton {
                id: advancedFilterButton
                objectName: "advancedFilterButton"
                variant: page.activeFilterCount() > 0 ? "primary" : "ghost"
                text: "⛏ 筛选"
                height: 36
                onClicked: filterDialog.openDialog(page.criteria)
                Rectangle {
                    objectName: "advancedFilterBadge"
                    visible: page.activeFilterCount() > 0
                    width: Math.max(18, badgeText.implicitWidth + 8); height: 18
                    radius: 9
                    color: P.Style.danger
                    border.width: 2; border.color: P.Style.surface
                    anchors.left: parent.right; anchors.leftMargin: -10
                    anchors.top: parent.top; anchors.topMargin: -6
                    Text {
                        id: badgeText
                        anchors.centerIn: parent
                        text: String(page.activeFilterCount())
                        font.pixelSize: 10; font.bold: true; color: P.Style.surface
                    }
                }
            }
        }

        // ---------- ③ 地图：列表态紧凑条 / 地图态大图 + peek 卡 ----------
        Rectangle {
            id: mapHub
            width: parent.width
            height: page.viewMode === "map" ? 300 : 128
            radius: P.Style.radiusLg
            color: P.Style.surface
            border.width: 1
            border.color: P.Style.line
            clip: true
            Behavior on height {
                NumberAnimation { duration: P.Style.motionEnabled ? P.Style.durEnter : 0 }
            }
            StationMapItem {
                objectName: "stationMapPanel"
                anchors.fill: parent
                markers: {
                    const out = []
                    for (let i = 0; i < stationModel.count; ++i) {
                        const r = stationModel.get(i)
                        out.push({ lat: r.lat, lng: r.lng, label: r.name, selected: i === page.selectedMarker })
                    }
                    return out
                }
                onMarkerClicked: index => page.focusStation(index)
            }
            // peek 卡（地图态选中浮层：价格大字 + 空闲 + 进详情）
            Rectangle {
                objectName: "stationPeekCard"
                visible: page.viewMode === "map" && page.selectedMarker >= 0
                         && stationModel.count > 0
                width: parent.width - P.Style.spaceMd
                height: 66
                radius: P.Style.radiusLg
                anchors.bottom: parent.bottom
                anchors.bottomMargin: P.Style.spaceSm
                anchors.horizontalCenter: parent.horizontalCenter
                color: P.Style.surface
                opacity: visible ? 1 : 0
                border.width: 1
                border.color: P.Style.brandEdge
                Behavior on opacity { NumberAnimation { duration: P.Style.motionEnabled ? 120 : 0 } }
                Row {
                    anchors.fill: parent
                    anchors.leftMargin: P.Style.spaceMd; anchors.rightMargin: P.Style.spaceSm
                    spacing: P.Style.spaceSm
                    Column {
                        width: parent.width - 130
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 1
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            text: stationModel.count > page.selectedMarker && page.selectedMarker >= 0
                                  ? stationModel.get(page.selectedMarker).name : ""
                            font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink
                        }
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            text: {
                                const r = stationModel.count > page.selectedMarker && page.selectedMarker >= 0
                                          ? stationModel.get(page.selectedMarker) : null
                                return r ? r.address : ""
                            }
                            font.pixelSize: P.Style.fontSm; color: P.Style.faint
                        }
                        Text {
                            text: {
                                const r = stationModel.count > page.selectedMarker && page.selectedMarker >= 0
                                          ? stationModel.get(page.selectedMarker) : null
                                return r ? "空闲 " + r.availableChargers + "/" + r.totalChargers
                                           + " · " + page.distText(r.distanceMeters) : ""
                            }
                            font.pixelSize: P.Style.fontSm; color: P.Style.brandDeep
                        }
                    }
                    Column {
                        width: 70
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            anchors.right: parent.right
                            text: {
                                const r = stationModel.count > page.selectedMarker && page.selectedMarker >= 0
                                          ? stationModel.get(page.selectedMarker) : null
                                return r ? "¥" + page.money(r.priceCentsPerKwh) : ""
                            }
                            font.pixelSize: P.Style.fontLg2; font.bold: true; color: P.Style.brandDeep
                        }
                        Text {
                            anchors.right: parent.right
                            text: "/kWh"; font.pixelSize: 10; color: P.Style.faint
                        }
                    }
                    P.ActionButton {
                        objectName: "stationPeekOpenButton"
                        variant: "primary"; text: "详情"
                        width: 56; height: 30
                        anchors.verticalCenter: parent.verticalCenter
                        onClicked: {
                            const r = stationModel.get(page.selectedMarker)
                            if (App && r) App.navigate("station_detail", {
                                id: r.stationId, name: r.name, address: r.address,
                                priceCentsPerKwh: r.priceCentsPerKwh,
                                distanceMeters: r.distanceMeters, status: r.status })
                        }
                    }
                }
            }
        }

        // ---------- ④ 筛选胶囊条（排序三 chip + 电价；漏斗在 ② 行尾带徽标） ----------
        Rectangle {
            objectName: "stationFilterBar"
            width: parent.width
            height: 40
            radius: P.Style.radiusSm
            color: P.Style.surface
            border.width: 1
            border.color: P.Style.line
            Row {
                anchors.fill: parent
                anchors.leftMargin: 5; anchors.rightMargin: 5
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                P.ActionButton {
                    objectName: "sortRecommendedButton"
                    variant: "chip"
                    anchors.verticalCenter: parent.verticalCenter
                    selected: page.sortMode === 2
                    text: "综合"
                    onClicked: { page.sortMode = 2; page.project() }
                }
                P.ActionButton {
                    objectName: "sortAvailableButton"
                    variant: "chip"
                    anchors.verticalCenter: parent.verticalCenter
                    selected: page.sortMode === 0
                    text: "空闲"
                    onClicked: { page.sortMode = 0; page.project() }
                }
                P.ActionButton {
                    objectName: "sortDistanceButton"
                    variant: "chip"
                    anchors.verticalCenter: parent.verticalCenter
                    selected: page.sortMode === 1
                    text: "最近"
                    onClicked: { page.sortMode = 1; page.project() }
                }
                Rectangle { width: 1; height: 22; anchors.verticalCenter: parent.verticalCenter
                    color: P.Style.line }
                ComboBox {
                    id: priceCombo
                    objectName: "priceFilterComboBox"
                    width: parent.width - x - 1   // 吃满行尾，胶囊条不留空档
                    anchors.verticalCenter: parent.verticalCenter
                    model: ["全部电价", "≤ ¥1.00", "≤ ¥1.20", "≤ ¥1.50"]
                    onActivated: idx => { page.priceMax = page.priceTiers[idx]; page.project() }
                }
            }
        }

        // 演示数据标注（同优惠券页口径：不冒充真实查询结果）
        Text {
            objectName: "homeDemoCaption"
            visible: page.demo
            width: parent.width
            wrapMode: Text.WordWrap
            text: "当前为演示数据（站点查询桥未就绪，接入后自动替换）；点卡片可进详情预约"
            font.pixelSize: P.Style.fontSm; color: P.Style.faint
        }

        // ---------- ⑤ 列表四态 ----------
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
                    // PullToRefreshArea 内容是 Column：显式尺寸 + x 负偏移等价还原出血。
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
                                distanceMeters: distanceMeters, status: status })
                        }
                        Row {
                            width: parent.width
                            spacing: P.Style.spaceMd
                            Column {
                                width: parent.width - 118
                                spacing: 4
                                Text {
                                    width: parent.width; elide: Text.ElideRight
                                    text: name; font.pixelSize: P.Style.fontLg
                                    font.bold: true; color: P.Style.ink
                                }
                                Text {
                                    width: parent.width; elide: Text.ElideRight
                                    text: address; font.pixelSize: P.Style.fontSm; color: P.Style.muted
                                }
                                // 空闲比例条（可用性色彩：充足 brand / 紧张 warning / 无 danger）
                                Item {
                                    width: parent.width; height: 14
                                    Rectangle {
                                        id: availTrack
                                        anchors.left: parent.left; anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        height: 6; radius: 3
                                        color: P.Style.ghost
                                        Rectangle {
                                            anchors.left: parent.left
                                            anchors.verticalCenter: parent.verticalCenter
                                            height: 6
                                            radius: 3
                                            width: parent.width * Math.min(1,
                                                totalChargers > 0 ? availableChargers / totalChargers : 0)
                                            Behavior on width {
                                                NumberAnimation { duration: P.Style.durEnter }
                                            }
                                            color: availableChargers === 0 ? P.Style.danger
                                                 : (totalChargers > 0 && availableChargers / totalChargers < 0.34)
                                                   ? P.Style.warning : P.Style.brand
                                        }
                                    }
                                }
                                Text {
                                    text: "空闲 " + availableChargers + "/" + totalChargers
                                          + " · " + page.distText(distanceMeters)
                                          + (status !== "active" ? " · 暂停运营" : "")
                                    font.pixelSize: P.Style.fontSm; color: P.Style.muted
                                }
                            }
                            Item {
                                width: 104
                                height: parent.height
                                Column {
                                    anchors.centerIn: parent
                                    spacing: P.Style.spaceXs
                                    // 价格大字（右挂对齐成列，fontLg2+小单位）
                                    Column {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: "¥" + page.money(priceCentsPerKwh)
                                            font.pixelSize: P.Style.fontLg2; font.bold: true
                                            color: P.Style.brandDeep
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: "/kWh"; font.pixelSize: 10; color: P.Style.faint
                                        }
                                    }
                                    P.StatusTag {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        tone: status === "active" ? "success" : "neutral"
                                        text: status === "active" ? "营业中" : "暂停运营"
                                    }
                                    MouseArea {
                                        objectName: "favoriteStarButton"
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        width: 34
                                        height: 24
                                        Text {
                                            anchors.centerIn: parent
                                            text: page.isFav(stationId) ? "★" : "☆"
                                            font.pixelSize: 18
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
