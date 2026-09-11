import QtQuick
import QtQuick.Controls.Basic as Controls
import "."

// Application-owned colors prevent a system dark palette from producing a
// black input on a light page. Public TextField behavior remains unchanged.
Controls.TextField {
    id: control
    implicitWidth: 180
    implicitHeight: Math.max(44, contentHeight + topPadding + bottomPadding)
    leftPadding: Style.spaceMd
    rightPadding: Style.spaceMd
    topPadding: Style.spaceSm
    bottomPadding: Style.spaceSm
    font.pixelSize: Style.fontMd
    color: enabled ? Style.ink : Style.faint
    placeholderTextColor: Style.muted
    selectionColor: Style.brandDeep
    selectedTextColor: "#FFFFFF"
    selectByMouse: true
    hoverEnabled: true
    background: Rectangle {
        color: control.enabled ? Style.surface : Style.ghost
        radius: Style.radiusSm
        border.color: control.activeFocus ? Style.brand
                      : control.hovered ? Style.lineStrong : Style.line
        border.width: control.activeFocus ? 2 : 1
    }
}
