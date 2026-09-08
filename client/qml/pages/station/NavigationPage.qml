import QtQuick
import QtQuick.Controls.Basic
import QtWebEngine
import "../../platform" as P

// QML twin of widgets NavigationPage (objectName "navigationPage").
// arg = ReservationRecord map（确认页成功弹层带来）。
//
// "地图 APP" 化交互（2026-09-08 用户指定）：
// ① 进页自动 IP 定位起点（ws/location/v1/ip）；失败 → 明确"定位失败·请手动输入"态，
//    绝不静默；起点也可手动输入（地址 → 正地理编码；"纬度,经度" → 直用）。
// ② 选定预约站点后按真起点 requestDrivingRoute 画真实路线 + 距离（ws/direction/v1），
//    地图优先腾讯静态图（ws/staticmap/v2 真瓦片 PNG），失败回落 StationMapItem
//    Canvas 真折线；无 key 保持模拟先行（与 widgets 口径逐字节一致）。
// ③ "跳转腾讯地图导航" = URI API routeplan 页（Qt.openUrlExternally）——URL 内嵌
//    referer=key，绝不打印/入库；无 key 时按钮置灰。
// ④ 目的地行（2026-09-08 批量指令③）：下方输入框显示选中的充电站目标位置，
//    右侧【更换】→ 弹层站列表（名+价+距当前起点实时 haversine，全量检索/
//    演示清单兜底）→ 选中即换 record 目标并重算路线/静态图；主按钮文案
//    "点击导航"（唤起外部腾讯地图，语义同③）。
// 消费面 = MapGeoService 的 qml* 转发信号（载荷 QVariantMap，见映射稿 §桥缺口）；
// requestId 代际过滤丢弃过期回调（信号广播，其他页面的请求也会到这里）。
Item {
    id: page
    objectName: "navigationPage"
    property string route: "navigation"
    property var arg: ({})
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600
    property var record: page.arg || ({})
    // 用户默认位置与 StationMapPanel 中心同口径（无 key / 定位失败时的兜底起点）
    readonly property real demoLat: 22.541
    readonly property real demoLng: 113.943
    property bool usingRealRoute: false
    property int pendingRouteReq: -1    // qmlRouteReady 代际过滤
    property int pendingIpReq: -1
    property int pendingGeoReq: -1
    property int pendingStaticReq: -1
    property var realPolyline: []       // [[lat,lng],…]（qml* 转发面口径）
    property var realSteps: []          // [{instruction,distanceMeters},…]
    property string caption: defaultCaption()

    // —— 起点状态机：mock（无 key 演示位）| locating | located | failed | manual ——
    property string originState: "mock"
    property real originLat: demoLat
    property real originLng: demoLng
    property string originLabel: ""
    property bool originKnown: originState === "located" || originState === "manual"

    readonly property bool mapOnline: !!mapGeoService && mapGeoService.usable()
    readonly property string originHint: {
        if (originState === "locating") return "📡 正在自动定位…"
        if (originState === "located")  return "📍 已定位：" + (originLabel || "当前位置")
        if (originState === "failed")   return "⚠️ 定位失败 · 请手动输入起点"
        if (originState === "manual")   return "📍 起点：" + (originLabel || "手动位置")
        return "📍 演示位置（未配置地图密钥）"
    }

    // —— 目的地（预约站点坐标）——
    readonly property bool hasLoc: record.hasStationLocation === true
                                   && (record.stationLatitude !== undefined || (record.station && record.station.latitude !== undefined))
    readonly property real destLat: hasLoc
        ? (record.stationLatitude !== undefined ? record.stationLatitude : record.station.latitude) : 0
    readonly property real destLng: hasLoc
        ? (record.stationLongitude !== undefined ? record.stationLongitude : record.station.longitude) : 0

    function distText(m) {
        const d = Math.max(0, m || 0)
        return d >= 1000 ? "全程约 " + (d / 1000).toFixed(1) + " km" : "全程约 " + d + " m"
    }
    function hhmm(v) {
        if (typeof v === "number") { const d = new Date(v); return ("0"+d.getHours()).slice(-2)+":"+("0"+d.getMinutes()).slice(-2) }
        if (typeof v === "string" && v.length) { const d = new Date(v); if (!isNaN(d.getTime())) return ("0"+d.getHours()).slice(-2)+":"+("0"+d.getMinutes()).slice(-2) }
        return null
    }
    readonly property int travelMinutes: usingRealRoute && realDurationMinutes > 0
        ? realDurationMinutes
        : 5 + Math.ceil(Math.max(0, record.distanceMeters || 0) / 500)
    property int realDurationMinutes: -1   // 真路线分钟口径（direction duration=分钟）
    readonly property string etaText: {
        let t = "预计行驶约 " + travelMinutes + " 分钟"
        const st = hhmm(record.startAtUtc)
        if (st !== null) {
            const dep = new Date()
            dep.setHours(0, 0, 0, 0)
            const parts = st.split(":")
            dep.setMinutes(parseInt(parts[0]) * 60 + parseInt(parts[1]) - travelMinutes - 5)
            t += " · 建议 " + ("0"+dep.getHours()).slice(-2) + ":" + ("0"+dep.getMinutes()).slice(-2)
                 + " 前出发（预约 " + st + " 开始）"
        }
        return t
    }

    function defaultCaption() {
        return "导航路线为模拟数据 · 腾讯地图路线接口就绪后自动切换真实路线"
    }

    // —— 上游 2026-09-08 merge：WebEngine 真路线通道 —— 驾车/步行 chips（卡列区）引用
    //    page.mode / page.requestRoute / mapBridge.routeHtml；起点由 applyOriginPoint
    //    写入共享 MapGeoService.userLocation，无需另设。routeHtml 为空时静态图/画布
    //    回落（演示通道⑥保留）。JS 密钥 env TENCENT_MAP_JS_KEY，缺位则本层静默退位。
    property string mode: "driving"
    property string webError: ""
    readonly property bool webRouteReady:
        typeof mapBridge !== "undefined" && mapBridge.routeHtml.length > 0
    // mapBridge 缺位（裸 QML 测试无此 context 属性）时的 typeof 守卫降权面：
    // 全部退回本页演示通道语义（指令⑥），避免整点式 ReferenceError。
    readonly property string mapErr: typeof mapBridge !== "undefined" ? mapBridge.error : ""
    readonly property bool mapBusy: typeof mapBridge !== "undefined" && mapBridge.busy
    readonly property real mapRouteMeters:
        typeof mapBridge !== "undefined" ? mapBridge.routeDistanceMeters : -1
    readonly property int mapDurationMin: typeof mapBridge !== "undefined" ? mapBridge.durationMinutes : 0

    function requestRoute() {
        webError = ""
        const lat = record.stationLatitude !== undefined ? record.stationLatitude : record.latitude
        const lng = record.stationLongitude !== undefined ? record.stationLongitude : record.longitude
        if (typeof mapBridge === "undefined") return
        mapBridge.requestRoute(typeof lat === "number" ? lat : NaN,
                               typeof lng === "number" ? lng : NaN, mode)
    }

    // —— 请求链 ——
    function autoLocate() {
        if (!mapOnline) { originState = "mock"; requestRealRoute(); return }
        originState = "locating"
        try { page.pendingIpReq = mapGeoService.requestIpLocation() }
        catch (e) { originState = "failed"; requestRealRoute() }
    }
    function applyOriginPoint(point, stateWhenOk) {
        page.originLat = point.latitude
        page.originLng = point.longitude
        page.originState = stateWhenOk
        try { mapGeoService.setUserLocationLatLng(point.latitude, point.longitude) } catch (e) {}
        requestRealRoute()
    }
    function requestRealRoute() {
        if (!hasLoc) return
        if (!mapOnline) return   // 无 key：保持模拟先行（与 widgets 口径一致，不发请求）
        caption = "正在规划真实路线…"
        page.pendingRouteReq = mapGeoService.requestDrivingRoute(
            originLat, originLng, destLat, destLng)
        // 上游 WebEngine 通道并轨（2026-09-08 merge）：HTML 路线层与静态图通道并行，
        // 起点走共享 MapGeoService.userLocation；routeHtml 先回者先接管渲染。
        requestRoute()
    }
    function requestStaticImage() {
        if (!mapOnline || !usingRealRoute || realPolyline.length < 2) return
        const midLat = (originLat + destLat) / 2
        const midLng = (originLng + destLng) / 2
        const spanKm = Math.max(0.2, (record.distanceMeters || 2000) / 1000)
        const z = spanKm > 20 ? 10 : spanKm > 8 ? 11 : spanKm > 4 ? 12 : spanKm > 2 ? 13
                : spanKm > 1 ? 14 : spanKm > 0.5 ? 15 : 16
        const w = Math.round(mapCard.width), h = Math.round(mapCard.height)
        page.pendingStaticReq = mapGeoService.requestStaticMap(
            midLat, midLng, z, w, h, realPolyline,
            [{ latitude: originLat, longitude: originLng, label: "起" },
             { latitude: destLat, longitude: destLng, label: "终" }])
    }
    function parseManualOrigin(text) {
        const t = (text || "").trim()
        if (!t.length) { if (App) App.showToast("请输入起点地址或坐标", "warning"); return }
        // "纬度,经度"（支持中英文逗号/空格）→ 直用坐标。
        const m = t.match(/^(-?\d+(?:\.\d+)?)\s*[,，]\s*(-?\d+(?:\.\d+)?)$/)
        if (m) {
            const lat = parseFloat(m[1]), lng = parseFloat(m[2])
            if (Math.abs(lat) <= 90 && Math.abs(lng) <= 180) {
                applyOriginPoint({ latitude: lat, longitude: lng }, "manual")
                originLabel = t
                staticMapFile = ""
                return
            }
            if (App) App.showToast("坐标超出范围（纬度±90 / 经度±180）", "warning")
            return
        }
        if (!mapOnline) { if (App) App.showToast("未配置地图密钥，无法解析地址", "warning"); return }
        caption = "正在解析地址…"
        page.pendingGeoReq = mapGeoService.requestAddressGeocode(t)
    }

    // —— 目的地更换（2026-09-08 批量指令③）——
    // 弹层数据 = stationQueryService.search("") 全量；桥缺位/失败 → 演示清单兜底
    //（同 StationHomePage 演示通道口径，行=名+价+距当前起点直线距离）。
    property var pickList: []
    property bool pickLoading: false
    function haversineMeters(lat1, lng1, lat2, lng2) {
        const r = Math.PI / 180
        const dLat = (lat2 - lat1) * r, dLng = (lng2 - lng1) * r
        const a = Math.sin(dLat / 2) * Math.sin(dLat / 2)
                + Math.cos(lat1 * r) * Math.cos(lat2 * r)
                  * Math.sin(dLng / 2) * Math.sin(dLng / 2)
        return 6371000 * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a))
    }
    function pickLatLng(s) {
        const lat = s.latitude !== undefined ? s.latitude
                  : (s.station && s.station.latitude !== undefined ? s.station.latitude : NaN)
        const lng = s.longitude !== undefined ? s.longitude
                  : (s.station && s.station.longitude !== undefined ? s.station.longitude : NaN)
        return { lat: lat, lng: lng }
    }
    function pickName(s) { return s.name || (s.station && s.station.name) || "充电站" }
    function pickAddress(s) { return s.address || (s.station && s.station.address) || "" }
    function pickDistText(s) {
        const p = pickLatLng(s)
        if (isNaN(p.lat) || isNaN(p.lng)) return "无坐标"
        const d = Math.round(haversineMeters(originLat, originLng, p.lat, p.lng))
        return d >= 1000 ? "约 " + (d / 1000).toFixed(1) + " km" : "约 " + d + " m"
    }
    function demoPickRows() {
        return [
            { id: 9001, name: "滨海快充站", address: "南山区滨海大道 2012 号",
              priceCentsPerKwh: 128, latitude: 22.5372, longitude: 113.9401 },
            { id: 9002, name: "科技园慢充站", address: "高新区科苑南路 3188 号",
              priceCentsPerKwh: 98, latitude: 22.5448, longitude: 113.9512 },
            { id: 9003, name: "深圳湾超充站", address: "东滨路 1008 号",
              priceCentsPerKwh: 145, latitude: 22.5233, longitude: 113.9438 },
            { id: 9004, name: "世界之窗充电站", address: "深南大道 9037 号",
              priceCentsPerKwh: 119, latitude: 22.5391, longitude: 113.9716 }
        ]
    }
    function openPick() {
        if (page.pickList.length === 0) {
            page.pickLoading = true
            try { stationQueryService.search("") }
            catch (e) { page.pickList = page.demoPickRows(); page.pickLoading = false }
        }
        destPickPopup.open()
    }
    function chooseDestination(s) {
        const p = pickLatLng(s)
        if (isNaN(p.lat) || isNaN(p.lng)) {
            if (App) App.showToast("该站点暂无坐标，无法作为导航目标", "warning")
            return
        }
        page.record = Object.assign({}, page.record, {
            stationName: pickName(s), stationAddress: pickAddress(s),
            stationLatitude: p.lat, stationLongitude: p.lng,
            hasStationLocation: true,
            chargerCode: "", chargerSpec: "",
            distanceMeters: Math.round(haversineMeters(originLat, originLng, p.lat, p.lng))
        })
        page.usingRealRoute = false          // 旧路线作废，按新目标重算
        page.realPolyline = []
        page.realSteps = []
        page.staticMapFile = ""
        destPickPopup.close()
        requestRealRoute()                   // 在线：requestDrivingRoute→静态图链自动跟进
    }

    // 静态图落盘文件（qmlStaticMapReady 给路径；file:// URL 挂 Image）
    property string staticMapFile: ""
    readonly property url staticMapUrl: staticMapFile.length ? Url.fileUrl(staticMapFile) : ""

    Connections {
        target: mapGeoService
        function onQmlIpLocationReady(requestId, point) {
            if (requestId !== page.pendingIpReq) return   // 过期/他页请求
            page.pendingIpReq = -1
            const city = point && point.city ? point.city : ""
            page.originLabel = (city ? city + " · " : "") + "IP 定位"
            page.applyOriginPoint(point, "located")
        }
        function onQmlIpLocationError(requestId, message) {
            if (requestId !== page.pendingIpReq) return
            page.pendingIpReq = -1
            page.originState = "failed"     // 定位失败明示（用户指定：没定位到=定位失败，不猜）
            page.requestRealRoute()         // 兜底演示起点，页面永不空
        }
        function onQmlGeocodeReady(requestId, point) {
            if (requestId !== page.pendingGeoReq) return
            page.pendingGeoReq = -1
            if (point && point.latitude !== undefined) {
                page.originLabel = point.address || originInput.text
                page.applyOriginPoint(point, "manual")
                staticMapFile = ""
            }
        }
        function onQmlGeocodeError(requestId, message) {
            if (requestId !== page.pendingGeoReq) return
            page.pendingGeoReq = -1
            if (App) App.showToast("地址解析失败：" + message, "warning")
        }
        function onQmlRouteReady(requestId, route) {
            if (requestId !== page.pendingRouteReq) return
            if (!route) return
            page.usingRealRoute = true
            page.realPolyline = route.polyline || []
            page.realSteps = route.steps || []
            page.realDurationMinutes = route.durationMinutes !== undefined
                ? (route.durationMinutes || 0) : -1
            if (route.distanceMeters !== undefined) {
                // 真实行驶距离覆盖模拟值（概要卡实时重绑）。
                page.record = Object.assign({}, page.record, {
                    distanceMeters: route.distanceMeters })
            }
            page.caption = "真实导航路线 · 腾讯地图"
            page.requestStaticImage()
        }
        function onQmlRouteError(requestId, message) {
            if (requestId !== page.pendingRouteReq) return
            page.caption = "导航路线为模拟数据（接口异常：" + message + "）"
            if (mapOnline && message !== "未配置地图密钥" && App)
                App.showToast("地图服务暂不可用（" + message + "），已展示模拟路线", "warning")
        }
        function onQmlStaticMapReady(requestId, filePath) {
            if (requestId !== page.pendingStaticReq) return
            page.staticMapFile = filePath
        }
        function onQmlStaticMapError(requestId, message) {
            // 静态图缺失不是错误态：Canvas 真折线照常可用，静默回落。
            if (requestId !== page.pendingStaticReq) return
            page.pendingStaticReq = -1
            page.staticMapFile = ""
        }
    }
    // 目的地弹层数据源（桥广播信号，pickLoading 门控只认本页发起的检索）。
    Connections {
        target: stationQueryService
        function onQuerySucceeded(stations) {
            if (!page.pickLoading) return
            page.pickLoading = false
            page.pickList = stations || []
        }
        function onQueryFailed(message) {
            if (!page.pickLoading) return
            page.pickLoading = false
            page.pickList = page.demoPickRows()   // 失败重试 UI 之外再兜一层演示清单
        }
    }
    Component.onCompleted: { autoLocate(); requestRoute() }

    // 折线数据源（Canvas 回落层）：真实 polyline 到达则替换，否则两点示意线。
    readonly property var routeLine: usingRealRoute && realPolyline.length >= 2
        ? realPolyline
        : (hasLoc ? [[originLat, originLng], [destLat, destLng]] : [])

    // 上游 WebEngine 通道联动（2026-09-08 merge）：routeHtml 就绪即注入；arg 变更
    // （换目标站）重发路线；页面销毁撤销在途请求。
    Connections {
        target: typeof mapBridge !== "undefined" ? mapBridge : null
        function onChanged() {
            if (page.webRouteReady && webLoader.item)
                webLoader.item.loadHtml(mapBridge.routeHtml, "https://map.qq.com/")
        }
    }
    Component.onDestruction: { if (typeof mapBridge !== "undefined") mapBridge.cancelRoute() }
    onArgChanged: requestRoute()

    Rectangle { anchors.fill: parent; color: P.Style.bg }
    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd
        Text {
            objectName: "navigationPageTitle"
            text: "前往 " + (page.record.stationName || page.record.name || "充电站")
            width: parent.width; wrapMode: Text.WordWrap
            font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink
        }
        // 起点状态行由本页 originRow（📍 定位/手动/mock 三态）承担，
        // 上游单行"起点："提示并入其语义、不再并列（2026-09-08 merge 注记）。
        Row {
            spacing: P.Style.spaceSm
            P.ActionButton {
                objectName: "drivingRouteButton"
                variant: "chip"; selected: page.mode === "driving"; text: "驾车"
                onClicked: { page.mode = "driving"; page.requestRoute() }
            }
            P.ActionButton {
                objectName: "walkingRouteButton"
                variant: "chip"; selected: page.mode === "walking"; text: "步行"
                onClicked: { page.mode = "walking"; page.requestRoute() }
            }
            P.ActionButton { variant: "ghost"; text: "重新规划"; enabled: !page.mapBusy; onClicked: page.requestRoute() }
        }
        Text {
            objectName: "navigationCaptionLabel"
            width: parent.width; wrapMode: Text.WordWrap
            // 上游 mapBridge 真实路线口径优先（2026-09-08 merge）；无路线/无密钥时
            // 回落 page.caption（本页模拟先行口径，指令⑥两通道文案自洽）。
            text: page.mapErr.length ? page.mapErr : page.webError.length ? page.webError
                  : page.mapRouteMeters >= 0
                    ? "腾讯地图 · " + (page.mapRouteMeters / 1000).toFixed(1)
                      + " km · 约 " + page.mapDurationMin + " 分钟"
                    : page.caption
            color: page.mapErr.length || page.webError.length ? P.Style.danger : P.Style.brandDeep
            font.pixelSize: P.Style.fontSm
        }

        // —— 起点行：状态提示 + 手动输入 + 重新定位（地图 APP 同款"我的位置"入口）——
        Rectangle {
            objectName: "originRow"
            width: parent.width
            height: 76
            radius: P.Style.radiusLg
            color: P.Style.surface
            border.width: 1
            border.color: P.Style.line
            Column {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: P.Style.spaceMd; anchors.rightMargin: P.Style.spaceMd
                spacing: P.Style.spaceXs
                Text {
                    objectName: "originHintLabel"
                    text: page.originHint
                    font.pixelSize: P.Style.fontSm
                    color: page.originState === "failed" ? P.Style.warning : P.Style.muted
                }
                Row {
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Rectangle {
                        width: parent.width - locateButton.width - goButton.width - 2 * P.Style.spaceSm
                        height: 36
                        radius: P.Style.radiusSm
                        color: P.Style.ghost
                        border.width: 1
                        border.color: originInput.activeFocus ? P.Style.brand : P.Style.line
                        TextField {
                            id: originInput
                            objectName: "originField"
                            anchors.fill: parent
                            anchors.leftMargin: P.Style.spaceSm
                            anchors.rightMargin: P.Style.spaceSm
                            verticalAlignment: TextInput.AlignVCenter
                            placeholderText: "输入起点：地址 或 纬度,经度"
                            placeholderTextColor: P.Style.faint
                            color: P.Style.ink
                            font.pixelSize: P.Style.fontSm
                            background: Item {}
                            onAccepted: page.parseManualOrigin(text)
                        }
                    }
                    P.ActionButton {
                        id: goButton
                        objectName: "originGoButton"
                        variant: "primary"; text: "路线"
                        width: 60; height: 36
                        onClicked: page.parseManualOrigin(originInput.text)
                    }
                    P.ActionButton {
                        id: locateButton
                        objectName: "originLocateButton"
                        variant: "secondary"; text: "🎯"
                        width: 40; height: 36
                        enabled: page.mapOnline
                        onClicked: page.autoLocate()
                    }
                }
            }
        }

        // —— 目的地行（批量指令③）：输入框=选中的充电站目标位置，右侧【更换】 ——
        Rectangle {
            objectName: "destinationRow"
            width: parent.width
            height: 76
            radius: P.Style.radiusLg
            color: P.Style.surface
            border.width: 1
            border.color: P.Style.line
            Column {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: P.Style.spaceMd; anchors.rightMargin: P.Style.spaceMd
                spacing: P.Style.spaceXs
                Text {
                    objectName: "destinationHintLabel"
                    text: "🎯 目标充电站（可更换）"
                    font.pixelSize: P.Style.fontSm
                    color: P.Style.muted
                }
                Row {
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Rectangle {
                        width: parent.width - destChangeButton.width - P.Style.spaceSm
                        height: 36
                        radius: P.Style.radiusSm
                        color: P.Style.ghost
                        border.width: 1
                        border.color: P.Style.line
                        TextField {
                            id: destField
                            objectName: "destinationField"
                            anchors.fill: parent
                            anchors.leftMargin: P.Style.spaceSm
                            anchors.rightMargin: P.Style.spaceSm
                            readOnly: true
                            verticalAlignment: TextInput.AlignVCenter
                            text: hasLoc ? (record.stationName || "已选充电站")
                                          + (record.stationAddress ? " · " + record.stationAddress : "")
                                 : "尚未选择充电站（点右侧更换）"
                            color: hasLoc ? P.Style.ink : P.Style.faint
                            font.pixelSize: P.Style.fontSm
                            background: Item {}
                        }
                    }
                    P.ActionButton {
                        id: destChangeButton
                        objectName: "destinationChangeButton"
                        variant: "secondary"; text: "更换"
                        width: 64; height: 36
                        onClicked: page.openPick()
                    }
                }
            }
        }

        // —— 地图：静态图（真瓦片）优先，Canvas 真折线回落 ——
        Rectangle {
            id: mapCard
            objectName: "navigationMapPanel"
            width: parent.width
            height: 230
            radius: P.Style.radiusLg
            color: P.Style.surface
            border.width: 1
            border.color: P.Style.line
            clip: true
            StationMapItem {
                objectName: "navigationMapCanvas"
                anchors.fill: parent
                visible: page.staticMapFile.length === 0 && !page.webRouteReady
                route: page.routeLine
                markers: hasLoc
                    ? [{ lat: destLat, lng: destLng,
                         label: record.stationName || "", selected: true },
                       { lat: originLat, lng: originLng,
                         label: page.originKnown ? "我的位置" : "起点", selected: false }]
                    : []
            }
            Image {
                objectName: "navigationStaticMapImage"
                anchors.fill: parent
                visible: page.staticMapFile.length > 0 && !page.webRouteReady
                source: page.staticMapUrl
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
            // 上游 WebEngine 真路线层（2026-09-08 merge 并入本面板）：routeHtml 就绪
            // 即接管；为空时画布/静态图回落层继续工作（演示通道⑥）。objectName 让位
            // 给面板本体（navigationMapPanel 保持锚点在 Rectangle 上，测试口径不变）。
            // Loader 惰性化：WebEngineView 创建会拉起 WebEngineContext，裸 QQmlEngine
            // 测试进程未调 QtWebEngineQuick::initialize()（仅 preview main.cpp 有）→
            // 直接实例化段错误。webRouteReady 需 mapBridge+routeHtml 双在，测试恒假
            // →组件永不落地；preview 首帧就绪即实例化并 loadHtml。
            Loader {
                id: webLoader
                anchors.fill: parent
                active: page.webRouteReady
                sourceComponent: webRouteComponent
                onLoaded: {
                    if (typeof mapBridge !== "undefined" && mapBridge.routeHtml.length > 0)
                        item.loadHtml(mapBridge.routeHtml, "https://map.qq.com/")
                }
            }
            Component {
                id: webRouteComponent
                WebEngineView {
                    objectName: "navigationRouteWebView"
                    anchors.fill: parent
                    settings.localContentCanAccessRemoteUrls: true
                    onLoadingChanged: function(info) {
                        if (info.status === WebEngineView.LoadFailedStatus)
                            page.webError = "地图页面加载失败，请检查网络和 JavaScript 地图密钥授权"
                    }
                }
            }
        }

        // 行程概要
        P.Card {
            objectName: "navigationSummaryCard"
            width: parent.width
            Column {
                width: parent.width
                spacing: P.Style.spaceXs
                Text {
                    objectName: "navigationTargetLabel"
                    width: parent.width; wrapMode: Text.WordWrap
                    text: "前往：" + (record.stationName || "--") + " · " + (record.chargerCode || "--")
                          + "（" + (record.chargerSpec || "充电桩") + "）"
                    font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink
                }
                Text {
                    objectName: "navigationDistanceLabel"
                    text: page.distText(record.distanceMeters)
                          + (usingRealRoute ? " · 行驶 " + travelMinutes + " 分钟" : "")
                    font.pixelSize: P.Style.fontMd; color: P.Style.brandDeep
                }
                Text {
                    objectName: "navigationEtaLabel"
                    width: parent.width; wrapMode: Text.WordWrap
                    text: page.etaText
                    font.pixelSize: P.Style.fontMd; color: P.Style.muted
                }
            }
        }

        // 步骤列表（真实路线 steps[].instruction；模拟期给出概览行）
        ListView {
            objectName: "navigationStepsList"
            width: parent.width
            height: parent.height - y - 110
            clip: true
            spacing: P.Style.spaceXs
            model: {
                if (!usingRealRoute) return ["模拟路线 · 接口就绪后展示真实转向指引"]
                // = widgets 截断口径：前 15 段 + "…后续 %1 段已省略"
                const steps = page.realSteps.slice(0, 15)
                if (page.realSteps.length > 15)
                    steps.push("…后续 " + (page.realSteps.length - 15) + " 段已省略")
                return steps
            }
            delegate: Row {
                width: parent.width
                spacing: P.Style.spaceSm
                Text { text: "·"; color: P.Style.brand; font.pixelSize: P.Style.fontMd
                    anchors.verticalCenter: parent.verticalCenter }
                Text {
                    width: parent.width - 20
                    wrapMode: Text.WordWrap
                    text: typeof modelData === "object"
                          ? (modelData.instruction || "")
                            + (modelData.distanceMeters ? "（" + page.distText(modelData.distanceMeters).replace("全程约 ", "") + "）" : "")
                          : modelData
                    font.pixelSize: P.Style.fontMd; color: P.Style.ink
                }
            }
        }

        // —— 跳转腾讯地图导航（URI API 接力真导航；无 key 置灰）——
        // —— 点击导航：唤起外部腾讯地图（URI API routeplan；无 key 置灰）——
        P.ActionButton {
            objectName: "navigationExternalButton"
            variant: "primary"; text: "点击导航"
            width: parent.width
            enabled: page.mapOnline && hasLoc
            onClicked: {
                const url = mapGeoService.navigationUriUrl(
                    originLat, originLng, page.originLabel || "我的位置",
                    destLat, destLng, record.stationName || "充电站")
                if (!url.length) {
                    if (App) App.showToast("地图密钥未配置，无法跳转导航", "warning")
                    return
                }
                if (!Qt.openUrlExternally(url) && App)
                    App.showToast("未找到可打开地图的应用", "warning")
            }
        }
        P.ActionButton {
            objectName: "navigationBackButton"
            variant: "secondary"; text: "返回"
            width: parent.width
            onClicked: { if (App) App.back() }
        }
    }

    // —— 目标充电站选择弹层（【更换】入口；行=名+价+距起点直线距离）——
    Popup {
        id: destPickPopup
        objectName: "navigationPickPopup"
        modal: true
        x: Math.max(P.Style.spaceLg, (page.width - width) / 2)
        y: Math.max(P.Style.spaceLg, (page.height - height) / 2)
        width: Math.min(360, page.width - 2 * P.Style.spaceLg)
        height: Math.min(420, page.height - 2 * P.Style.spaceLg)
        padding: P.Style.spaceMd
        background: Rectangle {
            radius: P.Style.radiusLg
            color: P.Style.surface
            border.width: 1
            border.color: P.Style.line
        }
        Column {
            // anchors.fill 而非裸 width：Column 默认高度=implicitHeight 由子项
            // 反推，下面 ListView 又写 parent.height - y（parent=本 Column），
            // 裸宽时是自我循环→polish() loop 每帧刷（2026-09-08 用户实测导航
            // 弹窗冻结根因）。显式定高切断回边。
            anchors.fill: parent
            spacing: P.Style.spaceSm
            Text {
                text: "选择目标充电站"
                font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink
            }
            ListView {
                id: pickListView
                objectName: "navigationPickList"
                width: parent.width
                height: parent.height - y - P.Style.spaceXs
                clip: true
                spacing: P.Style.spaceXs
                model: page.pickList
                delegate: Rectangle {
                    id: pickRow
                    required property var modelData
                    objectName: "navigationPickRow"
                    width: pickListView.width
                    height: 50
                    radius: P.Style.radiusSm
                    color: pickRowMa.containsMouse ? P.Style.ghost : P.Style.surface
                    border.width: 1
                    border.color: P.Style.line
                    Behavior on color { ColorAnimation { duration: 90 } }
                    Row {
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: P.Style.spaceSm; anchors.rightMargin: P.Style.spaceSm
                        spacing: P.Style.spaceSm
                        Column {
                            width: parent.width - 96
                            spacing: 2
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: page.pickName(pickRow.modelData)
                                      + (page.pickName(pickRow.modelData) === (page.record.stationName || "")
                                         ? " · 当前" : "")
                                font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink
                            }
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: page.pickAddress(pickRow.modelData)
                                font.pixelSize: P.Style.fontXs; color: P.Style.faint
                            }
                        }
                        Column {
                            width: 92
                            spacing: 2
                            Text {
                                anchors.right: parent.right
                                text: {
                                    const v = pickRow.modelData.priceCentsPerKwh
                                    return v !== undefined ? "¥" + (v / 100).toFixed(2) + "/度" : "--"
                                }
                                font.pixelSize: P.Style.fontSm; color: P.Style.brandDeep
                            }
                            Text {
                                anchors.right: parent.right
                                text: page.pickDistText(pickRow.modelData)
                                font.pixelSize: P.Style.fontXs; color: P.Style.muted
                            }
                        }
                    }
                    MouseArea {
                        id: pickRowMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.chooseDestination(pickRow.modelData)
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: page.pickLoading
                    text: "正在加载站点…"
                    font.pixelSize: P.Style.fontSm; color: P.Style.muted
                }
                Text {
                    anchors.centerIn: parent
                    visible: !page.pickLoading && page.pickList.length === 0
                    text: "暂无可选站点"
                    font.pixelSize: P.Style.fontSm; color: P.Style.faint
                }
            }
        }
    }
}
