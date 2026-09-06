import QtQuick
import "../../platform" as P

// QML twin of widgets OrderDetailPage (route: "order_detail", arg = order map).
Item {
    id: page
    objectName: "orderDetailPage"
    property string route: "order_detail"
    property var arg: ({})
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    readonly property var statusCn: ({
        reserved: "已预约", charging: "充电中", waiting_payment: "待支付",
        completed: "已完成", cancelled: "已取消" })
    readonly property var statusTone: ({
        reserved: "info", charging: "warning", waiting_payment: "danger",
        completed: "success", cancelled: "neutral" })

    function money(cents) { return (cents / 100).toFixed(2) }
    function dur(sec) {
        var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60)
        return (h > 0 ? h + " 小时 " : "") + m + " 分钟"
    }
    // arg may arrive empty when deep-linked by route id alone (screenshot mode):
    // fall back to fetching the first order so the page always renders.
    function ensureData() {
        if (page.arg && page.arg.id) return
        orderService.fetchOrders("all", 1)
    }
    Connections {
        target: orderService
        function onOrdersLoaded(orders, total, hasMore) {
            if (page.arg && page.arg.id) return
            if (orders.length > 0) page.arg = orders[0]
        }
    }
    Connections {
        target: chargingService
        function onPaymentCompleted(amountCents, balanceAfterCents) {
            if (!App) return
            App.showToast("支付成功 ¥" + (amountCents / 100).toFixed(2)
                          + "，余额 ¥" + (balanceAfterCents / 100).toFixed(2), "success")
            App.navigate("order")
        }
        function onOperationFailed(type, code, message) {
            if (App) App.showToast("支付失败：" + message, "danger")
        }
    }
    Component.onCompleted: ensureData()

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacing: P.Style.spaceLg
        visible: page.arg && page.arg.id !== undefined

        Row {
            width: parent.width
            Text {
                text: page.arg.orderNo || "订单详情"
                font.pixelSize: P.Style.fontXl; color: P.Style.ink
                anchors.verticalCenter: parent.verticalCenter
            }
            Item { width: parent.width * 0.25; height: 1 }
            P.StatusTag {
                anchors.verticalCenter: parent.verticalCenter
                tone: page.statusTone[page.arg.status] || "neutral"
                text: page.statusCn[page.arg.status] || String(page.arg.status || "")
            }
        }

        P.Card {
            objectName: "uiOrderDetailCard"
            width: parent.width
            Column {
                width: parent.width
                spacing: P.Style.spaceMd
                Repeater {
                    model: page.arg && page.arg.id ? [
                        { k: "充电站", v: page.arg.stationName || "—" },
                        { k: "桩编号", v: page.arg.chargerCode || "—" },
                        { k: "单价", v: "¥ " + money(page.arg.unitPriceCentsPerKwh || 0) + " /kWh" },
                        { k: "电量", v: ((page.arg.energyWh || 0) / 1000).toFixed(2) + " kWh" },
                        { k: "时长", v: page.dur(page.arg.durationSeconds || 0) },
                        { k: "下单时间", v: page.arg.createdAt || "—" },
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

        Row {
            width: parent.width
            Text {
                text: "合计"
                font.pixelSize: P.Style.fontMd; color: P.Style.muted
                anchors.verticalCenter: parent.verticalCenter
            }
            Item { width: parent.width * 0.5; height: 1 }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "¥ " + money(page.arg.amountCents || 0)
                font.pixelSize: 26; color: P.Style.danger
            }
        }

        P.ActionButton {
            objectName: "orderDetailPayButton"
            visible: page.arg.status === "waiting_payment"
            width: parent.width
            variant: "primary"
            text: "立即支付"
            onClicked: if (chargingService) chargingService.payOrder(page.arg.id)
        }
        P.ActionButton {
            variant: "ghost"; text: "返回"
            onClicked: if (App) App.back()
        }
    }
}
