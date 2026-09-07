import QtQuick
import QtQuick.Controls.Basic
import "."

// QML twin of widgets ActionButton — same name, same variant vocabulary:
//   primary | secondary | danger | ghost | chip
Button {
    id: root
    property string variant: "primary"
    objectName: "actionButton_" + (text || "")

    implicitHeight: 44
    leftPadding: Style.spaceLg
    rightPadding: Style.spaceLg
    topPadding: Style.spaceSm
    bottomPadding: Style.spaceSm

    readonly property color fg: ({
        "primary": Style.surface, "danger": Style.surface,
        "secondary": Style.ink, "ghost": Style.brandDeep, "chip": Style.muted
    })[variant] || Style.surface
    readonly property color bg: ({
        "primary": Style.brand, "danger": Style.danger,
        "secondary": Style.ghost, "ghost": "transparent", "chip": Style.brandSoft
    })[variant] || Style.brand
    readonly property int rad: variant === "chip" ? Style.radiusPill : Style.radiusMd

    contentItem: Text {
        text: root.text
        font.pixelSize: Style.fontMd
        font.bold: root.variant === "primary" || root.variant === "danger"
        color: root.fg
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: root.rad
        color: root.bg
        border.width: root.variant === "secondary" ? 1 : 0
        border.color: Style.lineStrong
        opacity: root.down ? 0.85 : 1.0
        Behavior on opacity {
            NumberAnimation { duration: root.enabled && Style.motionEnabled ? Style.durMicro : 0 }
        }
    }
}
