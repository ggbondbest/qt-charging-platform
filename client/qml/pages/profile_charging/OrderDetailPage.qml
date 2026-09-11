import QtQuick
import QtQuick.Controls.Basic   // 批次E 评价卡用 TextField
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

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
    property bool paying: false
    function dur(sec) {
        var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60)
        return (h > 0 ? h + " 小时 " : "") + m + " 分钟"
    }
    function fmtRatingTime(iso) {
        const d = new Date(iso)
        if (isNaN(d.getTime())) return ""
        const p = function (n) { return (n < 10 ? "0" : "") + n }
        return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) + " "
            + p(d.getHours()) + ":" + p(d.getMinutes())
    }

    // ———— 批次E：完成态电桩评价（一单一评，服务端 order_id UNIQUE 幂等；
    // 改评 TODO(contract) 二期）。GET_MY_RATINGS 只取首页 20 条——老单评价落在
    // 首页外时本页仍显示可编辑形态，提交由服务端幂等返回 alreadyRated +
    // 首评原值，页面据此进入"已评价"态，不产生第二行。
    property var myRating: null            // 非 null = 已评价（服务端行形）
    property bool ratingReqActive: false
    property bool ratingSubmitting: false
    // 在途 SUBMIT 归属戳（审查 P2#6）：记录发起提交的订单 id。响应到达时
    // 与当前页面订单比对——提交 A → 切到 B → A 的延迟响应不得写入 B 页。
    property string ratingSubmitOrder: ""
    property int pickedStars: 0
    function loadRating() {
        const a = page.arg
        if (!a || a.status !== "completed" || a.id === undefined || a.id === "") return
        if (page.myRating !== null || page.ratingReqActive) return
        if (ratingsService.isBusy()) return   // 与他页共用单飞：静默放行
        page.ratingReqActive = true
        ratingsService.fetchMyRatings(1, 20)
    }
    function submitRatingNow() {
        if (page.ratingSubmitting || page.pickedStars < 1) return
        if (ratingsService.isBusy()) {
            if (App) App.showToast("评价服务正在处理其他请求，请稍候", "info")
            return
        }
        page.ratingSubmitting = true
        page.ratingSubmitOrder = String(page.arg.id)
        ratingsService.submitRating(String(page.arg.id), page.pickedStars,
                                    ratingCommentField.text)
    }
    Connections {
        target: ratingsService
        function onRatingsLoaded(ratings, total) {
            page.ratingReqActive = false
            const id = page.arg && page.arg.id !== undefined ? String(page.arg.id) : ""
            if (!id) return
            for (var i = 0; i < ratings.length; ++i) {
                if (String(ratings[i].orderId) === id) {
                    page.myRating = ratings[i]
                    return
                }
            }
        }
        function onRatingSubmitted(row, alreadyRated) {
            const id = page.arg && page.arg.id !== undefined ? String(page.arg.id) : ""
            // 单飞保证同刻只有一笔在途提交：响应到达即解除在途标志，
            // 但仅当归属匹配（本页发起 + 响应行同单）才展示结果。
            const mine = page.ratingSubmitOrder !== ""
                         && page.ratingSubmitOrder === id
                         && row && String(row.orderId) === id
            if (page.ratingSubmitOrder !== "") {
                page.ratingSubmitting = false
                page.ratingSubmitOrder = ""
            }
            if (!mine) return   // 旧订单的迟到响应：忽略，不污染当前页面
            page.myRating = row
            if (App) App.showToast(alreadyRated
                ? "该订单已评价过，为你展示首次评价" : "评价成功，感谢反馈",
                alreadyRated ? "info" : "success")
        }
        function onOperationFailed(type, code, message) {
            if (type === "GET_MY_RATINGS") { page.ratingReqActive = false; return }
            if (type !== "SUBMIT_CHARGER_RATING") return
            const id = page.arg && page.arg.id !== undefined ? String(page.arg.id) : ""
            const mine = page.ratingSubmitOrder !== "" && page.ratingSubmitOrder === id
            if (page.ratingSubmitOrder !== "") {
                page.ratingSubmitting = false
                page.ratingSubmitOrder = ""
            }
            if (!mine) return   // 旧订单提交的失败：不向当前订单页弹错误
            if (App) App.showToast("评价提交失败：" + message, "danger")
        }
    }
    // 深链截图模式 arg 异步补齐（onOrdersLoaded）时再尝试拉评价。
    // 页面实例跨订单复用（Shell 只换 arg）：清展示态防上一单残留；
    // 在途提交标志不动，由响应按归属戳自行收口。
    onArgChanged: {
        page.myRating = null
        page.pickedStars = 0
        page.ratingReqActive = false
        loadRating()
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
            if (!page.paying) return
            page.paying = false
            if (!App) return
            App.showToast("支付成功 ¥" + (amountCents / 100).toFixed(2)
                          + "，余额 ¥" + (balanceAfterCents / 100).toFixed(2), "success")
            App.navigate("order")
        }
        function onOperationFailed(type, code, message) {
            if (type !== "PAY_ORDER" || !page.paying) return
            page.paying = false
            if (App) App.showToast("支付失败：" + message, "danger")
        }
    }
    Component.onCompleted: { ensureData(); loadRating() }

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
                        { k: "停止原因", v: ({"TARGET_AMOUNT": "按预算自动结束", "TARGET_ENERGY": "达到电量目标", "TARGET_DURATION": "达到时长目标", "MANUAL": "用户主动结束"})[page.arg.stopReason] || "—" },
                        { k: "下单时间（北京时间）", v: App ? App.displayTime(page.arg.createdAt || "—") : "—" },
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

        P.ActionButton {
            objectName: "orderManageReservationButton"
            visible: page.arg.status === "reserved"
            width: parent.width
            text: "管理预约 / 取消预约"
            variant: "secondary"
            onClicked: App.navigate("reservation_module")
        }

        // ———— 批次E：电桩评价卡（仅完成态）————
        P.Card {
            objectName: "uiOrderRatingCard"
            visible: page.arg.status === "completed"
            width: parent.width
            Column {
                width: parent.width
                spacing: P.Style.spaceMd

                Text {
                    objectName: "uiRatingTitle"
                    text: page.myRating !== null ? "你已评价过这单" : "给这根桩打个分"
                    font.pixelSize: P.Style.fontLg; font.weight: Font.Bold; color: P.Style.ink
                }

                // 已评价：只读星行 + 首评原文 + 时间（改评 TODO(contract) 二期）
                Column {
                    visible: page.myRating !== null
                    width: parent.width
                    spacing: 4
                    Row {
                        objectName: "uiRatingReadStars"
                        spacing: 2
                        Repeater {
                            model: page.myRating !== null ? page.myRating.rating : 0
                            Image {
                                width: Math.round(18 * P.Style.fontScaleFactor)
                                height: width
                                source: Glyphs.source("star-filled", P.Style.warning)
                            }
                        }
                    }
                    Text {
                        objectName: "uiRatingReadComment"
                        width: parent.width
                        visible: text.length > 0
                        text: page.myRating !== null ? String(page.myRating.comment || "") : ""
                        wrapMode: Text.Wrap
                        font.pixelSize: P.Style.fontMd; color: P.Style.muted
                    }
                    Text {
                        objectName: "uiRatingReadTime"
                        visible: text.length > 0
                        text: page.myRating !== null ? page.fmtRatingTime(page.myRating.createdAtUtc) : ""
                        font.pixelSize: P.Style.fontSm; color: P.Style.faint
                    }
                }

                // 未评价：可点五星 + 留言 + 提交
                Column {
                    visible: page.myRating === null
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Row {
                        objectName: "uiRatingStarPicker"
                        spacing: 6
                        Repeater {
                            model: 5
                            Item {
                                width: Math.round(32 * P.Style.fontScaleFactor)
                                height: Math.round(34 * P.Style.fontScaleFactor)
                                Image {
                                    anchors.centerIn: parent
                                    width: Math.round(26 * P.Style.fontScaleFactor)
                                    height: width
                                    source: index < page.pickedStars
                                            ? Glyphs.source("star-filled", P.Style.warning)
                                            : Glyphs.source("star", P.Style.faint)
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: page.pickedStars = index + 1
                                }
                            }
                        }
                    }
                    P.TextField {
                        id: ratingCommentField
                        objectName: "uiRatingCommentEdit"
                        width: parent.width
                        placeholderText: "说说这次体验（选填，140 字内）"
                        // 6.2 TextField 无 maxLength：软截断；服务端 normalize
                        // 的 ≤140 trim 校验才是最终裁决。
                        onTextChanged: if (text.length > 140) text = text.slice(0, 140)
                    }
                    P.ActionButton {
                        objectName: "uiRatingSubmitButton"
                        width: parent.width
                        variant: "primary"
                        enabled: page.pickedStars >= 1 && !page.ratingSubmitting
                        text: page.ratingSubmitting ? "提交中…"
                              : page.pickedStars >= 1 ? "提交评价 · " + page.pickedStars + " 星"
                              : "请先选择星级"
                        onClicked: page.submitRatingNow()
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
            enabled: !page.paying
            onClicked: if (chargingService) { page.paying = true; chargingService.payOrder(String(page.arg.id)) }
        }
        P.ActionButton {
            objectName: "orderDetailChargingButton"
            visible: page.arg.status === "charging"
            width: parent.width
            variant: "primary"; text: "查看实时充电"
            onClicked: if (App) App.navigate("charging_run", page.arg)
        }
        P.ActionButton {
            visible: page.arg.status === "waiting_payment"
            width: parent.width; variant: "secondary"; text: "去充值"
            onClicked: if (App) App.navigate("recharge")
        }
        P.ActionButton {
            variant: "ghost"; text: "返回"
            onClicked: if (App) App.back()
        }
    }
}
