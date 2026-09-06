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
    property bool hasMore: false       // 服务端分页落定（GET_ORDERS.hasMore，仅当前列表的筛选）
    property bool loadMoreError: false // 下一页失败 → 按钮变体重试（retry 基准见 loadedPage）
    property int loadedPage: 1         // 已成功加载页码；在途失败不回滚它，重试天然重发同页
    // —— 在途请求身份 ——
    // 服务层（OrderService）单飞且对在途重复提交**静默丢弃无回执**，ordersLoaded
    // 也不携带请求参数；因此页面自己记录"在途的是哪个 (filter, page)"，并保证
    // 同时至多一个请求由本页面发出。响应到达时：filter 与当前不一致 = 过期响应，
    // 不应用、立即按当前筛选重查；一致（含下拉在途）= 该响应即满足刷新语义。
    property bool reqActive: false
    property string reqFilter: "all"
    property int reqPage: 1
    property bool reqFirst: true
    property bool queuedReload: false  // 他页在途占用通道时记录的补查意图
    readonly property bool loadingMore: reqActive && !reqFirst
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
        if (reqActive) {
            // 自己那路在途：不发第二条（服务层会静默吞掉），也绝不替在途请求收摊。
            // 下拉 → 在途响应到达即满足刷新；切筛选 → 落定时判过期并重查（见 finishRequest）。
            return
        }
        if (orderService.isFetchingOrders()) {
            // 同一 OrderService 上别的页面在途（order_detail/charging 深链回落）：
            // 本请求会被丢弃且无回执，记补查意图，等那一路落定后统一走第一页刷新。
            queuedReload = true
            return
        }
        reqActive = true; reqFirst = first; reqFilter = page.filter
        reqPage = first ? 1 : loadedPage + 1
        loadMoreError = false
        orderService.fetchOrders(reqFilter, reqPage)
    }
    // 在途 GET_ORDERS 落定的公共收尾：过期（筛选已切换）的响应不应用；
    // 过期或有补查意图则立刻按当前筛选重查第一页。
    function finishRequest() {
        reqActive = false
        listScroll.setRefreshing(false)
        const requery = reqFilter !== page.filter || queuedReload
        queuedReload = false
        if (requery) load(true)
    }

    // objectName 让测试能从根节点直读行数——offscreen 下 Repeater delegate 是否
    // 挂进 QObject 树取决于异步编译时序，不能依赖 findChildren(delegate)。
    ListModel { id: ordersModel; objectName: "uiOrdersModel" }

    Connections {
        target: orderService
        function onOrdersLoaded(orders, total, hasMore) {
            if (!reqActive) {               // 他页在途的响应：不应用，只放行补查
                if (queuedReload) finishRequest()
                return
            }
            if (reqFilter === page.filter) { // 过期响应直接丢弃
                if (reqFirst) ordersModel.clear()
                for (var i = 0; i < orders.length; ++i)
                    ordersModel.append(orders[i])
                page.hasMore = hasMore
                page.loadedPage = reqPage
                page.loadMoreError = false
            }
            finishRequest()
        }
        function onStatusCountsUpdated(chargingCount, waitingPaymentCount, completedCount) {
            page.counts = ({ charging: chargingCount, waitingPayment: waitingPaymentCount,
                             completed: completedCount })
        }
        function onOperationFailed(type, code, message) {
            if (type !== "GET_ORDERS") return   // 计数类失败不动订单在途状态/胶囊
            if (!reqActive) {
                if (queuedReload) finishRequest()
                return
            }
            const stale = reqFilter !== page.filter
            if (!stale) {
                if (!reqFirst) page.loadMoreError = true // loadedPage 未被在途污染 → 重试仍请求同页
                if (App) App.showToast("加载失败：" + message, "danger")
            }
            finishRequest()
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
                    objectName: "uiOrderFilter" + modelData.id
                    variant: page.filter === modelData.id ? "primary" : "chip"
                    text: modelData.label
                    onClicked: {
                        if (page.filter === modelData.id) return
                        page.filter = modelData.id
                        load(true)   // 在途时是意图登记：过期检测负责落定后重查
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
                                objectName: "uiOrderCardStatus"
                                anchors.right: parent.right
                                tone: page.statusTone[model.status] || "neutral"
                                text: page.statusCn[model.status] || model.status
                            }
                        }
                    }
                }
            }

            // 分页入口：服务端报告还有下一页（或上一页失败）才现身。
            P.ActionButton {
                objectName: "uiOrderLoadMore"
                visible: ordersModel.count > 0 && (page.hasMore || page.loadMoreError)
                width: listScroll.width
                variant: "chip"
                enabled: !page.reqActive
                text: page.loadMoreError ? "重试加载下一页"
                      : (page.loadingMore ? "加载中…" : "加载更多")
                onClicked: load(false)
            }
        }
    }
}
