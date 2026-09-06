import QtQuick
import "."

// QML twin of widgets PullToRefreshArea — same state machine, same constants:
//   Collapsed → Pulling → Armed → Refreshing
//   Style.pullActivatePx (8) / pullThresholdPx (56) / pullRestGapPx (44)
//   rubberGap(raw) = 90·(1−e^(−raw/90)); pill ⌄下拉刷新 / ⌃松开刷新 / ⟳正在刷新…
// Contract API: default content item · pullEnabled · refreshing ·
//   refreshRequested() · setRefreshing(active)
Item {
    id: pull
    default property alias content: inner.data
    property bool pullEnabled: true
    property bool refreshing: false
    signal refreshRequested()
    objectName: "pullToRefreshArea"

    property int gap: 0                       // displayed rubber-banded gap
    readonly property bool armed: gap >= Style.pullThresholdPx
    readonly property bool pulling: gap >= Style.pullActivatePx

    function setRefreshing(active) {
        refreshing = active
        if (!active)
            gap = 0
    }
    function rubberGap(raw) {
        return raw <= 0 ? 0 : 90 * (1 - Math.exp(-raw / 90))
    }
    Behavior on gap {
        NumberAnimation { duration: Style.motionEnabled ? Style.durExit : 0 }
    }

    // Invisible drag proxy — the strip's MouseArea drags this; its y is raw pull.
    Item { id: dragProxy; y: 0; width: 1; height: 1 }

    // Pull gesture strip at the very top (see NOTE below for full-surface TODO).
    MouseArea {
        id: pullMouse
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 24
        enabled: pull.pullEnabled && !pull.refreshing
        drag.target: dragProxy
        drag.axis: Drag.YAxis
        drag.minimumY: 0
        drag.maximumY: 110
        propagateComposedEvents: true
        onPressed: (mouse) => {
            // Only claim the press when the list is at the very top.
            if (flick.contentY > 0.5)
                mouse.accepted = false
            else
                mouse.accepted = true
        }
        onPositionChanged: {
            if (dragProxy.y >= Style.pullActivatePx)
                pull.gap = pull.rubberGap(dragProxy.y)
        }
        onReleased: {
            var trigger = pull.armed
            dragProxy.y = 0
            if (trigger) {
                pull.refreshing = true
                pull.gap = Style.pullRestGapPx
                pull.refreshRequested()
            } else {
                pull.gap = 0
            }
        }
    }
    // NOTE(sprint): real-device full-surface pull (C++ tracked viewport-wide
    // drags and gated buttons via a qApp filter) → tomorrow: DragHandler on
    // the flickable with translate+axis filters. Strip keeps 6.2 dead simple.

    // Pill floats in the gap band, above content.
    Rectangle {
        id: pill
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(0, pull.gap - 36)
        width: pillText.implicitWidth + 2 * Style.spaceLg
        height: pull.pulling ? 28 : 0
        radius: Style.radiusPill
        color: Style.ghost
        visible: height > 0
        clip: true
        Behavior on height {
            NumberAnimation { duration: Style.motionEnabled ? Style.durMicro : 0 }
        }
        Text {
            id: pillText
            anchors.centerIn: parent
            font.pixelSize: Style.fontSm
            color: Style.muted
            text: pull.refreshing ? "⟳ 正在刷新…"
                : (pull.armed ? "⌃ 松开刷新" : "⌄ 下拉刷新")
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(inner.implicitHeight + pull.gap, height)
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Column {
            id: inner
            width: flick.width
            y: pull.gap
            spacing: pull.spacingHint
        }
    }
    property int spacingHint: 0   // pages may set row spacing via this alias-ish
}
