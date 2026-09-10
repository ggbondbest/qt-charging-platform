import QtQuick
import QtQuick.Controls.Basic
import "."
import "Glyphs.js" as Glyphs

// QML twin of widgets ActionButton — same name, same variant vocabulary:
//   primary | secondary | danger | ghost | chip | logout
// Skin mirrors resources/qss/client_platform.qss #uiActionButton rules
// verbatim: pill radius 24 (chip=12), white secondary, QSS pressed/disabled
// solid colors, and the checked chip state (浅绿底+绿描边+深绿字) exposed as
// the `selected` property instead of the widgets-only :checked pseudo-class.
// logout variant = QSS #logoutButton: white card, 1px #E5E9EF, red 15px/600
// text, pressed #FDEBEB — deliberately not a solid-danger button.
Button {
    id: root
    property string variant: "primary"
    property bool selected: false      // chip 选中态（排序/tab/筛选 chips 用）
    property string glyph: ""          // image://glyphs 母版名（空=纯文本按钮）
    property color glyphColor: fg      // 图标染色（默认随字色，可传 starGold 等）
    objectName: "actionButton_" + (text || "")

    implicitHeight: Math.max(44, implicitContentHeight + topPadding + bottomPadding)
    leftPadding: Style.spaceLg
    rightPadding: Style.spaceLg
    topPadding: Style.spaceSm
    bottomPadding: Style.spaceSm

    // QSS uiStatusTag 同源字色映射
    readonly property color fg: !enabled ? ({
            "primary": "#F2FBF7", "danger": "#FDF3F3",
            "secondary": "#B7BFC9", "ghost": Style.faint, "chip": "#B7BFC9",
            "logout": "#FDF3F3"
        })[variant] || Style.faint : ({
        "primary": "#FFFFFF", "danger": "#FFFFFF",
        "secondary": Style.ink, "ghost": Style.ink,
        "chip": root.selected ? Style.brandDeep : Style.ink,
        "logout": Style.danger
    })[variant] || Style.surface
    readonly property color bg: !enabled ? ({
            "primary": Style.brandEdge, "danger": "#F3B9BC",
            "secondary": Style.surface, "ghost": "transparent", "chip": Style.surface,
            "logout": Style.surface
        })[variant] || Style.surface : root.down ? ({
            "primary": Style.brandPressed, "danger": Style.dangerPressed,
            "secondary": Style.ghost, "ghost": "transparent", "chip": Style.bg,
            "logout": Style.dangerSoft
        })[variant] || Style.surface : ({
        "primary": Style.brand, "danger": Style.danger,
        "secondary": Style.surface, "ghost": "transparent",
        "chip": root.selected ? Style.brandSoft : Style.surface,
        "logout": Style.surface
    })[variant] || Style.brand
    readonly property int rad: variant === "chip" ? Style.radiusMd
                               : variant === "logout" ? Style.radiusLg : Style.radiusChip
    readonly property bool bordered: variant === "secondary" || variant === "logout"
                                     || (variant === "chip" && !root.selected)
    readonly property int borderW: variant === "secondary" || variant === "logout"
                                   || variant === "chip" ? 1 : 0
    readonly property color borderColor: variant === "logout" ? Style.line
                                         : root.selected ? Style.brand : Style.lineStrong

    contentItem: Row {
        spacing: glyphIcon.visible && root.text.length > 0 ? 4 : 0
        Image {
            id: glyphIcon
            visible: root.glyph.length > 0
            anchors.verticalCenter: parent.verticalCenter
            width: Math.round((root.variant === "primary" || root.variant === "danger"
                               || root.variant === "chip" || root.variant === "logout"
                               ? Style.fontLg : Style.fontMd) * Style.fontScaleFactor)
            height: width
            source: visible ? Glyphs.source(root.glyph, root.glyphColor) : ""
        }
        Text {
            text: root.text
            visible: text.length > 0
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: root.variant === "primary" || root.variant === "danger"
                            || root.variant === "chip" || root.variant === "logout"
                            ? Style.fontLg : Style.fontMd
            font.bold: root.variant === "primary" || root.variant === "danger"
                       || root.variant === "logout"
                       || (root.variant === "chip" && root.selected)
            color: root.fg
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
    background: Rectangle {
        radius: root.rad
        color: root.bg
        border.width: root.borderW
        border.color: root.borderColor
    }
}
