import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "StationState.js" as StationState

// QML twin of widgets StationDetailPage (objectName "stationDetailPage" kept).
// arg = 列表页点击卡片带来的 map：{id, name, address, priceCentsPerKwh, distanceMeters, status}。
// 进页 fetchDetailById（struct 参数版 QML 过不去 → 新桥方法，TODO(contract)）；
// 桩卡彩签/故障红框/预约三重准入均按 widgets 同语义直译。
Item {
    id: page
    objectName: "stationDetailPage"
    property string route: "station_detail"
    property var arg: ({})
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property var station: page.arg || ({})
    readonly property bool hasHeader: !!(page.station && page.station.name)
    property bool detailLoading: false
    property bool detailLoaded: false
    property bool detailFailed: false
    property string failMessage: ""
    property var chargers: []
    property bool demo: false

    function money(c) { return (c / 100).toFixed(2) }
    function distText(m) { return (m === undefined || m < 0) ? "--" : (m / 1000).toFixed(1) + "km" }
    function isActive() { return String(station.status).toLowerCase() === "active" }
    function availableCount() {
        let n = 0
        for (const c of chargers) if (String(c.status).toLowerCase() === "available") ++n
        return n
    }
    // 桩状态 → 文案/彩签 tone（= widgets statusView 逐字）
    function statusText(st) {
        return ({ available: "空闲", charging: "占用·充电中", reserved: "占用·已预约",
                  fault: "故障", offline: "离线" })[String(st).toLowerCase()] || "未知"
    }
    function statusTone(st) {
        return ({ available: "success", charging: "warning", reserved: "info",
                  fault: "danger", offline: "neutral" })[String(st).toLowerCase()] || "neutral"
    }

    function fetch() {
        if (!stationQueryService || !station.id) return
        detailLoading = true; detailFailed = false
        // TODO(contract): struct 参数 fetchDetail(Station,int) QML 不可达，
        //                桥补 fetchDetailById(int stationId, int distanceMeters)。
        try { stationQueryService.fetchDetailById(station.id, station.distanceMeters || -1) }
        catch (e) {
            detailLoading = false; detailLoaded = false; detailFailed = true
            failMessage = "站点详情服务不可用，请重新登录后重试"
            chargers = []
        }
    }
    Connections {
        target: stationQueryService
        function onDetailStarted() { page.detailLoading = true; page.detailFailed = false }
        function onDetailSucceeded(detail) {
            const incoming = detail && detail.station ? detail.station : detail
            if (!incoming || String(incoming.id) !== String(page.station.id)) return
            page.detailLoading = false; page.detailLoaded = true; page.detailFailed = false
            page.demo = false                       // 真数据到位，演示桩位退位
            // 桥 map 形状：{station…, distanceMeters, chargers[], hasChargerData}；
            // 兼容 station 子对象与拍平两种形状（TODO(contract) 成员3 定形）。
            const src = (detail && detail.station) ? detail.station : (detail || {})
            page.station = Object.assign({}, page.station, src,
                { distanceMeters: (detail && detail.distanceMeters !== undefined)
                                     ? detail.distanceMeters : page.station.distanceMeters })
            page.chargers = (detail && detail.chargers) || []
        }
        function onDetailFailed(message) {
            page.detailLoading = false; page.detailLoaded = false; page.detailFailed = true
            page.failMessage = message
        }
    }
    Component.onCompleted: fetch()

    // ---- 预约三重准入（widgets handleReserveRequested + HomeShell 弹层直译进页）----
    function call(target, fn, args) {
        try { return target[fn].apply(target, args) } catch (e) { return undefined }
    }
    function vehicleCount() {
        const v = call(settingsService, "vehicles", [])
        if (v !== undefined) return v.length
        return StationState.vehicles.length   // 桥缺位期读设置页本地通道
    }
    function activeReservationCount() {
        // TODO(contract): reservationService.activeReservationCount() invokable；缺位返回 -1 放行。
        const n = call(reservationService, "activeReservationCount", [])
        return (typeof n === "number") ? n : -1
    }
    function requestReserve(charger) {
        if (String(charger.status).toLowerCase() !== "available") {
            if (App) App.showToast("仅空闲充电桩可预约", "warning")
            return
        }
        if (!(App && App.loggedIn)) {
            if (App) { App.showToast("请先登录再发起预约", "warning"); App.navigate("login") }
            return
        }
        if (App.mockMode && vehicleCount() === 0) {
            vehicleRequiredPrompt.open()
            return
        }
        const act = activeReservationCount()
        if (App.mockMode && act >= 0 && act >= vehicleCount()) {
            unfinishedReservationPrompt.open()
            return
        }
        if (App) App.checkBeforeReservation({
            stationId: String(page.station.id), stationName: page.station.name,
            priceCentsPerKwh: page.station.priceCentsPerKwh,
            distanceMeters: page.station.distanceMeters,
            stationLatitude: page.station.latitude, stationLongitude: page.station.longitude,
            hasStationLocation: typeof page.station.latitude === "number" && typeof page.station.longitude === "number",
            chargerId: String(charger.id), chargerCode: charger.code,
            chargerType: charger.type, chargerPowerWatts: charger.powerWatts })
    }

    // 无车辆引导（旧版 vehicleRequiredPrompt 弹层同文案同钮）
    Popup {
        id: vehicleRequiredPrompt
        objectName: "vehicleRequiredPrompt"
        modal: true
        anchors.centerIn: parent
        width: parent ? Math.min(320, parent.width - P.Style.spaceXl) : 320
        padding: P.Style.spaceLg
        background: Rectangle {
            radius: P.Style.radiusLg; color: P.Style.surface
            border.color: P.Style.line; border.width: 1
        }
        Column {
            width: parent.width
            spacing: P.Style.spaceMd
            Text { width: parent.width; wrapMode: Text.WordWrap
                text: "需要添加车辆"
                font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
            Text { width: parent.width; wrapMode: Text.WordWrap
                text: "预约名额由车辆决定，请先在「设置 - 车辆管理」添加车辆。"
                font.pixelSize: P.Style.fontMd; color: P.Style.muted }
            Row {
                width: parent.width
                spacing: P.Style.spaceSm
                P.ActionButton {
                    variant: "ghost"; text: "稍后再说"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: vehicleRequiredPrompt.close()
                }
                P.ActionButton {
                    objectName: "vehicleGoSettingsButton"
                    variant: "primary"; text: "去添加车辆"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: { vehicleRequiredPrompt.close(); if (App) App.navigate("settings") }
                }
            }
        }
    }

    // 名额占满引导（旧版 unfinishedReservationPrompt 同文案同钮）
    Popup {
        id: unfinishedReservationPrompt
        objectName: "unfinishedReservationPrompt"
        modal: true
        anchors.centerIn: parent
        width: parent ? Math.min(320, parent.width - P.Style.spaceXl) : 320
        padding: P.Style.spaceLg
        background: Rectangle {
            radius: P.Style.radiusLg; color: P.Style.surface
            border.color: P.Style.line; border.width: 1
        }
        Column {
            width: parent.width
            spacing: P.Style.spaceMd
            Text { width: parent.width; wrapMode: Text.WordWrap
                text: "无法发起新预约"
                font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
            Text { width: parent.width; wrapMode: Text.WordWrap
                text: "可预约名额已全部占用（名额 = 车辆数），请结束当前预约后再发起新预约"
                font.pixelSize: P.Style.fontMd; color: P.Style.muted }
            Row {
                width: parent.width
                spacing: P.Style.spaceSm
                P.ActionButton {
                    variant: "ghost"; text: "知道了"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: unfinishedReservationPrompt.close()
                }
                P.ActionButton {
                    objectName: "unfinishedGoLookButton"
                    variant: "primary"; text: "去查看"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: { unfinishedReservationPrompt.close(); if (App) App.navigate("reservation_module") }
                }
            }
        }
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd
        // 头卡用列表传来的 arg 即出（不等详情桥）；桩区单独走状态门。

        // 页面标题（widgets titleLabel "站点详情" 同位）
        Text {
            objectName: "detailTitle"; text: "站点详情"
            font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink
        }

        // 站点信息头卡
        P.Card {
            objectName: "detailHeaderCard"
            width: parent.width
            Column {
                width: parent.width
                spacing: P.Style.spaceXs
                Row {
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Text {
                        objectName: "detailNameLabel"
                        width: parent.width - 80
                        text: station.name || "站点详情"
                        font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink
                        elide: Text.ElideRight
                    }
                    P.StatusTag {
                        objectName: "detailStatusTag"
                        anchors.verticalCenter: parent.verticalCenter
                        tone: isActive() ? "success" : "neutral"
                        text: isActive() ? "营业中" : "已离线"
                    }
                }
                // 行序对齐 widgets：名称+状态 → 地址 → 价格·距离（成员3 690b189）；
                // 价格/距离两锚点恢复 widgets 分离标签（detailPriceLabel/detailDistanceLabel）。
                Text {
                    objectName: "detailAddressLabel"
                    width: parent.width; wrapMode: Text.WordWrap
                    text: station.address || ""; font.pixelSize: P.Style.fontSm; color: P.Style.muted
                }
                Row {
                    width: parent.width
                    spacing: P.Style.spaceMd
                    Text {
                        objectName: "detailPriceLabel"
                        text: "¥" + money(station.priceCentsPerKwh || 0) + "/kWh"
                        font.pixelSize: P.Style.fontMd; color: P.Style.brandDeep
                    }
                    Text {
                        objectName: "detailDistanceLabel"
                        text: "距您 " + distText(station.distanceMeters)
                        font.pixelSize: P.Style.fontMd; color: P.Style.brandDeep
                    }
                }
            }
        }

        // 离线横幅（warningSoft 底）
        Rectangle {
            objectName: "detailOfflineBanner"
            visible: !isActive()
            width: parent.width
            height: 34
            radius: P.Style.radiusSm
            color: P.Style.warningSoft
            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left; anchors.leftMargin: P.Style.spaceMd
                text: "⚠️ 该站点当前处于离线状态，暂不可用，请稍后再试或选择其他站点"
                font.pixelSize: P.Style.fontSm; color: P.Style.ink
            }
        }

        // 桩区两态（有头卡时内联，不遮站点信息）
        P.NoticePanel {
            objectName: "detailLoadingLabel"
            visible: page.hasHeader && !page.detailLoaded && !page.detailFailed
            width: parent.width
            height: 120
            glyph: "⏳"
            title: "正在加载充电桩列表…"
            description: "详情桥补全后展示桩位与预约入口"
            actionText: ""
        }
        P.NoticePanel {
            objectName: "detailErrorNotice"
            visible: page.hasHeader && page.detailFailed
            width: parent.width
            height: 120
            glyph: "⚠️"
            title: "充电桩列表加载失败"
            description: page.failMessage
            actionText: "重试"
            onActionTriggered: page.fetch()
        }

        Text {
            objectName: "detailChargerSummaryLabel"
            visible: page.detailLoaded
            width: parent.width
            wrapMode: Text.WordWrap      // NoWrap 长标注会溢出裁字
            text: (chargers.length > 0
                   ? "充电桩（空闲 " + availableCount() + " / 共 " + chargers.length + "）"
                   : "充电桩")
                  + (page.demo ? " · 演示数据（详情桥未就绪，接入后自动替换）" : "")
            font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink
        }

        // 桩列表（故障红框 = 原属性选择器的绑定化）
        ListView {
            id: chargerList
            objectName: "chargerList"
            width: parent.width
            height: parent.height - y
            clip: true
            spacing: P.Style.spaceSm
            model: page.chargers
            visible: chargers.length > 0
            delegate: P.Card {
                objectName: "chargerCard"
                width: chargerList.width
                height: 96
                border.color: String(modelData.status).toLowerCase() === "fault"
                               ? P.Style.danger : P.Style.line
                border.width: String(modelData.status).toLowerCase() === "fault" ? 2 : 1
                Column {
                    width: parent.width
                    spacing: P.Style.spaceXs
                    Row {
                        width: parent.width
                        Text {
                            width: parent.width - 96
                            text: modelData.code
                            font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink
                        }
                        P.StatusTag {
                            objectName: "chargerStatusTag"
                            anchors.verticalCenter: parent.verticalCenter
                            tone: page.statusTone(modelData.status)
                            text: page.statusText(modelData.status)
                        }
                    }
                    Row {
                        width: parent.width
                        spacing: P.Style.spaceMd
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: (String(modelData.type).toLowerCase() === "fast"
                                   ? "直流快充 " : "交流慢充 ")
                                  + Math.round((modelData.powerWatts || 0) / 1000) + " kW"
                            font.pixelSize: P.Style.fontSm; color: P.Style.muted
                        }
                        Item { width: parent.width - 200; height: 1 }
                        P.ActionButton {
                            objectName: "detailReserveButton"
                            anchors.verticalCenter: parent.verticalCenter
                            variant: "primary"
                            text: "预约"
                            enabled: String(modelData.status).toLowerCase() === "available"
                            onClicked: page.requestReserve(modelData)
                        }
                    }
                }
            }
        }

        // 站点正常但无桩
        P.NoticePanel {
            objectName: "detailChargerEmptyNotice"
            visible: page.detailLoaded && chargers.length === 0
            width: parent.width
            height: 140
            glyph: "🔌"
            title: "该站点暂无充电桩"
            description: "站点信息已展示；桩位尚未录入，暂无法预约或充电。"
            actionText: ""
        }
    }

    // 整页两态：仅当 arg 无头卡信息（深链直达）时才遮全页
    P.NoticePanel {
        objectName: "detailNotice"
        anchors.fill: parent
        visible: !page.hasHeader
        glyph: page.detailFailed ? "⚠️" : "⏳"
        title: page.detailFailed ? "站点详情加载失败" : "正在加载站点详情…"
        description: page.detailFailed ? page.failMessage : ""
        actionText: page.detailFailed ? "返回首页" : ""
        onActionTriggered: { if (App) App.back() }
    }
    P.LoadingOverlay { running: page.detailLoading && !page.detailLoaded }
}
