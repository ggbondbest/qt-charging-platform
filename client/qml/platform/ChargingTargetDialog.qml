import QtQuick
import QtQuick.Controls.Basic as Controls
import "."

Controls.Dialog {
    id: dialog
    objectName: "chargingTargetDialog"
    property string reservationId: ""
    property int selectedType: 0
    signal confirmed(string reservationId, string targetType, double targetValue)
    modal: true
    padding: Style.spaceLg
    width: Math.min(parent ? parent.width - 24 : 360, 380)
    height: Math.min(implicitHeight, Math.max(1, (parent ? parent.height : 640) - 24))
    anchors.centerIn: parent
    background: Rectangle { color: Style.surface; radius: Style.radiusLg; border.color: Style.line }
    header: Text {
        text: "设置充电目标"; padding: Style.spaceLg; color: Style.ink
        font.pixelSize: Style.fontLg; font.bold: true
    }
    onOpened: { dialog.selectedType = 0; amount.text = "20" }
    readonly property double rawValue: Number(amount.text)
    readonly property double normalized: Math.round(rawValue * (selectedType === 0 ? 100 : selectedType === 1 ? 1000 : 60))
    readonly property bool validValue: amount.acceptableInput && isFinite(rawValue) && rawValue > 0
                                      && normalized > 0 && normalized <= 9007199254740991
    contentItem: Controls.ScrollView {
        id: scroll
        implicitHeight: content.implicitHeight
        contentWidth: availableWidth
        clip: true
        Column {
        id: content
        width: scroll.availableWidth
        spacing: Style.spaceMd
        Text {
            width: parent.width; wrapMode: Text.Wrap
            text: "达到目标后由服务器自动结束，并进入待支付。你也可以随时提前停止。"
            color: Style.muted; font.pixelSize: Style.fontSm
        }
        ComboBox {
            objectName: "chargingTargetType"
            width: parent.width
            model: ["按金额（元）", "按电量（度 / kWh）", "按时长（分钟）"]
            currentIndex: dialog.selectedType
            onActivated: {
                dialog.selectedType = currentIndex
                amount.text = currentIndex === 0 ? "20" : currentIndex === 1 ? "10" : "30"
            }
        }
        TextField {
            id: amount
            objectName: "chargingTargetValue"
            width: parent.width
            placeholderText: dialog.selectedType === 0 ? "最多充多少元" : dialog.selectedType === 1 ? "补充多少度电" : "充多少分钟"
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            validator: RegularExpressionValidator {
                regularExpression: dialog.selectedType === 2 ? /^[0-9]{1,7}$/ : /^[0-9]{1,7}(\.[0-9]{1,2})?$/
            }
        }
        Text {
            width: parent.width; wrapMode: Text.Wrap
            text: dialog.selectedType === 0 ? "金额为上限。计费按整 Wh 四舍五入；必要时提前截断，保证不超过预算。免费电站请选择电量或时长。"
                                           : "目标进度以服务器计量为准，关闭客户端也会按目标自动停止。"
            color: Style.muted; font.pixelSize: Style.fontXs
        }
        ActionButton {
            objectName: "confirmChargingTargetButton"
            width: parent.width; text: "确认目标并开始充电"; enabled: dialog.validValue
            onClicked: {
                dialog.confirmed(dialog.reservationId, ["AMOUNT", "ENERGY", "DURATION"][dialog.selectedType], dialog.normalized)
                dialog.close()
            }
        }
        ActionButton { width: parent.width; text: "暂不开始"; variant: "secondary"; onClicked: dialog.close() }
        }
    }
}
