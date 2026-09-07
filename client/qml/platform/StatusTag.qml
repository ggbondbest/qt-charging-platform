import QtQuick
import "."

// QML twin of widgets StatusTag — tone: neutral | success | warning | danger | info
// Skin mirrors #uiStatusTag in client_platform.qss: 9px radius pilllet,
// 11px/600 text, 2×10 padding (soft bg + deep text per tone).
Rectangle {
    id: root
    property string tone: "neutral"
    property alias text: label.text
    implicitWidth: label.implicitWidth + 2 * Style.spaceMd
    implicitHeight: 22
    radius: Style.radiusTag
    color: ({
        "neutral": Style.ghost, "success": Style.brandSoft, "warning": Style.warningSoft,
        "danger": Style.dangerSoft, "info": Style.infoSoft
    })[tone] || Style.ghost

    Text {
        id: label
        anchors.centerIn: parent
        font.pixelSize: Style.fontXs
        font.weight: Font.DemiBold
        color: ({
            "neutral": Style.muted, "success": Style.brandDeep, "warning": Style.warning,
            "danger": Style.danger, "info": Style.info
        })[root.tone] || Style.muted
    }
}
