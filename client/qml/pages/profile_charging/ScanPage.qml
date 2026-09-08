import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "../station/StationState.js" as StationState

// 扫码充电页（成员3 新页，2026-09-08 批次F）：mock 模拟扫码演示线
// 「选桩码 → 查桩 → 引导预约」。真实摄像头扫码 + 桩码协议未冻结——
// TODO(contract)：届时以隐藏入口替换本页的码列表/手输框（scanSource 属性即
// 预留给真通道的接缝，本轮恒为 "mock"）。
// 查桩走 stationQueryService（search 缓存站点行 + fetchDetailById 反查桩），
// 命中后拼 ReservationConfirmPage 同款 8 字段 arg；准入门（登录/空闲/车辆/
// 名额）与 StationDetailPage.requestReserve 同口径，弹层简化为 toast 引导。
Item {
    id: page
    objectName: "scanPage"
    property string route: "scan"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    readonly property string scanSource: "mock"   // TODO(contract): 真通道 = "camera"
    property var stations: []                     // search 结果缓存（列表行 map）
    property string phase: "idle"                 // idle|searching|found|miss
    property bool reqActive: false
    property bool loadedOnce: false
    property var foundStation: ({})
    property var foundCharger: ({})
    property string failMessage: ""
    // 待匹配 payload（fetchDetailById 回执里裁决）。
    property string pendingStationId: ""
    property string pendingChargerId: ""
    property string codeInput: ""
    // 预约 arg 快照（reserveNow 组包后落这里，按钮跳转与测试都读这一份）。
    property var reserveArg: ({})

    function call(target, fn, args) {
        try { return target[fn].apply(target, args) } catch (e) { return undefined }
    }
    function vehicleCount() {
        const v = call(settingsService, "vehicles", [])
        if (v !== undefined) return v.length
        return StationState.vehicles.length   // 桥缺位期读设置页本地通道
    }
    function activeReservationCount() {
        // TODO(contract): reservationService.activeReservationCount() 同 StationDetail 口径。
        const n = call(reservationService, "activeReservationCount", [])
        return (typeof n === "number") ? n : -1
    }
    function availableChargers(chargers) {
        const list = []
        for (const c of chargers)
            if (String(c.status).toLowerCase() === "available") list.push(c)
        return list
    }

    function loadStations() {
        if (page.reqActive) return
        page.reqActive = true
        stationQueryService.search("")
    }
    // 模拟扫描一路站点码（quick-pick）：payload = 列表行。
    function scanStation(index) {
        const s = page.stations[index]
        if (!s || page.reqActive) return
        page.pendingStationId = String(s.id)
        page.pendingChargerId = ""            // 未指定：detail 里取首台空闲桩
        page.phase = "searching"
        page.reqActive = true
        stationQueryService.fetchDetailById(s.id, s.distanceMeters || -1)
    }
    // 手输码：兼容 "CHG://<stationId>/<chargerId>" 与裸 stationId。
    function scanCode(text) {
        if (page.reqActive) return
        const raw = String(text || "").trim()
        let stationId = ""
        let chargerId = ""
        const m = raw.match(/^chg:\/\/\s*(\d+)(?:\/(\d+))?/i)
        if (m) { stationId = m[1]; chargerId = m[2] || "" }
        else if (/^\d+$/.test(raw)) stationId = raw
        if (!stationId) {
            page.phase = "miss"
            page.failMessage = "二维码内容无法识别（demo 码形如 CHG://1/2）"
            return
        }
        page.pendingStationId = stationId
        page.pendingChargerId = chargerId
        page.phase = "searching"
        page.reqActive = true
        const cached = page.stations.find(function (s) { return String(s.id) === stationId })
        stationQueryService.fetchDetailById(stationId, cached ? (cached.distanceMeters || -1) : -1)
    }
    function reserveNow() {
        if (page.phase !== "found") return
        if (!App || !App.loggedIn) {
            if (App) { App.showToast("请先登录再发起预约", "warning"); App.navigate("login") }
            return
        }
        if (String(page.foundCharger.status).toLowerCase() !== "available") {
            if (App) App.showToast("仅空闲充电桩可预约", "warning")
            return
        }
        if (vehicleCount() === 0) {
            if (App) { App.showToast("请先在「设置 - 车辆管理」添加车辆", "warning")
                       App.navigate("settings") }
            return
        }
        const act = activeReservationCount()
        if (act >= 0 && act >= vehicleCount()) {
            if (App) App.showToast("可预约名额已全部占用（名额 = 车辆数）", "warning")
            return
        }
        const s = page.foundStation
        const c = page.foundCharger
        page.reserveArg = {
            stationId: s.id, stationName: s.name,
            priceCentsPerKwh: s.priceCentsPerKwh, distanceMeters: s.distanceMeters,
            chargerId: c.id, chargerCode: c.code,
            chargerType: c.type, chargerPowerWatts: c.powerWatts }
        if (App) App.navigate("reservation_confirm", page.reserveArg)
    }
    function statusText(st) {
        return ({ available: "空闲", charging: "占用·充电中", reserved: "占用·已预约",
                  fault: "故障", offline: "离线" })[String(st).toLowerCase()] || "未知"
    }
    function statusTone(st) {
        return ({ available: "success", charging: "warning", reserved: "info",
                  fault: "danger", offline: "neutral" })[String(st).toLowerCase()] || "neutral"
    }

    Connections {
        target: stationQueryService
        function onQueryStarted() { page.reqActive = true }
        function onQuerySucceeded(stations) {
            page.stations = stations || []
            page.reqActive = false
            page.loadedOnce = true
        }
        function onQueryFailed(message) {
            page.reqActive = false
            page.loadedOnce = true
            page.failMessage = message
        }
        function onDetailSucceeded(detail) {
            page.reqActive = false
            const src = (detail && detail.station) ? detail.station : (detail || {})
            const chargers = (detail && detail.chargers) || []
            let picked = null
            if (page.pendingChargerId) {
                for (const c of chargers)
                    if (String(c.id) === page.pendingChargerId) { picked = c; break }
            } else {
                const free = availableChargers(chargers)
                picked = free.length > 0 ? free[0] : null
            }
            if (!picked) {
                page.phase = "miss"
                page.failMessage = page.pendingChargerId
                    ? "码上的充电桩不存在或已下线"
                    : "该站点暂无空闲充电桩，换个码再试"
                return
            }
            page.foundStation = Object.assign({}, src,
                { id: page.pendingStationId || src.id,
                  name: src.name || "",
                  distanceMeters: (detail && detail.distanceMeters !== undefined)
                                     ? detail.distanceMeters : src.distanceMeters })
            page.foundCharger = picked
            page.phase = "found"
        }
        function onDetailFailed(message) {
            page.reqActive = false
            page.phase = "miss"
            page.failMessage = message
        }
    }
    Component.onCompleted: loadStations()

    P.PullToRefreshArea {
        id: listScroll
        objectName: "uiScanStack"
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacingHint: P.Style.spaceMd
        visible: page.phase !== "found" && page.phase !== "miss"
        onRefreshRequested: loadStations()

        Item {
            width: listScroll.width
            height: Math.round(40 * P.Style.fontScaleFactor)   // 字号档呼吸（批次F 补）
            Text {
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                objectName: "uiScanTitle"
                text: "扫码充电"
                font.pixelSize: P.Style.fontXl; font.weight: Font.Bold; color: P.Style.ink
            }
            Text {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                objectName: "uiScanSourceTag"
                text: "模拟通道"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
        }

        // ---- 取景框（扫描动画呼吸条）----
        Rectangle {
            objectName: "uiScanFrame"
            width: 200; height: 200
            anchors.horizontalCenter: parent.horizontalCenter
            radius: P.Style.radiusLg
            color: P.Style.ghost
            border.width: 2
            border.color: P.Style.brand
            Rectangle {
                objectName: "uiScanLaser"
                width: parent.width - 24; height: 3
                radius: 2
                anchors.horizontalCenter: parent.horizontalCenter
                color: P.Style.brandBright
                y: 20
                SequentialAnimation on y {
                    loops: Animation.Infinite
                    running: P.Style.motionEnabled
                    NumberAnimation { to: 172; duration: 1200; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 20; duration: 1200; easing.type: Easing.InOutSine }
                }
            }
            Text {
                anchors.centerIn: parent
                text: page.reqActive ? "识别中…" : "📷"
                font.pixelSize: page.reqActive ? P.Style.fontLg : 48
                color: P.Style.muted
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "对准桩身二维码（本轮为模拟演示）"
            font.pixelSize: P.Style.fontSm; color: P.Style.muted
        }

        // ---- 手输码 ----
        Row {
            width: listScroll.width
            spacing: P.Style.spaceSm
            TextField {
                id: codeField
                objectName: "uiScanCodeEdit"
                width: parent.width - scanGoButton.implicitWidth - parent.spacing
                placeholderText: "粘贴桩码，如 CHG://1/2"
                text: page.codeInput
                onTextChanged: page.codeInput = text
            }
            P.ActionButton {
                id: scanGoButton
                objectName: "uiScanGoButton"
                anchors.verticalCenter: parent.verticalCenter
                variant: "primary"
                text: "扫描"
                enabled: !page.reqActive
                onClicked: page.scanCode(page.codeInput)
            }
        }

        // ---- 模拟码速选（search 回来的每站一张）----
        Text {
            width: listScroll.width
            text: "或点选模拟桩码"
            font.pixelSize: P.Style.fontSm; color: P.Style.muted
            visible: page.stations.length > 0
        }
        Repeater {
            model: page.stations
            Rectangle {
                objectName: "uiScanCodeCard"
                width: listScroll.width
                height: Math.max(Math.round(56 * P.Style.fontScaleFactor),
                                 codeCol.implicitHeight + 16)
                radius: P.Style.radiusLg
                color: P.Style.surface
                border.width: 1
                border.color: P.Style.line
                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 16; anchors.rightMargin: 16
                    spacing: P.Style.spaceMd
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "🔳"; font.pixelSize: 20
                    }
                    Column {
                        id: codeCol
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            text: modelData.name || ("站点 " + modelData.id)
                            font.pixelSize: P.Style.fontLg2; font.weight: Font.DemiBold
                            color: P.Style.ink
                        }
                        Text {
                            text: "桩码 CHG://" + modelData.id + "/·（取首台空闲桩）"
                            font.pixelSize: P.Style.fontSm; color: P.Style.faint
                        }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.scanStation(index)
                }
            }
        }
    }

    // ---- 扫描结果态（found/miss 覆盖在取景页上）----
    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacing: P.Style.spaceLg
        visible: page.phase === "found" || page.phase === "miss"

        Item {
            width: parent.width
            height: 40
            Text {
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                objectName: "uiScanResultTitle"
                text: page.phase === "found" ? "扫描成功" : "未找到充电桩"
                font.pixelSize: P.Style.fontXl; font.weight: Font.Bold
                color: page.phase === "found" ? P.Style.brandDeep : P.Style.danger
            }
        }

        P.Card {
            objectName: "uiScanResultCard"
            width: parent.width
            visible: page.phase === "found"
            Column {
                width: parent.width
                spacing: P.Style.spaceSm
                Row {
                    width: parent.width
                    Text {
                        objectName: "uiScanStationName"
                        width: parent.width - 96
                        text: page.foundStation.name || "—"
                        font.pixelSize: P.Style.fontXl; font.weight: Font.Bold
                        color: P.Style.ink; elide: Text.ElideRight
                    }
                    P.StatusTag {
                        anchors.verticalCenter: parent.verticalCenter
                        objectName: "uiScanChargerStatusTag"
                        tone: page.statusTone(page.foundCharger.status)
                        text: page.statusText(page.foundCharger.status)
                    }
                }
                Text {
                    objectName: "uiScanChargerCode"
                    text: "桩号 " + (page.foundCharger.code || "—") + " · "
                        + (String(page.foundCharger.type).toLowerCase() === "fast"
                           ? "直流快充 " : "交流慢充 ")
                        + Math.round((page.foundCharger.powerWatts || 0) / 1000) + " kW"
                    font.pixelSize: P.Style.fontMd; color: P.Style.muted
                }
                Text {
                    text: "电价 ¥" + ((page.foundStation.priceCentsPerKwh || 0) / 100).toFixed(2)
                        + " /kWh"
                    font.pixelSize: P.Style.fontMd; color: P.Style.brandDeep
                }
            }
        }

        P.NoticePanel {
            objectName: "uiScanMissNotice"
            width: parent.width
            height: 160
            visible: page.phase === "miss"
            glyph: "🔍"
            title: "这个码没认出来"
            description: page.failMessage
            actionText: ""
        }

        P.ActionButton {
            objectName: "uiScanReserveButton"
            visible: page.phase === "found"
            width: parent.width
            variant: "primary"
            text: "去预约"
            onClicked: page.reserveNow()
        }
        P.ActionButton {
            objectName: "uiScanRedoButton"
            text: "重新扫描"
            onClicked: { page.phase = "idle"; page.failMessage = ""; page.codeInput = "" }
        }
    }
}
