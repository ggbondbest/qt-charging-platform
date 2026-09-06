import QtQuick
import QtQuick.Controls.Basic

// QML twin of widgets LoadingOverlay — dim + spinner over its parent.
// C++ showFor()/hideFor() kept verbatim; `running` is the declarative form.
Item {
    id: overlay
    property bool running: false
    anchors.fill: parent
    visible: opacity > 0.01
    opacity: running ? 1.0 : 0.0
    Behavior on opacity {
        NumberAnimation { duration: Style.motionEnabled ? Style.durMicro : 0 }
    }

    function showFor() { running = true }
    function hideFor() { running = false }

    Rectangle {
        anchors.fill: parent
        color: "#661F2937"
        radius: overlay.radius || 0
    }
    BusyIndicator {
        anchors.centerIn: parent
        running: overlay.running
        width: 44
        height: 44
    }
}
