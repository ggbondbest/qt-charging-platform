import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets ProfilePage (objectName "profilePage").
// Skeleton mirrors pages/profile_charging/src/profile_page.cpp buildUi():
//   1) 150px edge-to-edge gradient hero (64px avatar / nickname+phone / hint)
//   2) wallet float card: 余额 | 充值 | 充值记录 三列同卡（1×40 divider）
//   3) 双格入口：我的订单（待支付角标）/ 我的预约
//   4) 「账号与服务」分组：收藏 / 设置 52px 行
//   5) 退出登录 = 白卡红字 logout variant（不是 danger 红底）
// 结构由成员3 于 2026-09-07 按 widgets 原版重建（成员2 文件，PR 报备）。
Item {
    id: page
    objectName: "profilePage"
    property string route: "profile"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property var user: (App && App.currentUser) ? App.currentUser : null
    property int waitingCount: 0
    function money(cents) { return ((cents || 0) / 100).toFixed(2) }

    // 待支付角标数据（widgets ordersCell waitingBadge_ 同款）
    Connections {
        target: orderService
        function onStatusCountsUpdated(chargingCount, waitingPaymentCount, completedCount) {
            page.waitingCount = waitingPaymentCount
        }
    }
    Component.onCompleted: { try { orderService.fetchStatusCounts() } catch (e) {} }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: col.implicitHeight
        clip: true

        Column {
            id: col
            width: parent.width
            padding: P.Style.spaceLg
            spacing: P.Style.spaceMd
            readonly property int contentW: width - padding * 2

            // ---------- ① 渐变头图（顶到边、150 高、64 头像） ----------
            Rectangle {
                objectName: "profileHeroButton"
                x: -col.padding               // 抵消 Column padding 顶到左右边
                width: col.width
                height: 150
                radius: 0
                gradient: Gradient {
                    orientation: Gradient.Horizontal   // Qt6.2 无 Diagonal
                    GradientStop { position: 0.0; color: P.Style.heroFrom }
                    GradientStop { position: 1.0; color: P.Style.heroTo }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { if (App) App.navigate("profile_edit") }
                }
                Item {
                    anchors.fill: parent
                    anchors.leftMargin: 22; anchors.topMargin: 28
                    anchors.rightMargin: 20; anchors.bottomMargin: 26
                    Rectangle {
                        id: avatarHub
                        objectName: "uiAvatarHub"
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 64; height: 64; radius: 32
                        color: P.Style.brand
                        border.width: 2; border.color: "#66FFFFFF"
                        Text {
                            anchors.centerIn: parent
                            text: page.user && page.user.nickname ? page.user.nickname[0] : "用"
                            font.pixelSize: 22; font.bold: true; color: P.Style.surface
                        }
                    }
                    Column {
                        objectName: "profileIdentity"
                        anchors.left: avatarHub.right
                        anchors.leftMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 64 - 90 - 14
                        spacing: 4
                        Text {
                            objectName: "nicknameLabel"
                            width: parent.width; elide: Text.ElideRight
                            text: page.user ? (page.user.nickname || "未设置") : "未登录"
                            font.pixelSize: P.Style.fontHero; font.bold: true; color: P.Style.surface
                        }
                        Text {
                            objectName: "heroPhoneLabel"
                            text: page.user ? (page.user.phone || "--") : "点这里登录"
                            font.pixelSize: P.Style.fontSm; color: P.Style.heroPhone
                        }
                    }
                    Text {
                        objectName: "profileEditHint"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: "编辑资料 ›"; font.pixelSize: P.Style.fontSm
                        font.weight: Font.DemiBold; color: P.Style.heroPhone
                    }
                }
            }

            // ---------- ② 钱包三列浮卡（一张卡：余额 | 充值 | 充值记录） ----------
            Rectangle {
                objectName: "walletCard"
                width: col.contentW
                height: 86
                radius: P.Style.radiusLg
                color: P.Style.surface
                border.width: 1
                border.color: P.Style.line
                Row {
                    anchors.fill: parent
                    anchors.margins: 1
                    readonly property int cellW: (width - 2) / 3
                    // 余额列（值上注下，点击进钱包）
                    Item {
                        width: parent.cellW; height: parent.height
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: { if (App) App.navigate("wallet") } }
                        Column {
                            anchors.centerIn: parent
                            spacing: 4
                            Text {
                                objectName: "balanceLabel"
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "¥ " + (page.user ? money(page.user.balanceCents) : "--")
                                font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.brandDeep
                            }
                            Text { anchors.horizontalCenter: parent.horizontalCenter
                                text: "余额（元）"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                            }
                    }
                    Rectangle { width: 1; height: 40
                        y: (parent.height - height) / 2; color: P.Style.line }
                    Repeater {
                        model: [
                            { obj: "openRechargeButton", glyph: "💳", caption: "充值",     route: "recharge" },
                            { obj: "openWalletButton",   glyph: "🧾", caption: "充值记录", route: "wallet" }
                        ]
                        delegate: Item {
                            objectName: modelData.obj
                            width: parent.cellW; height: 84
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: { if (App) App.navigate(modelData.route) } }
                            Column {
                                anchors.centerIn: parent
                                spacing: 4
                                Text { anchors.horizontalCenter: parent.horizontalCenter
                                    text: modelData.glyph; font.pixelSize: 18 }
                                Text { anchors.horizontalCenter: parent.horizontalCenter
                                    text: modelData.caption; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                            }
                        }
                    }
                }
            }

            // ---------- ③ 双格入口：我的订单 / 我的预约 ----------
            Row {
                objectName: "profileCellsRow"
                width: col.contentW
                spacing: P.Style.spaceSm
                Repeater {
                    model: [
                        { obj: "openOrdersButton",       glyph: "📋", title: "我的订单",
                          caption: "全部充电订单", route: "order",           badge: true },
                        { obj: "openReservationsButton", glyph: "📒", title: "我的预约",
                          caption: "时段预约记录", route: "reservation_module", badge: false }
                    ]
                    delegate: Rectangle {
                        objectName: modelData.obj
                        width: (col.contentW - P.Style.spaceSm) / 2
                        height: 78
                        radius: P.Style.radiusLg
                        color: P.Style.surface
                        border.width: 1
                        border.color: P.Style.line
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: { if (App) App.navigate(modelData.route) } }
                        Item {
                            anchors.fill: parent
                            anchors.margins: 16
                            Text {
                                id: cellIcon
                                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                                text: modelData.glyph; font.pixelSize: 22
                            }
                            Column {
                                anchors.left: cellIcon.right; anchors.leftMargin: P.Style.spaceSm
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3
                                Text { text: modelData.title
                                    font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink }
                                Text { text: modelData.caption
                                    font.pixelSize: P.Style.fontSm; color: P.Style.faint }
                            }
                            // 待支付角标（订单格）/ chevron（预约格）钉右缘
                            P.StatusTag {
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                visible: modelData.badge && page.waitingCount > 0
                                tone: "warning"; text: String(page.waitingCount)
                            }
                            Text {
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                visible: !modelData.badge
                                text: "›"; font.pixelSize: P.Style.fontLg2; color: P.Style.faint
                            }
                        }
                    }
                }
            }

            // ---------- ④ 账号与服务（52px 行） ----------
            Text {
                objectName: "sectionTitleAccount"
                text: "账号与服务"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
            Repeater {
                model: [
                    { obj: "openFavoritesButton", row: "⭐　收藏",   route: "favorites" },
                    // 2026-09-08 成员3 追加两入口行（成员2 文件，PR 报备）：
                    // 月报为成员3 新页，优惠券接成员2 CouponPage。
                    { obj: "openStatsButton",     row: "📊　充电月报", route: "stats" },
                    { obj: "openCouponButton",    row: "🎫　优惠券",  route: "coupon" },
                    { obj: "openPointsButton",    row: "🪙　签到积分", route: "points" },
                    { obj: "openSettingsButton",  row: "⚙️　设置", route: "settings" }
                ]
                delegate: Rectangle {
                    objectName: modelData.obj
                    width: col.contentW
                    height: 52
                    radius: P.Style.radiusLg
                    color: P.Style.surface
                    border.width: 1
                    border.color: P.Style.line
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: { if (App) App.navigate(modelData.route) } }
                    Item {
                        anchors.fill: parent
                        anchors.leftMargin: P.Style.spaceLg; anchors.rightMargin: P.Style.spaceLg
                        Text {
                            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                            text: modelData.row
                            font.pixelSize: P.Style.fontMd; color: P.Style.ink
                        }
                        Text {
                            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            text: "›"; font.pixelSize: P.Style.fontLg2
                            color: "#C3CBD6"   // QSS cellChevron
                        }
                    }
                }
            }

            // ---------- ⑤ 退出登录：白卡红字（QSS #logoutButton） ----------
            P.ActionButton {
                objectName: "logoutButton"
                variant: "logout"
                text: "退出登录"
                width: col.contentW
                height: 50
                visible: !!(App && App.loggedIn)
                onClicked: {
                    // TODO(contract): authService.logout() invokable；成功后壳收 loginStateChanged 自动翻页
                    if (authService) {
                        try { authService.logout() } catch (e) {
                            if (App) App.showToast("退出登录桥未就绪", "warning")
                        }
                        return
                    }
                    // mock 通道 authService==nullptr（app_bridge.cpp:99）：回退 App.logout()
                    if (App) App.logout()
                }
            }
        }
    }

    // 登录态翻转时重取 currentUser（绑定已跟 App.currentUser，这里兜壳重推）
    Connections {
        target: App
        function onLoginStateChanged() { page.user = (App && App.currentUser) ? App.currentUser : null }
    }
}
