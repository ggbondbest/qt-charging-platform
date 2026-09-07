import QtQuick
import "."

// QML twin of widgets ClickableCard — Card + clicked().
// NOTE: cannot derive from Card here — Card routes instance children into an
// internal Column, and a fill-anchored MouseArea must not live inside a Column
// (anchors there are ignored and QML warn). Kept visually identical instead.
// Fluent interaction parity: Card's lit-top/weighted-bottom edge + hover
// border darken (patterns borrowed from zhuzichu520/FluentUI).
Rectangle {
    id: card
    objectName: "clickableCard"
    property bool hovered: false
    radius: Style.radiusLg
    color: Style.surface
    border.width: 1
    border.color: card.hovered ? Style.lineStrong : Style.cardEdgeHi
    scale: 1.0
    signal clicked()

    default property alias content: body.data
    implicitHeight: body.implicitHeight + 2 * Style.spaceLg

    Column {
        id: body
        x: Style.spaceLg
        y: Style.spaceLg
        width: parent.width - 2 * Style.spaceLg
        spacing: Style.spaceMd
    }
    // 底缘重色和弦条（同 Card，Fluent 渐变下边框近似，避开圆角端点）
    Rectangle {
        x: card.radius
        y: card.height - 2
        width: Math.max(0, card.width - 2 * card.radius)
        height: 1
        color: Style.cardEdgeLo
    }

    MouseArea {
        anchors.fill: parent           // sibling of the Column, direct child of Rectangle
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onEntered: card.hovered = true
        onExited: card.hovered = false
        onPressed: card.scale = 0.985
        onReleased: card.scale = 1.0
        onClicked: card.clicked()
    }

    Behavior on scale {
        NumberAnimation { duration: card.enabled && Style.motionEnabled ? Style.durMicro : 0 }
    }
    Behavior on border.color {
        ColorAnimation { duration: card.enabled && Style.motionEnabled ? Style.durHover : 0 }
    }
}
