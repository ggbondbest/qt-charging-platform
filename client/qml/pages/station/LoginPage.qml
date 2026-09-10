import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "StationState.js" as StationState

// QML twin of widgets LoginPage (objectName "loginPage" kept).
// Phone-only login (no SMS code): validator mirrors QRegularExpressionValidator("1[0-9]{0,10}", 11).
// 二级保护密码在登录环节校验（用户二轮指定口径）：输入了设置过密码并开启
// 保护的手机号时，卡内出现"二级保护密码"输入行，验证通过才放行登录。
// route "login"：Shell 登录闸的唯一去处——未登录时 pushRoute 一律重定向到本页，
// App.loginRequested（其他页要登录）也推它；登录成功由 App.loginStateChanged
// 驱动 Shell 翻回原路由，本页自己不做任何导航。
// 数据流：phoneField → submit() → authService.login()（桥→TCP 服务）；结果既走
// 返回值兜底、也走 onLoginSucceeded/Failed 信号；二级密码双通道：服务真值优先，
// 桥缺位退 StationState 本地库。属"station 域 P0 六页"批。
Item {
    id: page
    objectName: "loginPage"
    property string route: "login"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }   // self-painted bg (offscreen grab)

    // busy 一词三用：防重入闸（submit 短路）、按钮禁用、LoadingOverlay 显隐同源。
    property bool busy: false
    property string resultText: "请输入11位手机号"
    property string resultTone: ""          // "" neutral | success | error

    // 金额分→元单点格式化（服务侧整型分防浮点误差，只在展示层除）。
    function money(cents) { return (cents / 100).toFixed(2) }
    // 提交门用"完整号段"正则；输入期宽松交给 maximumLength/掩码兜着，二者分工。
    function phoneOk(s) { return /^1[0-9]{10}$/.test(s) }

    // 该手机号是否需要二级密码（服务通道在线时以服务全局开关为准，否则库判定）
    // 二级密码是本机口令闸（存 SettingsService/StationState，从不上传服务端），
    // 与登录走 mock 还是真 TCP 无关——live 模式同样必须拦（缺陷修复 2026-09-10：
    // f54cead 曾在 live 模式整体短路本判定，导致设了密码后登录不再询问）。
    function secondRequired(phone) {
        try {
            // 严格 === true：桥半截或方法缺位会回 undefined——真值判定只认服务
            // 明确说"是"，其余一律退库通道，不让 truthy 杂值冒充开关。
            // 方法名走桥契约 second*（缺陷修复 2026-09-10：曾误用裸服务
            // protection* 名，桥无此方法、call 吞异常，登录永远读不到持久化真值）。
            if (settingsService && settingsService.hasSecondPassword
                && settingsService.hasSecondPassword() === true
                && settingsService.protectionEnabled
                && settingsService.protectionEnabled() === true) return true
        } catch (e) { /* 桥缺位 → 库通道 */ }
        return StationState.needsSecondPassword(phone)
    }
    function secondOk(pw) {
        // 同第二道门的双通道次序：服务回答必须是真布尔才采纳，缺位/杂值退库校验。
        try {
            if (settingsService && settingsService.verifySecondPassword) {
                const v = settingsService.verifySecondPassword(pw)
                if (typeof v === "boolean") return v
            }
        } catch (e) { /* 桥缺位 → 库通道 */ }
        return StationState.verifySecondPassword(pw)
    }
    // 绑定用：手机号打全且命中保护账号才显示密码行
    // （派生只读属性而非命令式显隐：换号/删号时密码行跟着正则自动收起，无需收尾）。
    readonly property bool secondVisible: phoneOk(phoneField.text)
                                          && page.secondRequired(phoneField.text)

    // 全量回初始：busy、两个输入框、回执文案与色调一起归零（登出/换号复用）。
    function resetState() {
        busy = false
        phoneField.text = ""
        secondField.text = ""
        resultText = "请输入11位手机号"
        resultTone = ""
    }

    // 登录成功回执文案的纯函数版（信号通道 onLoginSucceeded 内联同构文案）。
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
                // 错即清空：不留错误残值，逼整段重输
                secondField.text = ""
                resultTone = "error"
                resultText = "二级保护密码错误，请重新输入"
                secondField.forceActiveFocus()
                return
            }
        }
        // 两段式回执：登录() 同步 false=当场被拒（在途/号段问题）；受理后的
        // 成败走下方 Connections 异步信号，busy 也在那里收尾。
        busy = true
        resultText = "正在连接服务端并查询用户…"
        resultTone = ""
        StationState.noteLoginPhone(phoneField.text)   // 设置页绑定密码用最近登录号
        try {
            if (!authService || !authService.login(phoneField.text)) {
                busy = false; resultTone = "error"
                resultText = "请检查手机号，或等待当前登录请求完成"
            }
        } catch (e) {
            // authService 整个未暴露时读它就抛 ReferenceError——catch 兜"桥不存在"，
            // 区别于上面"桥在但拒单"的同步 false 分支。
            busy = false
            resultTone = "error"
            resultText = "登录服务不可用，请重新启动客户端"
        }
    }

    // ---- 登录回执：结果以服务信号为准，submit() 返回值只表"当场是否受理" ----
    Connections {
        target: authService
        // 服务侧自报"已开始"也置 busy：在途态双端都有话语权，不依赖调用方那条赋值。
        function onLoginStarted() { page.busy = true }
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
        // Flickable 量不了内容：contentHeight 手拼 = 列高 + 上下留白，小屏才滚得动。
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
                    P.TextField {
                        id: phoneField
                        objectName: "phoneLineEdit"
                        // -44 = "+86" 前缀文本列宽与 Row spacing 的实测合计扣位，
                        // 手机号框恰好吃满行内剩余宽度（改前缀文案需同步此数）。
                        width: parent.width - 44
                        placeholderText: "请输入 11 位手机号"
                        maximumLength: 11
                        // 显式声明无掩码：镜像 widgets 的"自由输入+提交期严判"，输入期不套格式枷锁。
                        inputMask: ""
                        enabled: !page.busy
                        // 号段校验交给 phoneOk()（IntValidator 装不下 1e10，超 qint32）
                        onAccepted: page.submit()
                    }
                }

                // 二级保护密码行：命中"设过密码且开启保护"的手机号才出现。
                P.TextField {
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
