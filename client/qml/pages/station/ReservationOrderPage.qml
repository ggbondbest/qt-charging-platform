import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// —— 本人行·文件级说明（QML 迁移 P0 station 域批骨架；"预约即时生效"改造保留）：
// 进行中预约页，无独立 route——ReservationModulePage Tab0 经 Loader 嵌入的哑视图；
// record/loading/parentFailed 全由父页 Qt.binding 注入（列表信号在父页收敛防双监听，本页只直订 cancel/start）。
// The server owns cancellation/expiry. Never hide the reservation before ACK.
Item {
    id: page
    objectName: "reservationOrderPage"
    width: parent ? parent.width : 420
    height: parent ? parent.height : 500
    // 本人行：record 由父页注入；rec 是判空只读窗，后续绑定一律走 rec 防 null 解引用。
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
    // 本人行：lastExpiryRefresh = 到期复核节流基准（Timer 用它防请求风暴）。
    property double lastExpiryRefresh: 0

    function parseTs(value) {
        if (typeof value === "number") return value
        if (typeof value === "string" && /(Z|[+-]\d\d:\d\d)$/.test(value))
            return Date.parse(value)
        // 本人行（parseTs 尾）：非数值/无时区后缀 → NaN，上层统一落到"正在确认状态…"，不本地猜时区。
        return NaN
    }
    function clock(seconds) {
        const minutes = Math.floor(seconds / 60), remainder = seconds % 60
        return (minutes < 10 ? "0" : "") + minutes + ":" + (remainder < 10 ? "0" : "") + remainder
    // 本人行（clock 尾）：倒计时只做 mm:ss 补零，时/日不进秒级心跳的显示层。
    }
    function hhmm(value) {
        const milliseconds = parseTs(value)
        return isNaN(milliseconds) ? "--" : new Date(milliseconds + 8 * 3600000).toISOString().slice(11, 19)
    // 本人行（hhmm 尾）：+8h 后取 UTC 串切出 HH:MM:SS——北京时间靠显式偏移换算，跟设备时区无关。
    }
    readonly property int remainingSecs: {
        page.tick
        const expiry = parseTs(rec.expiresAtUtc)
        return !hasActive || isNaN(expiry) ? 0 : Math.max(0, Math.floor((expiry - Date.now()) / 1000))
    }
    readonly property string countdownText: remainingSecs > 0 ? clock(remainingSecs) : "正在确认状态…"
    // 本人行：心跳 Timer——每秒只递增 tick 驱动 remainingSecs 重估（QML 绑定不知道墙钟在走，必须人造依赖），顺带管到期复核。
    Timer {
        objectName: "countdownTimer"
        interval: 1000; repeat: true
        running: page.hasActive && !page.parentFailed
        onTriggered: {
            page.tick++
            if (page.remainingSecs === 0 && Date.now() - page.lastExpiryRefresh > 5000) {
                page.lastExpiryRefresh = Date.now()
                reservationService.fetchList()
            // 本人行（到期复核尾）：本地表走到 0 不算"已过期"——5 秒节流 fetchList 复核，服务端列表回执才算数（时钟漂移不许误杀）。
            }
        }
    }

    function requestCancel() {
        if (!hasActive || cancelBusy || starting) return
        cancelNote = ""
        cancelDialog.open()
    }
    // 本人行：取消走两段式（requestCancel 开确认框→cancel() 才真发请求）；闸=有效+不在途+不在启动，catch 兜桥异常。
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
    // 本人行（回执块尾）：三条取消回执都按 reservationId/在途态过滤——桥广播所有预约事件，本页只认自己这条在途操作。
    }
    Connections {
        target: chargingService
        function onStartCompleted(status) { page.starting = false }
        function onOperationFailed(type, code, message) {
            if (type !== "START_CHARGING" || !page.starting) return
            page.starting = false; page.cancelNote = message
        }
    }

    // 本人行：无有效预约的空态卡（取消/过期/已开始的单去 Tab1 历史看，本页只盯"当前活动"一条）。
    P.NoticePanel {
        objectName: "orderEmptyNotice"
        anchors.fill: parent
        visible: !page.hasActive && !page.loading
        glyph: "🅿️"; title: "暂无进行中的预约"
        description: "已取消、已过期或已开始充电的预约可在预约历史中查看。"
        actionText: "去找桩"
        // 本人行：空态出口=直接开新一轮找桩-预约；本页不放"刷新"按钮（列表拉取时机收敛在父页）。
        onActionTriggered: App.navigate("station")
    }
    P.LoadingOverlay { running: page.loading && !page.hasActive }
    // 有活动单时的滚动主区（与上方空态卡按 hasActive 互斥）：倒计时卡为唯一内容，
    // contentHeight 用卡片 implicitHeight 实量不写魔数。
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
                // 本人行（信息块尾）：站名/桩号直接吃快照透传字段，本页不再查站（预约上下文以提交时刻为准）。
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
                    // 本人行（倒计时块尾）：大字 <5 分钟转红给紧迫感，截止行只复述服务端 expiresAtUtc，两个数不各算各的。
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
                // 本人行（按钮块尾）：开始充电的闸含 remainingSecs>0——表走到 0 即禁用，但"真过期"仍以服务端复核回执为准（见 Timer）。
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
            // 本人行（对话框尾）："保留预约"零副作用只关弹窗——取消必须过确认框两段式，防误触释放电桩。
            }
        }
    }
}
