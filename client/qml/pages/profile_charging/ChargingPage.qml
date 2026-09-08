import QtQuick
import "../../platform" as P

// QML twin of widgets ChargingPage (route: "charging_run", arg = order map/id).
// charging_pulse's QQuickPaintedItem is NOT rewritten — replaced by a native
// breathing Rectangle (Style.durBreathe parity), same visual semantics.
Item {
    id: page
    objectName: "chargingPage"
    property string route: "charging_run"
    property var arg: null
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property var status: null
    property int seconds: 0
    property bool stopping: false

    function dur(sec) {
        var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60
        return (h > 0 ? h + ":" : "") + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0")
    }

    Connections {
        target: chargingService
        function onStatusLoaded(s) {
            const expected = page.arg && page.arg.id !== undefined ? page.arg.id : page.arg
            if (expected && String(s.id) !== String(expected)) return
            page.status = s
            page.seconds = s.durationSeconds || 0
            if (App && s.status === "waiting_payment") App.navigate("settlement", s)
            else if (App && s.status === "completed") App.navigate("order")
        }
        function onStopCompleted(s) {
            page.stopping = false
            chargingService.stopTracking()
            if (!App) return
            App.showToast("已停止充电", "success")
            App.navigate("settlement", s)
        }
        function onOperationFailed(type, code, message) {
            if (type === "STOP_CHARGING") page.stopping = false
            if (App) App.showToast("操作失败：" + message, "danger")
        }
    }
    // Deep-link fallback (screenshots / --view=charging_run): no arg → track
    // the first live charging order, mirroring the widgets home-shell flow.
    Connections {
        target: orderService
        function onOrdersLoaded(orders, total, hasMore) {
            if (page.arg || orders.length === 0) return
            page.arg = orders[0]
            if (chargingService) chargingService.startTracking(String(orders[0].id))
        }
    }
    Component.onCompleted: {
        const id = page.arg && page.arg.id !== undefined ? page.arg.id : page.arg
        if (id) chargingService.startTracking(String(id))
        chargingService.fetchStatusNow()
        if (!id) orderService.fetchOrders("charging", 1)
    }
    Component.onDestruction: {
        if (chargingService) chargingService.stopTracking()  // teardown-safe (C++ lives longer)
    }

    // Local 1s tick between server pushes so the timer always moves.
    Timer {
        running: page.status !== null && page.status.status === "charging"
                 && P.Style.motionEnabled
        repeat: true; interval: 1000
        onTriggered: page.seconds += 1
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacing: P.Style.spaceLg

        Text {
            text: page.status ? ((page.status.stationName || "充电站") + " · " + (page.status.chargerCode || ""))
                              : "连接充电桩…"
            font.pixelSize: P.Style.fontMd; color: P.Style.muted
        }

        // Breathing pulse ring.
        Item {
            objectName: "chargingPulse"
            anchors.horizontalCenter: parent.horizontalCenter
            width: 220; height: 220
            Rectangle {
                anchors.centerIn: parent
                width: 220; height: 220; radius: 110
                color: P.Style.brandSoft
                SequentialAnimation on scale {
                    running: P.Style.motionEnabled
                    loops: Animation.Infinite
                    NumberAnimation { from: 0.92; to: 1.0
                        duration: P.Style.durBreathe / 2; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 1.0; to: 0.92
                        duration: P.Style.durBreathe / 2; easing.type: Easing.InOutSine }
                }
            }
            Rectangle {
                anchors.centerIn: parent
                width: 176; height: 176; radius: 88
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0; color: P.Style.brand }
                    GradientStop { position: 1.0; color: P.Style.brandDeep }
                }
                Column {
                    anchors.centerIn: parent
                    spacing: P.Style.spaceXs
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: page.status && page.status.powerKnown
                              ? ((page.status.powerWatts / 1000).toFixed(1)) : "—"
                        font.pixelSize: 34; color: P.Style.surface
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "kW 实时功率"
                        font.pixelSize: P.Style.fontSm; color: P.Style.brandBright
                    }
                }
            }
        }

        Grid {
            objectName: "chargingStats"
            anchors.horizontalCenter: parent.horizontalCenter
            columns: 3
            columnSpacing: P.Style.spaceXl
            Repeater {
                model: [
                    { k: "已充电量", v: page.status ? ((page.status.energyWh || 0) / 1000).toFixed(2) + " kWh" : "—" },
                    { k: "时长", v: page.dur(page.seconds) },
                    { k: "费用", v: page.status ? "¥ " + ((page.status.amountCents || 0) / 100).toFixed(2) : "¥ 0.00" },
                ]
                Column {
                    spacing: 2
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: modelData.v
                        font.pixelSize: P.Style.fontLg; color: P.Style.ink
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: modelData.k
                        font.pixelSize: P.Style.fontSm; color: P.Style.muted
                    }
                }
            }
        }

        Item { width: 1; height: P.Style.spaceMd }

        P.ActionBar {
            objectName: "stopChargingBar"
            width: parent.width
            variant: "danger"
            actionText: "停止充电"
            caption: "结束后进入结算"
            enabled: !!page.status && page.status.status === "charging" && !page.stopping
            onClicked: { page.stopping = true; chargingService.stopCharging() }
        }
    }
}
