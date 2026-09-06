import QtQuick
import QtQuick.Window
import "platform" as P

// Smoke host — replaced by Shell.qml in the 16:10 block. Exists to clear the
// single hardest unknown of the migration: does QML render + grab under the
// offscreen platform on this machine. If this screenshot is non-blank, all
// later page work is on proven ground.
Window {
    id: root
    width: 420
    height: 860
    visible: true
    color: P.Style.bg
    objectName: "qmlRoot"

    property string view: typeof chargingView === "string" ? chargingView : "base"

    Column {
        anchors.centerIn: parent
        spacing: P.Style.spaceMd
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 64
            height: 64
            radius: P.Style.radiusLg
            color: P.Style.brand
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "QML shell — " + root.view
            font.pixelSize: P.Style.fontXl
            color: P.Style.ink
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "style tokens OK · offscreen grab OK"
            font.pixelSize: P.Style.fontSm
            color: P.Style.muted
        }
    }
}
