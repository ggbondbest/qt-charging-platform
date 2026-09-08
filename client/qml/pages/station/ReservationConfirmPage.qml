import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

Item {
    id: page
    objectName: "reservationConfirmPage"
    property string route: "reservation_confirm"
    property var arg: ({})
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600
    property var station: page.arg || ({})
    property bool busy: false
    property var lastRecord: null
    property string failure: ""

    function beijingTime(iso) {
        if (typeof iso !== "string" || !/(Z|[+-]\d\d:\d\d)$/.test(iso)) return "--"
        const ms = Date.parse(iso)
        if (isNaN(ms)) return "--"
        return new Date(ms + 8 * 3600000).toISOString().replace("T", " ").slice(0, 19) + "（北京时间）"
    }
    function confirm() {
        if (busy || !station.chargerId) return
        busy = true; failure = ""
        reservationService.submit({
            stationId: String(station.stationId), chargerId: String(station.chargerId),
            stationName: station.stationName || "", chargerCode: station.chargerCode || "",
            chargerType: station.chargerType || "fast", chargerPowerWatts: station.chargerPowerWatts || 0,
            priceCentsPerKwh: station.priceCentsPerKwh || 0,
            stationLatitude: station.stationLatitude, stationLongitude: station.stationLongitude,
            hasStationLocation: station.hasStationLocation === true,
            distanceMeters: station.distanceMeters === undefined ? -1 : station.distanceMeters
        })
    }
    Connections {
        target: reservationService
        function onSubmitStarted(chargerId) { page.busy = true }
        function onSubmitSucceeded(record) {
            page.busy = false
            page.lastRecord = Object.assign({}, page.station, record)
            successDialog.open()
        }
        function onSubmitFailed(reason) { page.busy = false; page.failure = reason }
        function onSubmitRejected(code, details, message) {
            page.busy = false; page.failure = message
            if (details && details.reason === "UNFINISHED_ORDER")
                App.recoverUnfinishedOrder()
        }
    }
    Rectangle { anchors.fill: parent; color: P.Style.bg }
    Flickable {
        anchors.fill: parent; contentWidth: width
        contentHeight: content.implicitHeight + 2 * P.Style.spaceLg
        clip: true
        Column {
            id: content
            x: P.Style.spaceLg; y: P.Style.spaceLg
            width: parent.width - 2 * P.Style.spaceLg
            spacing: P.Style.spaceLg
            Text { text: "确认预约"; font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink }
            P.Card {
                width: parent.width
                Column {
                    width: parent.width; spacing: P.Style.spaceSm
                    Text { width: parent.width; wrapMode: Text.WordWrap; text: page.station.stationName || "充电站"; font.bold: true; font.pixelSize: P.Style.fontLg; color: P.Style.ink }
                    Text { text: "电桩：" + (page.station.chargerCode || "--"); color: P.Style.muted }
                    Text { text: "额定功率：" + ((page.station.chargerPowerWatts || 0) / 1000) + " kW"; color: P.Style.muted }
                    Text { text: "电价：¥" + ((page.station.priceCentsPerKwh || 0) / 100).toFixed(2) + "/kWh"; color: P.Style.brandDeep }
                }
            }
            Text {
                objectName: "reservationPolicyLabel"
                width: parent.width; wrapMode: Text.WordWrap
                text: "预约成功立即生效，电桩保留 15 分钟。请在服务端返回的截止时间前开始充电；超时由服务端释放。当前不支持未来时间段预约。"
                color: P.Style.muted; font.pixelSize: P.Style.fontMd
            }
            Text {
                width: parent.width; wrapMode: Text.WordWrap
                text: "预约不扣费。实际费用按照服务端电价快照、充电量和停止时的账单计算。"
                color: P.Style.muted; font.pixelSize: P.Style.fontSm
            }
            Text { width: parent.width; wrapMode: Text.WordWrap; visible: page.failure.length > 0; text: page.failure; color: P.Style.danger }
            P.ActionButton { objectName: "confirmReservationButton"; width: parent.width; text: page.busy ? "正在预约…" : "确认立即预约"; enabled: !page.busy && !!page.station.chargerId; onClicked: page.confirm() }
            P.ActionButton { width: parent.width; variant: "secondary"; text: "返回"; enabled: !page.busy; onClicked: App.back() }
        }
    }
    Popup {
        id: successDialog
        objectName: "reservationSuccessDialog"
        modal: true; closePolicy: Popup.NoAutoClose
        anchors.centerIn: parent
        width: Math.min(350, page.width - 32)
        padding: P.Style.spaceLg
        background: Rectangle { color: P.Style.surface; radius: P.Style.radiusLg; border.color: P.Style.line }
        Column {
            width: parent.width; spacing: P.Style.spaceMd
            Text { text: "预约成功"; font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
            Text {
                width: parent.width; wrapMode: Text.WordWrap
                text: "请在 " + (page.lastRecord ? page.beijingTime(page.lastRecord.expiresAtUtc) : "--") + " 前开始充电。"
                color: P.Style.muted
            }
            P.ActionButton { width: parent.width; text: "查看预约 / 开始充电"; onClicked: { successDialog.close(); App.navigate("charging") } }
            P.ActionButton { width: parent.width; variant: "secondary"; text: "导航前往电站"; onClicked: { successDialog.close(); App.navigate("navigation", page.lastRecord) } }
        }
    }
}
