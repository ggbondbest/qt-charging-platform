import QtQuick
import "."

// QML twin of widgets StatusTag — tone: neutral | success | warning | danger | info
Rectangle {
    id: root
    property string tone: "neutral"
    property alias text: label.text
    implicitWidth: label.implicitWidth + 2 * Style.spaceMd
    implicitHeight: 24
    radius: Style.radiusPill
    color: ({
        "neutral": Style.ghost, "success": Style.brandSoft, "warning": Style.warningSoft,
        "danger": Style.dangerSoft, "info": Style.infoSoft
    })[tone] || Style.ghost

    Text {
        id: label
        anchors.centerIn: parent
        font.pixelSize: Style.fontSm
        font.bold: true
        color: ({
            "neutral": Style.muted, "success": Style.brandDeep, "warning": Style.warning,
            "danger": Style.danger, "info": Style.info
        })[root.tone] || Style.muted
    }
}
