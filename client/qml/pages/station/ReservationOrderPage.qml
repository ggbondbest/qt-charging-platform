import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// The server owns cancellation/expiry. Never hide the reservation before ACK.
Item {
    id: page
    objectName: "reservationOrderPage"
    width: parent ? parent.width : 420
    height: parent ? parent.height : 500
    property var record: null
    readonly property var rec: record || ({})
    property bool loading: false
    property bool parentFailed: false
    readonly property string reservationId: String(rec.reservationId || rec.id || "")
    readonly property bool hasActive: reservationId.length > 0
                                      && String(rec.status).toLowerCase() === "active"
    property bool cancelBusy: false
    property bool starting: false
    property string cancelNote: ""
    property int tick: 0
    property double lastExpiryRefresh: 0

    function parseTs(value) {
        if (typeof value === "number") return value
        if (typeof value === "string" && /(Z|[+-]\d\d:\d\d)$/.test(value))
            return Date.parse(value)
        return NaN
    }
    function clock(seconds) {
        const minutes = Math.floor(seconds / 60), remainder = seconds % 60
        return (minutes < 10 ? "0" : "") + minutes + ":" + (remainder < 10 ? "0" : "") + remainder
    }
    function hhmm(value) {
        const milliseconds = parseTs(value)
        return isNaN(milliseconds) ? "--" : new Date(milliseconds + 8 * 3600000).toISOString().slice(11, 19)
    }
    readonly property int remainingSecs: {
        page.tick
        const expiry = parseTs(rec.expiresAtUtc)
        return !hasActive || isNaN(expiry) ? 0 : Math.max(0, Math.floor((expiry - Date.now()) / 1000))
    }
    readonly property string countdownText: remainingSecs > 0 ? clock(remainingSecs) : "正在确认状态…"
    Timer {
        objectName: "countdownTimer"
        interval: 1000; repeat: true
        running: page.hasActive && !page.parentFailed
        onTriggered: {
            page.tick++
            if (page.remainingSecs === 0 && Date.now() - page.lastExpiryRefresh > 5000) {
                page.lastExpiryRefresh = Date.now()
                reservationService.fetchList()
            }
        }
    }

    function requestCancel() {
        if (!hasActive || cancelBusy || starting) return
        cancelNote = ""
        cancelDialog.open()
    }
    function cancel() {
        if (!hasActive || cancelBusy || starting) return
        cancelBusy = true
        cancelNote = ""
        try { reservationService.cancel(reservationId) }
        catch (error) {
            cancelBusy = false
            cancelNote = "取消预约失败，请刷新后重试"
        }
    }
    function start() {
        if (!hasActive || cancelBusy || starting || remainingSecs <= 0) return
        starting = true; cancelNote = ""
        try { chargingService.startCharging(reservationId) }
        catch (error) { starting = false; cancelNote = "启动失败，请刷新后重试" }
    }
    Connections {
        target: reservationService
        function onCancelStarted(id) {
            if (String(id) === page.reservationId) page.cancelBusy = true
        }
        function onCancelFailed(message) {
            if (!page.cancelBusy) return
            page.cancelBusy = false; page.cancelNote = message
        }
        function onCancelSucceeded(id) {
            if (String(id) !== page.reservationId) return
            page.cancelBusy = false
            page.cancelNote = "预约已取消，电桩已释放"
        }
    }
    Connections {
        target: chargingService
        function onStartCompleted(status) { page.starting = false }
        function onOperationFailed(type, code, message) {
            if (type !== "START_CHARGING" || !page.starting) return
            page.starting = false; page.cancelNote = message
        }
    }

    P.NoticePanel {
        objectName: "orderEmptyNotice"
        anchors.fill: parent
        visible: !page.hasActive && !page.loading
        glyph: "car"; title: "暂无进行中的预约"
        description: "已取消、已过期或已开始充电的预约可在预约历史中查看。"
        actionText: "去找桩"
        onActionTriggered: App.navigate("station")
    }
    P.LoadingOverlay { running: page.loading && !page.hasActive }
    Flickable {
        id: scroll
        anchors.fill: parent
        visible: page.hasActive
        contentWidth: width
        contentHeight: card.implicitHeight
        clip: true
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        P.Card {
            id: card
            objectName: "countdownCard"
            width: scroll.width
            Column {
                width: parent.width; spacing: P.Style.spaceMd
                Text { text: "电桩已为你保留"; color: P.Style.brandDeep; font.bold: true; font.pixelSize: P.Style.fontLg }
                Text {
                    objectName: "orderActiveInfoLabel"
                    width: parent.width; wrapMode: Text.WordWrap
                    text: (page.rec.stationName || "充电站") + "\n电桩 " + (page.rec.chargerCode || "--")
                    color: P.Style.ink; font.pixelSize: P.Style.fontMd
                }
                Rectangle {
                    width: parent.width; height: countdown.implicitHeight + P.Style.spaceMd * 2
                    radius: P.Style.radiusMd; color: P.Style.brandSoft
                    Column {
                        id: countdown
                        x: P.Style.spaceMd; y: P.Style.spaceMd
                        width: parent.width - P.Style.spaceMd * 2; spacing: P.Style.spaceSm
                        Text { text: "剩余保留时间"; color: P.Style.muted; font.pixelSize: P.Style.fontSm }
                        Text {
                            objectName: "reservationCountdownLabel"
                            text: page.countdownText; font.pixelSize: 30; font.bold: true
                            color: page.remainingSecs < 300 ? P.Style.danger : P.Style.brandDeep
                        }
                        Text {
                            width: parent.width; wrapMode: Text.WordWrap
                            text: "截止 " + page.hhmm(page.rec.expiresAtUtc) + "（北京时间）"
                            color: P.Style.muted; font.pixelSize: P.Style.fontSm
                        }
                    }
                }
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: "预约不扣费。到站后开始充电；如计划有变，可在开始充电前取消，立即释放电桩。"
                    color: P.Style.muted; font.pixelSize: P.Style.fontSm
                }
                P.ActionButton {
                    objectName: "reservationStartChargingButton"
                    width: parent.width; text: page.starting ? "正在启动…" : "开始充电"
                    enabled: !page.starting && !page.cancelBusy && page.remainingSecs > 0
                    onClicked: page.start()
                }
                P.ActionButton {
                    objectName: "reservationOrderCancelButton"
                    width: parent.width; variant: "logout"
                    text: page.cancelBusy ? "正在取消…" : "取消预约"
                    enabled: !page.cancelBusy && !page.starting && page.hasActive
                    onClicked: page.requestCancel()
                }
                Text {
                    objectName: "reservationCancelNote"
                    visible: page.cancelNote.length > 0
                    width: parent.width; wrapMode: Text.WordWrap
                    text: page.cancelNote; color: P.Style.danger; font.pixelSize: P.Style.fontSm
                }
            }
        }
    }
    Popup {
        id: cancelDialog
        objectName: "reservationCancelDialog"
        anchors.centerIn: parent
        width: Math.min(360, page.width - 24)
        padding: P.Style.spaceLg; modal: true
        background: Rectangle { color: P.Style.surface; radius: P.Style.radiusLg; border.color: P.Style.line }
        Column {
            width: parent.width; spacing: P.Style.spaceMd
            Text { text: "确认取消预约？"; font.bold: true; font.pixelSize: P.Style.fontLg; color: P.Style.ink }
            Text {
                width: parent.width; wrapMode: Text.WordWrap
                text: "取消后电桩将释放给其他用户，本次预约不扣费。已开始充电的订单不能通过取消预约结束。"
                color: P.Style.muted; font.pixelSize: P.Style.fontMd
            }
            P.ActionButton {
                objectName: "reservationCancelConfirmButton"
                width: parent.width; variant: "danger"; text: "确认取消预约"
                onClicked: { cancelDialog.close(); page.cancel() }
            }
            P.ActionButton {
                objectName: "reservationCancelKeepButton"
                width: parent.width; variant: "secondary"; text: "保留预约"
                onClicked: cancelDialog.close()
            }
        }
    }
}
