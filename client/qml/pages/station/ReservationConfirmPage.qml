// —— 本人行·文件级说明（QML 迁移 P0 station 域批骨架，"预约即时生效/不绑定车辆"业务改造保留）：
// 确认预约页 route="reservation_confirm"，arg=站点快照（StationDetailPage/ScanPage 拼好透传）；
// 入口先过 App.checkBeforeReservation 闸（navigate 直达同被此闸拦）：充电中/待支付/已预约
// 三类未完成订单各截去对应页，检查失败一律拒绝（fail-closed）——能进本页即可提交；
// submit 走 reservationService 真通道，页面状态全程回执驱动，本地不预设结果。
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
    // 本人行：confirm() 把快照按契约字段装配提交（distanceMeters 未知=-1 如实传，不伪造 0）；busy 重入挡 + 缺 chargerId 直返是入口闸后的双保险。
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
    // 本人行：提交回执四态全收——Started/Succeeded 管 busy，Succeeded 合并记录开弹窗，Failed/Rejected 落 failure 文本。
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
        // 本人行（块尾）：业务拒绝默认留在本页只落 failure 文案，唯有 UNFINISHED_ORDER 例外——直接救回在途订单页，不让人原地重试。
        }
    }
    Rectangle { anchors.fill: parent; color: P.Style.bg }
    Flickable {
        anchors.fill: parent; contentWidth: width
        contentHeight: content.implicitHeight + 2 * P.Style.spaceLg
        // 本人行：滚动列骨架——内容高用 implicitHeight 实量（无魔数），clip 防溢出，子块纵排在 Column。
        clip: true
        Column {
            id: content
            // 本人行：内容列原点=页边距，统一用 Style token。
            x: P.Style.spaceLg; y: P.Style.spaceLg
            width: parent.width - 2 * P.Style.spaceLg
            spacing: P.Style.spaceLg
            Text { text: "确认预约"; font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink }
            // 本人行：站点快照卡——名称/桩号/功率/电价全部来自 arg 透传字段，本页不再查服务（所见即所订）。
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
            // 本人行（规则文案尾）：15 分钟保留、截止时间以服务端回执为准、暂不支持未来时段——三条口径全部对齐服务端即时预约通道，页面无一处在本地改口。
            }
            Text {
                width: parent.width; wrapMode: Text.WordWrap
                text: "预约不扣费。实际费用按照服务端电价快照、充电量和停止时的账单计算。"
                color: P.Style.muted; font.pixelSize: P.Style.fontSm
            // 本人行（免扣费文案尾）：先破"预约=预付"误解，费用口径三要素（电价快照/电量/账单）全在服务端。
            }
            Text { width: parent.width; wrapMode: Text.WordWrap; visible: page.failure.length > 0; text: page.failure; color: P.Style.danger }
            P.ActionButton { objectName: "confirmReservationButton"; width: parent.width; text: page.busy ? "正在预约…" : "确认立即预约"; enabled: !page.busy && !!page.station.chargerId; onClicked: page.confirm() }
            P.ActionButton { width: parent.width; variant: "secondary"; text: "返回"; enabled: !page.busy; onClicked: App.back() }
        }
    }
    // 本人行：成功模态框 NoAutoClose（要么去充电要么离开，不允许"顺手关掉当没事发生"），截止时间文案用服务端回执字段本地换算。
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
            // 本人行：截止提醒只呈现 beijingTime(expiresAtUtc)——时间权威在服务端，缺字段显示 "--" 不本地臆算（行尾即此块）。
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
