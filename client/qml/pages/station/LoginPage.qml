import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "StationState.js" as StationState

// QML twin of widgets LoginPage (objectName "loginPage" kept).
// Phone-only login (no SMS code): validator mirrors QRegularExpressionValidator("1[0-9]{0,10}", 11).
// 二级保护密码在登录环节校验（用户二轮指定口径）：输入了设置过密码并开启
// 保护的手机号时，卡内出现"二级保护密码"输入行，验证通过才放行登录。
Item {
    id: page
    objectName: "loginPage"
    property string route: "login"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }   // self-painted bg (offscreen grab)

    property bool busy: false
    property string resultText: "请输入11位手机号"
    property string resultTone: ""          // "" neutral | success | error

    function money(cents) { return (cents / 100).toFixed(2) }
    function phoneOk(s) { return /^1[0-9]{10}$/.test(s) }

    // 该手机号是否需要二级密码（服务通道在线时以服务全局开关为准，否则库判定）
    function secondRequired(phone) {
        try {
            if (settingsService && settingsService.hasProtectionPassword
                && settingsService.hasProtectionPassword() === true
                && settingsService.protectionEnabled
                && settingsService.protectionEnabled() === true) return true
        } catch (e) { /* 桥缺位 → 库通道 */ }
        return StationState.needsSecondPassword(phone)
    }
    function secondOk(pw) {
        try {
            if (settingsService && settingsService.verifyProtectionPassword) {
                const v = settingsService.verifyProtectionPassword(pw)
                if (typeof v === "boolean") return v
            }
        } catch (e) { /* 桥缺位 → 库通道 */ }
        return StationState.verifySecondPassword(pw)
    }
    // 绑定用：手机号打全且命中保护账号才显示密码行
    readonly property bool secondVisible: phoneOk(phoneField.text)
                                          && page.secondRequired(phoneField.text)

    function resetState() {
        busy = false
        phoneField.text = ""
        secondField.text = ""
        resultText = "请输入11位手机号"
        resultTone = ""
    }

    function echoUser(u, created) {
        busy = false
        resultTone = "success"
        var m = u || {}
        resultText = "登录成功" + (created ? "（已自动注册）" : "")
                     + "\n用户ID：" + (m.id !== undefined ? m.id : "--")
                     + "\n昵称：" + (m.nickname || "")
                     + "\n余额：" + money(m.balanceCents || 0) + " 元"
    }

    function submit() {
        if (busy || !phoneOk(phoneField.text)) return
        // ---- 二级保护密码门（登录环节）----
        if (page.secondVisible) {
            if (secondField.text.length === 0) {
                resultTone = "error"
                resultText = "该账号已开启二级保护密码，请输入密码后登录"
                secondField.forceActiveFocus()
                return
            }
            if (!page.secondOk(secondField.text)) {
                secondField.text = ""
                resultTone = "error"
                resultText = "二级保护密码错误，请重新输入"
                secondField.forceActiveFocus()
                return
            }
        }
        busy = true
        resultText = "正在连接服务端并查询用户…"
        resultTone = ""
        StationState.noteLoginPhone(phoneField.text)   // 设置页绑定密码用最近登录号
        // Contract name verbatim; AuthService is a raw service tonight (bridge pending) —
        // call may fail until member-3's bridge lands. TODO(contract): login(phone) invokable
        // + loginSucceeded(userMap, created) / loginFailed(message).
        try {
            if (authService) { authService.login(phoneField.text); return }  // 成功经信号回显
            // mock 通道 QmlApp::authService()==nullptr（app_bridge.cpp:99），空调用不抛异常
            // 会永久卡 busy——回退 App.login()（C++ mock 直登，壳收 loginStateChanged 翻页）。
            if (App && App.login(phoneField.text)) {
                App.showToast("已登录（mock 通道直登）", "success")   // 已注册口径不作假，不标"自动注册"
                echoUser(App.currentUser, false)
                secondField.text = ""
            } else {
                busy = false; resultTone = "error"
                resultText = "登录失败：手机号无效或登录服务未就绪"
            }
        } catch (e) {                            // 桥缺位：显式回退而不是卡转圈
            busy = false
            resultTone = "error"
            resultText = "登录桥未就绪（等待服务桥今晚补全），请稍后重试"
        }
    }

    Connections {
        target: authService
        function onLoginSucceeded(user, created) {
            busy = false
            var u = user || {}
            resultTone = "success"
            resultText = "登录成功" + (created ? "（已自动注册）" : "")
                         + "\n用户ID：" + u.id + "\n昵称：" + (u.nickname || "")
                         + "\n余额：" + money(u.balanceCents || 0) + " 元"
        }
        function onLoginFailed(message) {
            busy = false
            resultTone = "error"
            resultText = "登录失败：" + message
        }
    }
    // Shell flips pages on App.loginStateChanged — nothing else to do here.

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: loginCol.height + 2 * P.Style.spaceXl
        clip: true

        Column {
            id: loginCol
            x: P.Style.spaceXl
            y: P.Style.spaceXl
            width: parent.width - 2 * P.Style.spaceXl
            spacing: P.Style.spaceXl

        Item { width: 1; height: P.Style.spaceXl }

        // Brand block (widgets parity: glyph / title / subtitle).
        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: P.Style.spaceSm
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "⚡"; font.pixelSize: 44
                color: P.Style.brand
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "电动汽车充电桩应用管理平台"
                font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "手机号快捷登录 · 充电服务一触即达"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
        }

        // Centered login card.
        P.Card {
            objectName: "loginCard"
            width: parent.width
            Column {
                width: parent.width
                spacing: P.Style.spaceMd

                Text { text: "手机号登录"; font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }

                // 行序对齐 widgets：标题→手机号行→说明→按钮
                Row {
                    width: parent.width          // 显式宽：否则子项引用 parent.width 成环（polish loop）
                    spacing: P.Style.spaceSm
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "+86"; font.pixelSize: P.Style.fontMd; color: P.Style.muted
                    }
                    TextField {
                        id: phoneField
                        objectName: "phoneLineEdit"
                        width: parent.width - 44
                        placeholderText: "请输入 11 位手机号"
                        maximumLength: 11
                        inputMask: ""
                        enabled: !page.busy
                        // 号段校验交给 phoneOk()（IntValidator 装不下 1e10，超 qint32）
                        onAccepted: page.submit()
                    }
                }

                // 二级保护密码行：命中"设过密码且开启保护"的手机号才出现。
                TextField {
                    id: secondField
                    objectName: "secondPasswordEdit"
                    width: parent.width
                    visible: page.secondVisible
                    placeholderText: "二级保护密码"
                    echoMode: TextInput.Password
                    enabled: !page.busy
                    onAccepted: page.submit()
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: "未注册的手机号将自动创建账号，登录后即可找站、预约与充电。"
                    font.pixelSize: P.Style.fontSm; color: P.Style.muted
                }

                P.ActionButton {
                    objectName: "loginButton"
                    width: parent.width
                    text: page.busy ? "登录中…" : "登 录"
                    enabled: !page.busy && phoneOk(phoneField.text)
                    onClicked: page.submit()
                }

                Text {
                    objectName: "resultLabel"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: page.resultText
                    font.pixelSize: P.Style.fontSm
                    color: page.resultTone === "success" ? P.Style.brandDeep
                         : page.resultTone === "error" ? P.Style.danger : P.Style.muted
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: "登录即表示同意用户服务协议与隐私政策"
                    font.pixelSize: P.Style.fontSm; color: P.Style.faint
                }
            }
        }
        }
    }

    P.LoadingOverlay { running: page.busy }
}
