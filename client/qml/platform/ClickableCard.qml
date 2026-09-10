import QtQuick
import "."

// QML twin of widgets ClickableCard — Card + clicked().
// NOTE: cannot derive from Card here — Card routes instance children into an
// internal Column, and a fill-anchored MouseArea must not live inside a Column
// (anchors there are ignored and QML warns). Kept visually identical instead.
Rectangle {
    id: card
    objectName: "clickableCard"
    radius: Style.radiusLg
    color: Style.surface
    border.width: 1
    border.color: Style.line
    scale: 1.0
    signal clicked()

    default property alias content: body.data
    implicitHeight: body.implicitHeight + 2 * Style.spaceLg

    // Keep the card hit area below its interactive children: navigation,
    // favorite and reservation actions must not turn into a card click.
    MouseArea {
        anchors.fill: parent           // sibling of the Column, direct child of Rectangle
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: card.scale = 0.985
        onReleased: card.scale = 1.0
        onCanceled: card.scale = 1.0
        onClicked: card.clicked()
    }

    // 收藏点击修复批（2026-09-08）：内容列声明在 MouseArea 之后——QML 同层后来者
    // 在上，收藏星/预约钮等子交互件才能盖住卡面命中区、先于 clicked() 吃到点击。
    Column {
        id: body
        x: Style.spaceLg
        y: Style.spaceLg
        width: parent.width - 2 * Style.spaceLg
        spacing: Style.spaceMd
    }

    Behavior on scale {
        NumberAnimation { duration: card.enabled && Style.motionEnabled ? Style.durMicro : 0 }
    }
}
