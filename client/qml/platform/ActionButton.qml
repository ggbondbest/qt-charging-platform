import QtQuick
import QtQuick.Controls.Basic
import "."

// QML twin of widgets ActionButton — same name, same variant vocabulary:
//   primary | secondary | danger | ghost | chip
// Skin mirrors resources/qss/client_platform.qss #uiActionButton rules,
// extended with Fluent-style interaction tiers (patterns borrowed from
// zhuzichu520/FluentUI's FluButton/FluControlBackground, brand-green kept):
// normal → hover (120ms fade) → pressed (solid deep), `selected` chip
// state, and a 2px focus ring that rides just outside the control.
Button {
    id: root
    property string variant: "primary"
    property bool selected: false      // chip 选中态（排序/tab/筛选 chips 用）
    objectName: "actionButton_" + (text || "")

    implicitHeight: 44
    leftPadding: Style.spaceLg
    rightPadding: Style.spaceLg
    topPadding: Style.spaceSm
    bottomPadding: Style.spaceSm

    // QSS uiStatusTag 同源字色映射
    readonly property color fg: !enabled ? ({
            "primary": "#F2FBF7", "danger": "#FDF3F3",
            "secondary": "#B7BFC9", "ghost": Style.faint, "chip": "#B7BFC9"
        })[variant] || Style.faint : ({
        "primary": Style.surface, "danger": Style.surface,
        "secondary": Style.ink, "ghost": Style.ink,
        "chip": root.selected ? Style.brandDeep : Style.ink
    })[variant] || Style.surface
    // 四态背景：disabled → pressed → hover → normal（Fluent 分层）
    readonly property color bg: !enabled ? ({
            "primary": Style.brandEdge, "danger": "#F3B9BC",
            "secondary": Style.surface, "ghost": "transparent", "chip": Style.surface
        })[variant] || Style.surface : root.down ? ({
            "primary": Style.brandPressed, "danger": Style.dangerPressed,
            "secondary": Style.ghostHover, "ghost": Style.ghostHover,
            "chip": root.selected ? Style.chipSelectedHover : Style.bg
        })[variant] || Style.surface : root.hovered ? ({
            "primary": Style.brandHover, "danger": Style.dangerHover,
            "secondary": Style.surfaceHover, "ghost": Style.ghostHover,
            "chip": root.selected ? Style.chipSelectedHover : Style.chipHover
        })[variant] || Style.surface : ({
        "primary": Style.brand, "danger": Style.danger,
        "secondary": Style.surface, "ghost": "transparent",
        "chip": root.selected ? Style.brandSoft : Style.surface
    })[variant] || Style.brand
    readonly property int rad: variant === "chip" ? Style.radiusMd : Style.radiusChip
    readonly property bool bordered: variant === "secondary" || (variant === "chip" && !root.selected)
    readonly property int borderW: variant === "secondary" || variant === "chip" ? 1 : 0
    readonly property color borderColor: root.selected ? Style.brand : Style.lineStrong

    contentItem: Text {
        text: root.text
        font.pixelSize: root.variant === "primary" || root.variant === "danger"
                        || root.variant === "chip" ? Style.fontLg : Style.fontMd
        font.bold: root.variant === "primary" || root.variant === "danger"
                   || (root.variant === "chip" && root.selected)
        color: root.fg
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Item {
        Rectangle {
            id: base
            anchors.fill: parent
            radius: root.rad
            color: root.bg
            border.width: root.borderW
            border.color: root.borderColor
            // Fluent fade: state colors never snap, they cross-fade
            Behavior on color {
                ColorAnimation {
                    duration: root.enabled && Style.motionEnabled ? Style.durHover : 0
                    easing.type: Easing.OutCubic
                }
            }
            Behavior on border.color {
                ColorAnimation {
                    duration: root.enabled && Style.motionEnabled ? Style.durHover : 0
                    easing.type: Easing.OutCubic
                }
            }
        }
        // Focus ring（FluentUI FluFocusRectangle 手法：2px 描边置顶，
        // activeFocus 才现身；用品牌绿替代其黑白描边）
        Rectangle {
            anchors.centerIn: base
            width: base.width + 4
            height: base.height + 4
            radius: base.radius + 2
            color: "transparent"
            border.width: 2
            border.color: Style.focusRing
            visible: root.activeFocus
        }
    }
}
