import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../../platform" as P

// The single live-charging surface. All values are server snapshots; the ring
// is decorative only and never increments energy, time, power or billed money.
Item {
    id: page
    objectName: "chargingHomePage"
    property string route: "charging"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    property int chargingCount: 0
    property int waitingCount: 0
    property var activeOrder: null
    property bool ordersArrived: false
    property var status: null
    property var reservations: []
    property bool ordersRequested: false
    property bool queuedOrders: false
    property string loadError: ""
    property string statusError: ""
    property bool startPending: false
    property bool stopping: false
    property bool leaving: false
    property string pendingCancelId: ""
    property string trackingId: ""
    property bool ownsTracking: false
    property var cancelCandidate: null

    readonly property bool busy: activeOrder !== null
    readonly property bool hasSnapshot: status !== null && status.status === "charging"
    readonly property int seconds: hasSnapshot ? (status.durationSeconds || 0) : 0
    readonly property real kw: hasSnapshot && status.powerKnown ? status.powerWatts / 1000 : -1
    readonly property real kwh: hasSnapshot ? (status.energyWh || 0) / 1000 : 0
    readonly property real yuan: hasSnapshot ? (status.amountCents || 0) / 100 : 0
    readonly property real pageMargin: Math.min(P.Style.spaceXl, width / 20)

    function dur(sec) {
        var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60
        return (h > 0 ? h + ":" : "") + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0")
    }
    function accepts(s) {
        return page.ownsTracking && s && page.trackingId.length > 0 && String(s.id) === page.trackingId
    }
    function track(order) {
        page.activeOrder = order
        const id = order ? String(order.id) : ""
        if (page.trackingId !== id) page.status = null
        if (!id) { page.releaseTracking(); return }
        if (page.ownsTracking && page.trackingId === id) return
        page.trackingId = id
        page.ownsTracking = true
        chargingService.startTracking(id)
    }
    function releaseTracking() {
        if (!page.ownsTracking) return
        page.ownsTracking = false
        if (chargingService) chargingService.stopTracking()
    }
    function refreshAll() {
        page.loadError = ""
        orderService.fetchStatusCounts()
        reservationService.fetchList()
        requestChargingOrders()
    }
    function requestChargingOrders() {
        if (orderService.isFetchingOrders()) { page.queuedOrders = true; return }
        page.queuedOrders = false
        page.ordersRequested = true
        orderService.fetchOrders("charging", 1)
    }
    function startReservation(id) {
        if (page.startPending || page.pendingCancelId.length > 0 || chargingService.isStarting()) return
        targetDialog.reservationId = String(id)
        targetDialog.open()
    }
    function confirmStart(id, targetType, targetValue) {
        if (page.startPending || page.pendingCancelId.length > 0 || chargingService.isStarting()) return
        page.startPending = true
        page.loadError = ""
        chargingService.startChargingWithTarget(String(id), targetType, targetValue)
    }
    function requestCancel(record) {
        if (page.startPending || page.pendingCancelId.length > 0) return
        page.cancelCandidate = record
        cancelDialog.open()
    }
    function confirmCancel() {
        if (!page.cancelCandidate || page.pendingCancelId.length > 0 || page.startPending) return
        page.pendingCancelId = String(page.cancelCandidate.reservationId || page.cancelCandidate.id)
        page.loadError = ""
        reservationService.cancel(page.pendingCancelId)
    }
    function stopCurrentCharging() {
        if (page.stopping || !page.hasSnapshot || !page.ownsTracking) return
        page.stopping = true
        page.statusError = ""
        chargingService.stopCharging()
    }
    function finishCharging(s) {
        if (!page.accepts(s) || page.leaving) return
        page.leaving = true
        page.stopping = false
        page.status = s
        page.activeOrder = null
        page.releaseTracking()
        P.TabCache.charging = null
        if (s.stopReason && s.stopReason.indexOf("TARGET_") === 0 && App)
            App.showToast("已按目标自动停止充电，请确认账单", "success")
        if (App) App.navigate(s.status === "waiting_payment" ? "settlement" : "order", s)
    }
    function targetQuantity(value, type) {
        return type === "AMOUNT" ? "¥" + (value / 100).toFixed(2)
             : type === "ENERGY" ? (value / 1000).toFixed(3) + " 度"
             : page.dur(value)
    }

    Component.onCompleted: {
        if (page.arg && page.arg.id && page.arg.status === "charging") {
            page.ordersArrived = true
            page.track(page.arg)
            // START_CHARGING may carry a full authoritative snapshot.
            if (page.arg.powerKnown !== undefined) page.status = page.arg
        }
        page.refreshAll()
    }
    Component.onDestruction: page.releaseTracking()

    Timer {
        interval: 100; repeat: true; running: page.queuedOrders && !page.leaving
        onTriggered: if (!orderService.isFetchingOrders()) page.requestChargingOrders()
    }
    Connections {
        target: reservationService
        function onListSucceeded(records) {
            page.reservations = records.filter(function(r) {
                return r.status === "active" && Date.parse(r.expiresAtUtc) > Date.now()
            })
        }
        function onListFailed(message) { page.loadError = message }
        function onCancelSucceeded(id) {
            if (String(id) !== page.pendingCancelId) return
            page.pendingCancelId = ""
            page.cancelCandidate = null
            page.refreshAll()
            if (App) App.showToast("预约已取消，充电桩已释放", "success")
        }
        function onCancelFailed(message) {
            if (!page.pendingCancelId.length) return
            page.pendingCancelId = ""
            page.loadError = message
        }
    }
    Connections {
        target: orderService
        function onStatusCountsUpdated(chargingCount, waitingPaymentCount, completedCount) {
            page.chargingCount = chargingCount
            page.waitingCount = waitingPaymentCount
        }
        function onOrdersLoaded(orders, total, hasMore) {
            if (!page.ordersRequested || page.leaving) return
            page.ordersRequested = false
            page.ordersArrived = true
            const selected = orders.filter(function(o) { return String(o.id) === page.trackingId })
            // A server tick may finish the order before this list refresh
            // arrives. Keep ownership until its terminal status is read, or
            // the queued STOP snapshot would be ignored and settlement lost.
            if (!selected.length && !orders.length && page.ownsTracking && page.trackingId.length) {
                chargingService.fetchStatusNow()
                if (pull.refreshing) pull.setRefreshing(false)
                return
            }
            page.track(selected.length ? selected[0] : orders.length ? orders[0] : null)
            if (pull.refreshing) pull.setRefreshing(false)
        }
        function onOperationFailed(type, code, message) {
            if (type !== "GET_ORDERS" || !page.ordersRequested) return
            page.ordersRequested = false
            page.ordersArrived = true
            page.loadError = message
            if (pull.refreshing) pull.setRefreshing(false)
        }
    }
    Connections {
        target: chargingService
        function onStartCompleted(s) { page.startPending = false }
        function onStatusLoaded(s) {
            if (!page.accepts(s) || page.leaving) return
            page.statusError = ""
            if (s.status === "waiting_payment" || s.status === "completed") {
                page.finishCharging(s)
                return
            }
            if (s.status === "charging") page.status = s
        }
        function onStopCompleted(s) {
            if (!page.accepts(s)) return
            if (App) App.showToast("已停止充电，请确认支付", "success")
            page.finishCharging(s)
        }
        function onOperationFailed(type, code, message) {
            if (type === "START_CHARGING" && page.startPending) {
                page.startPending = false
                page.refreshAll()
                page.loadError = message
                if (App) App.showToast("启动失败：" + message, "danger")
            } else if (type === "STOP_CHARGING" && page.stopping) {
                page.stopping = false
                page.statusError = "停止失败：" + message + "。请重试"
            } else if (type === "GET_CHARGING_STATUS" && page.ownsTracking) {
                page.statusError = "状态更新失败，当前为上次数据：" + message
            }
        }
    }

    Rectangle { anchors.fill: parent; color: P.Style.bg }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: page.pageMargin
        spacing: P.Style.spaceMd
        RowLayout {
            Layout.fillWidth: true
            Text { text: "充电"; color: P.Style.ink; font.pixelSize: P.Style.fontXl; Layout.fillWidth: true }
            P.ActionButton {
                objectName: "chargingRefreshButton"
                text: "刷新"; variant: "ghost"
                enabled: !page.stopping && !page.startPending && !page.pendingCancelId.length
                onClicked: page.refreshAll()
            }
        }
        P.PullToRefreshArea {
            id: pull
            objectName: "uiChargingPull"
            Layout.fillWidth: true
            Layout.fillHeight: true
            pullEnabled: !page.stopping && !page.startPending && !page.pendingCancelId.length
            onRefreshRequested: page.refreshAll()
            Column {
                width: pull.width
                spacing: P.Style.spaceMd
                Text {
                    width: parent.width; wrapMode: Text.Wrap
                    visible: page.loadError.length > 0
                    text: page.loadError; color: P.Style.danger; font.pixelSize: P.Style.fontSm
                }
                Repeater {
                    model: page.reservations
                    delegate: P.Card {
                        width: parent.width
                        Column {
                            width: parent.width; spacing: P.Style.spaceSm
                            Text {
                                width: parent.width; wrapMode: Text.Wrap
                                text: (modelData.stationName || "充电站") + " · 已预约"
                                color: P.Style.ink; font.pixelSize: P.Style.fontMd; font.bold: true
                            }
                            Text {
                                width: parent.width; wrapMode: Text.Wrap
                                text: "桩号 " + (modelData.chargerCode || "—") + " · 请在预约有效期内开始"
                                color: P.Style.muted; font.pixelSize: P.Style.fontSm
                            }
                            Flow {
                                width: parent.width; spacing: P.Style.spaceSm
                                P.ActionButton {
                                    objectName: "startReservationButton"
                                    text: page.startPending ? "正在启动…" : "开始充电"; variant: "primary"
                                    enabled: !page.startPending && !page.pendingCancelId.length
                                    onClicked: page.startReservation(modelData.reservationId || modelData.id)
                                }
                                P.ActionButton {
                                    objectName: "chargingCancelReservationButton"
                                    text: page.pendingCancelId === String(modelData.reservationId || modelData.id)
                                          ? "取消中…" : "取消预约"
                                    variant: "secondary"
                                    enabled: !page.startPending && !page.pendingCancelId.length
                                    onClicked: page.requestCancel(modelData)
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "uiChargingHero"
                    width: parent.width
                    height: heroContent.implicitHeight + 2 * P.Style.spaceLg
                    radius: P.Style.radiusLg
                    color: page.busy ? "#00AC83" : P.Style.surface
                    border.width: page.busy ? 0 : 1
                    border.color: P.Style.line
                    gradient: page.busy ? chargingGradient : null
                    Gradient {
                        id: chargingGradient
                        GradientStop { position: 0; color: "#00A77C" }
                        GradientStop { position: 1; color: "#00C898" }
                    }
                    Column {
                        id: heroContent
                        x: P.Style.spaceLg; y: P.Style.spaceLg
                        width: parent.width - 2 * P.Style.spaceLg
                        spacing: P.Style.spaceLg
                        Column {
                            width: parent.width; spacing: P.Style.spaceXs
                            RowLayout {
                                width: parent.width
                                Text {
                                    objectName: "uiHeroTitle"
                                    Layout.fillWidth: true; wrapMode: Text.Wrap
                                    text: page.busy ? "正在充电" : page.ordersArrived ? "暂无进行中的充电" : "正在加载…"
                                    font.pixelSize: P.Style.fontLg2; font.bold: true
                                    color: page.busy ? "#FFFFFF" : P.Style.ink
                                }
                                Text {
                                    visible: page.busy
                                    text: "● 充电中"; font.pixelSize: P.Style.fontXs; color: "#E5FFF5"
                                }
                            }
                            Text {
                                width: parent.width; wrapMode: Text.Wrap
                                text: page.busy
                                      ? (page.activeOrder.stationName || (page.status && page.status.stationName) || "充电站")
                                        + " · 桩号 " + (page.activeOrder.chargerCode || (page.status && page.status.chargerCode) || "—")
                                      : "预约后点击开始充电，实时状态会显示在这里"
                                font.pixelSize: P.Style.fontSm
                                color: page.busy ? "#E5FFF5" : P.Style.muted
                            }
                        }
                        RowLayout {
                            width: parent.width
                            visible: page.busy
                            spacing: P.Style.spaceMd
                            Item { Layout.fillWidth: true }
                            Canvas {
                                objectName: "chargingPulse"
                                Layout.preferredWidth: Math.min(56 * P.Style.fontScaleFactor, heroContent.width / 5)
                                Layout.preferredHeight: width
                                property real phase: 0
                                onPhaseChanged: requestPaint()
                                onWidthChanged: requestPaint()
                                onPaint: {
                                    const ctx = getContext("2d"), c = width / 2
                                    ctx.clearRect(0, 0, width, height)
                                    ctx.lineWidth = width / 14
                                    ctx.strokeStyle = "rgba(255,255,255,0.26)"
                                    ctx.beginPath(); ctx.arc(c, c, width * 0.4, 0, 2 * Math.PI); ctx.stroke()
                                    ctx.strokeStyle = "white"; ctx.lineCap = "round"
                                    const a = phase * 2 * Math.PI - Math.PI / 2
                                    ctx.beginPath(); ctx.arc(c, c, width * 0.4, a, a + 1.75); ctx.stroke()
                                    ctx.fillStyle = "#FFFFFF"
                                    ctx.beginPath()
                                    ctx.moveTo(width * 0.52, width * 0.23)
                                    ctx.lineTo(width * 0.35, width * 0.54)
                                    ctx.lineTo(width * 0.5, width * 0.54)
                                    ctx.lineTo(width * 0.47, width * 0.77)
                                    ctx.lineTo(width * 0.67, width * 0.44)
                                    ctx.lineTo(width * 0.52, width * 0.44)
                                    ctx.closePath(); ctx.fill()
                                }
                                NumberAnimation on phase {
                                    running: page.busy && page.visible && P.Style.motionEnabled
                                    from: 0; to: 1; duration: 1600; loops: Animation.Infinite
                                }
                            }
                            Column {
                                Layout.maximumWidth: heroContent.width * 0.65
                                spacing: 2
                                Text {
                                    objectName: "chargingPowerValue"
                                    width: Math.min(implicitWidth, heroContent.width * 0.65)
                                    text: (page.kw >= 0 ? page.kw.toFixed(1) : "—") + " kW"
                                    font.pixelSize: P.Style.fontPower; font.bold: true
                                    minimumPixelSize: P.Style.fontLg; fontSizeMode: Text.Fit
                                    color: "#FFFFFF"
                                }
                                Text {
                                    text: "实时充电功率"; color: "#E5FFF5"
                                    font.pixelSize: P.Style.fontSm
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }
                        Rectangle { visible: page.busy; width: parent.width; height: 1; color: "#40FFFFFF" }
                        Grid {
                            id: chargingStatsGrid
                            objectName: "chargingStats"
                            visible: page.busy
                            width: parent.width
                            columns: width < 300 * P.Style.fontScaleFactor ? 1 : 3
                            rowSpacing: P.Style.spaceMd
                            Repeater {
                                model: [
                                    { value: page.hasSnapshot ? page.kwh.toFixed(2) + " kWh" : "—", label: "已充电量" },
                                    { value: page.hasSnapshot ? page.dur(page.seconds) : "—", label: "充电时长" },
                                    { value: page.hasSnapshot ? "¥" + page.yuan.toFixed(2) : "—", label: "预估费用" }
                                ]
                                delegate: Column {
                                    width: chargingStatsGrid.width / chargingStatsGrid.columns
                                    spacing: P.Style.spaceXs
                                    Text {
                                        width: parent.width; horizontalAlignment: Text.AlignHCenter
                                        text: modelData.value; color: "#FFFFFF"
                                        font.pixelSize: P.Style.fontStat; font.bold: true
                                        fontSizeMode: Text.Fit; minimumPixelSize: P.Style.fontSm
                                    }
                                    Text {
                                        width: parent.width; horizontalAlignment: Text.AlignHCenter
                                        text: modelData.label; color: "#E5FFF5"; font.pixelSize: P.Style.fontSm
                                    }
                                }
                            }
                        }
                        Column {
                            objectName: "chargingTargetProgress"
                            width: parent.width; spacing: P.Style.spaceSm
                            visible: page.hasSnapshot && !!page.status.target && page.status.target.type !== undefined
                            property var target: visible ? page.status.target : ({})
                            Text {
                                width: parent.width; wrapMode: Text.Wrap; color: "#FFFFFF"
                                font.pixelSize: P.Style.fontMd; font.bold: true
                                text: "充电目标 · " + page.targetQuantity(parent.target.value || 0, parent.target.type)
                            }
                            Rectangle {
                                width: parent.width; height: 8; radius: 4; color: "#40FFFFFF"
                                Rectangle {
                                    width: parent.width * Math.min(1, Math.max(0, (parent.parent.target.progressPercent || 0) / 100))
                                    height: parent.height; radius: parent.radius; color: "white"
                                }
                            }
                            Text {
                                width: parent.width; wrapMode: Text.Wrap; color: "#E5FFF5"; font.pixelSize: P.Style.fontSm
                                text: "已完成 " + page.targetQuantity(parent.target.completedValue || 0, parent.target.type)
                                      + " · 剩余 " + page.targetQuantity(parent.target.remainingValue || 0, parent.target.type)
                            }
                        }
                    }
                }

                Text {
                    width: parent.width; wrapMode: Text.Wrap
                    visible: page.statusError.length > 0
                    text: page.statusError; color: P.Style.danger; font.pixelSize: P.Style.fontSm
                }
                Text {
                    width: parent.width; wrapMode: Text.Wrap; visible: page.busy
                    text: page.hasSnapshot ? "数据由服务器定期更新，最终费用以停止充电后的结算为准。" : "正在读取充电桩状态…"
                    color: P.Style.muted; font.pixelSize: P.Style.fontSm
                }
                P.ActionButton {
                    objectName: "stopChargingBar"
                    visible: page.busy; width: parent.width
                    variant: "danger"; text: page.stopping ? "正在停止…" : "停止充电并结算"
                    enabled: page.hasSnapshot && !page.stopping
                    onClicked: page.stopCurrentCharging()
                }
                P.Card {
                    objectName: "rechargePendingNotice"
                    visible: page.waitingCount > 0
                    width: parent.width
                    Column {
                        width: parent.width; spacing: P.Style.spaceSm
                        Text {
                            width: parent.width; wrapMode: Text.Wrap
                            text: "有 " + page.waitingCount + " 笔待支付订单"
                            font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink
                        }
                        P.ActionButton {
                            width: parent.width
                            text: "查看待支付订单"; variant: "secondary"
                            onClicked: if (App) App.navigate("order", "waiting_payment")
                        }
                    }
                }
                P.ActionButton {
                    objectName: "simulatedScanButton"
                    visible: typeof CHARGING_CHANNEL !== "undefined" && CHARGING_CHANNEL === "mock"
                    width: parent.width; variant: "secondary"; text: "扫码充电"
                    onClicked: if (App) App.navigate("scan")
                }
                Item { width: 1; height: P.Style.spaceSm }
            }
        }
    }
    P.ChargingTargetDialog {
        id: targetDialog
        onConfirmed: function(reservationId, targetType, targetValue) {
            page.confirmStart(reservationId, targetType, targetValue)
        }
    }
    Dialog {
        id: cancelDialog
        objectName: "chargingCancelDialog"
        anchors.centerIn: parent
        width: Math.min(page.width - 2 * page.pageMargin, 380)
        modal: true; title: "取消预约"
        padding: P.Style.spaceLg
        background: Rectangle { color: P.Style.surface; radius: P.Style.radiusLg; border.color: P.Style.line }
        header: Text { text: "取消预约"; padding: P.Style.spaceLg; color: P.Style.ink; font.pixelSize: P.Style.fontLg; font.bold: true }
        contentItem: Column {
            spacing: P.Style.spaceMd
            Text {
                width: parent.width; wrapMode: Text.Wrap; color: P.Style.ink; font.pixelSize: P.Style.fontMd
                text: "确认取消本次预约？取消后将释放充电桩，需要时可重新预约。"
            }
            P.ActionButton {
                objectName: "confirmChargingCancelButton"
                width: parent.width; text: "确认取消"; variant: "danger"
                onClicked: { page.confirmCancel(); cancelDialog.close() }
            }
            P.ActionButton {
                objectName: "keepChargingReservationButton"
                width: parent.width; text: "保留预约"; variant: "secondary"
                onClicked: cancelDialog.close()
            }
        }
    }
}
