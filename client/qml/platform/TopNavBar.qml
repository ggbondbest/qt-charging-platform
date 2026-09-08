import QtQuick
import QtQuick.Controls.Basic
import "."
import "." as P

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
    readonly property bool defaultAvatar: !hasUser || !user.avatarKey
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
            id: platformTitle
            anchors.verticalCenter: parent.verticalCenter
            text: nav.searchVisible ? "充电" : "⚡ 充电平台"
            font.pixelSize: Style.fontLg
            font.bold: true
            color: Style.ink
        }
        Item {
            anchors.verticalCenter: parent.verticalCenter
            width: nav.searchVisible
                 ? Math.max(96, nav.width - 2 * Style.spaceLg - platformTitle.implicitWidth
                            - 32 - filterText.implicitWidth - notificationText.implicitWidth
                            - 5 * Style.spaceSm) : 0
            height: 36
            visible: nav.searchVisible
            P.TextField {
                id: searchField
                anchors.fill: parent
                placeholderText: "搜索站点/地址…"
                font.pixelSize: Style.fontMd
                onAccepted: nav.searchSubmitted(text.trim())
            }
        }
        Item { // Draw the filter icon rather than relying on a rare font glyph.
            id: filterText
            anchors.verticalCenter: parent.verticalCenter
            visible: nav.filterVisible
            implicitWidth: 24
            width: implicitWidth
            height: 32
            Canvas {
                anchors.centerIn: parent
                width: 20
                height: 20
                property color ink: filterMouse.pressed ? Style.brandDeep : Style.muted
                onInkChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.strokeStyle = ink
                    ctx.lineWidth = 1.7
                    ctx.lineJoin = "round"
                    ctx.beginPath()
                    ctx.moveTo(2, 3)
                    ctx.lineTo(18, 3)
                    ctx.lineTo(12, 10)
                    ctx.lineTo(12, 17)
                    ctx.lineTo(8, 15)
                    ctx.lineTo(8, 10)
                    ctx.closePath()
                    ctx.stroke()
                }
            }
            MouseArea { id: filterMouse; anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: nav.filterRequested() }
        }
        Text { // 通知铃铛
            id: notificationText
            anchors.verticalCenter: parent.verticalCenter
            visible: nav.notificationsVisible
            text: "🔔"
            font.pixelSize: Style.fontLg
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: nav.notificationsRequested() }
        }
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
            color: nav.defaultAvatar ? "#D5D9DE" : Style.brandSoft
            Image {
                anchors.fill: parent
                visible: nav.hasUser
                         && String(nav.user.avatarKey || "").indexOf("data:image/png;base64,") === 0
                source: visible ? nav.user.avatarKey : ""
                fillMode: Image.PreserveAspectCrop
            }
            Text { anchors.centerIn: parent
                visible: !nav.hasUser || !nav.user.avatarKey
                         || String(nav.user.avatarKey).indexOf("data:image/png;base64,") !== 0
                text: nav.defaultAvatar ? "👤" : (nav.user.nickname ? nav.user.nickname[0] : "👤")
                font.pixelSize: Style.fontSm; color: nav.defaultAvatar ? "#626A73" : Style.brandDeep }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: nav.profileRequested() }
        }
    }
}
