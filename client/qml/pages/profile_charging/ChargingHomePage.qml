import QtQuick
import "../../platform" as P

// QML twin of widgets ChargingHomePage (tab route: "charging").
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
    property var activeOrder: null   // first charging order summary map

    Connections {
        target: orderService
        function onStatusCountsUpdated(chargingCount, waitingPaymentCount, completedCount) {
            page.chargingCount = chargingCount
            page.waitingCount = waitingPaymentCount
        }
        function onOrdersLoaded(orders, total, hasMore) {
            page.activeOrder = orders.length > 0 ? orders[0] : null
        }
    }
    Component.onCompleted: {
        orderService.fetchStatusCounts()
        orderService.fetchOrders("charging", 1)
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacing: P.Style.spaceLg

        Text { text: "充电"; font.pixelSize: P.Style.fontXl; color: P.Style.ink }

        // Hero card — uiChargingHero parity (glyph/title/caption objectNames kept).
        Rectangle {
            objectName: "uiChargingHero"
            width: parent.width
            height: 180
            radius: P.Style.radiusLg
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: P.Style.brandDeep }
                GradientStop { position: 1.0; color: P.Style.brand }
            }
            Column {
                anchors.centerIn: parent
                spacing: P.Style.spaceSm
                Text {
                    objectName: "uiHeroGlyph"
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: page.chargingCount > 0 ? "⚡" : "🔌"
                    font.pixelSize: 44
                }
                Text {
                    objectName: "uiHeroTitle"
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: page.chargingCount > 0
                          ? "有 " + page.chargingCount + " 单正在充电" : "暂无进行中的充电"
                    font.pixelSize: P.Style.fontLg; color: P.Style.surface
                }
                Text {
                    objectName: "uiHeroCaption"
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: page.chargingCount > 0
                          ? "实时功率、电量、费用全程可见" : "扫码或预约后在这里查看实时状态"
                    font.pixelSize: P.Style.fontSm; color: P.Style.brandBright
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
