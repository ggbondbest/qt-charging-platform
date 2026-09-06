import QtQuick

// QML twin of widgets ClickableCard — Card + clicked().
Card {
    id: card
    objectName: "clickableCard"
    signal clicked()

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: card.scale = 0.985
        onReleased: card.scale = 1.0
        onClicked: card.clicked()
    }
    scale: 1.0
    Behavior on scale {
        NumberAnimation { duration: card.enabled && Style.motionEnabled ? Style.durMicro : 0 }
    }
}
