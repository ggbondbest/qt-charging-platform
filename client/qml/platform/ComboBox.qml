import QtQuick
import QtQuick.Controls.Basic as Controls
import "."

// Both the field and the popup use the same design tokens as the page.
// textAt() preserves string models and role-based models without changing
// currentIndex, activated(), or editable/accepted() contracts.
Controls.ComboBox {
    id: control
    implicitWidth: 180
    implicitHeight: Math.max(44, contentItem.implicitHeight + topPadding + bottomPadding)
    leftPadding: Style.spaceMd
    rightPadding: 32
    topPadding: Style.spaceSm
    bottomPadding: Style.spaceSm
    font.pixelSize: Style.fontMd
    hoverEnabled: true

    delegate: Controls.ItemDelegate {
        id: option
        required property int index
        width: control.width - 8
        implicitHeight: Math.max(40, itemText.implicitHeight + 16)
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            id: itemText
            text: control.textAt(index)
            font: control.font
            color: option.highlighted ? Style.brandDeep : Style.ink
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: Style.radiusSm - 2
            color: option.highlighted ? Style.brandSoft : "transparent"
        }
    }

    contentItem: Controls.TextField {
        implicitHeight: contentHeight
        text: control.editable ? control.editText : control.displayText
        font: control.font
        color: control.enabled ? Style.ink : Style.faint
        selectionColor: Style.brandDeep
        selectedTextColor: "#FFFFFF"
        placeholderTextColor: Style.muted
        padding: 0
        enabled: control.editable
        readOnly: control.down
        selectByMouse: control.editable
        autoScroll: control.editable
        validator: control.validator
        inputMethodHints: control.inputMethodHints
        verticalAlignment: Text.AlignVCenter
        background: null
    }

    indicator: Canvas {
        x: control.width - width - 12
        y: (control.height - height) / 2
        width: 12
        height: 8
        property color ink: control.enabled ? Style.muted : Style.faint
        onInkChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = ink
            ctx.lineWidth = 1.8
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.beginPath()
            ctx.moveTo(1, 1.5)
            ctx.lineTo(6, 6.5)
            ctx.lineTo(11, 1.5)
            ctx.stroke()
        }
    }

    background: Rectangle {
        radius: Style.radiusSm
        color: control.enabled ? Style.surface : Style.ghost
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Style.brand
                      : control.hovered ? Style.lineStrong : Style.line
    }

    popup: Controls.Popup {
        y: control.height + 4
        width: control.width
        implicitHeight: Math.min(260, list.contentHeight + 8)
        padding: 4
        contentItem: ListView {
            id: list
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            Controls.ScrollIndicator.vertical: Controls.ScrollIndicator { }
        }
        background: Rectangle {
            color: Style.surface
            radius: Style.radiusSm
            border.color: Style.lineStrong
        }
    }
}
