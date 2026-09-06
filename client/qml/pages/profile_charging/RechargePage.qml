import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets RechargePage (route: "recharge", from 钱包页-充值).
Item {
    id: page
    objectName: "rechargePage"
    property string route: "recharge"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property int amountCents: {
        const v = parseInt(amountField.text)
        return isNaN(v) || v <= 0 ? 0 : v * 100
    }

    Connections {
        target: walletService
        function onRechargeCompleted(amountCents, balanceAfterCents) {
            if (!App) return
            App.showToast("充值成功 ¥" + (amountCents / 100).toFixed(2)
                          + "，余额 ¥" + (balanceAfterCents / 100).toFixed(2), "success")
            App.back()
        }
        function onOperationFailed(type, code, message) {
            if (App) App.showToast("充值失败：" + message, "danger")
        }
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacing: P.Style.spaceLg

        Text { text: "账户充值"; font.pixelSize: P.Style.fontXl; color: P.Style.ink }

        P.Card {
            width: parent.width
            Column {
                width: parent.width
                spacing: P.Style.spaceMd
                Text { text: "充值金额（元）"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                TextField {
                    id: amountField
                    objectName: "rechargeAmountField"
                    width: parent.width
                    placeholderText: "上限 ¥100000"
                    font.pixelSize: P.Style.fontXl
                    color: P.Style.ink
                    validator: IntValidator { bottom: 1; top: 100000 }
                    background: Rectangle {
                        radius: P.Style.radiusMd
                        color: P.Style.ghost
                        border.color: amountField.activeFocus ? P.Style.brand : P.Style.line
                        border.width: 1
                    }
                }
                // Quick chips — widgets parity.
                Row {
                    spacing: P.Style.spaceSm
                    Repeater {
                        model: [20, 50, 100, 200]
                        P.ActionButton {
                            variant: "chip"
                            text: "¥ " + modelData
                            onClicked: amountField.text = String(modelData)
                        }
                    }
                }
            }
        }

        P.ActionButton {
            objectName: "rechargeConfirmButton"
            width: parent.width
            variant: "primary"
            text: page.amountCents > 0
                  ? "确认充值 ¥" + (page.amountCents / 100).toFixed(2) : "请输入金额"
            enabled: page.amountCents > 0
            onClicked: walletService.recharge(page.amountCents)
        }

        Text {
            width: parent.width
            text: "充值为演示通道，走 mock 契约 v1 RECHARGE；真实支付协议待定 TODO(contract)"
            font.pixelSize: P.Style.fontSm
            color: P.Style.faint
        }
    }
}
