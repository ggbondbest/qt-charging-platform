import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets StationHomePage (objectName "stationHomePage" kept).
// 自上而下：地图示意（StationMapItem，真 WebEngine 图=明天）→ 筛选操作栏
// （排序 chips + 电价下拉 + 高级筛选入口）→ 站点卡片 ListView（星星收藏）。
// 三源投影（关键词/电价/8 组条件）在 QML 侧复现 applyStationFilter 语义；
// 服务桥未补前所有调用按契约名盲写（TODO(contract) 见 docs/design/qml-station-mapping.md）。
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
            page.loading = false; page.loaded = false; page.failed = true
            pull.setRefreshing(false)
            if (App) App.showToast("站点查询失败：" + message, "danger")
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

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd

        // 地图示意（选卡联动高亮；真地图=明天 WebEngine 决策）
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
                text: "⛏ 筛选"      // 原手绘漏斗图标位，明天换 icon 资源
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
                                distanceMeters: distanceMeters, status: status })
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
                                          + " · " + page.distText(distanceMeters)
                                          + (status !== "active" ? " · 暂停运营" : "")
                                    font.pixelSize: P.Style.fontSm; color: P.Style.brandDeep
                                }
                            }
                            Item {
                                width: 96
                                height: parent.height
                                Column {
                                    anchors.centerIn: parent
                                    spacing: P.Style.spaceXs
                                    P.StatusTag {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        tone: status === "active" ? "success" : "neutral"
                                        text: status === "active" ? "营业中" : "暂停运营"
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
