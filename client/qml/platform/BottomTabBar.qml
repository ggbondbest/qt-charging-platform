import QtQuick
import "."

// QML twin of widgets BottomTabBar.
//   property var tabs: [{id:"station", text:"🔍 找站"}, …]
//   currentTab / setCurrentTab(id) / tabChanged(id)
Rectangle {
    id: bar
    property var tabs: []
    property string currentTab: ""
    signal tabChanged(string id)
    objectName: "bottomTabBar"

    function setCurrentTab(id) {
        if (id === currentTab) return
        for (var i = 0; i < tabs.length; ++i)
            if (tabs[i].id === id) { currentTab = id; tabChanged(id); return }
    }

    implicitHeight: 64
    color: Style.surface
    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Style.line }

    Row {
        anchors.fill: parent
        Repeater {
            model: bar.tabs
            delegate: Item {
                required property var modelData
                width: bar.width / bar.tabs.length
                height: bar.height
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bar.setCurrentTab(modelData.id)
                }
                Column {
                    anchors.centerIn: parent
                    spacing: 2
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: modelData.text
                        font.pixelSize: Style.fontSm
                        font.bold: bar.currentTab === modelData.id
                        color: bar.currentTab === modelData.id ? Style.brandDeep : Style.faint
                        Behavior on color {
                            ColorAnimation { duration: Style.motionEnabled ? Style.durExit : 0 }
                        }
                    }
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 18; height: 3; radius: 2
                        visible: bar.currentTab === modelData.id
                        color: Style.brand
                    }
                }
            }
        }
    }
}
