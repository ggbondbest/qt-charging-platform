import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets ReservationOrderPage (objectName "reservationOrderPage").
// 本页是 ReservationModulePage 的 Tab0 子视图：records 由母页统一拉取后以
// `record` 属性喂入（哑视图，不再自订列表信号，只发 cancel/expire 动作）。
// 三栏：📍距离 ｜ ⏱预约倒计时（>30min 绿 / 5~30min 黄 / <5min 红 / 归零流转）｜ 🔋SOC 占位。
Item {
    id: page
    objectName: "reservationOrderPage"
    width: parent ? parent.width : 420
    height: parent ? parent.height : 500

    property var record: null            // 母页 activeRecords[0]（map）
    readonly property var rec: (record === null || record === undefined) ? ({}) : record
    property bool loading: false
    property bool parentFailed: false

    readonly property bool hasActive: record !== null && record !== undefined
    readonly property int greenThreshold: 30 * 60
    readonly property int yellowFloor: 5 * 60

    // 桥 map 时间字段形状未定（TODO(contract)：epoch 毫秒或 ISO 串双收）。
    function parseTs(v) {
        if (typeof v === "number") return v
        if (typeof v === "string" && v.length > 0) { const t = Date.parse(v); return isNaN(t) ? NaN : t }
        return NaN
    }
    function clock(secs) {
        const m = Math.floor(secs / 60), s = secs % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }
    function hhmm(v) {
        const t = parseTs(v)
        if (isNaN(t)) return "--"
        const d = new Date(t)
        return (d.getHours() < 10 ? "0" : "") + d.getHours() + ":"
             + (d.getMinutes() < 10 ? "0" : "") + d.getMinutes()
    }
    function money(cents) { return ((cents || 0) / 100).toFixed(2) }

    // 每秒 tick（QML 无墙钟绑定，用 Timer 推 tick 强制求值）
    property int tick: 0
    Timer {
        objectName: "countdownTimer"
        running: page.hasActive && !page.parentFailed
        interval: 1000; repeat: true
        onTriggered: {
            page.tick++
            const r = page.record
            if (!r) return
            const nowMs = Date.now()
            const remaining = Math.floor((page.parseTs(r.expiresAtUtc) - nowMs) / 1000)
            if (remaining <= 0) {
                // 归零：预约中→已过期 流转（服务端通道由后端负责，本地同步收敛展示）。
                try { reservationService.expireReservation(r.reservationId !== undefined ? r.reservationId : r.id) }
                catch (e) { /* 桥未就绪：tick 继续，母页重拉时收敛 */ }
            } else {
                // 迟到扫描（任务 #17 二迭代）：每秒驱动全库 开始+15min 未到站 自动取消。
                try { reservationService.cancelLateReservations() } catch (e) {}
            }
        }
    }

    readonly property int remainingSecs: {
        page.tick   // 依赖声明：让本绑定每秒重算
        if (!hasActive) return 0
        const t = parseTs(record.expiresAtUtc)
        return isNaN(t) ? 0 : Math.max(0, Math.floor((t - Date.now()) / 1000))
    }
    readonly property bool waitingPhase: {
        page.tick
        if (!hasActive) return false
        const st = parseTs(record.startAtUtc)
        return !isNaN(st) && st > Date.now()
    }
    readonly property string countdownText: !hasActive ? "--"
        : remainingSecs <= 0 ? "00:00"
        : waitingPhase ? "距开始 " + clock(remainingSecs) : clock(remainingSecs)
    readonly property color countdownColor: remainingSecs > greenThreshold ? P.Style.brand
                                            : remainingSecs >= yellowFloor ? P.Style.warning
                                            : P.Style.danger

    function cancel() {
        if (!hasActive) return
        // TODO(contract): reservationService.cancel(id) 桥 invokable。
        try { reservationService.cancel(record.reservationId !== undefined ? record.reservationId : record.id) }
        catch (e) { if (App) App.showToast("取消桥未就绪", "warning") }
    }
    property string cancelNote: ""
    property bool cancelBusy: false
    Connections {
        target: reservationService
        function onCancelStarted(reservationId) { page.cancelBusy = true }
        function onCancelFailed(message) { page.cancelBusy = false; page.cancelNote = "⚠️ " + message }
        function onCancelSucceeded(reservationId) { page.cancelBusy = false }
    }

    // ---- 空态 ----
    P.NoticePanel {
        objectName: "orderEmptyNotice"
        anchors.fill: parent
        visible: !hasActive && !page.loading
        glyph: "🅿️"
        title: "暂无进行中的预约"
        description: "去站点详情页挑选空闲充电桩，发起新的预约。"
        actionText: "🔍 去找桩"
        onActionTriggered: { if (App) App.navigate("station") }
    }
    P.LoadingOverlay { running: page.loading && !hasActive }

    // ---- 三栏主视图 ----
    Row {
        id: cols
        anchors.fill: parent
        spacing: P.Style.spaceSm
        visible: hasActive

        P.Card {
            objectName: "distanceCard"
            width: (page.width - cols.spacing * 2) / 3
            height: parent.height
            Column {
                width: parent.width                // Card 内容进 Column 容器：anchors 被忽略且告警
                spacing: P.Style.spaceSm
                Text { text: "📍 距离"; font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink }
                Text {
                    objectName: "distanceLabel"
                    text: (page.rec.distanceMeters === undefined || page.rec.distanceMeters < 0) ? "--"
                        : page.rec.distanceMeters >= 1000 ? "约 " + (page.rec.distanceMeters / 1000).toFixed(1) + " km"
                        : "约 " + page.rec.distanceMeters + " m"
                    font.pixelSize: P.Style.fontLg; color: P.Style.brandDeep
                }
                Text { text: "虚拟数据 · 导航功能后续对接"
                    font.pixelSize: P.Style.fontSm; color: P.Style.faint }
            }
        }

        P.Card {
            objectName: "countdownCard"
            width: (page.width - cols.spacing * 2) / 3
            height: parent.height
            Column {
                width: parent.width                // Card 内容进 Column 容器：anchors 被忽略且告警
                spacing: P.Style.spaceSm
                Text { text: "⏱ 预约倒计时"; font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink }
                Text {
                    objectName: "reservationCountdownLabel"
                    text: page.countdownText
                    font.pixelSize: P.Style.fontXl; font.bold: true
                    color: page.countdownColor
                }
                Text {
                    objectName: "reservationInfoLabel"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: (page.rec.stationName || "--") + " · " + (page.rec.chargerCode || "--") + "\n"
                          + (page.rec.chargerSpec || "充电桩") + " · " + (page.rec.durationMinutes || 0) + " 分钟 · 预估 ¥" + money(page.rec.estimatedFeeCents) + "\n"
                          + "车辆 " + (page.rec.vehiclePlate || "未关联") + " · 时段 "
                          + hhmm(page.rec.startAtUtc) + "—" + hhmm(page.rec.expiresAtUtc)
                    font.pixelSize: P.Style.fontSm; color: P.Style.muted
                }
                Item { width: 1; height: parent.height - 200 }
                P.ActionButton {
                    objectName: "reservationCancelButton"
                    variant: "danger"; text: "取消预约"
                    width: parent.width
                    enabled: !page.cancelBusy
                    onClicked: { page.cancelBusy = true; page.cancelNote = ""; page.cancel() }
                }
                Text {
                    visible: page.cancelNote.length > 0
                    text: page.cancelNote
                    font.pixelSize: P.Style.fontSm; color: P.Style.danger
                }
            }
        }

        P.Card {
            objectName: "batteryCard"
            width: (page.width - cols.spacing * 2) / 3
            height: parent.height
            Column {
                width: parent.width                // Card 内容进 Column 容器：anchors 被忽略且告警
                spacing: P.Style.spaceSm
                Text { text: "🔋 汽车电量"; font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink }
                Text { objectName: "batteryLabel"; text: "SOC --%"
                    font.pixelSize: P.Style.fontLg; color: P.Style.info }
                Text { text: "虚拟占位 · 电量对接功能暂不实现"
                    font.pixelSize: P.Style.fontSm; color: P.Style.faint }
            }
        }
    }
}
