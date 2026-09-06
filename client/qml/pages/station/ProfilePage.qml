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

            // 身份头卡（点击=编辑资料）
            P.ClickableCard {
                objectName: "profileHeroButton"
                width: col.width - col.padding * 2
                onClicked: { if (App) App.navigate("profile_edit") }
                Row {
                    anchors.fill: parent
                    anchors.margins: P.Style.spaceMd
                    spacing: P.Style.spaceMd
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 46; height: 46; radius: 23
                        color: P.Style.brandSoft
                        Text {
                            anchors.centerIn: parent
                            text: "👤"; font.pixelSize: 22
                        }
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 46 - 80 - parent.spacing * 2
                        spacing: 2
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            text: page.user ? (page.user.nickname || "未设置") : "未登录"
                            font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink
                        }
                        Text {
                            text: page.user ? (page.user.phone || "") : "点这里登录"
                            font.pixelSize: P.Style.fontSm; color: P.Style.muted
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "编辑资料 ›"; font.pixelSize: P.Style.fontSm; color: P.Style.faint
                    }
                }
            }

            // 余额卡
            P.ClickableCard {
                objectName: "balanceButton"
                width: col.width - col.padding * 2
                onClicked: { if (App) App.navigate("wallet") }
                Column {
                    anchors.fill: parent
                    anchors.margins: P.Style.spaceMd
                    spacing: P.Style.spaceXs
                    Text { text: "余额（元）"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                    Text {
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
                    { obj: "openSettingsButton",  glyph: "⚙️", text: "设置",       caption: "",             route: "settings" }
                ]
                delegate: P.ClickableCard {
                    objectName: modelData.obj
                    width: col.width - col.padding * 2
                    height: 56
                    onClicked: { if (App) App.navigate(modelData.route) }
                    Row {
                        anchors.fill: parent
                        anchors.margins: P.Style.spaceMd
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
                    try { authService.logout() } catch (e) {
                        if (App) App.showToast("退出登录桥未就绪", "warning")
                    }
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
