import QtQuick
import "../../platform" as P

// 我的评价页（成员3 新页，2026-09-08 批次E）：GET_MY_RATINGS 展示端。
// 行 = 服务端联查响应形 {id,orderId,chargerId,chargerCode,stationName,
// rating,comment,createdAtUtc} 新→旧——桩名/站名由服务端 JOIN 定稿，
// 客户端零拼接；一单一评幂等锚在 order_id UNIQUE，本页只读（改评
// TODO(contract) 二期）。卡面沿用积分页设计语言：流水卡 + 级联入场 + 下拉刷新。
Item {
    id: page
    objectName: "ratingsPage"
    property string route: "ratings"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    // 在途身份镜像（PointsPage 同款单飞口径）：本页面同时至多一条在途。
    property bool reqActive: false
    property bool loadedOnce: false

    ListModel { id: ratingsModel; objectName: "uiRatingsModel" }

    function fmtTime(iso) {
        const d = new Date(iso)
        if (isNaN(d.getTime())) return ""
        const p = function (n) { return (n < 10 ? "0" : "") + n }
        return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) + " "
            + p(d.getHours()) + ":" + p(d.getMinutes())
    }
    function load() {
        if (reqActive) return                    // 本页至多一条在途
        if (ratingsService.isBusy()) {
            listScroll.setRefreshing(false)      // 他页在途同服务：静默放行
            return
        }
        reqActive = true
        ratingsService.fetchMyRatings(1, 20)
    }

    Connections {
        target: ratingsService
        function onRatingsLoaded(ratings, total) {
            ratingsModel.clear()
            for (var i = 0; i < ratings.length; ++i)
                ratingsModel.append(ratings[i])
            reqActive = false
            loadedOnce = true
            listScroll.setRefreshing(false)
        }
        function onOperationFailed(type, code, message) {
            if (type !== "GET_MY_RATINGS") return
            reqActive = false
            listScroll.setRefreshing(false)
            if (App) App.showToast("评价加载失败：" + message, "danger")
        }
    }
    Component.onCompleted: load()

    P.PullToRefreshArea {
        id: listScroll
        objectName: "uiRatingsListStack"
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacingHint: P.Style.spaceMd
        onRefreshRequested: load()

        // ---- header ----
        Item {
            width: listScroll.width
            height: 40
            Text {
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                objectName: "uiRatingsTitle"
                text: "我的评价"
                font.pixelSize: P.Style.fontXl; font.weight: Font.Bold; color: P.Style.ink
            }
            Text {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                objectName: "uiRatingsCaption"
                text: ratingsModel.count > 0 ? ratingsModel.count + " 条评价" : "一单一评"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
        }

        // ---- 评价卡列表（级联入场，积分页同款节拍）----
        Repeater {
            model: ratingsModel
            Rectangle {
                objectName: "uiRatingsCard"
                width: listScroll.width
                height: cardCol.implicitHeight + 28
                radius: P.Style.radiusLg
                color: P.Style.surface
                border.width: 1
                border.color: P.Style.line
                Column {
                    id: cardCol
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 16
                    spacing: 6
                    Row {
                        spacing: P.Style.spaceSm
                        Text {
                            objectName: "uiRatingsStation"
                            text: (model.stationName || "充电站")
                                + " · " + (model.chargerCode || "")
                            font.pixelSize: P.Style.fontLg2; font.weight: Font.DemiBold
                            color: P.Style.ink
                        }
                    }
                    // 五星行：实心星个数 = rating（服务端 CHECK 1..5 兜底）
                    Row {
                        id: starsRow
                        objectName: "uiRatingsStars"
                        spacing: 2
                        property int rating: model.rating
                        Repeater {
                            model: 5
                            Text {
                                text: index < starsRow.rating ? "★" : "☆"
                                font.pixelSize: 16
                                color: index < starsRow.rating ? P.Style.warning : P.Style.faint
                            }
                        }
                    }
                    Text {
                        objectName: "uiRatingsComment"
                        width: cardCol.width
                        visible: text.length > 0
                        text: model.comment || ""
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        font.pixelSize: P.Style.fontMd; color: P.Style.muted
                    }
                    Text {
                        objectName: "uiRatingsTime"
                        text: page.fmtTime(model.createdAtUtc)
                        font.pixelSize: P.Style.fontSm; color: P.Style.faint
                    }
                }
                Component.onCompleted: {
                    if (!P.Style.motionEnabled) {
                        opacity = 1.0
                        return
                    }
                    rowDelay.start()   // widgets motion parity: 40ms stagger, 8 行封顶
                }
                opacity: 0
                Timer {
                    id: rowDelay
                    interval: Math.min(index, 8) * 40
                    onTriggered: rowIn.start()
                }
                NumberAnimation on opacity {
                    id: rowIn
                    from: 0; to: 1; duration: P.Style.durEnter
                }
            }
        }

        // ---- 空态 ----
        P.NoticePanel {
            objectName: "uiRatingsEmptyNotice"
            width: listScroll.width
            height: 200
            visible: page.loadedOnce && !page.reqActive && ratingsModel.count === 0
            glyph: "⭐"
            title: "还没有评价"
            description: "充电订单完成后，在订单详情页就能给电桩打个分。"
            actionText: ""
        }
    }
}
