import QtQuick
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// 积分页（成员3 新页，2026-09-08 批次C；2026-09-09 去签到化）。
// GET_POINTS 展示端：总分 = 服务端 SUM(points_ledger) 单一事实源
// （pointsService 桥透传），客户端不做累加对账。
// 签到入口已上移至「我的」页名片卡胶囊（heroCheckInPill），本页只陈述与流水，
// 不再放签到按钮/文案（用户反馈 2026-09-09："积分页不要再出现签到"）。
// 获取途径两类：每日签到（在"我的"页）+ 订单支付结算返点（1 元 = 1 分，
// 比例 TODO(contract)）。卡面沿用月报/订单页设计语言：hero 总分 + 流水卡
// + 级联入场 + 下拉刷新。
Item {
    id: page
    objectName: "pointsPage"
    property string route: "points"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    // 在途身份镜像（StatsPage 同款单飞口径）：本页面至多一条 GET_POINTS 在途。
    property bool reqActive: false
    property bool loadedOnce: false
    property int points: 0

    ListModel { id: ledgerModel; objectName: "uiPointsModel" }

    function fmtTime(iso) {
        const d = new Date(iso)
        if (isNaN(d.getTime())) return ""
        const p = function (n) { return (n < 10 ? "0" : "") + n }
        return p(d.getMonth() + 1) + "-" + p(d.getDate()) + " " + p(d.getHours()) + ":" + p(d.getMinutes())
    }
    function load() {
        if (reqActive) return                 // 本页至多一条在途
        if (pointsService.isBusy()) {
            listScroll.setRefreshing(false)   // 他页在途同服务：静默放行
            return
        }
        reqActive = true
        pointsService.fetchPoints(1, 20)
    }

    Connections {
        target: pointsService
        function onPointsLoaded(points, entries, total) {
            page.points = points
            ledgerModel.clear()
            for (var i = 0; i < entries.length; ++i)
                ledgerModel.append(entries[i])
            reqActive = false
            loadedOnce = true
            listScroll.setRefreshing(false)
        }
        function onOperationFailed(type, code, message) {
            if (type !== "GET_POINTS") return   // 签到在途归"我的"页胶囊管，本页不接
            reqActive = false
            listScroll.setRefreshing(false)
            if (App) App.showToast("积分加载失败：" + message, "danger")
        }
    }
    Component.onCompleted: load()

    P.PullToRefreshArea {
        id: listScroll
        objectName: "uiPointsListStack"
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
                objectName: "uiPointsTitle"
                text: "积分"
                font.pixelSize: P.Style.fontXl; font.weight: Font.Bold; color: P.Style.ink
            }
            Text {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                objectName: "uiPointsCaption"
                text: "支付结算 1 元 = 1 分"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
        }

        // ---- hero 总积分卡（签到按钮已上移至"我的"页名片卡）----
        Rectangle {
            objectName: "uiPointsHero"
            width: listScroll.width
            height: 104
            radius: P.Style.radiusLg
            gradient: Gradient {
                orientation: Gradient.Horizontal   // Qt6.2 无 Diagonal
                GradientStop { position: 0.0; color: P.Style.heroFrom }
                GradientStop { position: 1.0; color: P.Style.heroTo }
            }
            Item {
                anchors.fill: parent
                anchors.leftMargin: 20; anchors.rightMargin: 16
                anchors.topMargin: 14; anchors.bottomMargin: 12
                Column {
                    anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text {
                        objectName: "uiPointsTotal"
                        text: String(page.points)
                        font.pixelSize: P.Style.fontHero; font.weight: Font.ExtraBold
                        color: P.Style.surface
                    }
                    Text {
                        text: "当前积分 · 流水明细在下方"
                        font.pixelSize: P.Style.fontSm; color: P.Style.heroPhone
                    }
                }
            }
        }

        // ---- 流水卡列表（级联入场，月报同款节拍）----
        Repeater {
            model: ledgerModel
            Rectangle {
                objectName: "uiPointsLedgerCard"
                width: listScroll.width
                height: 64
                radius: P.Style.radiusLg
                color: P.Style.surface
                border.width: 1
                border.color: P.Style.line
                Item {
                    anchors.fill: parent
                    anchors.leftMargin: 16; anchors.rightMargin: 16
                    Rectangle {
                        id: hub
                        anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                        width: 40; height: 40; radius: 20
                        color: P.Style.brandSoft
                        Image {
                            anchors.centerIn: parent
                            width: Math.round(20 * P.Style.fontScaleFactor)
                            height: width
                            source: Glyphs.source("coin", P.Style.brandDeep)
                        }
                    }
                    Column {
                        anchors.left: hub.right
                        anchors.right: amountText.left
                        anchors.leftMargin: P.Style.spaceMd
                        anchors.rightMargin: P.Style.spaceMd
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Text {
                            objectName: "uiPointsLedgerReason"
                            width: parent.width; elide: Text.ElideRight
                            text: model.reason || "积分"
                            font.pixelSize: P.Style.fontLg2; font.weight: Font.DemiBold
                            color: P.Style.ink
                        }
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            objectName: "uiPointsLedgerTime"
                            text: page.fmtTime(model.createdAtUtc)
                            font.pixelSize: P.Style.fontSm; color: P.Style.faint
                        }
                    }
                    Text {
                        id: amountText
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        objectName: "uiPointsLedgerAmount"
                        text: (model.amount >= 0 ? "+" : "") + model.amount
                        font.pixelSize: 17; font.weight: Font.ExtraBold
                        color: model.amount >= 0 ? P.Style.brandDeep : P.Style.danger
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
            objectName: "uiPointsEmptyNotice"
            width: listScroll.width
            height: 200
            visible: page.loadedOnce && !page.reqActive && ledgerModel.count === 0
            glyph: "coin"
            title: "还没有积分流水"
            description: "签到（在「我的」页名片卡）或支付订单后，获得的积分会记在这里。"
            actionText: ""
        }
    }
}
