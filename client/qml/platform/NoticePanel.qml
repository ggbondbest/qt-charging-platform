import QtQuick
import "."

// QML twin of widgets NoticePanel — inline empty/error placeholder with
// optional retry action; signal actionTriggered().
Card {
    id: panel
    property alias glyph: glyphText.text
    property alias title: titleText.text
    property alias description: descText.text
    property alias actionText: actionBtn.text
    signal actionTriggered()
    objectName: "noticePanel"

    function setContent(g, t, d, a) {   // C++ parity helper
        panel.glyph = g; panel.title = t; panel.description = d
        panel.actionText = a === undefined ? "" : a
    }

    Column {
        // NOTE: this Column is a Card content child (lives inside Card's body
        // Column) — vertical/center anchors are illegal there; center via width.
        width: parent ? parent.width : 0
        spacing: Style.spaceSm
        Text {
            id: glyphText
            anchors.horizontalCenter: parent.horizontalCenter
            font.pixelSize: 36
            text: "ℹ️"
        }
        Text {
            id: titleText
            anchors.horizontalCenter: parent.horizontalCenter
            font.pixelSize: Style.fontLg
            font.bold: true
            color: Style.ink
        }
        Text {
            id: descText
            anchors.horizontalCenter: parent.horizontalCenter
            width: panel.width - 2 * Style.spaceXl
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Style.fontSm
            color: Style.muted
            wrapMode: Text.WordWrap
        }
        ActionButton {
            id: actionBtn
            anchors.horizontalCenter: parent.horizontalCenter
            variant: "ghost"
            visible: text.length > 0
            onClicked: panel.actionTriggered()
        }
    }
}
