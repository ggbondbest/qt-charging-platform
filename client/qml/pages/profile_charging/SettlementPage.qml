import QtQuick
import "../../platform" as P

// QML twin of widgets SettlementPage (route: "settlement", arg = stopped status map).
Item {
    id: page
    objectName: "settlementPage"
    property string route: "settlement"
    property var arg: ({})
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    function money(cents) { return ((cents || 0) / 100).toFixed(2) }
    function dur(sec) {
        sec = sec || 0
        var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60)
        return (h > 0 ? h + " 小时 " : "") + m + " 分钟"
    }
    property var order: page.arg && page.arg.id !== undefined ? page.arg : ({})
    property bool paying: false

    Connections {
        target: chargingService
        function onPaymentCompleted(amountCents, balanceAfterCents) {
            if (!page.paying) return
            page.paying = false
            if (!App) return
            App.showToast("支付成功，余额 ¥" + (balanceAfterCents / 100).toFixed(2), "success")
            App.navigate("order")
        }
        function onOperationFailed(type, code, message) {
            if (type !== "PAY_ORDER" || !page.paying) return
            page.paying = false
            if (App) App.showToast("支付失败：" + message, "danger")
        }
    }
    // Deep-link fallback: no arg (screenshot / 直接跳转) → show the first
    // waiting-payment order, same chain shape as the widgets settlement entry.
    Connections {
        target: orderService
        function onOrdersLoaded(orders, total, hasMore) {
            if (page.order.id || orders.length === 0) return
            page.arg = orders[0]
        }
    }
    Component.onCompleted: {
        walletService.fetchProfile()
        if (!page.order.id) orderService.fetchOrders("waiting_payment", 1)
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacing: P.Style.spaceLg

        Text { text: "充电结算"; font.pixelSize: P.Style.fontXl; color: P.Style.ink }

        P.Card {
            objectName: "uiSettlementCard"
            width: parent.width
            Column {
                width: parent.width
                spacing: P.Style.spaceSm
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "本次费用"
                    font.pixelSize: P.Style.fontSm; color: P.Style.muted
                }
                Text {
                    objectName: "settlementAmount"
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "¥ " + money(page.order.amountCents)
                    font.pixelSize: 36; color: P.Style.danger
                }
            }
        }

        P.Card {
            width: parent.width
            Column {
                width: parent.width
                spacing: P.Style.spaceMd
                Repeater {
                    model: page.order.id ? [
                        { k: "充电站", v: page.order.stationName || "—" },
                        { k: "桩编号", v: page.order.chargerCode || "—" },
                        { k: "电量", v: ((page.order.energyWh || 0) / 1000).toFixed(2) + " kWh" },
                        { k: "时长", v: page.dur(page.order.durationSeconds) },
                        { k: "单价", v: "¥ " + money(page.order.unitPriceCentsPerKwh) + " /kWh" },
                        { k: "账户余额", v: "¥ " + (App && App.currentUser ? money(App.currentUser.balanceCents) : "0.00") },
                    ] : []
                    Row {
                        width: parent.width
                        Text { text: modelData.k; width: parent.width * 0.3
                               font.pixelSize: P.Style.fontMd; color: P.Style.muted }
                        Text { text: modelData.v; width: parent.width * 0.7
                               horizontalAlignment: Text.AlignRight
                               font.pixelSize: P.Style.fontMd; color: P.Style.ink }
                    }
                }
            }
        }

        P.ActionBar {
            objectName: "settlementPayBar"
            width: parent.width
            variant: "primary"
            actionText: "确认支付 ¥" + money(page.order.amountCents)
            caption: "从账户余额扣除"
            enabled: !!page.order.id && !page.paying
            onClicked: if (page.order.id) { page.paying = true; chargingService.payOrder(String(page.order.id)) }
        }
        P.ActionButton {
            objectName: "settlementRechargeButton"
            variant: "secondary"; text: "余额不足？去充值"
            enabled: !page.paying
            onClicked: if (App) App.navigate("recharge")
        }
        P.ActionButton {
            variant: "ghost"; text: "稍后支付"
            onClicked: if (App) App.navigate("order")
        }
    }
}
