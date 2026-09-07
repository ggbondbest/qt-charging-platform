import QtQuick
import "."

// QML twin of widgets Card.
// Fluent "lit top, weighted bottom" edge (FluControlBackground's border
// gradient, approximated with a 1px chord strip so rounded corners keep
// their geometry).
Rectangle {
    id: root
    default property alias content: body.data
    property alias spacing: body.spacing
    radius: Style.radiusLg
    color: Style.surface
    border.width: 1
    border.color: Style.cardEdgeHi
    implicitHeight: body.implicitHeight + 2 * Style.spaceLg

    Column {
        id: body
        x: Style.spaceLg
        width: parent.width - 2 * Style.spaceLg
        y: Style.spaceLg
        spacing: Style.spaceMd
    }
    // 底缘重色和弦条（避开圆角两端，模拟 Fluent 渐变下边框）
    Rectangle {
        x: root.radius
        y: root.height - 2
        width: Math.max(0, root.width - 2 * root.radius)
        height: 1
        color: Style.cardEdgeLo
    }
}
