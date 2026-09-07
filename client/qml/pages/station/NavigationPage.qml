import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets NavigationPage (objectName "navigationPage").
// arg = ReservationRecord map（确认页成功弹层带来）。
// 口径 = widgets 完全一致的模拟先行：行驶分钟 = 5 + ⌈米/500⌉（recommendSlot 同源），
// 路线折线 = 用户默认位置 → 站点坐标两点示意；桥就绪且记录带坐标时
// requestDrivingRoute 异步升级真实路线，代际号丢弃过期回调，失败保持模拟+Toast。
Item {
    id: page
    objectName: "navigationPage"
    property string route: "navigation"
    property var arg: ({})
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property var record: page.arg || ({})
    // 用户默认位置与 StationMapPanel 中心同口径（不动成员3 文件的常量复制）
    readonly property real userLat: 22.541
    readonly property real userLng: 113.943
    property bool usingRealRoute: false
    property int routeGen: 0            // 过期回调丢弃（= widgets routeGeneration_ 同语义）
    property int pendingReqId: -1
    property var realPolyline: []
    property var realSteps: []          // route.steps（桥落地后真实转向指引）
    property string caption: "导航路线为模拟数据 · 腾讯地图路线接口就绪后自动切换真实路线"

    function distText(m) {
        const d = Math.max(0, m || 0)
        return d >= 1000 ? "全程约 " + (d / 1000).toFixed(1) + " km" : "全程约 " + d + " m"
    }
    function hhmm(v) {
        if (typeof v === "number") { const d = new Date(v); return ("0"+d.getHours()).slice(-2)+":"+("0"+d.getMinutes()).slice(-2) }
        if (typeof v === "string" && v.length) { const d = new Date(v); if (!isNaN(d.getTime())) return ("0"+d.getHours()).slice(-2)+":"+("0"+d.getMinutes()).slice(-2) }
        return null
    }
    readonly property int travelMinutes:
        5 + Math.ceil(Math.max(0, record.distanceMeters || 0) / 500)
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
    readonly property bool hasLoc: record.hasStationLocation === true
                                   && (record.stationLatitude !== undefined || (record.station && record.station.latitude !== undefined))

    function requestRealRoute() {
        if (!mapGeoService || !hasLoc) return
        const lat = record.stationLatitude !== undefined ? record.stationLatitude : record.station.latitude
        const lng = record.stationLongitude !== undefined ? record.stationLongitude : record.station.longitude
        ++routeGen
        caption = "正在加载真实导航路线…"
        // TODO(contract): requestDrivingRoute(fromLat,fromLng,toLat,toLng)→requestId（成员3 定形）。
        try { page.pendingReqId = mapGeoService.requestDrivingRoute(userLat, userLng, lat, lng) }
        catch (e) { page.pendingReqId = -1; caption = page.defaultCaption(); if (App) App.showToast("地图服务暂不可用，已展示模拟路线", "warning") }
    }
    function defaultCaption() {
        return "导航路线为模拟数据 · 腾讯地图路线接口就绪后自动切换真实路线"
    }
    Connections {
        target: mapGeoService
        // 桥保 C++ 全形（requestId 首参，契约"信号名/参数序不变仅载荷改 map"）。
        function onRouteSucceeded(requestId, route) {
            if (requestId !== undefined && page.pendingReqId >= 0 && requestId !== page.pendingReqId) return  // 过期丢弃
            if (route === undefined || route === null) return
            page.usingRealRoute = true
            page.realPolyline = (route.polyline || [])
            page.realSteps = (route.steps || [])
            if (route.distanceMeters !== undefined) {
                // 真实口径覆盖模拟距离（概要卡实时重绑）。
                page.record = Object.assign({}, page.record, {
                    distanceMeters: route.distanceMeters })
            }
            page.caption = "真实导航路线 · 腾讯地图"
        }
        function onRouteFailed(requestId, error, message) {
            if (requestId !== undefined && page.pendingReqId >= 0 && requestId !== page.pendingReqId) return
            page.caption = "导航路线为模拟数据（接口异常：" + message + "）"
            if (App) App.showToast("地图服务暂不可用（" + message + "），已展示模拟路线", "warning")
        }
    }
    Component.onCompleted: requestRealRoute()

    // 折线数据源：真实 polyline 到达则替换，否则两点示意线。
    readonly property var routeLine: usingRealRoute && realPolyline.length >= 2
        ? realPolyline
        : (hasLoc ? [[userLat, userLng],
                     [record.stationLatitude !== undefined ? record.stationLatitude : record.station.latitude,
                      record.stationLongitude !== undefined ? record.stationLongitude : record.station.longitude]]
                  : [])

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd

        Text { objectName: "navigationPageTitle"; text: "导航前往充电桩"
            font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink }
        Text {
            objectName: "navigationCaptionLabel"
            width: parent.width; wrapMode: Text.WordWrap
            text: page.caption
            font.pixelSize: P.Style.fontSm; color: P.Style.faint
        }

        // 地图（Canvas 示意 → 明天 WebEngineView 原位替换）
        StationMapItem {
            objectName: "navigationMapPanel"
            width: parent.width
            height: 190
            route: page.routeLine
            markers: hasLoc
                ? [{ lat: routeLine.length ? +routeLine[routeLine.length - 1][0] : userLat,
                     lng: routeLine.length ? +routeLine[routeLine.length - 1][1] : userLng,
                     label: record.stationName || "", selected: true }]
                : []
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
            height: parent.height - y
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
                    text: typeof modelData === "object" ? (modelData.instruction || "") : modelData
                    font.pixelSize: P.Style.fontMd; color: P.Style.ink
                }
            }
        }

        P.ActionButton {
            objectName: "navigationBackButton"
            variant: "secondary"; text: "返回"
            width: parent.width
            onClicked: { if (App) App.back() }
        }
    }
}
