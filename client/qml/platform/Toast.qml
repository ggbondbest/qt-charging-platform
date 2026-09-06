import QtQuick
import QtQuick.Controls      // for the Overlay attached type
import QtQuick.Controls.Basic
import "."

// QML twin of widgets Toast — transient pill, tone vocabulary of StatusTag.
// Usage: <Toast id: t> … t.show("已充值 ¥30", "success")
Popup {
    id: toast
    property string tone: "neutral"
    parent: Overlay.overlay
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? parent.height - height - 96 : 0
    padding: 0
    implicitWidth: label.implicitWidth + 2 * Style.spaceLg
    implicitHeight: 40
    closePolicy: Popup.NoAutoClose

    function show(message, t) {
        label.text = message
        tone = t === undefined ? "neutral" : t
        open()
        hideTimer.restart()
    }
    Timer {
        id: hideTimer
        interval: 2200
        onTriggered: toast.close()
    }

    background: Rectangle {
        radius: Style.radiusPill
        color: ({
            "neutral": Style.ink, "success": Style.brandDeep, "warning": Style.warning,
            "danger": Style.danger, "info": Style.info
        })[toast.tone] || Style.ink
    }
    contentItem: Text {
        id: label
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font.pixelSize: Style.fontMd
        color: Style.surface
    }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1.0
            duration: Style.motionEnabled ? Style.durEnter : 0 }
        NumberAnimation { property: "y"; from: toast.y + 16; to: toast.y
            duration: Style.motionEnabled ? Style.durEnter : 0 }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1.0; to: 0
            duration: Style.motionEnabled ? Style.durExit : 0 }
    }
}
