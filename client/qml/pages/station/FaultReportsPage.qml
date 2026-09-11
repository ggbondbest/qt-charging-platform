import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../../platform" as P

Item {
    id: page
    objectName: "faultReportsPage"
    property string route: "fault_reports"
    // chargerId/code/stationName opens the submission form; no chargerId opens
    // “我的报障”. Identity is always taken from the authenticated connection.
    property var arg: ({})
    property var workflow: typeof App !== "undefined" ? App.workflowService : null
    property var reports: []
    property var selected: ({})
    property string listRequest: ""
    property string detailRequest: ""
    property string submitRequest: ""
    property var submissionAttempt: null
    property string accountId: ""
    property string message: ""
    property int currentPage: 1
    property int total: 0
    property bool formVisible: !!(arg && arg.chargerId)
    readonly property bool submitting: submitRequest.length > 0
    readonly property bool showingDetails: !!selected.id
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    function identity() {
        return typeof App !== "undefined" && App.loggedIn ? String(App.currentUser.id || "") : ""
    }
    function statusText(status) {
        return ({SUBMITTED: "已提交", ACCEPTED: "已受理", PROCESSING: "处理中", RESOLVED: "已恢复"})[status] || "—"
    }
    function problemText(problem) {
        return ({CONNECTION: "连接异常", SCREEN: "屏幕异常", CONNECTOR: "充电枪损坏",
                 CHARGING: "无法正常充电", OTHER: "其他问题"})[problem] || "其他问题"
    }
    function timeText(value) {
        var d = new Date(value)
        return isNaN(d.getTime()) ? "—" : d.toLocaleString(Qt.locale(), "MM-dd HH:mm:ss")
    }
    function load() {
        if (!workflow || !identity() || listRequest.length) return
        listRequest = workflow.request("REPAIR_GET_MINE", {page: currentPage, pageSize: 20})
    }
    function openDetail(report) {
        selected = report
        detailRequest = ""
        refreshDetail()
    }
    function refreshDetail() {
        if (!workflow || !identity() || !selected.id || detailRequest.length) return
        detailRequest = workflow.request("REPAIR_GET", {id: String(selected.id)})
    }
    function submit() {
        if (!workflow || !identity() || submitting || !arg || !arg.chargerId) return
        if (!submissionAttempt) {
            var description = descriptionInput.text.trim()
            if (!description.length || description.length > 200) { message = "请填写 1～200 字的问题说明"; return }
            submissionAttempt = {
                chargerId: String(arg.chargerId),
                problemType: ["CONNECTION", "SCREEN", "CONNECTOR", "CHARGING", "OTHER"][problemCombo.currentIndex],
                description: description,
                operationId: "repair-" + Date.now() + "-" + Math.random().toString(36).substring(2, 12)
            }
        }
        message = "正在提交，请勿重复操作"
        submitRequest = workflow.request("REPAIR_SUBMIT", submissionAttempt)
    }
    function synchronizeIdentity() {
        var next = identity()
        if (next === accountId) return
        accountId = next
        // Ignore results from an old session; never show another account's
        // reports or replay its pending mutation after an account switch.
        reports = []; selected = ({}); total = 0; currentPage = 1
        listRequest = ""; detailRequest = ""; submitRequest = ""
        submissionAttempt = null; message = ""
        if (visible) load()
    }
    Connections {
        target: page.workflow
        function onFinished(requestId, type, success, data, error) {
            if (page.accountId !== page.identity()) return
            if (requestId === page.submitRequest && type === "REPAIR_SUBMIT") {
                page.submitRequest = ""
                if (success) {
                    page.submissionAttempt = null
                    page.message = "报障已提交，管理员核实后处理。提交本身不会停用电桩。"
                    page.formVisible = false; page.openDetail(data.item || ({})); page.currentPage = 1; page.load()
                } else {
                    var code = error && error.code ? String(error.code) : ""
                    page.message = error && error.message ? String(error.message) : "提交失败，请重试"
                    // Transport errors are unknown outcomes: retain exactly the
                    // same operationId and payload for an idempotent retry.
                    if (["INVALID_ARGUMENT", "USER_FROZEN", "NOT_FOUND", "ALREADY_EXISTS", "CONFLICT"].indexOf(code) >= 0)
                        page.submissionAttempt = null
                    page.load()
                }
            } else if (requestId === page.listRequest && type === "REPAIR_GET_MINE") {
                page.listRequest = ""
                if (success) { page.reports = data.items || []; page.total = Number(data.total || 0) }
                else page.message = error && error.message ? String(error.message) : "刷新失败，保留上次结果"
            } else if (requestId === page.detailRequest && type === "REPAIR_GET") {
                page.detailRequest = ""
                if (success && data.item && String(data.item.id) === String(page.selected.id)) page.selected = data.item
                else if (!success) page.message = error && error.message ? String(error.message) : "详情刷新失败，保留上次结果"
            }
        }
        function onChanged() {
            if (page.visible) { page.load(); page.refreshDetail() }
        }
    }
    Connections {
        target: typeof App !== "undefined" ? App : null
        function onUserChanged() { page.synchronizeIdentity() }
        function onLoginStateChanged() { page.synchronizeIdentity() }
    }
    Timer {
        interval: 2000; repeat: true
        running: page.visible && page.accountId.length > 0 && !page.submitting
        onTriggered: { page.load(); page.refreshDetail() }
    }
    Component.onCompleted: { accountId = identity(); load() }
    onVisibleChanged: if (visible) { synchronizeIdentity(); load(); refreshDetail() }

    ScrollView {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        clip: true
        contentWidth: availableWidth
        Column {
            width: parent.width
            spacing: P.Style.spaceMd
            RowLayout {
                width: parent.width
                Text {
                    Layout.fillWidth: true
                    text: page.showingDetails ? "报障进度" : page.formVisible ? "一键报障" : "我的报障"
                    color: P.Style.ink; font.pixelSize: P.Style.fontXl; font.bold: true
                }
                P.ActionButton {
                    variant: "ghost"
                    text: page.showingDetails ? "返回列表" : "刷新"
                    onClicked: {
                        if (page.showingDetails) { page.selected = ({}); page.detailRequest = "" }
                        page.load()
                    }
                }
            }
            Text {
                width: parent.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
                text: page.message || "提交不会直接停用电桩。管理员核实后维护，模拟维修完成后通知您。"
                color: page.message.length ? P.Style.brandDeep : P.Style.muted
                font.pixelSize: P.Style.fontSm
            }
            P.Card {
                width: parent.width
                visible: page.formVisible && !page.showingDetails
                Text {
                    width: parent.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
                    text: (page.arg.stationName || "充电站") + " · " + (page.arg.chargerCode || page.arg.code || "")
                    font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink
                }
                P.ComboBox {
                    id: problemCombo; objectName: "repairProblemType"
                    width: parent.width; enabled: !page.submissionAttempt
                    model: ["连接异常", "屏幕异常", "充电枪损坏", "无法正常充电", "其他问题"]
                }
                TextArea {
                    id: descriptionInput; objectName: "repairDescription"
                    width: parent.width; height: 126; enabled: !page.submissionAttempt
                    placeholderText: "请简要描述问题，例如：插枪后屏幕无响应（最多 200 字）"
                    wrapMode: TextEdit.Wrap; selectByMouse: true; padding: 12
                    color: P.Style.ink; placeholderTextColor: P.Style.muted
                    font.pixelSize: P.Style.fontMd
                    onTextChanged: if (text.length > 200) text = text.substring(0, 200)
                    background: Rectangle {
                        color: P.Style.surface; radius: P.Style.radiusSm
                        border.color: descriptionInput.activeFocus ? P.Style.brand : P.Style.line
                        border.width: descriptionInput.activeFocus ? 2 : 1
                    }
                }
                Text { text: descriptionInput.text.length + "/200"; color: P.Style.muted; font.pixelSize: P.Style.fontSm }
                P.ActionButton {
                    objectName: "repairSubmitButton"
                    width: parent.width; enabled: !page.submitting && page.accountId.length > 0
                    text: page.submitting ? "提交中…" : page.submissionAttempt ? "重试提交（同一操作）" : "提交报障"
                    onClicked: page.submit()
                }
            }
            P.Card {
                visible: page.showingDetails; width: parent.width
                Text {
                    width: parent.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
                    text: (page.selected.stationName || "") + "\n" + (page.selected.chargerCode || "")
                    color: P.Style.ink; font.pixelSize: P.Style.fontMd; font.bold: true
                }
                P.StatusTag { text: page.statusText(page.selected.status); tone: page.selected.status === "RESOLVED" ? "success" : "warning" }
                Text {
                    width: parent.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
                    text: page.problemText(page.selected.problemType) + "：" + (page.selected.description || "")
                    color: P.Style.ink; font.pixelSize: P.Style.fontMd
                }
                Text {
                    width: parent.width; wrapMode: Text.Wrap
                    text: "更新于 " + page.timeText(page.selected.updatedAt) + (page.selected.maintenance ? " · 电桩维护中" : "")
                    color: P.Style.muted; font.pixelSize: P.Style.fontSm
                }
                Repeater {
                    model: page.selected.timeline || []
                    RowLayout {
                        width: parent.width; spacing: 12
                        Rectangle { Layout.alignment: Qt.AlignTop; width: 12; height: 12; radius: 6; color: P.Style.brand }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 5
                            Text { text: page.statusText(modelData.status); font.bold: true; color: P.Style.ink; font.pixelSize: P.Style.fontMd }
                            Text { text: page.timeText(modelData.createdAt); color: P.Style.muted; font.pixelSize: P.Style.fontSm }
                            Text {
                                Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
                                text: modelData.note || "—"; color: P.Style.ink; font.pixelSize: P.Style.fontMd
                            }
                            Item { height: 12 }
                        }
                    }
                }
            }
            Repeater {
                model: page.showingDetails ? [] : page.reports
                P.Card {
                    width: parent.width
                    Text {
                        width: parent.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
                        text: (modelData.stationName || "") + " · " + (modelData.chargerCode || "")
                        color: P.Style.ink; font.pixelSize: P.Style.fontMd; font.bold: true
                    }
                    Text {
                        width: parent.width; wrapMode: Text.Wrap
                        text: page.problemText(modelData.problemType) + " · " + page.statusText(modelData.status)
                        color: P.Style.brandDeep; font.pixelSize: P.Style.fontMd
                    }
                    Text { text: "更新于 " + page.timeText(modelData.updatedAt); color: P.Style.muted; font.pixelSize: P.Style.fontSm }
                    P.ActionButton { width: parent.width; variant: "secondary"; text: "查看处理时间轴"; onClicked: page.openDetail(modelData) }
                }
            }
            Text {
                visible: !page.showingDetails && !page.reports.length
                width: parent.width; wrapMode: Text.Wrap
                text: page.listRequest.length ? "正在加载…" : "暂无报障记录，可在电站详情中选择电桩报障。"
                color: P.Style.muted; font.pixelSize: P.Style.fontMd
            }
            RowLayout {
                width: parent.width; visible: !page.showingDetails && page.total > 20
                P.ActionButton {
                    objectName: "repairPreviousPage"
                    text: "上一页"; variant: "secondary"; Layout.fillWidth: true
                    enabled: page.currentPage > 1 && !page.listRequest.length
                    onClicked: { --page.currentPage; page.load() }
                }
                Text { text: page.currentPage + "/" + Math.ceil(page.total / 20); color: P.Style.muted }
                P.ActionButton {
                    objectName: "repairNextPage"
                    text: "下一页"; variant: "secondary"; Layout.fillWidth: true
                    enabled: page.currentPage * 20 < page.total && !page.listRequest.length
                    onClicked: { ++page.currentPage; page.load() }
                }
            }
        }
    }
}
