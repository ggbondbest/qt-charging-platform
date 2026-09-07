import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets ProfilePage (objectName "profilePage").
// Shell 路由表已把 "profile" 指到本文件。入口路由沿用旧页口径
// （wallet/recharge/order 已在 migrated 表内可直接 navigate；
//  reservation_module/favorites/settings 路由待成员3 明天追加，先 navigate 占位）。
Item {
    id: page
    objectName: "profilePage"
    property string route: "profile"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property var user: (App && App.currentUser) ? App.currentUser : null
    function money(cents) { return ((cents || 0) / 100).toFixed(2) }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: col.height
        clip: true

        Column {
            id: col
            width: parent.width
            padding: P.Style.spaceLg
            spacing: P.Style.spaceMd

            // 身份头图（QSS uiProfileHeroButton：电动绿对角渐变 + 白字系；
            // 原白卡版由成员3 于 2026-09-07 皮肤对齐轮升级，成员2 文件已报备；
            // 锚点恢复 widgets 名 uiProfileHeroButton=QSS 选择器同名）
            Rectangle {
                objectName: "uiProfileHeroButton"
                width: col.width - col.padding * 2
                height: 86
                radius: P.Style.radiusLg
                gradient: Gradient {
                    orientation: Gradient.Diagonal
                    GradientStop { position: 0.0; color: P.Style.heroFrom }
                    GradientStop { position: 1.0; color: P.Style.heroTo }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { if (App) App.navigate("profile_edit") }
                }
                Row {
                    anchors.fill: parent
                    anchors.margins: P.Style.spaceLg
                    spacing: P.Style.spaceMd
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 54; height: 54; radius: 27
                        color: P.Style.brand
                        border.width: 2; border.color: "#66FFFFFF"
                        Text {
                            anchors.centerIn: parent
                            text: page.user && page.user.nickname ? page.user.nickname[0] : "⚡"
                            font.pixelSize: 22; font.bold: true; color: P.Style.surface
                        }
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 54 - 90 - parent.spacing * 2
                        spacing: 2
                        Text {
                            objectName: "nicknameLabel"
                            width: parent.width; elide: Text.ElideRight
                            text: page.user ? (page.user.nickname || "未设置") : "未登录"
                            font.pixelSize: P.Style.fontHero; font.bold: true; color: P.Style.surface
                        }
                        Text {
                            text: page.user ? (page.user.phone || "") : "点这里登录"
                            font.pixelSize: P.Style.fontSm; color: P.Style.heroPhone
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "编辑资料 ›"; font.pixelSize: P.Style.fontSm
                        font.weight: Font.DemiBold; color: P.Style.heroPhone
                    }
                }
            }

            // 余额卡
            P.ClickableCard {
                objectName: "balanceButton"
                width: col.width - col.padding * 2
                height: 78
                onClicked: { if (App) App.navigate("wallet") }
                Column {
                    width: parent.width
                    spacing: P.Style.spaceXs
                    Text { text: "余额（元）"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                    Text {
                        objectName: "balanceLabel"
                        text: page.user ? money(page.user.balanceCents) : "--"
                        font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.brandDeep
                    }
                }
            }

            // 快捷两钮
            Row {
                objectName: "profileQuickActions"
                width: col.width - col.padding * 2
                spacing: P.Style.spaceSm
                P.ActionButton {
                    objectName: "openRechargeButton"
                    variant: "chip"; text: "💳 充值"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: { if (App) App.navigate("recharge") }
                }
                P.ActionButton {
                    objectName: "openWalletButton"
                    variant: "chip"; text: "🧾 充值记录"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: { if (App) App.navigate("wallet") }
                }
            }

            // 入口列表
            Repeater {
                model: [
                    { obj: "openOrdersButton",    glyph: "🧾", text: "我的订单", caption: "全部充电订单",   route: "order" },
                    { obj: "openReservationsButton", glyph: "📅", text: "我的预约", caption: "时段预约记录", route: "reservation_module" },
                    { obj: "openFavoritesButton", glyph: "⭐", text: "收藏",       caption: "",             route: "favorites" },
                    { obj: "openCouponsButton",   glyph: "🎫", text: "优惠券",     caption: "立减券·折扣券", route: "coupon" },
                    { obj: "openSettingsButton",  glyph: "⚙️", text: "设置",       caption: "",             route: "settings" }
                ]
                delegate: P.ClickableCard {
                    objectName: modelData.obj
                    width: col.width - col.padding * 2
                    onClicked: { if (App) App.navigate(modelData.route) }
                    Row {
                        width: parent.width
                        spacing: P.Style.spaceSm
                        Text { anchors.verticalCenter: parent.verticalCenter
                            text: modelData.glyph; font.pixelSize: P.Style.fontLg }
                        Text { anchors.verticalCenter: parent.verticalCenter
                            text: modelData.text; font.pixelSize: P.Style.fontMd; color: P.Style.ink }
                        Text { anchors.verticalCenter: parent.verticalCenter
                            text: modelData.caption; font.pixelSize: P.Style.fontSm; color: P.Style.faint }
                        Text { anchors.verticalCenter: parent.verticalCenter
                            text: "›"; font.pixelSize: P.Style.fontLg; color: P.Style.faint }
                    }
                }
            }

            // 退出登录
            P.ActionButton {
                objectName: "logoutButton"
                variant: "danger"
                text: "退出登录"
                width: col.width - col.padding * 2
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
                    // （C++ 侧置空登录态并 emit loginStateChanged，壳自动翻回登录页）。
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
