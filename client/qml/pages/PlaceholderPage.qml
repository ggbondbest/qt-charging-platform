import QtQuick
import "../platform" as P

// Placeholder for routes not migrated yet this sprint (member 2's pages land
// under pages/station/, mine under pages/profile_charging/). Delete at cutover.
Item {
    id: page
    property string route: ""
    objectName: "placeholder_" + route
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    // grabToImage has no window backdrop — every page must paint its own bg.
    Rectangle {
        anchors.fill: parent
        color: P.Style.bg
    }

    Column {
        anchors.centerIn: parent
        spacing: P.Style.spaceSm
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "🚧"
            font.pixelSize: 36
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "route: " + page.route
            font.pixelSize: P.Style.fontLg
            color: P.Style.ink
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: page.route ? "（页面迁移中）" : "（未知路由）"
            font.pixelSize: P.Style.fontSm
            color: P.Style.muted
        }
    }
}
