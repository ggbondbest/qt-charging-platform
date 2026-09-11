import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// QML twin of widgets StationDetailPage (objectName "stationDetailPage" kept).
// arg = 列表页点击卡片带来的 map：{id, name, address, priceCentsPerKwh, distanceMeters, status}。
// 进页 fetchDetailById（struct 参数版 QML 过不去 → 新桥方法，TODO(contract)）；
// 桩卡彩签/故障红框/预约三重准入均按 widgets 同语义直译。
// route "station_detail"：找站首页/收藏页卡片 App.navigate 带 arg 进来（深链可达）。
// 数据流：arg 缓存先撑首帧秒开 → fetchDetailById 拉真值增量合并（id 护栏丢迟到
// 回包）；预约链路 = 本页三道门 → App.checkBeforeReservation（桥预检）→ 确认页。
// 准入沿革：2026-09-08 业务变更撤"车辆强制/名额=车辆数/每车唯一"旧三重门并加
// chargingBusyPrompt 充电中拦截；该拦截后被 e8546fa 撤下，现口径 = 页面态门 +
// 空闲桩 + 登录 + 桥预检（见 requestReserve）。属 station 域 P0 六页批。
Item {
    id: page
    objectName: "stationDetailPage"
    property string route: "station_detail"
    property var arg: ({})
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    // station 是"arg 缓存 ∪ detail 真值"的合并视图：打开即有名址价格，不等网络。
    property var station: page.arg || ({})
    // 有无名称决定两态分工：有→头卡+桩区内联两态；无→整页 NoticePanel 兜底。
    readonly property bool hasHeader: !!(page.station && page.station.name)
    property bool detailLoading: false
    property bool detailLoaded: false
    property bool detailFailed: false
    property string failMessage: ""
    property var chargers: []
    Connections {
        target: App && App.workflowService ? App.workflowService : null
        function onChanged() { if (page.visible && !page.detailLoading) page.fetch() }
    }

    function money(c) { return (c / 100).toFixed(2) }
    // -1/undefined=未定位：显示"--"而非 0.0km，不骗用户"就在旁边"。
    function distText(m) { return (m === undefined || m < 0) ? "--" : (m / 1000).toFixed(1) + "km" }
    function isActive() { return String(station.status).toLowerCase() === "active" }
    // 空闲数从 chargers 现算：真值每次刷新列表即跟着变，不另查服务。
    function availableCount() {
        let n = 0
        for (const c of chargers) if (String(c.status).toLowerCase() === "available") ++n
        return n
    }
    // 桩状态 → 文案/彩签 tone（= widgets statusView 逐字）
    // 表驱动两函数配套（文案+色调同源），未知状态兜"未知/neutral"：服务扩枚举也不会出空签。
    function statusText(st) {
        return ({ available: "空闲", charging: "占用·充电中", reserved: "占用·已预约",
                  fault: "故障", offline: "离线" })[String(st).toLowerCase()] || "未知"
    }
    function statusTone(st) {
        return ({ available: "success", charging: "warning", reserved: "info",
                  fault: "danger", offline: "neutral" })[String(st).toLowerCase()] || "neutral"
    }

    function fetch() {
        if (!stationQueryService || !station.id) {
            detailLoading = false; detailLoaded = false; detailFailed = true
            failMessage = "缺少有效电站信息，请返回首页重新选择"
            return
        }
        detailLoading = true; detailFailed = false
        // TODO(contract): struct 参数 fetchDetail(Station,int) QML 不可达，
        //                桥补 fetchDetailById(int stationId, int distanceMeters)。
        try { stationQueryService.fetchDetailById(station.id, station.distanceMeters === undefined ? -1 : station.distanceMeters) }
        catch (e) {
            detailLoading = false; detailLoaded = false; detailFailed = true
            failMessage = "站点详情服务不可用，请重新登录后重试"
            chargers = []
        }
    }
    Connections {
        target: stationQueryService
        function onDetailStarted() { page.detailLoading = true; page.detailFailed = false }
        function onDetailSucceeded(detail) {
            const incoming = detail && detail.station ? detail.station : detail
            if (!incoming || String(incoming.id) !== String(page.station.id)) return
            page.detailLoading = false; page.detailLoaded = true; page.detailFailed = false
            // 桥 map 形状：{station…, distanceMeters, chargers[], hasChargerData}；
            // 兼容 station 子对象与拍平两种形状（TODO(contract) 成员3 定形）。
            const src = (detail && detail.station) ? detail.station : (detail || {})
            // 增量合并而非整页替换：真值覆盖同名缓存字段，服务没回传的字段保留，
            // 首帧已渲染的内容不闪回空白。
            page.station = Object.assign({}, page.station, src,
                { distanceMeters: (detail && detail.distanceMeters !== undefined)
                                     ? detail.distanceMeters : page.station.distanceMeters })
            // 桩表整体重赋值：var 属性靠"换引用"触发 ListView/汇总行重算。
            page.chargers = (detail && detail.chargers) || []
        }
        function onDetailFailed(message) {
            page.detailLoading = false; page.detailLoaded = false; page.detailFailed = true
            page.failMessage = message
        }
    }
    // 进页即拉真值：arg 缓存只撑首帧，桩位/价格以这次 fetchDetailById 为准。
    Component.onCompleted: fetch()

    function requestReserve(charger) {
        if (!isActive() || detailLoading || detailFailed || (App && App.checkingUnfinishedOrder)) return
        // 门①：按钮 enabled 是第一道，函数里再判是第二道（回车等旁路也拦得住）。
        if (String(charger.status).toLowerCase() !== "available") {
            if (App) App.showToast("仅空闲充电桩可预约", "warning")
            return
        }
        // 门②：未登录 toast + 推登录页；登录成功回本页重新点预约即可。
        if (!(App && App.loggedIn)) {
            if (App) { App.showToast("请先登录再发起预约", "warning"); App.navigate("login") }
            return
        }
        if (App) App.checkBeforeReservation({
            stationId: String(page.station.id), stationName: page.station.name,
            priceCentsPerKwh: page.station.priceCentsPerKwh,
            distanceMeters: page.station.distanceMeters,
            stationLatitude: page.station.latitude, stationLongitude: page.station.longitude,
            hasStationLocation: typeof page.station.latitude === "number" && typeof page.station.longitude === "number",
            chargerId: String(charger.id), chargerCode: charger.code,
            chargerType: charger.type, chargerPowerWatts: charger.powerWatts })
    }

    ListView {
        id: chargerList
        objectName: "chargerList"
        model: page.chargers
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd
        header: Column {
        width: chargerList.width
        spacing: P.Style.spaceMd
        // Station information scrolls with chargers on compact windows.

        // 页面标题（widgets titleLabel "站点详情" 同位）
        Text {
            objectName: "detailTitle"; text: "站点详情"
            font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink
        }

        // 站点信息头卡
        // 数据取合并视图 station：detail 还在飞/已失败也不遮列表页带来的名址。
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
                        objectName: "detailNameLabel"
                        width: parent.width - 80
                        text: station.name || "站点详情"
                        font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink
                        elide: Text.ElideRight
                    }
                    P.StatusTag {
                        objectName: "detailStatusTag"
                        anchors.verticalCenter: parent.verticalCenter
                        tone: isActive() ? "success" : "neutral"
                        text: isActive() ? "营业中" : "已离线"
                    }
                }
                // 行序对齐 widgets：名称+状态 → 地址 → 价格·距离（成员3 690b189）；
                // 价格/距离两锚点恢复 widgets 分离标签（detailPriceLabel/detailDistanceLabel）。
                Text {
                    objectName: "detailAddressLabel"
                    width: parent.width; wrapMode: Text.WordWrap
                    text: station.address || ""; font.pixelSize: P.Style.fontSm; color: P.Style.muted
                }
                Row {
                    width: parent.width
                    spacing: P.Style.spaceMd
                    Text {
                        objectName: "detailPriceLabel"
                        text: "¥" + money(station.priceCentsPerKwh || 0) + "/kWh"
                        font.pixelSize: P.Style.fontMd; color: P.Style.brandDeep
                    }
                    Text {
                        objectName: "detailDistanceLabel"
                        text: "距您 " + distText(station.distanceMeters)
                        font.pixelSize: P.Style.fontMd; color: P.Style.brandDeep
                    }
                }
            }
        }

        // 离线横幅（warningSoft 底）
        Rectangle {
            objectName: "detailOfflineBanner"
            visible: !isActive()
            width: parent.width
            height: offlineText.implicitHeight + 20
            radius: P.Style.radiusSm
            color: P.Style.warningSoft
            Text {
                id: offlineText
                anchors.centerIn: parent; width: parent.width - 24; wrapMode: Text.Wrap
                text: "该站点当前处于离线状态，暂不可用，请稍后再试或选择其他站点"
                font.pixelSize: P.Style.fontSm; color: P.Style.ink
            }
        }

        // 桩区两态（有头卡时内联，不遮站点信息）
        // hasHeader 才出内联面板：无头卡时整区让位给页底兜底，不叠两层"加载中"。
        P.NoticePanel {
            objectName: "detailLoadingLabel"
            visible: page.hasHeader && !page.detailLoaded && !page.detailFailed
            width: parent.width
            height: 120
            glyph: "clock"
            title: "正在加载充电桩列表…"
            description: "正在查询电站的实时桩位状态"
            actionText: ""
        }
        P.NoticePanel {
            objectName: "detailErrorNotice"
            visible: page.hasHeader && page.detailFailed
            width: parent.width
            height: 120
            glyph: "triangle-alert"
            glyphColor: P.Style.warning
            title: "充电桩列表加载失败"
            description: page.failMessage
            actionText: "重试"
            onActionTriggered: page.fetch()
        }

        Text {
            objectName: "detailChargerSummaryLabel"
            // 汇总行等真值才出：arg 缓存不知道桩数与空闲数，宁可不显示不瞎报。
            visible: page.detailLoaded
            width: parent.width
            wrapMode: Text.WordWrap      // NoWrap 长标注会溢出裁字
            text: (chargers.length > 0
                   ? "充电桩（空闲 " + availableCount() + " / 共 " + chargers.length + "）"
                   : "充电桩")

            font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink
        }

        Item { width: 1; height: P.Style.spaceSm }
        }
            delegate: P.Card {
                objectName: "chargerCard"
                width: chargerList.width
                // 故障桩红框加粗：彩签只占右侧小块，边框是扫列表时一眼可辨的第二通道。
                border.color: String(modelData.status).toLowerCase() === "fault"
                               ? P.Style.danger : P.Style.line
                border.width: String(modelData.status).toLowerCase() === "fault" ? 2 : 1
                Column {
                    width: parent.width
                    spacing: P.Style.spaceXs
                    RowLayout {
                        width: parent.width
                        Text {
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            elide: Text.ElideRight
                            text: modelData.code
                            font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink
                        }
                        P.StatusTag {
                            objectName: "chargerStatusTag"
                            tone: page.statusTone(modelData.status)
                            text: modelData.maintenance ? "维护中" : page.statusText(modelData.status)
                        }
                    }
                    RowLayout {
                        width: parent.width
                        spacing: P.Style.spaceMd
                        Text {
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            text: (String(modelData.type).toLowerCase() === "fast"
                                   ? "直流快充 " : "交流慢充 ")
                                  + Math.round((modelData.powerWatts || 0) / 1000) + " kW"
                            font.pixelSize: P.Style.fontSm; color: P.Style.muted
                        }
                        P.ActionButton {
                            objectName: "detailReserveButton"
                            variant: "primary"
                            text: "预约"
                            enabled: page.isActive() && !page.detailLoading && !page.detailFailed
                                     && !(App && App.checkingUnfinishedOrder)
                                     && String(modelData.status).toLowerCase() === "available"
                            // 点击只走 requestReserve 单点：按钮 enabled 与函数门禁同一
                            // 判据，两处逻辑不漂移（旁路触发也有第二道闸）。
                            onClicked: page.requestReserve(modelData)
                        }
                    }
                    RowLayout {
                        width: parent.width
                        P.ActionButton {
                            objectName: "detailQueueButton"
                            Layout.fillWidth: true
                            variant: "secondary"
                            text: "加入排队"
                            visible: page.availableCount() === 0
                            enabled: page.isActive() && !page.detailLoading && !modelData.maintenance
                                     && (String(modelData.status).toLowerCase() === "charging"
                                         || String(modelData.status).toLowerCase() === "reserved")
                            onClicked: App.navigate("queue", {chargerId: String(modelData.id),
                                chargerCode: modelData.code, stationId: String(page.station.id), stationName: page.station.name})
                        }
                        P.ActionButton {
                            objectName: "detailReportButton"
                            Layout.fillWidth: true
                            variant: "secondary"
                            text: "报障"
                            onClicked: App.navigate("fault_reports", {chargerId: String(modelData.id),
                                chargerCode: modelData.code, stationName: page.station.name})
                        }
                    }
                }
            }

        // 站点正常但无桩
        // 空桩态挂 footer 而非整页遮罩：站信息仍可读，"站真、桩未录入"自明。
        footer: P.NoticePanel {
            objectName: "detailChargerEmptyNotice"
            visible: page.detailLoaded && chargers.length === 0
            width: parent.width
            height: 140
            glyph: "plug"
            title: "该站点暂无充电桩"
            description: "站点信息已展示；桩位尚未录入，暂无法预约或充电。"
            actionText: ""
        }
    }

    // 整页两态：仅当 arg 无头卡信息（深链直达）时才遮全页
    P.NoticePanel {
        objectName: "detailNotice"
        anchors.fill: parent
        visible: !page.hasHeader
        glyph: page.detailFailed ? "triangle-alert" : "clock"
        glyphColor: page.detailFailed ? P.Style.warning : P.Style.ink
        title: page.detailFailed ? "站点详情加载失败" : "正在加载站点详情…"
        description: page.detailFailed ? page.failMessage : ""
        actionText: page.detailFailed ? "返回首页" : ""
        // 无头卡即无 id，重试无从发起——唯一出路是回首页重选站点。
        onActionTriggered: { if (App) App.back() }
    }
    // 遮罩只盖"arg 都没带全"的窗口期：有头卡时刷新桩位不遮整页、不清用户视野。
    P.LoadingOverlay { running: page.detailLoading && !page.detailLoaded }
}
