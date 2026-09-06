import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets StationDetailPage (objectName "stationDetailPage" kept).
// arg = 列表页点击卡片带来的 map：{id, name, address, priceCentsPerKwh, distanceMeters, status}。
// 进页 fetchDetailById（struct 参数版 QML 过不去 → 新桥方法，TODO(contract)）；
// 桩卡彩签/故障红框/预约三重准入均按 widgets 同语义直译。
Item {
    id: page
    objectName: "stationDetailPage"
    property string route: "station_detail"
    property var arg: ({})
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property var station: page.arg || ({})
    property bool detailLoading: false
    property bool detailLoaded: false
    property bool detailFailed: false
    property string failMessage: ""
    property var chargers: []

    function money(c) { return (c / 100).toFixed(2) }
    function distText(m) { return (m === undefined || m < 0) ? "--" : (m / 1000).toFixed(1) + "km" }
    function isActive() { return String(station.status).toLowerCase() === "active" }
    function availableCount() {
        let n = 0
        for (const c of chargers) if (String(c.status).toLowerCase() === "available") ++n
        return n
    }
    // 桩状态 → 文案/彩签 tone（= widgets statusView 逐字）
    function statusText(st) {
        return ({ available: "空闲", charging: "占用·充电中", reserved: "占用·已预约",
                  fault: "故障", offline: "离线" })[String(st).toLowerCase()] || "未知"
    }
    function statusTone(st) {
        return ({ available: "success", charging: "warning", reserved: "info",
                  fault: "danger", offline: "neutral" })[String(st).toLowerCase()] || "neutral"
    }

    function fetch() {
        if (!stationQueryService || !station.id) return
        detailLoading = true; detailFailed = false
        // TODO(contract): struct 参数 fetchDetail(Station,int) QML 不可达，
        //                桥补 fetchDetailById(int stationId, int distanceMeters)。
        try { stationQueryService.fetchDetailById(station.id, station.distanceMeters || -1) }
        catch (e) { detailLoading = false; detailFailed = true; failMessage = "详情桥未就绪" }
    }

    Connections {
        target: stationQueryService
        function onDetailStarted() { page.detailLoading = true; page.detailFailed = false }
        function onDetailSucceeded(detail) {
            page.detailLoading = false; page.detailLoaded = true; page.detailFailed = false
            // 桥 map 形状：{station…, distanceMeters, chargers[], hasChargerData}；
            // distance 挂在 detail 层，并入 station 供头卡/导航参数使用。
            if (detail && detail.station)
                page.station = Object.assign({}, detail.station,
                    { distanceMeters: detail.distanceMeters !== undefined
                                         ? detail.distanceMeters : page.station.distanceMeters })
            page.chargers = (detail && detail.chargers) || []
        }
        function onDetailFailed(message) {
            page.detailLoading = false; page.detailLoaded = false; page.detailFailed = true
            page.failMessage = message
        }
    }
    Component.onCompleted: fetch()

    // ---- 预约三重准入（widgets handleReserveRequested 同序）----
    function requestReserve(charger) {
        if (String(charger.status).toLowerCase() !== "available") {
            if (App) App.showToast("仅空闲充电桩可预约", "warning")
            return
        }
        if (!(App && App.loggedIn)) {
            if (App) { App.showToast("请先登录再发起预约", "warning"); App.navigate("login") }
            return
        }
        // 车辆数 / 在途名额两项依赖未补的桥 invokable（TODO(contract):
        // settingsService.vehicleCount()、reservationService.activeReservationCount()
        // /unfinishedSlotLimit()）。桥缺位期放行，服务端提交仍会二次校验。
        if (App) App.navigate("reservation_confirm", {
            stationId: page.station.id, stationName: page.station.name,
            priceCentsPerKwh: page.station.priceCentsPerKwh,
            distanceMeters: page.station.distanceMeters,
            chargerId: charger.id, chargerCode: charger.code,
            chargerType: charger.type, chargerPowerWatts: charger.powerWatts })
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd
        visible: page.detailLoaded

        // 站点信息头卡
        P.Card {
            objectName: "detailHeaderCard"
            width: parent.width
            Column {
                width: parent.width
                spacing: P.Style.spaceXs
                Row {
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Text {
                        width: parent.width - 80
                        text: station.name || "站点详情"
                        font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink
                        elide: Text.ElideRight
                    }
                    P.StatusTag {
                        anchors.verticalCenter: parent.verticalCenter
                        tone: isActive() ? "success" : "neutral"
                        text: isActive() ? "营业中" : "已离线"
                    }
                }
                Text {
                    text: "距您 " + distText(station.distanceMeters)
                          + " · ¥" + money(station.priceCentsPerKwh || 0) + "/kWh"
                    font.pixelSize: P.Style.fontMd; color: P.Style.brandDeep
                }
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: station.address || ""; font.pixelSize: P.Style.fontSm; color: P.Style.muted
                }
            }
        }

        // 离线横幅（warningSoft 底）
        Rectangle {
            objectName: "offlineBanner"
            visible: !isActive()
            width: parent.width
            height: 34
            radius: P.Style.radiusSm
            color: P.Style.warningSoft
            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left; anchors.leftMargin: P.Style.spaceMd
                text: "⚠️ 该站点当前处于离线状态，暂不可用，请稍后再试或选择其他站点"
                font.pixelSize: P.Style.fontSm; color: P.Style.ink
            }
        }

        Text {
            objectName: "chargerSummaryLabel"
            text: chargers.length > 0
                  ? "充电桩（空闲 " + availableCount() + " / 共 " + chargers.length + "）"
                  : "充电桩"
            font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink
        }

        // 桩列表（故障红框 = 原属性选择器的绑定化）
        ListView {
            objectName: "chargerList"
            width: parent.width
            height: parent.height - y
            clip: true
            spacing: P.Style.spaceSm
            model: page.chargers
            visible: chargers.length > 0
            delegate: P.Card {
                objectName: "chargerCard"
                width: chargerList.width
                height: 96
                border.color: String(modelData.status).toLowerCase() === "fault"
                               ? P.Style.danger : P.Style.line
                border.width: String(modelData.status).toLowerCase() === "fault" ? 2 : 1
                Column {
                    width: parent.width
                    spacing: P.Style.spaceXs
                    Row {
                        width: parent.width
                        Text {
                            width: parent.width - 96
                            text: modelData.code
                            font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink
                        }
                        P.StatusTag {
                            objectName: "chargerStatusTag"
                            anchors.verticalCenter: parent.verticalCenter
                            tone: page.statusTone(modelData.status)
                            text: page.statusText(modelData.status)
                        }
                    }
                    Row {
                        width: parent.width
                        spacing: P.Style.spaceMd
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: (String(modelData.type).toLowerCase() === "fast"
                                   ? "直流快充 " : "交流慢充 ")
                                  + Math.round((modelData.powerWatts || 0) / 1000) + " kW"
                            font.pixelSize: P.Style.fontSm; color: P.Style.muted
                        }
                        Item { width: parent.width - 200; height: 1 }
                        P.ActionButton {
                            objectName: "detailReserveButton"
                            anchors.verticalCenter: parent.verticalCenter
                            variant: "primary"
                            text: "预约"
                            enabled: String(modelData.status).toLowerCase() === "available"
                            onClicked: page.requestReserve(modelData)
                        }
                    }
                }
            }
        }

        // 站点正常但无桩
        P.NoticePanel {
            objectName: "chargerEmptyNotice"
            visible: chargers.length === 0
            width: parent.width
            height: 140
            glyph: "🔌"
            title: "该站点暂无充电桩"
            description: "站点信息已展示；桩位尚未录入，暂无法预约或充电。"
            actionText: ""
        }
    }

    // 加载/失败两态（状态门，缺陷4 口径）
    P.NoticePanel {
        objectName: "detailNotice"
        anchors.fill: parent
        visible: !page.detailLoaded
        glyph: page.detailFailed ? "⚠️" : "⏳"
        title: page.detailFailed ? "站点详情加载失败" : "正在加载站点详情…"
        description: page.detailFailed ? page.failMessage : ""
        actionText: page.detailFailed ? "返回首页" : ""
        onActionTriggered: { if (App) App.back() }
    }
    P.LoadingOverlay { running: page.detailLoading && !page.detailLoaded }
}
