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
    readonly property bool hasUser: user !== null && user.id !== undefined && String(user.id) !== "0"
    // filter/notifications entries ride with the search group (C++ semantics).
    readonly property bool filterVisible: searchVisible
    readonly property bool notificationsVisible: searchVisible
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
        Text { // 高级筛选漏斗
            anchors.verticalCenter: parent.verticalCenter
            visible: nav.filterVisible
            text: "⛛"
            font.pixelSize: Style.fontXl
            color: Style.muted
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: nav.filterRequested() }
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
            Image {
                anchors.fill: parent
                visible: nav.hasUser && nav.user.avatarKey
                         && String(nav.user.avatarKey).indexOf("data:image/png;base64,") === 0
                source: visible ? nav.user.avatarKey : ""
                fillMode: Image.PreserveAspectCrop
            }
            Text { anchors.centerIn: parent
                visible: !nav.hasUser || !nav.user.avatarKey
                         || String(nav.user.avatarKey).indexOf("data:image/png;base64,") !== 0
                text: nav.hasUser && nav.user && nav.user.nickname ? nav.user.nickname[0] : "?"
                font.pixelSize: Style.fontSm; color: Style.brandDeep }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: nav.profileRequested() }
        }
    }
}
