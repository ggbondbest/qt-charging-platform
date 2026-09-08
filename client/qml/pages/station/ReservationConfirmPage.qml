import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "StationState.js" as StationState

// QML twin of widgets ReservationConfirmPage (objectName "reservationConfirmPage").
// arg = 详情页 map：{stationId, stationName, priceCentsPerKwh, distanceMeters,
//                    chargerId, chargerCode, chargerType, chargerPowerWatts}。
// QDateTimeEdit 无 QML 对应控件 → 起止各一条 15 分钟步进 Slider（0~1439 分钟位）。
// 约束/费用/人优先（userEdited 不被推荐覆盖）与 widgets 逐字对齐；
// 提交与车辆下拉按契约名盲写（TODO(contract) 见映射稿 §桥缺口）。
Item {
    id: page
    objectName: "reservationConfirmPage"
    property string route: "reservation_confirm"
    property var arg: ({})
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    readonly property int kMaxSlotMinutes: 45
    property var station: page.arg || ({})
    property int startMinutes: 0        // 当日分钟位
    property int endMinutes: 0
    property bool userEdited: false     // 任一 Slider 动过 → 推荐只更文案不覆盖值
    property bool busy: false
    property var lastRecord: null

    // 模拟推荐（服务桥缺位期页内同公式兜底：5+⌈米/500⌉ 分钟车程，
    // 起点对齐 15min 刻度，时长取规格上限 45）
    readonly property int travelMinutes: 5 + Math.ceil((station.distanceMeters || 0) / 500)
    property string recommendCaption: ""

    function hhmm(min) {
        const h = Math.floor(min / 60) % 24, m = min % 60
        return (h < 10 ? "0" : "") + h + ":" + (m < 10 ? "0" : "") + m
    }
    function slotMinutes() { return page.endMinutes - page.startMinutes }
    function feeCents() {
        return Math.round((station.priceCentsPerKwh || 0) * Math.max(0, slotMinutes()) / 60)
    }
    function computeRecommend() {
        // 与 recommendSlotFromTravelMinutes 同口径：now+车程（含秒）→ 向上对齐 15min 刻度。
        const now = new Date()
        const sod = now.getHours() * 3600 + now.getMinutes() * 60 + now.getSeconds()
                    + page.travelMinutes * 60
        const aligned = (Math.ceil(sod / 900) * 900) % 86400
        const start = Math.floor(aligned / 60)
        return { start: start, end: (start + page.kMaxSlotMinutes) % 1440 }
    }
    function applyRecommend() {
        const s = computeRecommend()
        page.startMinutes = s.start
        page.endMinutes = s.end
        page.userEdited = false
        page.recommendCaption = "✨ 推荐 " + hhmm(s.start) + "—" + hhmm(s.end)
                                 + " · 约 " + page.travelMinutes + " 分钟车程"
    }

    // ---- 车辆下拉（2026-09-08 业务变更：可选项，"（不绑定车辆）"为默认首项） ----
    property var vehicles: []
    property int vehicleIndex: -1     // -1 = 不绑定车辆
    function loadVehicles() {
        // TODO(contract): settingsService.vehicles() invokable（map 列表）。
        // 桥缺位期读 StationState（设置页添加的车辆跨页可见）。
        let v
        try { v = settingsService.vehicles() } catch (e) { v = undefined }
        page.vehicles = (v !== undefined) ? v : StationState.vehicles
        vehicleIndex = -1
        for (let i = 0; i < vehicles.length; ++i)
            if (vehicles[i].isDefault) { vehicleIndex = i; break }
    }

    // ---- 校验行（messageLabel 同语义绑定；车辆行已随业务变更移除） ----
    readonly property string validationMessage: busy ? "提交中…"
        : slotMinutes() <= 0 ? "⚠️ 结束时间必须晚于开始时间"
        : slotMinutes() > kMaxSlotMinutes ? "⚠️ 预约时间段不能超过 " + kMaxSlotMinutes + " 分钟，请缩短时段"
        : "模拟通道：预约即时生效；真实通道保留 15 分钟，暂不支持未来时段。"
    readonly property bool canSubmit: !busy && slotMinutes() > 0 && slotMinutes() <= kMaxSlotMinutes

    function confirm() {
        if (!canSubmit) return
        busy = true
        // TODO(contract): reservationService.submit(map) —— 载荷 {chargerId, stationId,
        //                startMinutesOfDay, endMinutesOfDay, vehicleId, vehiclePlate,
        //                distanceMeters}（今晚成员3 定形）。
        try {
            // vehicleIndex<0 = 不绑定车辆 → vehicleId 0 / 空牌（preview 通道已撤
            // settings 注入，finishMockSubmit 对 0 值无车辆门）。
            const v = vehicleIndex >= 0 ? vehicles[vehicleIndex] : null
            reservationService.submit({
                chargerId: station.chargerId, stationId: station.stationId,
                // 桩元数据透传给桥（mock 用 code/type/power 生成桩号与规格文案）
                chargerCode: station.chargerCode || "", chargerType: station.chargerType || "fast",
                chargerPowerWatts: station.chargerPowerWatts || 0,
                stationName: station.stationName || station.name || "",
                startMinutes: page.startMinutes, endMinutes: page.endMinutes,
                vehicleId: v ? v.id : 0, vehiclePlate: v ? v.plate : "",
                distanceMeters: station.distanceMeters || -1 })
        } catch (e) {
            busy = false
            if (App) App.showToast("预约服务未就绪，请稍后重试", "danger")
        }
    }

    Connections {
        target: reservationService
        function onSubmitStarted(chargerId) { page.busy = true }
        function onSubmitSucceeded(record) {
            page.busy = false
            page.lastRecord = record
            successDialog.openDialog(record)   // “是否现在前往充电？”（= HomeShell 弹层直译）
        }
        function onSubmitFailed(reason) {
            page.busy = false
            if (App) App.showToast(reason, "danger")
        }
    }

    Component.onCompleted: { loadVehicles(); applyRecommend() }

    // 整页可上下拖拽（用户二轮指定）：内容超视口即滚动，提交/关闭钮始终可达。
    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: confirmCol.height + 2 * P.Style.spaceLg
        clip: true

        Column {
            id: confirmCol
            x: P.Style.spaceLg
            y: P.Style.spaceLg
            width: parent.width - 2 * P.Style.spaceLg
            spacing: P.Style.spaceMd

        Text { objectName: "reservationConfirmTitle"; text: "预约确认"
            font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink }

        // 上下文信息卡
        P.Card {
            objectName: "confirmContextCard"
            width: parent.width
            Column {
                width: parent.width
                spacing: P.Style.spaceXs
                Repeater {
                    model: [
                        { k: "站点名称", v: station.stationName || station.name || "--" },
                        { k: "充电桩编号", v: station.chargerCode || "--" },
                        { k: "充电类型 / 功率",
                          v: (String(station.chargerType).toLowerCase() === "fast" ? "直流快充 " : "交流慢充 ")
                             + Math.round((station.chargerPowerWatts || 0) / 1000) + " kW" },
                        { k: "电价", v: "¥" + ((station.priceCentsPerKwh || 0) / 100).toFixed(2) + "/kWh" }
                    ]
                    Row {
                        width: parent.width
                        spacing: P.Style.spaceMd
                        Text { width: 100; text: modelData.k
                            font.pixelSize: P.Style.fontMd; color: P.Style.muted }
                        Text { width: parent.width - 100 - parent.spacing
                            text: modelData.v; font.pixelSize: P.Style.fontMd
                            color: P.Style.ink; elide: Text.ElideRight }
                    }
                }

                // 车辆下拉（可选：首项"（不绑定车辆）"，vehicleIndex=-1）
                Row {
                    width: parent.width
                    spacing: P.Style.spaceMd
                    Text { anchors.verticalCenter: parent.verticalCenter; width: 100
                        text: "预约车辆"; font.pixelSize: P.Style.fontMd; color: P.Style.muted }
                    ComboBox {
                        objectName: "reservationVehicleComboBox"
                        width: parent.width - 100 - parent.spacing
                        enabled: !page.busy
                        model: {
                            const out = ["（不绑定车辆）"]
                            for (const v of page.vehicles)
                                out.push(v.isDefault ? v.plate + "（默认）" : v.plate)
                            return out
                        }
                        currentIndex: page.vehicleIndex + 1
                        onActivated: idx => { page.vehicleIndex = idx - 1; page.userEdited = true }
                    }
                }
            }
        }

        // 时段选择（Slider 15min 步进；widgets QDateTimeEdit 的 QML 等价位）
        P.Card {
            objectName: "confirmSlotCard"
            width: parent.width
            Column {
                width: parent.width
                spacing: P.Style.spaceSm

                Text { text: "开始时间  " + page.hhmm(page.startMinutes)
                    font.pixelSize: P.Style.fontMd; color: P.Style.ink }
                Slider {
                    objectName: "startSlider"
                    width: parent.width
                    from: 0; to: 1440; stepSize: 15
                    value: page.startMinutes
                    enabled: !page.busy
                    onMoved: { page.startMinutes = value; page.userEdited = true }
                }
                Text { text: "结束时间  " + page.hhmm(page.endMinutes)
                    font.pixelSize: P.Style.fontMd; color: P.Style.ink }
                Slider {
                    objectName: "endSlider"
                    width: parent.width
                    from: 0; to: 1440; stepSize: 15
                    value: page.endMinutes
                    enabled: !page.busy
                    onMoved: { page.endMinutes = value; page.userEdited = true }
                }

                P.ActionButton {
                    objectName: "useRecommendedSlotButton"
                    variant: "secondary"
                    text: page.recommendCaption || "✨ 使用系统推荐时段"
                    enabled: !page.busy
                    onClicked: page.applyRecommend()   // 点击=用户主动意愿，覆盖现值并复位 userEdited
                }

                Text {
                    objectName: "reservationFeeLabel"
                    width: parent.width; wrapMode: Text.WordWrap
                    text: page.canSubmit || page.busy
                          ? "预估费用 ≈ ¥" + (page.feeCents() / 100).toFixed(2)
                            + "（¥" + ((station.priceCentsPerKwh || 0) / 100).toFixed(2)
                            + "/度 × " + Math.max(0, page.slotMinutes()) + " 分钟）"
                          : "预估费用 ≈ ¥--"
                    font.pixelSize: P.Style.fontMd; color: P.Style.brandDeep
                }
                Text {
                    objectName: "reservationMessageLabel"
                    width: parent.width; wrapMode: Text.WordWrap
                    text: page.validationMessage
                    font.pixelSize: P.Style.fontSm
                    color: page.validationMessage.charAt(0) === "⚠" ? P.Style.danger : P.Style.muted
                }
            }
        }

        Row {
            objectName: "confirmFooter"
            width: parent.width
            spacing: P.Style.spaceMd
            P.ActionButton {
                objectName: "reservationCloseButton"
                variant: "ghost"; text: "关闭"
                width: (parent.width - parent.spacing) / 2
                onClicked: { if (App) App.back() }
            }
            P.ActionButton {
                objectName: "reservationConfirmButton"
                variant: "primary"
                text: page.busy ? "提交中…" : "确认预约"
                width: (parent.width - parent.spacing) / 2
                enabled: page.canSubmit
                onClicked: page.confirm()
            }
        }
        }
    }

    // “是否现在前往充电？”——成功弹层（HomeShell 对话框直译进页内）
    Popup {
        id: successDialog
        objectName: "reservationSuccessDialog"
        modal: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: parent
        width: parent ? Math.min(320, parent.width - P.Style.spaceXl) : 320
        padding: P.Style.spaceLg
        function openDialog(record) { page.lastRecord = record; open() }
        background: Rectangle {
            radius: P.Style.radiusLg; color: P.Style.surface
            border.color: P.Style.line; border.width: 1
        }
        Column {
            width: parent.width
            spacing: P.Style.spaceMd
            Text {
                width: parent.width; wrapMode: Text.WordWrap
                text: "🎉 预约成功！是否现在前往充电站？"
                font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink
            }
            Row {
                width: parent.width
                spacing: P.Style.spaceSm
                P.ActionButton {
                    objectName: "goChargeButton"
                    variant: "primary"; text: "现在前往"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: {
                        successDialog.close()
                        if (App) App.navigate("navigation", page.lastRecord)
                    }
                }
                P.ActionButton {
                    objectName: "goOrderButton"
                    variant: "secondary"; text: "查看预约订单"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: {
                        successDialog.close()
                        if (App) App.navigate("reservation_module")
                    }
                }
            }
        }
    }

    P.LoadingOverlay { running: page.busy }
}
