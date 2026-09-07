import QtQuick
import "."

// QML twin of widgets ClickableCard — Card + clicked().
// NOTE: cannot derive from Card here — Card routes instance children into an
// internal Column, and a fill-anchored MouseArea must not live inside a Column
// (anchors there are ignored and QML warns). Kept visually identical instead.
Rectangle {
    id: card
    objectName: "clickableCard"
    radius: Style.radiusLg
    color: Style.surface
    border.width: 1
    border.color: Style.line
    scale: 1.0
    signal clicked()

    default property alias content: body.data
    implicitHeight: body.implicitHeight + 2 * Style.spaceLg

    // 盖层点击区必须先于内容声明（后声明=更高堆叠）：原序下整卡 MouseArea 压在
    // 内容子树之上，卡内任何交互件（收藏 ☆、行内按钮）收不到点击、被卡面吞走
    // （用户实测 "收藏按钮点不了" 根因，2026-09-07）。文本等无 handler 区域经
    // 命中栈下沉仍由盖层接住，卡面导航行为不变；widgets 对应物（QCard 子控件
    // 天然优先收事件）同此语义。
    MouseArea {
        anchors.fill: parent           // sibling of the Column, direct child of Rectangle
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: card.scale = 0.985
        onReleased: card.scale = 1.0
        onClicked: card.clicked()
    }

    Column {
        id: body
        x: Style.spaceLg
        y: Style.spaceLg
        width: parent.width - 2 * Style.spaceLg
        spacing: Style.spaceMd
    }

    Behavior on scale {
        NumberAnimation { duration: card.enabled && Style.motionEnabled ? Style.durMicro : 0 }
    }
}
