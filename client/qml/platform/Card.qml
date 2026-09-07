import QtQuick
import "."

// QML twin of widgets Card.
Rectangle {
    id: root
    default property alias content: body.data
    property alias spacing: body.spacing
    radius: Style.radiusLg
    color: Style.surface
    border.width: 1
    border.color: Style.line
    implicitHeight: body.implicitHeight + 2 * Style.spaceLg

    Column {
        id: body
        x: Style.spaceLg
        width: parent.width - 2 * Style.spaceLg
        y: Style.spaceLg
        spacing: Style.spaceMd
    }
}
