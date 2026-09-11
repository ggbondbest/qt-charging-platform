import QtQuick
import "."
import "Glyphs.js" as Glyphs

// QML twin of widgets NoticePanel — inline empty/error placeholder with
// optional retry action; signal actionTriggered().
// glyph = image://glyphs 母版名（assets/glyphs/，GlyphProvider 随主题染色），
// glyphColor 供错误态传 warning/danger 等语义色。
Card {
    id: panel
    property string glyph: "info-circle"
    property color glyphColor: Style.ink
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
        Image {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.round(36 * Style.fontScaleFactor)
            height: width
            source: Glyphs.source(panel.glyph, panel.glyphColor)
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
