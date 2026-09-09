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

    // 经验等级引擎（2026-09-09）：客户端本地成长系统，经 App.progressService
    // 透传；裸引擎/无 App 场景取 null，等级卡整体隐藏（页面对测试上下文健壮）。
    readonly property var progress: (App && App.progressService) ? App.progressService : null

    // 会员中心批（参考"白金会员"卡形态）：档位主题色与星级（页内字面量，
    // 与 LevelPage tierColor 同谱——两处小重复换零跨页耦合，映射稿有口径）。
    function tierColor(lv) {
        const c = ["#B0764A", "#8E9AAF", "#D9A32B", "#5FA8D3", "#3B3A52"]
        return c[Math.max(0, Math.min(4, (lv || 1) - 1))]
    }
    function tierStars(lv) {
        const n = Math.max(1, Math.min(5, lv || 1))
        return "★".repeat(n) + "☆".repeat(5 - n)
    }

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
                        color: "#9AA4B2"
                        border.width: 2; border.color: "#66FFFFFF"
                        Text {
                            anchors.centerIn: parent
                            text: page.user && page.user.avatarKey
                                  ? (page.user.nickname ? page.user.nickname[0] : "用") : "👤"
                            font.pixelSize: 22; font.bold: true; color: P.Style.surface
                            visible: profileAvatar.status !== Image.Ready
                        }
                        Image {
                            id: profileAvatar
                            objectName: "profileAvatarImage"
                            anchors.fill: parent
                            anchors.margins: 2
                            source: page.user && String(page.user.avatarKey || "").indexOf("data:image/png;base64,") === 0
                                    ? page.user.avatarKey : ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
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
                        // 等级三件套（经验等级批）已于会员中心批移出 hero——
                        // 现在是昵称框与钱包框之间独立的「会员等级卡」（uiLevelCard）。
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

            // ---------- ①.5 会员等级卡（会员中心批，参考"白金会员"卡形态） ----------
            // 昵称框与余额框之间的独立大卡：档位名+星级 → 成长值条 → 经验行
            // → 当前档权益；点卡任意处进会员中心页（等级+每日任务+礼包记录）。
            // 锚点沿用经验等级批：uiLevelBar/uiLevelXpLabel/uiLevelBadgeButton
            // （右侧"会员中心›"入口），回归钉不破。
            Rectangle {
                objectName: "uiLevelCard"
                visible: page.progress !== null
                width: col.contentW
                height: 118
                radius: P.Style.radiusLg
                gradient: Gradient {
                    orientation: Gradient.Horizontal   // Qt6.2 无 Diagonal
                    GradientStop { position: 0.0
                        color: page.progress ? page.tierColor(page.progress.level) : P.Style.heroFrom }
                    GradientStop { position: 1.0
                        color: Qt.lighter(page.progress ? page.tierColor(page.progress.level)
                                                         : P.Style.heroTo, 1.35) }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { if (App) App.navigate("level") }
                }
                Item {
                    anchors.fill: parent
                    anchors.leftMargin: 20; anchors.rightMargin: 16
                    anchors.topMargin: 13; anchors.bottomMargin: 12
                    Column {
                        anchors.left: parent.left; anchors.right: levelEntry.left
                        anchors.rightMargin: P.Style.spaceMd
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 5
                        Row {
                            spacing: P.Style.spaceSm
                            Text {
                                objectName: "uiLevelCardTier"
                                text: page.progress ? (page.progress.tierGlyph + " "
                                                       + page.progress.tierName) : ""
                                font.pixelSize: P.Style.fontLg2; font.weight: Font.Bold
                                color: "white"
                            }
                            Text {
                                objectName: "uiLevelCardStars"
                                anchors.verticalCenter: parent.verticalCenter
                                text: page.progress ? page.tierStars(page.progress.level) : ""
                                font.pixelSize: P.Style.fontSm
                                color: P.Style.starGold
                            }
                        }
                        Rectangle {
                            objectName: "uiLevelBar"
                            width: parent.width; height: 7; radius: 3.5
                            color: "#40FFFFFF"
                            Rectangle {
                                objectName: "uiLevelBarFill"
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                height: parent.height; radius: parent.radius
                                width: parent.width * (page.progress ? page.progress.progress : 0)
                                color: "white"
                                Behavior on width {
                                    enabled: P.Style.motionEnabled
                                    NumberAnimation { duration: P.Style.durValue }
                                }
                            }
                        }
                        Text {
                            objectName: "uiLevelXpLabel"
                            width: parent.width; elide: Text.ElideRight
                            text: page.progress
                                  ? (page.progress.xpToNext > 0
                                     ? "成长值 " + page.progress.xp
                                       + "/" + (page.progress.xp + page.progress.xpToNext)
                                       + " · 距" + page.progress.nextTierName
                                       + "还需 " + page.progress.xpToNext + " XP"
                                     : "成长值 " + page.progress.xp + " · 已达巅峰档位")
                                  : ""
                            font.pixelSize: P.Style.fontXs; color: "#E6FFFFFF"
                        }
                        Text {
                            objectName: "uiLevelCardPerk"
                            width: parent.width; elide: Text.ElideRight
                            text: page.progress
                                  ? String(page.progress.tiers[
                                       Math.max(0, page.progress.level - 1)].perk) : ""
                            font.pixelSize: P.Style.fontXs; color: "#BFFFFFFF"
                        }
                    }
                    Item {
                        id: levelEntry
                        objectName: "uiLevelBadgeButton"
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        width: entryCol.implicitWidth; height: entryCol.implicitHeight
                        Column {
                            id: entryCol
                            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                anchors.right: parent.right
                                text: "会员中心 ›"
                                font.pixelSize: P.Style.fontSm; font.weight: Font.DemiBold
                                color: "white"
                            }
                            Text {
                                anchors.right: parent.right
                                text: page.progress
                                      ? "查看 " + (5 - page.progress.level + 1) + " 档权益" : ""
                                font.pixelSize: P.Style.fontXs; color: "#BFFFFFFF"
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -6
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { if (App) App.navigate("level") }
                        }
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
                    // 2026-09-08 成员2 新增消息通知行（fe325d3）与成员3 四入口行合并：
                    // 月报/签到积分为成员3 新页，优惠券接成员2 CouponPage。
                    { obj: "openNotificationsButton", row: "🔔　消息通知", route: "notifications" },
                    { obj: "openStatsButton",     row: "📊　充电月报", route: "stats" },
                    { obj: "openCouponButton",    row: "🎫　优惠券",  route: "coupon" },
                    { obj: "openPointsButton",    row: "🪙　签到积分", route: "points" },
                    // 每日任务/会员等级两行（经验等级批）已被上方等级卡+会员中心
                    // 页吸收（路由保留可深链）；会员中心批与设置并列新增积分商城。
                    { obj: "openMallButton",      row: "🛍️　积分商城", route: "points_mall" },
                    { obj: "openRatingsButton",   row: "⭐　我的评价", route: "ratings" },
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
