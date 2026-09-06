import QtQuick
import "."

// QML twin of widgets ActionBar — variant: primary | danger.
Rectangle {
    id: root
    property string variant: "primary"
    property alias actionText: action.text
    property alias caption: cap.text
    signal clicked()
    objectName: "actionBar"

    implicitHeight: 76
    color: Style.surface
    radius: Style.radiusLg
    border.width: 1
    border.color: Style.line

    ActionButton {
        id: action
        anchors.right: parent.right
        anchors.rightMargin: Style.spaceLg
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: cap.right
        anchors.leftMargin: Style.spaceMd
        width: undefined
        variant: root.variant
        onClicked: root.clicked()
    }
    Text {
        id: cap
        anchors.left: parent.left
        anchors.leftMargin: Style.spaceLg
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width - action.implicitWidth - 4 * Style.spaceLg
        font.pixelSize: Style.fontSm
        color: Style.muted
        wrapMode: Text.WordWrap
    }
}
