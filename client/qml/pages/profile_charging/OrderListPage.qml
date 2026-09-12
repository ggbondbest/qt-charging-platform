import QtQuick
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// QML twin of widgets OrderListPage (tab route: "order").
// Billing-style list (industry conventions from ChargePoint / Electrix order
// screens): month group headers carrying a live summary ("N 单 · ¥X · Y kWh",
// summed over the *visible* rows — TODO(contract): server month-stats field),
// icon-hub cards where the amount is the right-hand focal number, and the
// widgets 40ms-staggered cascade entrance (capped) on each fresh first page.
// Pull-to-refresh shares the platform PullToRefreshArea (its gesture strip z
// was fixed 2026-09-07 — it sat behind the Flickable and swallowed presses).
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
    // Icon hub 座：tone → 软底色 + 线稿图标名 + 前景色（widgets buildMonthGroups 的状态图形语；
    // 图标经 GlyphProvider 染色，母版见 assets/glyphs/）
    readonly property var hubSpec: ({
        warning: { g: "bolt",     fg: P.Style.warning,   bg: P.Style.warningSoft },
        danger:  { g: "wallet",   fg: P.Style.danger,    bg: P.Style.dangerSoft },
        success: { g: "check",    fg: P.Style.brandDeep, bg: P.Style.brandSoft },
        info:    { g: "calendar-event", fg: P.Style.info, bg: P.Style.infoSoft },
        neutral: { g: "x",        fg: P.Style.muted,     bg: P.Style.ghost } })
    function hubOf(status) {
        return hubSpec[statusTone[status] || "neutral"] || hubSpec.neutral
    }

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

    // ---- month grouping (widgets rebuildMonthGroups parity) ----
    // rows = [{kind:"month"|"order", ...}], fed to the Repeater; ordersModel
    // keeps the raw applied rows so the test's count semantics stay intact.
    property var rows: []
    function monthKeyOf(createdAt) {
        const s = App ? App.displayTime(String(createdAt || "")) : String(createdAt || "")
        return /^\d{4}-\d{2}/.test(s) ? s.substring(0, 7) : "zz"
    }
    function monthLabelOf(key) {
        if (key === "zz") return "更早"          // widgets 兜底口径
        const p = key.split("-")
        return p[0] + "年" + parseInt(p[1], 10) + "月"
    }
    // fadeFrom = -1 → 整表级联入场（第一页）；否则 index ≥ fadeFrom 的新行才入场。
    function rebuildRows(fadeFrom) {
        const out = []
        var lastKey = ""
        var cur = null
        for (var i = 0; i < ordersModel.count; ++i) {
            const o = ordersModel.get(i)
            const key = monthKeyOf(o.createdAt)
            if (key !== lastKey) {
                cur = { kind: "month", title: monthLabelOf(key),
                        count: 0, cents: 0, wh: 0, summary: "",
                        fade: fadeFrom < 0 || i >= fadeFrom, sIndex: 0 }
                if (cur.fade) cur.sIndex = (fadeFrom < 0 ? i : i - fadeFrom)
                out.push(cur)
                lastKey = key
            }
            cur.count += 1
            cur.cents += o.amountCents || 0
            cur.wh += o.energyWh || 0
            out.push({ kind: "order", order: o,
                       fade: fadeFrom < 0 || i >= fadeFrom,
                       sIndex: fadeFrom < 0 ? i : i - fadeFrom })
        }
        // 汇总 = 可见行相加（本地翻页下不是整月真实值）TODO(contract): 月统计字段
        for (const r of out)
            if (r.kind === "month")
                r.summary = r.count + " 单 · ¥" + money(r.cents)
                             + " · " + (r.wh / 1000).toFixed(2) + " kWh"
        page.rows = out
    }

    Connections {
        target: orderService
        function onOrdersLoaded(orders, total, hasMore) {
            if (!reqActive) {               // 他页在途的响应：不应用，只放行补查
                if (queuedReload) finishRequest()
                return
            }
            if (reqFilter === page.filter) { // 过期响应直接丢弃
                if (reqFirst) ordersModel.clear()
                const prevCount = ordersModel.count
                for (var i = 0; i < orders.length; ++i)
                    ordersModel.append(orders[i])
                page.hasMore = hasMore
                page.loadedPage = reqPage
                page.loadMoreError = false
                rebuildRows(reqFirst ? -1 : prevCount)
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
        if (typeof page.arg === "string"
            && ["all", "charging", "waiting_payment", "completed"].indexOf(page.arg) >= 0)
            page.filter = page.arg
        orderService.fetchStatusCounts()
        load(true)
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacing: P.Style.spaceLg

        // ---- header（anchors 布局：Row 子项 anchors 在 6.2 是未定义行为）----
        Item {
            width: parent.width
            height: Math.round(40 * P.Style.fontScaleFactor)   // 字号档呼吸（2026-09-08）
            Text {
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                text: "我的订单"
                font.pixelSize: P.Style.fontXl; font.weight: Font.Bold; color: P.Style.ink
            }
            Text {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                objectName: "uiOrderCounts"
                text: "充电中 " + page.counts.charging + " · 待支付 " + page.counts.waitingPayment
                      + " · 已完成 " + page.counts.completed
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
        }

        // 胶囊行：放大档下 4 枚总宽会超页宽 → 横向 Flickable 兜底（批次A 字号
        // 档适配补漏，2026-09-08）。
        Flickable {
            width: parent.width
            height: filterRow.implicitHeight
            contentWidth: filterRow.implicitWidth
            contentHeight: filterRow.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            Row {
                id: filterRow
                spacing: P.Style.spaceSm
                Repeater {
                    model: page.filters
                    P.ActionButton {
                        objectName: "uiOrderFilter" + modelData.id
                        variant: "chip"
                        selected: page.filter === modelData.id
                        text: modelData.label + (modelData.id !== "all"
                              ? " " + ({ charging: page.counts.charging,
                                         waiting_payment: page.counts.waitingPayment,
                                         completed: page.counts.completed }[modelData.id] || 0)
                              : "")
                        onClicked: {
                            if (page.filter === modelData.id) return
                            page.filter = modelData.id
                            load(true)   // 在途时是意图登记：过期检测负责落定后重查
                        }
                    }
                }
            }
        }

        P.PullToRefreshArea {
            id: listScroll
            objectName: "uiOrderListStack"
            width: parent.width
            height: parent.height - y
            spacingHint: P.Style.spaceMd
            onRefreshRequested: {
                orderService.fetchStatusCounts()
                load(true)
            }

            // ---- mixed rows: month headers + order cards ----
            Repeater {
                model: page.rows
                Loader {
                    width: listScroll.width
                    height: item ? item.height : 0
                    sourceComponent: modelData.kind === "month"
                                     ? monthHeaderComp : orderCardComp
                    onLoaded: {
                        item.row = modelData
                        if (modelData.fade && P.Style.motionEnabled)
                            rowDelay.start()
                        else
                            opacity = 1.0
                    }
                    opacity: 0
                    // widgets motion parity: 40ms stagger, 8-row cap.
                    // (6.2 锚定动画拒绝 startDelay 赋值 → Timer 先延时再起跑)
                    Timer {
                        id: rowDelay
                        interval: Math.min(modelData.sIndex, 8) * 40
                        onTriggered: rowIn.start()
                    }
                    NumberAnimation on opacity {
                        id: rowIn
                        from: 0; to: 1; duration: P.Style.durEnter
                    }
                }
            }

            Component {
                id: monthHeaderComp
                Item {
                    property var row: ({})
                    height: Math.round(30 * P.Style.fontScaleFactor)
                    objectName: "uiMonthHeader"
                    Text {
                        anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                        text: row.title || ""
                        font.pixelSize: P.Style.fontLg2; font.weight: Font.Bold
                        color: P.Style.ink
                    }
                    Text {
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        text: row.summary || ""
                        font.pixelSize: P.Style.fontSm; color: P.Style.muted
                    }
                }
            }

            Component {
                id: orderCardComp
                P.ClickableCard {
                    id: card
                    property var row: ({})
                    readonly property var o: row.order || ({})
                    objectName: "uiOrderCard"
                    width: listScroll.width
                    height: Math.round(88 * P.Style.fontScaleFactor)
                    onClicked: {
                        if (App && row.order) App.navigate("order_detail", row.order)
                    }
                    // body 是 Column——内部一律用 Item 承载 anchors 布局（6.2）
                    Item {
                        height: Math.max(Math.round(56 * P.Style.fontScaleFactor),
                                         bodyCol.implicitHeight + 8)
                        width: parent ? parent.width : 0
                        Rectangle {
                            id: hub
                            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                            width: Math.round(44 * P.Style.fontScaleFactor)
                            height: Math.round(44 * P.Style.fontScaleFactor)
                            radius: width / 2
                            color: page.hubOf(card.o.status).bg
                            Image {
                                anchors.centerIn: parent
                                width: Math.round(18 * P.Style.fontScaleFactor)
                                height: width
                                source: Glyphs.source(page.hubOf(card.o.status).g,
                                                      page.hubOf(card.o.status).fg)
                            }
                        }
                        Item {
                            id: rightCol
                            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            width: Math.round(92 * P.Style.fontScaleFactor)
                            height: Math.max(Math.round(52 * P.Style.fontScaleFactor),
                                             moneyRow.implicitHeight
                                             + statusTag.implicitHeight + 6)
                            Row {
                                id: moneyRow
                                anchors.right: parent.right; anchors.top: parent.top
                                spacing: 2
                                Text {
                                    text: "¥"
                                    font.pixelSize: P.Style.fontSm; font.weight: Font.Bold
                                    color: P.Style.muted
                                    height: moneyVal.height
                                    verticalAlignment: Text.AlignBottom
                                    bottomPadding: 3
                                }
                                Text {
                                    id: moneyVal
                                    text: page.money(card.o.amountCents || 0)
                                    font.pixelSize: Math.round(17 * P.Style.fontScaleFactor)
                                    font.weight: Font.ExtraBold
                                    color: P.Style.ink
                                }
                            }
                            P.StatusTag {
                                id: statusTag
                                objectName: "uiOrderCardStatus"
                                anchors.right: parent.right; anchors.bottom: parent.bottom
                                tone: page.statusTone[card.o.status] || "neutral"
                                text: page.statusCn[card.o.status] || card.o.status || ""
                            }
                        }
                        Column {
                            id: bodyCol
                            anchors.left: hub.right; anchors.right: rightCol.left
                            anchors.leftMargin: P.Style.spaceMd
                            anchors.rightMargin: P.Style.spaceMd
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                width: parent.width; elide: Text.ElideRight
                                text: card.o.stationName || "充电站"
                                font.pixelSize: P.Style.fontLg2; font.weight: Font.DemiBold
                                color: P.Style.ink
                            }
                            Text {
                                width: parent.width; elide: Text.ElideRight
                                text: (card.o.orderNo || "--") + " · 桩号 " + (card.o.chargerCode || "--")
                                font.pixelSize: P.Style.fontSm; color: P.Style.faint
                            }
                            Text {
                                width: parent.width; elide: Text.ElideRight
                                text: (App ? App.displayTime(card.o.createdAt || "") : (card.o.createdAt || "")) + " · "
                                      + ((card.o.energyWh || 0) / 1000).toFixed(2) + " kWh"
                                font.pixelSize: P.Style.fontSm; color: P.Style.muted
                            }
                        }
                    }
                }
            }

            // ---- empty state（数据落定且确无订单）----
            Item {
                width: listScroll.width
                height: 240
                visible: page.rows.length === 0 && !page.reqActive
                Column {
                    anchors.centerIn: parent
                    spacing: P.Style.spaceSm
                    Image {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: Math.round(36 * P.Style.fontScaleFactor)
                        height: width
                        source: Glyphs.source("receipt", P.Style.faint)
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "暂无订单"
                        font.pixelSize: P.Style.fontLg2; font.weight: Font.DemiBold
                        color: P.Style.ink
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "下拉刷新，或换个筛选看看"
                        font.pixelSize: P.Style.fontSm; color: P.Style.muted
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
