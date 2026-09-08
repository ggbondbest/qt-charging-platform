import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets ProfileEditPage (route: "profile_edit", from 我的页).
// Nickname/avatar persist through UPDATE_USER_INFO with an explicit write ACK.
Item {
    id: page
    objectName: "profileEditPage"
    property string route: "profile_edit"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    // Copy the initial profile into a local draft. A read or the first write ACK
    // may update App.currentUser, but must never overwrite unsaved draft fields.
    property string nickname: ""
    property string avatarKey: ""
    Component.onCompleted: {
        var user = App && App.currentUser ? App.currentUser : ({})
        nickname = user.nickname || ""
        avatarKey = user.avatarKey || ""
    }

    // QML twin of widgets AvatarLibrary — 提交的永远是 key（契约白名单），
    // glyph/色底只做展示。test_qml_client_pages 的 parity 用例逐键比对
    // AvatarLibrary::all()，两边字面量漂移即红（同三方对拍治理模式）。
    readonly property var avatarChoices: [
        { key: "bolt",   glyph: "⚡",  color: "#00B578" },
        { key: "plug",   glyph: "🔌", color: "#1971C2" },
        { key: "car",    glyph: "🚗", color: "#D97706" },
        { key: "leaf",   glyph: "🌿", color: "#4CAF50" },
        { key: "cat",    glyph: "🐱", color: "#E0855E" },
        { key: "panda",  glyph: "🐼", color: "#607D8B" },
        { key: "moon",   glyph: "🌙", color: "#7E57C2" },
        { key: "rocket", glyph: "🚀", color: "#DC2626" }]

    // —— 顺序保存状态机 ——
    // WalletService 单飞且对在途重复提交静默丢弃（updateAvatar 撞更新中昵称会被吞），
    // 因此双改必须串行：昵称落定 → 再发头像 → 全部成功才退出；任一步失败停在页上。
    property bool sending: false
    property string pendingStep: ""    // "avatar" = 昵称成功后待补发的头像改动
    property string pendingField: ""

    Connections {
        target: walletService
        function onProfileUpdated(field, user) {
            if (!page.sending || field !== page.pendingField) return
            // Bridge syncs App.currentUser — 每步成功后差异自动缩小。
            if (page.pendingStep === "avatar") {
                page.pendingStep = ""
                page.pendingField = "avatar"
                walletService.updateAvatar(page.avatarKey)   // sending 保持 true
                return
            }
            page.sending = false
            page.pendingField = ""
            if (App) App.showToast("资料已更新", "success")
            if (App) App.back()
        }
        function onOperationFailed(type, code, message) {
            if (type !== "UPDATE_USER_INFO" || !page.sending) return
            page.sending = false
            page.pendingStep = ""
            page.pendingField = ""
            // 留在编辑页：已落定字段桥已写回 currentUser，再按保存只会补发剩余改动。
            if (App) App.showToast("保存失败：" + message, "danger")
        }
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacing: P.Style.spaceLg

        Text { text: "编辑资料"; font.pixelSize: P.Style.fontXl; color: P.Style.ink }

        Text { text: "头像"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
        Row {
            spacing: P.Style.spaceMd
            Image {
                width: 56; height: 56
                visible: page.avatarKey.indexOf("data:image/png;base64,") === 0
                source: visible ? page.avatarKey : ""
                fillMode: Image.PreserveAspectCrop
            }
            P.ActionButton {
                objectName: "uploadAvatarButton"
                text: "选择本地图片"; variant: "secondary"
                enabled: !page.sending
                onClicked: {
                    if (walletService.isUpdatingProfile()) {
                        if (App) App.showToast("资料正在保存，请稍后再试", "warning")
                        return
                    }
                    var image = App ? App.chooseAvatar() : ""
                    if (image.length > 0) page.avatarKey = image
                }
            }
        }
        Grid {
            objectName: "uiAvatarChoice"
            enabled: !page.sending
            columns: 5
            columnSpacing: P.Style.spaceMd
            rowSpacing: P.Style.spaceMd

            // An empty key uses the same gray default user avatar as the shell.
            Rectangle {
                objectName: "uiAvatarCell_default"
                width: 56; height: 56; radius: P.Style.radiusMd
                color: page.avatarKey === "" ? P.Style.brandSoft : P.Style.surface
                border.color: page.avatarKey === "" ? P.Style.brand : P.Style.line
                border.width: page.avatarKey === "" ? 2 : 1
                Rectangle {
                    anchors.centerIn: parent
                    width: 40; height: 40; radius: 20
                    color: P.Style.line
                    Text {
                        anchors.centerIn: parent
                        text: "👤"
                        font.pixelSize: 18; color: P.Style.surface
                    }
                }
                MouseArea {
                    objectName: "uiAvatarMouse"
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.avatarKey = ""
                }
            }
            Repeater {
                model: page.avatarChoices
                Rectangle {
                    objectName: "uiAvatarCell_" + modelData.key
                    width: 56; height: 56; radius: P.Style.radiusMd
                    color: page.avatarKey === modelData.key ? P.Style.brandSoft : P.Style.surface
                    border.color: page.avatarKey === modelData.key ? P.Style.brand : P.Style.line
                    border.width: page.avatarKey === modelData.key ? 2 : 1
                    Rectangle {
                        anchors.centerIn: parent
                        width: 40; height: 40; radius: 20
                        color: modelData.color
                        Text {
                            anchors.centerIn: parent
                            text: modelData.glyph; font.pixelSize: 18
                        }
                    }
                    MouseArea {
                        objectName: "uiAvatarMouse"
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.avatarKey = modelData.key
                    }
                }
            }
        }

        Text { text: "昵称"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
        TextField {
            objectName: "uiNicknameEdit"
            enabled: !page.sending
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
                enabled: page.nickname.trim().length > 0 && !page.sending
                onClicked: {
                    if (walletService.isUpdatingProfile()) {
                        if (App) App.showToast("资料正在保存，请稍后再试", "warning")
                        return
                    }
                    var cur = App && App.currentUser ? App.currentUser : null
                    var curNick = cur && cur.nickname ? cur.nickname : ""
                    var curKey = cur ? (cur.avatarKey || "") : ""
                    var nickChanged = page.nickname.trim() !== curNick
                    var avatarChanged = page.avatarKey !== curKey
                    if (!nickChanged && !avatarChanged) { if (App) App.back(); return }
                    page.sending = true
                    if (nickChanged) {
                        page.pendingStep = avatarChanged ? "avatar" : ""
                        page.pendingField = "nickname"
                        walletService.updateNickname(page.nickname)
                    } else {
                        page.pendingField = "avatar"
                        walletService.updateAvatar(page.avatarKey)
                    }
                }
            }
            P.ActionButton {
                variant: "ghost"; text: "取消"
                onClicked: if (App) App.back()
            }
        }

        Text {
            width: parent.width
            text: "本地图片将压缩为 PNG，保存后在其他设备登录也可显示。"
            font.pixelSize: P.Style.fontSm; color: P.Style.faint
        }
    }
}
