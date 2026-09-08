import QtQuick
import "../../platform" as P

// QML twin of widgets ChargingHomePage (tab route: "charging").
// Hero follows the EV-app "charging session card" pattern (Electrix /
// ArusEV / ChargePoint conventions): status header row → station meta →
// ONE focal live number (34px power, QSS powerValue) riding the sweeping
// pulse ring → hairline → three-column stats (20px values, QSS statValue)
// with bottom-aligned units. Empty state stays centered & calm; the glyph
// breathes in both states (charging_pulse breath_: 1400ms InOutSine,
// alpha .38→1, scale .92→1 — "空态不冷清").
// Anti-flash: skeleton until first data lands; P.TabCache keeps the last
// snapshot so tab revisits render stale-then-refresh silently.
Item {
    id: page
    objectName: "chargingHomePage"
    property string route: "charging"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property int chargingCount: 0
    property int waitingCount: 0
    property var activeOrder: null       // first charging order summary map
    property bool ordersArrived: false   // gates the hero: never flash 空态
    property var status: null            // latest GET_CHARGING_STATUS map
    property var reservations: []
    property bool ordersRequested: false
    property bool queuedOrders: false
    property string loadError: ""
    property bool startPending: false
    function startReservation(id) {
        if (page.startPending || chargingService.isStarting()) return
        page.startPending = true
        page.loadError = ""
        chargingService.startCharging(String(id))
    }
    property int seconds: 0
    property real breath: 1.0            // charging_pulse breath_ parity

    readonly property bool busy: chargingCount > 0 && activeOrder !== null
    readonly property real kw: status && status.powerKnown
                               ? status.powerWatts / 1000 : -1
    readonly property real kwh: status ? (status.energyWh || 0) / 1000 : 0
    readonly property real yuan: status ? (status.amountCents || 0) / 100 : 0

    function dur(sec) {
        var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60
        return (h > 0 ? h + ":" : "") + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0")
    }

    // ---- stale-while-revisit boot (before first paint) ----
    Component.onCompleted: {
        const c = P.TabCache.charging
        if (c) {
            page.ordersArrived = true
            page.chargingCount = c.chargingCount
            page.waitingCount = c.waitingCount
            page.activeOrder = c.activeOrder
            page.seconds = (c.activeOrder && c.activeOrder.durationSeconds) || 0
        }
        refreshAll()
        if (page.activeOrder) chargingService.startTracking(String(page.activeOrder.id))
    }
    Component.onDestruction: {
        if (chargingService) chargingService.stopTracking()   // teardown-safe
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
    Timer {
        interval: 100; repeat: true; running: page.queuedOrders
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
    }

    // ---- data plumbing ----
    Connections {
        target: orderService
        function onStatusCountsUpdated(chargingCount, waitingPaymentCount, completedCount) {
            page.chargingCount = chargingCount
            page.waitingCount = waitingPaymentCount
        }
        function onOrdersLoaded(orders, total, hasMore) {
            if (!page.ordersRequested) return
            page.ordersRequested = false
            page.ordersArrived = true
            page.activeOrder = orders.length > 0 ? orders[0] : null
            if (page.activeOrder)
                chargingService.startTracking(String(page.activeOrder.id))
            else
                chargingService.stopTracking()
            if (pull.refreshing) pull.setRefreshing(false)
            P.TabCache.charging = {
                chargingCount: page.chargingCount, waitingCount: page.waitingCount,
                activeOrder: page.activeOrder
            }
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
        function onOperationFailed(type, code, message) {
            if (type !== "START_CHARGING" || !page.startPending) return
            page.startPending = false
            page.refreshAll()
            page.loadError = message
            if (App) App.showToast("启动失败：" + message, "danger")
        }
        function onStatusLoaded(s) {
            page.status = s
            page.seconds = s.durationSeconds || 0
        }
    }
    // Local 1s tick between server pushes so the timer always moves.
    Timer {
        running: page.busy && P.Style.motionEnabled
        repeat: true; interval: 1000
        onTriggered: page.seconds += 1
    }
    // breathAnim_ port: 0.15→1→0.15, InOutSine, 1400ms, infinite.
    SequentialAnimation on breath {
        running: page.ordersArrived && P.Style.motionEnabled
        loops: Animation.Infinite
        NumberAnimation { to: 0.15; duration: P.Style.durBreathe / 2
            easing.type: Easing.InOutSine }
        NumberAnimation { to: 1.0; duration: P.Style.durBreathe / 2
            easing.type: Easing.InOutSine }
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacing: P.Style.spaceLg

        Text { text: "充电"; font.pixelSize: P.Style.fontXl; color: P.Style.ink }

        P.PullToRefreshArea {
            id: pull
            objectName: "uiChargingPull"
            width: parent.width
            height: parent.height - y
            onRefreshRequested: refreshAll()

            Column {
                width: pull.width
                spacing: P.Style.spaceLg

                // ================== session hero ==================
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    visible: page.loadError.length > 0
                    text: page.loadError + "（可下拉重试）"; color: P.Style.danger
                }
                Repeater {
                    model: page.reservations
                    delegate: P.Card {
                        width: parent.width
                        Column {
                            width: parent.width; spacing: P.Style.spaceSm
                            Text {
                                width: parent.width; wrapMode: Text.WordWrap
                                text: (modelData.stationName || "充电站") + " · "
                                      + (modelData.chargerCode || "") + " · 已预约"
                                color: P.Style.ink
                            }
                            P.ActionButton {
                                objectName: "startReservationButton"
                                text: "开始充电"; variant: "primary"
                                enabled: !page.startPending
                                onClicked: page.startReservation(modelData.reservationId || modelData.id)
                            }
                        }
                    }
                }
                Rectangle {
                    objectName: "uiChargingHero"
                    width: parent.width
                    height: 250
                    radius: P.Style.radiusLg
                    // Qt6.2: GradientStop bindings never re-evaluate → two
                    // constant layers switched by visible.
                    gradient: Gradient {
                        orientation: Gradient.Vertical
                        GradientStop { position: 0.0; color: P.Style.brandSoft }
                        GradientStop { position: 1.0; color: P.Style.infoSoft }
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: P.Style.radiusLg
                        // 注意：用 opacity 不用 visible——6.2 offscreen 实测
                        // visible 切换不刷新 gradient 纹理（白字浮浅底发白）。
                        opacity: page.busy ? 1.0 : 0.0
                        Behavior on opacity {
                            NumberAnimation { duration: P.Style.motionEnabled ? 250 : 0 }
                        }
                        gradient: Gradient {
                            orientation: Gradient.Vertical
                            GradientStop { position: 0.0; color: P.Style.heroFrom }
                            GradientStop { position: 1.0; color: P.Style.heroTo }
                        }
                    }

                    // --- first-load skeleton (equal height, no 空态 flash) ---
                    Text {
                        anchors.centerIn: parent
                        visible: !page.ordersArrived
                        text: "正在加载充电状态…"
                        font.pixelSize: P.Style.fontMd; color: P.Style.muted
                    }

                    // ================== busy layout ==================
                    // header row
                    Text {
                        id: heroTitle; objectName: "uiHeroTitle"
                        anchors.left: parent.left; anchors.top: parent.top
                        anchors.leftMargin: 22; anchors.topMargin: 18
                        visible: page.ordersArrived && page.busy
                        text: "正在充电"
                        font.pixelSize: P.Style.fontLg2; font.bold: true
                        color: P.Style.surface
                    }
                    Rectangle {
                        id: livePill
                        anchors.right: parent.right; anchors.top: parent.top
                        anchors.rightMargin: 22; anchors.topMargin: 16
                        visible: page.ordersArrived && page.busy
                        width: livePillText.implicitWidth + 22; height: 24
                        radius: 12
                        color: "#33FFFFFF"
                        Row {
                            anchors.centerIn: parent
                            spacing: 5
                            Rectangle {
                                width: 6; height: 6; radius: 3
                                anchors.verticalCenter: parent.verticalCenter
                                color: P.Style.surface
                                SequentialAnimation on opacity {
                                    running: page.busy && P.Style.motionEnabled
                                    loops: Animation.Infinite
                                    NumberAnimation { to: 0.35; duration: P.Style.durBreathe / 2 }
                                    NumberAnimation { to: 1.0; duration: P.Style.durBreathe / 2 }
                                }
                            }
                            Text {
                                id: livePillText
                                anchors.verticalCenter: parent.verticalCenter
                                text: "充电中"
                                font.pixelSize: P.Style.fontXs
                                font.weight: Font.DemiBold; color: P.Style.surface
                            }
                        }
                    }
                    Text {
                        id: heroMeta
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.top: heroTitle.bottom
                        anchors.leftMargin: 22; anchors.rightMargin: 22
                        anchors.topMargin: 5
                        visible: page.ordersArrived && page.busy
                        elide: Text.ElideRight
                        text: (page.activeOrder && page.activeOrder.stationName
                               ? page.activeOrder.stationName : "充电站")
                              + " · 桩号 " + ((page.activeOrder && page.activeOrder.chargerCode) || "--")
                        font.pixelSize: P.Style.fontSm; color: P.Style.heroPhone
                    }

                    // focal: pulse ring + 34px power
                    Row {
                        id: mainRow
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: heroMeta.bottom
                        anchors.topMargin: 16
                        visible: page.ordersArrived && page.busy
                        spacing: P.Style.spaceMd

                        // ChargingPulse port: 56px, white translucent track on
                        // the green wash + 100° round-cap sweep 1600ms Linear,
                        // vector lightning inside breathing (alpha .38→1,
                        // scale .92→1) — the widgets bolt polygon, same points.
                        Canvas {
                            objectName: "chargingPulse"
                            width: 56; height: 56
                            property real phase: 0.0
                            property real b: page.breath
                            onPhaseChanged: requestPaint()
                            onBChanged: requestPaint()
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                ctx.lineWidth = 4
                                ctx.strokeStyle = "rgba(255,255,255,0.28)"
                                ctx.beginPath()
                                ctx.arc(28, 28, 22, 0, 2 * Math.PI)
                                ctx.stroke()
                                ctx.lineCap = "round"
                                ctx.strokeStyle = "rgba(255,255,255,0.95)"
                                const a0 = (-90 + phase * 360) * Math.PI / 180
                                ctx.beginPath()
                                ctx.arc(28, 28, 22, a0, a0 + 100 * Math.PI / 180)
                                ctx.stroke()
                                const lvl = 0.38 + 0.62 * b
                                const sc = 0.92 + 0.08 * b
                                const bw = 15 * sc, bh = 24 * sc
                                ctx.fillStyle = "rgba(255,255,255," + lvl.toFixed(3) + ")"
                                ctx.beginPath()
                                ctx.moveTo(28 - bw * 0.15, 28 - bh / 2)
                                ctx.lineTo(28 - bw / 2,    28 + bh * 0.12)
                                ctx.lineTo(28 - bw * 0.05, 28 + bh * 0.12)
                                ctx.lineTo(28 + bw * 0.15, 28 + bh / 2)
                                ctx.lineTo(28 + bw / 2,    28 - bh * 0.12)
                                ctx.lineTo(28 + bw * 0.05, 28 - bh * 0.12)
                                ctx.closePath()
                                ctx.fill()
                            }
                            SequentialAnimation on phase {
                                running: P.Style.motionEnabled
                                loops: Animation.Infinite
                                NumberAnimation { from: 0; to: 1
                                    duration: 1600; easing.type: Easing.Linear }
                            }
                        }
                        Item {
                            width: powerCol.implicitWidth
                            height: 56
                            Column {
                                id: powerCol
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.top: parent.top; anchors.topMargin: 4
                                spacing: 2
                                Row {
                                    spacing: 4
                                    Text {
                                        id: powerVal
                                        text: page.kw >= 0 ? page.kw.toFixed(1) : "—"
                                        font.pixelSize: P.Style.fontPower
                                        font.weight: Font.Black; color: P.Style.surface
                                    }
                                    Text {
                                        text: "kW"
                                        font.pixelSize: P.Style.fontSm
                                        font.weight: Font.DemiBold; color: P.Style.heroPhone
                                        height: powerVal.height
                                        verticalAlignment: Text.AlignBottom
                                        bottomPadding: 6
                                    }
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "实时充电功率"
                                    font.pixelSize: P.Style.fontSm; color: P.Style.heroPhone
                                }
                            }
                        }
                    }

                    // hairline + three-column live stats (QSS statValue)
                    Rectangle {
                        id: heroHr
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.bottom: heroStats.top
                        anchors.bottomMargin: 14; anchors.leftMargin: 22; anchors.rightMargin: 22
                        visible: page.ordersArrived && page.busy
                        height: 1; color: "#30FFFFFF"
                    }
                    Row {
                        id: heroStats
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.bottom: parent.bottom; anchors.bottomMargin: 18
                        anchors.leftMargin: 22; anchors.rightMargin: 22
                        visible: page.ordersArrived && page.busy
                        readonly property real colW: (width - 2) / 3
                        Repeater {
                            model: [
                                { val: page.kwh.toFixed(2), unit: "kWh", k: "已充电量" },
                                { val: page.dur(page.seconds), unit: "",  k: "充电时长" },
                                { val: "¥" + page.yuan.toFixed(2), unit: "", k: "预估费用" },
                            ]
                            delegate: Item {
                                width: heroStats.colW; height: 50
                                Column {
                                    anchors.centerIn: parent
                                    spacing: 3
                                    Row {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        spacing: 3
                                        Text {
                                            id: sv
                                            text: modelData.val
                                            font.pixelSize: P.Style.fontStat
                                            font.weight: Font.ExtraBold; color: P.Style.surface
                                        }
                                        Text {
                                            visible: modelData.unit.length > 0
                                            text: modelData.unit
                                            font.pixelSize: P.Style.fontXs
                                            color: P.Style.heroPhone
                                            height: sv.height
                                            verticalAlignment: Text.AlignBottom
                                            bottomPadding: 3
                                        }
                                    }
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: modelData.k
                                        font.pixelSize: P.Style.fontXs; color: P.Style.heroPhone
                                    }
                                }
                            }
                        }
                        // column dividers — Row 会覆盖子项 x，故为 Row 兄弟节点
                        // （见下方 heroDiv1/2），语言同 widgets 卡内白细线。
                    }
                    Repeater {
                        model: 2
                        delegate: Rectangle {
                            width: 1; height: 30
                            x: heroStats.x + (index + 1) * heroStats.colW + index
                            y: heroStats.y - (height - heroStats.height) / 2
                            visible: page.ordersArrived && page.busy
                            color: "#26FFFFFF"
                        }
                    }

                    // ================== empty layout ==================
                    Column {
                        id: emptyCol
                        anchors.centerIn: parent
                        spacing: P.Style.spaceSm
                        visible: page.ordersArrived && !page.busy
                        Text {
                            objectName: "uiHeroGlyph"
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "⚡"
                            font.pixelSize: 40
                            // 空态也呼吸：widgets motion::startBreathing(glyph)
                            // "空态不冷清 —— 在等你的下一单"
                            opacity: 0.38 + 0.62 * page.breath
                            scale: 0.92 + 0.08 * page.breath
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "暂无进行中的充电"
                            font.pixelSize: P.Style.fontLg2; font.weight: Font.DemiBold
                            color: P.Style.ink
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "预约后点击开始充电，在这里查看实时状态"
                            font.pixelSize: P.Style.fontSm; color: P.Style.muted
                        }
                    }
                }

                P.ActionButton {
                    objectName: "viewChargingRunButton"
                    visible: page.busy
                    width: parent.width
                    variant: "primary"
                    text: "查看实时充电"
                    onClicked: if (App && page.activeOrder) App.navigate("charging_run", page.activeOrder)
                }
                P.ActionButton {
                    objectName: "simulatedScanButton"
                    visible: typeof CHARGING_CHANNEL !== "undefined" && CHARGING_CHANNEL === "mock"
                    width: parent.width
                    variant: "secondary"
                    text: "模拟扫码（demo）"
                    // TODO(contract): scan-start protocol undefined; keep demo honest.
                    onClicked: if (App) App.showToast("扫码启动协议未定，走 mock 预约流程（TODO(contract)）", "info")
                }

                // Pending-payment — compact reminder ROW (widgets buildPaymentCard
                // 语义：icon hub + 双行文案 + 小主按钮一行摆完)，不用竖排占位面板。
                Rectangle {
                    objectName: "rechargePendingNotice"
                    visible: page.waitingCount > 0
                    width: parent.width
                    height: 72
                    radius: P.Style.radiusLg
                    color: P.Style.surface
                    border.width: 1; border.color: P.Style.line

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (App) App.recoverUnfinishedOrder()
                    }
                    Rectangle {
                        id: payHub
                        anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 16
                        width: 40; height: 40; radius: 20
                        color: P.Style.warningSoft
                        Text {
                            anchors.centerIn: parent
                            text: "💰"; font.pixelSize: 18
                        }
                    }
                    P.ActionButton {
                        id: payBtn
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        anchors.rightMargin: 14
                        text: "去处理"
                        variant: "primary"
                        height: 36; implicitHeight: 36
                        leftPadding: 18; rightPadding: 18
                        topPadding: 4; bottomPadding: 4
                        onClicked: if (App) App.recoverUnfinishedOrder()
                    }
                    Column {
                        anchors.left: payHub.right; anchors.right: payBtn.left
                        anchors.leftMargin: 12; anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            text: "有 " + page.waitingCount + " 笔待支付订单"
                            font.pixelSize: P.Style.fontMd; font.bold: true
                            color: P.Style.ink
                        }
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            text: "先完成结算才能开始下一次充电"
                            font.pixelSize: P.Style.fontSm; color: P.Style.muted
                        }
                    }
                }
            }
        }
    }
}
