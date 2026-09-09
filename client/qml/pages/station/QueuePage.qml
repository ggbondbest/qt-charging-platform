import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

Item {
    id: page
    objectName: "queuePage"
    property string route: "queue"
    property var arg: ({})
    property var workflow: typeof App !== "undefined" ? App.workflowService : null
    property string accountId: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 700
    property var entry: null
    property bool loaded: false
    property bool busy: false
    property string errorMessage: ""
    property string readId: ""
    property string writeId: ""
    property string retryType: ""
    property var retryData: ({})
    property real receivedAt: 0
    property int remaining: 0
    readonly property bool active: !!entry && (entry.status === "WAITING" || entry.status === "CALLED")
    readonly property bool called: !!entry && entry.status === "CALLED"

    function identity() {
        return typeof App !== "undefined" && App.loggedIn ? String(App.currentUser.id || "") : ""
    }
    function synchronizeIdentity() {
        var next = identity()
        if (next === accountId) return
        accountId = next
        entry = null; loaded = false; busy = false; errorMessage = ""
        readId = ""; writeId = ""; retryType = ""; retryData = ({})
        remaining = 0; receivedAt = 0
        if (visible) refresh()
    }
    function operationId() {
        return "queue_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 14)
    }
    function refresh() {
        if (!workflow || !identity() || busy || readId.length > 0) return
        readId = workflow.request("QUEUE_GET_MINE", {})
    }
    function send(type, payload) {
        if (!workflow || !identity() || busy) return
        busy = true
        errorMessage = ""
        readId = "" // Ignore an older poll that could otherwise overwrite this write.
        // Preserve the exact original command for uncertain-response retries.
        retryType = type
        retryData = payload
        writeId = workflow.request(type, payload)
    }
    function accept(data) {
        entry = data.item || null
        loaded = true
        receivedAt = Date.now()
        remaining = called ? Math.max(0, Number(entry.confirmationSecondsRemaining || 0)) : 0
    }
    Connections {
        target: page.workflow
        function onFinished(requestId, type, success, data, error) {
            if (page.accountId !== page.identity()) return
            if (requestId === page.readId && page.readId.length > 0) {
                page.readId = ""
                if (success) page.accept(data)
                else page.errorMessage = error.message || "排队信息暂时无法更新"
                return
            }
            if (requestId !== page.writeId || page.writeId.length === 0) return
            page.writeId = ""
            page.busy = false
            if (!success) {
                page.errorMessage = error.message || "操作未完成，请重试"
                // Retain idempotency key only when result may be unknown.
                if (["INVALID_ARGUMENT", "USER_FROZEN", "NOT_FOUND", "CONFLICT", "IDEMPOTENCY_CONFLICT",
                     "INVALID_STATE_TRANSITION", "CHARGER_NOT_AVAILABLE", "UNKNOWN_REQUEST_TYPE"].indexOf(error.code) >= 0) {
                    page.retryType = ""
                    page.retryData = ({})
                }
                page.refresh()
                return
            }
            page.retryType = ""
            page.retryData = ({})
            page.errorMessage = ""
            page.accept(data)
            if (type === "QUEUE_CONFIRM") App.navigate("charging")
        }
        function onChanged() { if (page.visible) page.refresh() }
    }
    Connections {
        target: typeof App !== "undefined" ? App : null
        function onUserChanged() { page.synchronizeIdentity() }
        function onLoginStateChanged() { page.synchronizeIdentity() }
    }
    Timer { interval: 2000; repeat: true; running: page.visible && page.accountId.length > 0; onTriggered: page.refresh() }
    Timer {
        interval: 250; repeat: true; running: page.visible && page.called
        onTriggered: page.remaining = Math.max(0, Number(page.entry.confirmationSecondsRemaining || 0)
                                               - Math.floor((Date.now() - page.receivedAt) / 1000))
    }
    Component.onCompleted: { accountId = identity(); refresh() }
    onVisibleChanged: if (visible) { synchronizeIdentity(); refresh() }
    Rectangle { anchors.fill: parent; color: P.Style.bg }
    Flickable {
        anchors.fill: parent; contentWidth: width
        contentHeight: content.height + 2 * P.Style.spaceLg; clip: true
        Column {
            id: content
            x: P.Style.spaceLg; y: P.Style.spaceLg
            width: parent.width - 2 * P.Style.spaceLg
            spacing: P.Style.spaceLg
            Text { text: "我的排队"; color: P.Style.ink; font.pixelSize: P.Style.fontXl; font.bold: true }
            P.Card {
                width: parent.width
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: (page.active ? page.entry.stationName : page.arg.stationName) || "选择电桩，安心等候"
                    color: P.Style.ink; font.pixelSize: P.Style.fontLg; font.bold: true
                }
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: "电桩：" + ((page.active ? page.entry.chargerCode : page.arg.chargerCode) || "—")
                    color: P.Style.muted
                }
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: !page.loaded ? (page.errorMessage.length ? "排队状态暂时无法读取，请重试" : "正在读取排队状态…")
                          : page.called ? "轮到您了，请确认使用电桩"
                          : page.active ? (page.entry.maintenance ? "电桩维护中，队列保留，恢复后继续叫号" : "您已加入队列，服务器按先到先服务叫号")
                          : page.entry && page.entry.status === "EXPIRED" ? "上次排队机会已超时或失效，可重新加入"
                          : page.entry && page.entry.status === "CONFIRMED" ? "已确认并进入预约，请在截止时间前开始充电"
                          : "当前没有进行中的排队"
                    color: page.called ? P.Style.brandDeep : P.Style.muted
                }
                Row {
                    width: parent.width; spacing: 24; visible: page.active
                    Column {
                        spacing: 6
                        Text { text: page.called ? page.remaining + " 秒" : String(page.entry ? page.entry.position : "—")
                            font.pixelSize: 36; font.bold: true; color: P.Style.brandDeep }
                        Text { text: page.called ? "确认剩余时间" : "当前排队位置"; color: P.Style.muted }
                    }
                    Column {
                        spacing: 6
                        Text { text: String(page.entry ? page.entry.aheadCount : "—") + " 人"; font.pixelSize: 36; font.bold: true; color: P.Style.ink }
                        Text { text: "前方人数"; color: P.Style.muted }
                    }
                }
            }
            Text {
                width: parent.width; wrapMode: Text.WordWrap; color: P.Style.muted
                text: "本站暂无可用电桩时可加入队列。叫号后保留 60 秒，确认后进入 15 分钟预约倒计时；未确认或预约超时会自动释放给下一位。充电结束视为车位空出。"
            }
            Text { width: parent.width; wrapMode: Text.WordWrap; visible: page.errorMessage.length > 0
                text: page.errorMessage; color: P.Style.danger }
            P.ActionButton {
                objectName: "queueJoinButton"; width: parent.width
                visible: page.loaded && !page.active && !!page.arg.chargerId && page.retryType.length === 0
                text: page.busy ? "正在加入…" : "加入此电桩队列"; enabled: !page.busy
                onClicked: page.send("QUEUE_JOIN", {chargerId: String(page.arg.chargerId), operationId: page.operationId()})
            }
            P.ActionButton {
                objectName: "queueConfirmButton"; width: parent.width
                visible: page.called && page.retryType.length === 0
                text: page.busy ? "正在确认…" : "确认使用 · 开始预约"; enabled: !page.busy && page.remaining > 0
                onClicked: page.send("QUEUE_CONFIRM", {id: String(page.entry.id), operationId: page.operationId()})
            }
            P.ActionButton {
                objectName: "queueLeaveButton"; width: parent.width; variant: "secondary"
                visible: page.active && page.retryType.length === 0
                text: "退出排队"; enabled: !page.busy
                onClicked: page.send("QUEUE_LEAVE", {id: String(page.entry.id), operationId: page.operationId()})
            }
            P.ActionButton {
                objectName: "queueRetryButton"; width: parent.width
                visible: page.retryType.length > 0 && !page.busy
                text: "重试原操作（不会重复排队）"
                onClicked: page.send(page.retryType, page.retryData)
            }
            P.ActionButton { width: parent.width; variant: "secondary"; text: "查看我的预约";
                visible: !page.active && !!page.entry && page.entry.status === "CONFIRMED"
                onClicked: App.navigate("charging") }
            P.ActionButton { width: parent.width; variant: "ghost"; text: "刷新排队状态"; enabled: !page.busy; onClicked: page.refresh() }
            P.ActionButton { width: parent.width; variant: "secondary"; text: "返回"; onClicked: App.back() }
        }
    }
}
