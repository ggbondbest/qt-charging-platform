import QtQuick
import QtQuick.Controls.Basic
import "."

// QML twin of widgets TopNavBar. Signals kept verbatim:
//   searchSubmitted(keyword) loginRequested profileRequested
//   backRequested filterRequested notificationsRequested
Rectangle {
    id: nav
    property var user: null            // {nickname, avatarKey, …} from App.currentUser
    property bool backVisible: false
    property bool searchVisible: true
    property alias searchText: searchField.text
    readonly property bool hasUser: user !== null
    // filter/notifications entries ride with the search group (C++ semantics).
    readonly property bool filterVisible: searchVisible
    readonly property bool notificationsVisible: searchVisible
    // 2026-09-08：全局唯一高级筛选入口在本栏——漏斗右侧红点计数由宿主页绑定
    //（StationHomePage/FavoritesPage 的 activeFilterBadge，缺位=0）。
    property int filterBadgeCount: 0
    objectName: "topNavBar"

    // Signal set verbatim from top_nav_bar.h:
    signal searchSubmitted(string keyword)
    signal loginRequested()
    signal profileRequested()
    signal backRequested()
    signal filterRequested()
    signal notificationsRequested()

    function clearUser() { user = null }
    function clearSearch() { searchField.text = "" }

    implicitHeight: 56
    color: Style.surface
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Style.line }

    Row {
        anchors.fill: parent
        anchors.leftMargin: Style.spaceLg
        anchors.rightMargin: Style.spaceLg
        spacing: Style.spaceSm

        Item {
            // 容器尺寸对齐文案（原 1×1 容器 + verticalCenter → 文字半数溢出导航条、
            // 被窗口顶缘裁切；用户实测指定修复，仅改布局不改样式）。
            width: backText.implicitWidth
            height: nav.implicitHeight
            visible: nav.backVisible
            Text {
                id: backText
                anchors.verticalCenter: parent.verticalCenter
                visible: nav.backVisible
                text: "‹ 返回"
                font.pixelSize: Style.fontLg
                color: Style.brandDeep
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                    onClicked: nav.backRequested() }
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "⚡ 充电平台"
            font.pixelSize: Style.fontLg
            font.bold: true
            color: Style.ink
        }
        Item {
            anchors.verticalCenter: parent.verticalCenter
            width: nav.searchVisible
                 ? nav.width - 2 * Style.spaceLg - 320 : 0
            height: 36
            visible: nav.searchVisible
            TextField {
                id: searchField
                anchors.fill: parent
                placeholderText: "搜索站点/地址…"
                font.pixelSize: Style.fontMd
                onAccepted: nav.searchSubmitted(text.trim())
            }
        }
        Item { // 高级筛选漏斗（原 "⛛" U+26DB 字体缺字渲染成豆腐块 → Canvas 标准漏斗形）
            anchors.verticalCenter: parent.verticalCenter
            visible: nav.filterVisible
            width: 26; height: 30
            objectName: "topFilterButton"
            Canvas {
                id: funnelIcon
                anchors.centerIn: parent
                width: 22; height: 20
                property color ink: funnelMa.pressed ? Style.brand : Style.muted
                onInkChanged: requestPaint()
                onPaint: {
                    const c = getContext("2d")
                    c.reset()
                    c.fillStyle = ink
                    // 标准 filter 形：上宽杯体收腰 + 短柄
                    c.beginPath()
                    c.moveTo(1.5, 2.5); c.lineTo(20.5, 2.5)
                    c.lineTo(12.8, 11.2); c.lineTo(12.8, 17.8)
                    c.lineTo(9.2, 19.2); c.lineTo(9.2, 11.2)
                    c.closePath(); c.fill()
                }
            }
            Rectangle { // 红色计数徽标（沿用原页面右上 ⛏ 的 danger 计数视觉）
                visible: nav.filterBadgeCount > 0
                anchors.left: funnelIcon.right; anchors.leftMargin: -6
                anchors.top: parent.top; anchors.topMargin: 1
                width: Math.max(15, badgeText.implicitWidth + 8)
                height: 15; radius: 7.5
                color: Style.danger
                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: nav.filterBadgeCount > 9 ? "9+" : String(nav.filterBadgeCount)
                    font.pixelSize: 9; font.bold: true; color: "#FFFFFF"
                }
            }
            MouseArea {
                id: funnelMa
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: nav.filterRequested()
            }
        }
        Text { // 通知铃铛
            anchors.verticalCenter: parent.verticalCenter
            visible: nav.notificationsVisible
            text: "🔔"
            font.pixelSize: Style.fontLg
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: nav.notificationsRequested() }
        }
        Item { width: nav.hasUser ? 1 : 1; height: 1 }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: !nav.hasUser
            text: "登录"
            font.pixelSize: Style.fontMd
            font.bold: true
            color: Style.brandDeep
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: nav.loginRequested() }
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            visible: nav.hasUser
            width: 32; height: 32; radius: 16
            color: Style.brandSoft
            Text { anchors.centerIn: parent
                text: nav.hasUser && nav.user && nav.user.nickname ? nav.user.nickname[0] : "?"
                font.pixelSize: Style.fontSm; color: Style.brandDeep }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: nav.profileRequested() }
        }
    }
}
