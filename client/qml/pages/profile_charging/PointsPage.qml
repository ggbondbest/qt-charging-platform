import QtQuick
import "../../platform" as P

// 签到·积分页（成员3 新页，2026-09-08 批次C）：CHECK_IN + GET_POINTS 展示端。
// 总分 = 服务端 SUM(points_ledger) 单一事实源（pointsService 桥透传），
// 客户端不做累加对账；日粒度幂等由服务端 user_checkins 主键裁决，
// 按钮只镜像最近一次响应（todayCheckedIn）。
// 卡面沿用月报/订单页设计语言：hero 总分 + 流水卡 + 级联入场 + 下拉刷新。
Item {
    id: page
    objectName: "pointsPage"
    property string route: "points"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    // 在途身份镜像（StatsPage 同款单飞口径）：本页面同时至多一条在途。
    property bool reqActive: false
    property bool loadedOnce: false
    property int points: 0
    // 今日已签镜像：签到成功/重放响应都会置真（日粒度幂等由服务端裁决）。
    property bool todayCheckedIn: false
    property bool checkingIn: false

    ListModel { id: ledgerModel; objectName: "uiPointsModel" }

    function fmtTime(iso) {
        const d = new Date(iso)
        if (isNaN(d.getTime())) return ""
        const p = function (n) { return (n < 10 ? "0" : "") + n }
        return p(d.getMonth() + 1) + "-" + p(d.getDate()) + " " + p(d.getHours()) + ":" + p(d.getMinutes())
    }
    function load() {
        if (reqActive || checkingIn) return    // 本页至多一条在途
        if (pointsService.isBusy()) {
            listScroll.setRefreshing(false)     // 他页在途同服务：静默放行
            return
        }
        reqActive = true
        pointsService.fetchPoints(1, 20)
    }
    function checkInNow() {
        if (checkingIn || todayCheckedIn || reqActive) return
        checkingIn = true
        pointsService.checkIn()
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
        function onCheckInCompleted(day, points, gained, alreadyCheckedIn) {
            checkingIn = false
            page.points = points
            page.todayCheckedIn = true          // 成功与重放都进入"已签"态
            // ---- 本人行（经验等级批）：签到成功回执→reportEvent("checkin") 经验钩子，积分真账与 XP 成长账各记各的 ----
            // 经验等级批（2026-09-09）：签到真实积分入账后，同步上报每日任务
            // 事件（当日幂等）；裸引擎/无等级桥场景静默跳过。
            if (typeof App !== "undefined" && App && App.progressService)
                App.progressService.reportEvent("checkin")
            if (alreadyCheckedIn || gained <= 0) {
                if (App) App.showToast("今天已经签过啦", "info")
            } else {
                if (App) App.showToast("签到成功 +" + gained + " 积分", "success")
            }
            load()                              // 拉最新流水（+10 入账行）
        }
        function onOperationFailed(type, code, message) {
            if (type !== "CHECK_IN" && type !== "GET_POINTS") return
            reqActive = false
            checkingIn = false
            listScroll.setRefreshing(false)
            if (App) App.showToast(type === "CHECK_IN"
                ? "签到失败：" + message : "积分加载失败：" + message, "danger")
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
                text: "签到 · 积分"
                font.pixelSize: P.Style.fontXl; font.weight: Font.Bold; color: P.Style.ink
            }
            Text {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                objectName: "uiPointsCaption"
                text: "每日签到 +10"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
        }

        // ---- hero 总积分卡（渐变 + 签到按钮）----
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
                P.ActionButton {
                    id: checkInButton
                    objectName: "uiCheckInButton"
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    variant: "secondary"
                    enabled: !page.todayCheckedIn && !page.checkingIn
                    text: page.todayCheckedIn ? "今日已签到"
                          : page.checkingIn ? "签到中…" : "签到 +10"
                    onClicked: page.checkInNow()
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
                        Text {
                            anchors.centerIn: parent
                            text: "🪙"; font.pixelSize: 17
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
            glyph: "🪙"
            title: "还没有积分流水"
            description: "点上方「签到」领取每日积分，攒够可在券包换福利。"
            actionText: ""
        }
    }
}
