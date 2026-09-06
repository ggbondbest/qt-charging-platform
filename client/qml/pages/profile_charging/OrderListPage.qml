import QtQuick
import "../../platform" as P

// QML twin of widgets OrderListPage (tab route: "order").
Item {
    id: page
    objectName: "orderListPage"
    property string route: "order"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property string filter: "all"
    property int page_ : 1
    property var counts: ({ charging: 0, waitingPayment: 0, completed: 0 })

    readonly property var filters: [
        { id: "all", label: "全部" },
        { id: "charging", label: "充电中" },
        { id: "waiting_payment", label: "待支付" },
        { id: "completed", label: "已完成" }]
    readonly property var statusCn: ({
        reserved: "已预约", charging: "充电中", waiting_payment: "待支付",
        completed: "已完成", cancelled: "已取消" })
    readonly property var statusTone: ({
        reserved: "info", charging: "warning", waiting_payment: "danger",
        completed: "success", cancelled: "neutral" })

    function money(cents) { return (cents / 100).toFixed(2) }
    function load(first) {
        if (first) { page_ = 1 } else { page_ += 1 }
        orderService.fetchOrders(page.filter, page_)
    }

    ListModel { id: ordersModel }

    Connections {
        target: orderService
        function onOrdersLoaded(orders, total, hasMore) {
            if (page_ === 1) ordersModel.clear()
            for (var i = 0; i < orders.length; ++i)
                ordersModel.append(orders[i])
            listScroll.setRefreshing(false)
        }
        function onStatusCountsUpdated(chargingCount, waitingPaymentCount, completedCount) {
            page.counts = ({ charging: chargingCount, waitingPayment: waitingPaymentCount,
                             completed: completedCount })
        }
        function onOperationFailed(type, code, message) {
            listScroll.setRefreshing(false)
            if (App) App.showToast("加载失败：" + message, "danger")
        }
    }
    Component.onCompleted: {
        orderService.fetchStatusCounts()
        load(true)
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd

        Row {
            width: parent.width
            Text {
                text: "我的订单"
                font.pixelSize: P.Style.fontXl; color: P.Style.ink
                anchors.verticalCenter: parent.verticalCenter
            }
            Item { width: parent.width * 0.15; height: 1 }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "⚡" + page.counts.charging + "  💰" + page.counts.waitingPayment
                      + "  ✅" + page.counts.completed
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
        }

        Row {
            spacing: P.Style.spaceSm
            Repeater {
                model: page.filters
                P.ActionButton {
                    variant: page.filter === modelData.id ? "primary" : "chip"
                    text: modelData.label
                    onClicked: {
                        if (page.filter === modelData.id) return
                        page.filter = modelData.id
                        load(true)
                    }
                }
            }
        }

        P.PullToRefreshArea {
            id: listScroll
            objectName: "uiOrderListStack"
            width: parent.width
            height: parent.height - y
            onRefreshRequested: {
                orderService.fetchStatusCounts()
                load(true)
            }

            Repeater {
                model: ordersModel
                delegate: P.ClickableCard {
                    objectName: "uiOrderCard"
                    width: listScroll.width
                    height: Math.max(72, col.implicitHeight + 2 * P.Style.spaceMd)
                    onClicked: {
                        if (!App) return
                        App.navigate("order_detail", ({
                            id: model.id, orderNo: model.orderNo, status: model.status,
                            stationName: model.stationName, chargerCode: model.chargerCode,
                            energyWh: model.energyWh, durationSeconds: model.durationSeconds,
                            amountCents: model.amountCents,
                            unitPriceCentsPerKwh: model.unitPriceCentsPerKwh,
                            createdAt: model.createdAt }))
                    }
                    Row {
                        id: col
                        x: P.Style.spaceMd; y: P.Style.spaceMd
                        width: parent.width - 2 * P.Style.spaceMd
                        spacing: P.Style.spaceMd
                        Column {
                            width: parent.width * 0.6
                            spacing: 3
                            Text {
                                text: model.stationName || "充电站"
                                font.pixelSize: P.Style.fontMd; color: P.Style.ink
                            }
                            Text {
                                text: model.orderNo + " · " + model.chargerCode
                                font.pixelSize: P.Style.fontSm; color: P.Style.faint
                            }
                            Text {
                                text: model.createdAt + "  ·  "
                                      + (model.energyWh / 1000).toFixed(2) + " kWh"
                                font.pixelSize: P.Style.fontSm; color: P.Style.muted
                            }
                        }
                        Item { width: parent.width * 0.1; height: 1 }
                        Column {
                            width: parent.width * 0.3
                            spacing: 3
                            Text {
                                horizontalAlignment: Text.AlignRight
                                width: parent.width
                                text: "¥ " + page.money(model.amountCents)
                                font.pixelSize: P.Style.fontMd; color: P.Style.ink
                            }
                            P.StatusTag {
                                anchors.right: parent.right
                                tone: page.statusTone[model.status] || "neutral"
                                text: page.statusCn[model.status] || model.status
                            }
                        }
                    }
                }
            }
        }
    }
}
