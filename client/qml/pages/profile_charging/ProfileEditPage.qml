import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets ProfileEditPage (route: "profile_edit", from 我的页).
// Nickname/avatar persist through UPDATE_USER_INFO (mock echoes profileLoaded).
Item {
    id: page
    objectName: "profileEditPage"
    property string route: "profile_edit"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property string nickname: App && App.currentUser ? (App.currentUser.nickname || "") : ""
    property string avatarKey: App && App.currentUser ? (App.currentUser.avatarKey || "🐱") : "🐱"
    readonly property var avatarChoices: ["🐱", "🐶", "🦊", "🐼", "🐨", "🦁", "🐯", "🐰"]

    Connections {
        target: walletService
        function onProfileLoaded(user) {
            // Bridge syncs App.currentUser; celebrate once and leave.
            if (App) App.showToast("资料已更新", "success")
            if (App) App.back()
        }
        function onOperationFailed(type, code, message) {
            if (App) App.showToast("保存失败：" + message, "danger")
        }
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacing: P.Style.spaceLg

        Text { text: "编辑资料"; font.pixelSize: P.Style.fontXl; color: P.Style.ink }

        Text { text: "头像"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
        Grid {
            objectName: "uiAvatarChoice"
            columns: 4
            columnSpacing: P.Style.spaceMd
            rowSpacing: P.Style.spaceMd
            Repeater {
                model: page.avatarChoices
                Rectangle {
                    width: 64; height: 64; radius: P.Style.radiusMd
                    color: page.avatarKey === modelData ? P.Style.brandSoft : P.Style.surface
                    border.color: page.avatarKey === modelData ? P.Style.brand : P.Style.line
                    border.width: page.avatarKey === modelData ? 2 : 1
                    Text {
                        anchors.centerIn: parent
                        text: modelData; font.pixelSize: 30
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.avatarKey = modelData
                    }
                }
            }
        }

        Text { text: "昵称"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
        TextField {
            objectName: "uiNicknameEdit"
            width: parent.width
            text: page.nickname
            placeholderText: "输入昵称"
            color: P.Style.ink
            font.pixelSize: P.Style.fontMd
            background: Rectangle {
                radius: P.Style.radiusMd
                color: P.Style.ghost
                border.color: activeFocus ? P.Style.brand : P.Style.line
                border.width: 1
            }
            onTextChanged: page.nickname = text
        }

        Row {
            spacing: P.Style.spaceMd
            P.ActionButton {
                objectName: "profileSaveButton"
                variant: "primary"; text: "保存"
                enabled: page.nickname.trim().length > 0
                onClicked: {
                    if (page.nickname !== (App && App.currentUser ? App.currentUser.nickname : ""))
                        walletService.updateNickname(page.nickname)
                    else if (page.avatarKey !== (App && App.currentUser ? App.currentUser.avatarKey : ""))
                        walletService.updateAvatar(page.avatarKey)
                    else if (App) App.back()
                }
            }
            P.ActionButton {
                variant: "ghost"; text: "取消"
                onClicked: if (App) App.back()
            }
        }

        Text {
            width: parent.width
            text: "头像库与 widgets 共用 avatar_library 语义；持久化走 UPDATE_USER_INFO"
            font.pixelSize: P.Style.fontSm; color: P.Style.faint
        }
    }
}
