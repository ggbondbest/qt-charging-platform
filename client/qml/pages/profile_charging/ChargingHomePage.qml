import QtQuick
import "../../platform" as P

// QML twin of widgets ChargingHomePage (tab route: "charging").
// Layout/behaviour parity with charging_home_page.cpp + charging_pulse.cpp:
//   · hero 双态（有任务 深绿渐变白字 / 空态 浅渐变深字），⚡ 呼吸 1400ms
//     InOutSine alpha 0.38→1（widgets breath_），空态也呼吸（"空态不冷清"）；
//   · 有任务时 GET_CHARGING_STATUS 实时行：扫弧脉冲环 52px（1600ms Linear,
//     arc 100°, kTrack #E2F7EC）+ kW / 已充电量 / 充电时长 / 预估费用，
//     本地 1s tick 补帧（ChargingPage 同款），回访 tab 不闪空态；
//   · stale-while-revisit：数据落 P.TabCache.charging（单例，6.2 无
//     globalThis），重进页先渲染旧帧再后台刷（widgets QStackedWidget 常驻观感）。
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
    property int seconds: 0
    property real breath: 1.0            // charging_pulse breath_ parity

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
        if (page.activeOrder) chargingService.startTracking(Number(page.activeOrder.id))
    }
    Component.onDestruction: {
        if (chargingService) chargingService.stopTracking()   // teardown-safe
    }
    function refreshAll() {
        orderService.fetchStatusCounts()
        orderService.fetchOrders("charging", 1)
    }

    // ---- data plumbing ----
    Connections {
        target: orderService
        function onStatusCountsUpdated(chargingCount, waitingPaymentCount, completedCount) {
            page.chargingCount = chargingCount
            page.waitingCount = waitingPaymentCount
        }
        function onOrdersLoaded(orders, total, hasMore) {
            page.ordersArrived = true
            page.activeOrder = orders.length > 0 ? orders[0] : null
            if (page.activeOrder)
                chargingService.startTracking(Number(page.activeOrder.id))
            else
                chargingService.stopTracking()
            if (pull.refreshing) pull.setRefreshing(false)
            P.TabCache.charging = {
                chargingCount: page.chargingCount, waitingCount: page.waitingCount,
                activeOrder: page.activeOrder
            }
        }
    }
    Connections {
        target: chargingService
        function onStatusLoaded(s) {
            page.status = s
            page.seconds = s.durationSeconds || 0
        }
    }
    // Local 1s tick between server pushes so the timer always moves.
    Timer {
        running: page.chargingCount > 0 && P.Style.motionEnabled
        repeat: true; interval: 1000
        onTriggered: page.seconds += 1
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

                // ---- hero：未落数据=等高骨架（不闪"暂无"） ----
                // Qt6.2 坑：GradientStop.color 的外部属性绑定不随状态刷新
                // （初帧算成空态后一直浅色，busy 变→只剩白字浮浅底）。
                // 双态渐变改"两层常量渐变 Rectangle 切 visible"，零动态绑定。
                Rectangle {
                    objectName: "uiChargingHero"
                    width: parent.width
                    height: 180
                    radius: P.Style.radiusLg
                    readonly property bool busy: page.chargingCount > 0
                    gradient: Gradient {
                        orientation: Gradient.Vertical          // 空态层（常驻底）
                        GradientStop { position: 0.0; color: P.Style.brandSoft }
                        GradientStop { position: 1.0; color: P.Style.infoSoft }
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: P.Style.radiusLg
                        visible: parent.busy
                        gradient: Gradient {
                            orientation: Gradient.Vertical      // busy 层（整块切换）
                            GradientStop { position: 0.0; color: P.Style.heroFrom }
                            GradientStop { position: 1.0; color: P.Style.heroTo }
                        }
                    }

                    // 骨架/空态/有任务 三态内容
                    Text {
                        objectName: "uiHeroGlyph"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top; anchors.topMargin: 22
                        visible: page.ordersArrived
                        text: "⚡"
                        font.pixelSize: parent.busy ? 44 : P.Style.fontGlyph
                        // charging_pulse breath_: alpha .38→1→.38, scale .92→1,
                        // 1400ms InOutSine infinite — 空态同款慢呼吸。
                        opacity: 0.38 + 0.62 * page.breath
                        scale: 0.92 + 0.08 * page.breath
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: !page.ordersArrived
                        text: "正在加载充电状态…"; font.pixelSize: P.Style.fontMd
                        color: P.Style.muted
                    }
                    Column {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top; anchors.topMargin: 74
                        spacing: 4
                        visible: page.ordersArrived
                        Text {
                            objectName: "uiHeroTitle"
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: page.chargingCount > 0
                                  ? "有 " + page.chargingCount + " 单正在充电" : "暂无进行中的充电"
                            font.pixelSize: page.chargingCount > 0 ? P.Style.fontLg : P.Style.fontLg2
                            font.weight: page.chargingCount > 0 ? Font.Normal : Font.DemiBold
                            color: page.chargingCount > 0 ? P.Style.surface : P.Style.ink
                        }
                        Text {
                            objectName: "uiHeroCaption"
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: page.chargingCount > 0
                                  ? "实时功率、电量、费用全程可见" : "扫码或预约后在这里查看实时状态"
                            font.pixelSize: P.Style.fontSm
                            color: page.chargingCount > 0 ? P.Style.heroPhone : P.Style.muted
                        }
                    }

                    // ---- 实时行：扫弧脉冲环 + kW/电量/时长/费用（buildActiveCard 语义） ----
                    // Row 子项禁用 anchors（行为未定义），两列统一 52 高对齐。
                    Row {
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.bottom: parent.bottom; anchors.bottomMargin: 16
                        anchors.leftMargin: 20; anchors.rightMargin: 20
                        spacing: P.Style.spaceMd
                        visible: page.chargingCount > 0 && page.activeOrder !== null

                        // ChargingPulse 移植：52px，track #E2F7EC + 100° 品牌绿
                        // 圆头弧 1600ms Linear 扫圈（QtQuick.Shapes 本机缺装，
                        // 用 Canvas 等价绘制）。
                        Canvas {
                            objectName: "chargingPulse"
                            width: 52; height: 52
                            property real phase: 0.0
                            onPhaseChanged: requestPaint()
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                ctx.lineWidth = 4
                                ctx.strokeStyle = "#E2F7EC"          // kTrackColor
                                ctx.beginPath()
                                ctx.arc(26, 26, 22, 0, 2 * Math.PI)
                                ctx.stroke()
                                ctx.lineCap = "round"
                                ctx.strokeStyle = "#00B578"          // brand
                                const a0 = (-90 + phase * 360) * Math.PI / 180
                                ctx.beginPath()
                                ctx.arc(26, 26, 22, a0, a0 + 100 * Math.PI / 180)
                                ctx.stroke()
                            }
                            SequentialAnimation on phase {
                                running: P.Style.motionEnabled
                                loops: Animation.Infinite
                                NumberAnimation { from: 0; to: 1
                                    duration: 1600; easing.type: Easing.Linear }
                            }
                        }
                        Item {
                            width: statsGrid.implicitWidth
                            height: 52
                            Grid {
                                id: statsGrid
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                columns: 4
                                columnSpacing: P.Style.spaceMd
                                Repeater {
                                    model: [
                                        { k: "kW",   v: page.status && page.status.powerKnown
                                                        ? (page.status.powerWatts / 1000).toFixed(1) : "—" },
                                        { k: "kWh",  v: page.status ? ((page.status.energyWh || 0) / 1000).toFixed(2) : "—" },
                                        { k: "时长", v: page.dur(page.seconds) },
                                        { k: "预估¥", v: page.status ? ((page.status.amountCents || 0) / 100).toFixed(2) : "—" },
                                    ]
                                    Column {
                                        spacing: 2
                                        Text {
                                            text: modelData.v
                                            font.pixelSize: P.Style.fontMd; font.bold: true
                                            color: P.Style.surface
                                        }
                                        Text {
                                            text: modelData.k
                                            font.pixelSize: P.Style.fontXs; color: P.Style.heroPhone
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                P.ActionButton {
                    objectName: "viewChargingRunButton"
                    visible: page.chargingCount > 0 && page.activeOrder !== null
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

                // Pending-payment notice — widgets rechargePendingNotice parity path.
                P.NoticePanel {
                    objectName: "rechargePendingNotice"
                    visible: page.waitingCount > 0
                    width: parent.width
                    glyph: "💰"
                    title: "有 " + page.waitingCount + " 笔待支付订单"
                    description: "先完成结算才能开始下一次充电"
                    actionText: "去处理"
                    onActionTriggered: if (App) App.navigate("order")
                }

                Text {
                    width: parent.width
                    text: "提示：充电页轮询走 GET_CHARGING_STATUS（mock 每秒推一次实时功率）"
                    font.pixelSize: P.Style.fontSm; color: P.Style.faint
                }
            }
        }
    }

    // 呼吸驱动（widgets breathAnim_ 0.15→1→0.15, InOutSine, 1400ms 无限循环）
    SequentialAnimation on breath {
        running: page.ordersArrived && P.Style.motionEnabled
        loops: Animation.Infinite
        NumberAnimation { to: 0.15; duration: P.Style.durBreathe / 2
            easing.type: Easing.InOutSine }
        NumberAnimation { to: 1.0; duration: P.Style.durBreathe / 2
            easing.type: Easing.InOutSine }
    }
}
