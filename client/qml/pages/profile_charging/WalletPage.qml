import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets WalletPage (routes: "wallet", from 我的-钱包卡).
Item {
    id: page
    objectName: "walletPage"
    property string route: "wallet"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }   // grabToImage needs self-bg

    function money(cents) { return (cents / 100).toFixed(2) }
    function refresh() {
        walletService.fetchProfile()
        walletService.fetchRechargeRecords(1)
    }

    ListModel { id: recordsModel }

    Connections {
        target: walletService
        function onProfileLoaded(user) {
            // balance flows into the card through App.currentUser (bridge syncs it)
            recordsScroll.setRefreshing(false)
        }
        function onRechargeRecordsLoaded(records, hasMore) {
            recordsModel.clear()
            for (var i = 0; i < records.length; ++i)
                recordsModel.append(records[i])
            recordsScroll.setRefreshing(false)
        }
        function onOperationFailed(type, code, message) {
            recordsScroll.setRefreshing(false)
            if (App) App.showToast("加载失败：" + message, "danger")
        }
    }
    Component.onCompleted: refresh()

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacing: P.Style.spaceLg

        Text { text: "我的钱包"; font.pixelSize: P.Style.fontXl; color: P.Style.ink }

        // Balance card — widgets gradient header, QML twin with brand gradient.
        Rectangle {
            objectName: "uiWalletCard"
            width: parent.width
            height: 120
            radius: P.Style.radiusLg
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: P.Style.brand }
                GradientStop { position: 1.0; color: P.Style.brandDeep }
            }
            Column {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: P.Style.spaceLg
                spacing: P.Style.spaceXs
                Text {
                    text: "余额（元）"
                    font.pixelSize: P.Style.fontSm
                    color: P.Style.brandBright
                }
                Text {
                    text: "¥ " + (App && App.currentUser ? page.money(App.currentUser.balanceCents) : "0.00")
                    font.pixelSize: 30
                    color: P.Style.surface
                }
            }
        }

        Row {
            spacing: P.Style.spaceMd
            P.ActionButton {
                variant: "primary"; text: "充值"
                onClicked: if (App) App.navigate("recharge")
            }
            P.ActionButton {
                variant: "ghost"; text: "刷新"
                onClicked: page.refresh()
            }
        }

        Text { text: "充值记录"; font.pixelSize: P.Style.fontMd; color: P.Style.muted }

        P.PullToRefreshArea {
            id: recordsScroll
            objectName: "uiWalletRecords"
            width: parent.width
            height: parent.height - y
            onRefreshRequested: {
                walletService.fetchProfile()
                walletService.fetchRechargeRecords(1)
            }

            Repeater {
                model: recordsModel
                delegate: Column {
                    width: recordsScroll.width
                    P.Card {
                        objectName: "uiRecordRow"
                        width: parent.width
                        Row {
                            width: parent.width
                            spacing: P.Style.spaceMd
                            Column {
                                width: parent.width * 0.55
                                spacing: 2
                                Text {
                                    text: model.transactionNo
                                    font.pixelSize: P.Style.fontMd; color: P.Style.ink
                                }
                                Text {
                                    text: model.createdAt
                                    font.pixelSize: P.Style.fontSm; color: P.Style.faint
                                }
                            }
                            Item { width: parent.width * 0.2; height: 1 }
                            Column {
                                width: parent.width * 0.25
                                spacing: 2
                                Text {
                                    horizontalAlignment: Text.AlignRight
                                    text: "+¥ " + page.money(model.amountCents)
                                    font.pixelSize: P.Style.fontMd
                                    color: P.Style.brandDeep
                                }
                                Text {
                                    horizontalAlignment: Text.AlignRight
                                    text: "余额 ¥ " + page.money(model.balanceAfterCents)
                                    font.pixelSize: P.Style.fontSm; color: P.Style.muted
                                }
                            }
                        }
                    }
                    Rectangle {
                        objectName: "uiRecordSeparator"
                        width: recordsScroll.width
                        height: 1
                        color: index === recordsModel.count - 1 ? "transparent" : P.Style.line
                    }
                }
            }
        }
    }
}
