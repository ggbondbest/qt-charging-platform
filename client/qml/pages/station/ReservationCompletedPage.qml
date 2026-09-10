import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets ReservationCompletedPage (objectName "reservationCompletedPage").
// ReservationModulePage 的 Tab1 子视图：records 由母页喂入；点击卡片弹详情（全字段）。
// 本页无 route 属性：不注册 Shell 路由，只被母页 Loader 换 source 装载。
// 纯哑视图：不碰桥、不挂 Connections——查询/取消/过期信号全由母页收敛后灌数据，
// 子页若各自接线就会双份响应（母页头注定向口径）。属 P1 迁移批。
Item {
    id: page
    objectName: "reservationCompletedPage"
    width: parent ? parent.width : 420
    height: parent ? parent.height : 500

    // 母页 Qt.binding 喂入的活绑定输入口；detailRecord 仅是本地弹层选择态。
    property var records: []
    property bool loading: false
    property var detailRecord: null

    // UTC ISO → 北京时间 hh:mm:ss：整点 +8h 偏移后取 ISO 时间片段。
    // 关键判据在正则：串尾没有 Z/±hh:mm 时区后缀就当"时区不明"一律 "--"——
    // 猜时区会把保留窗口算错一小时，宁可显示不出来了断错。
    function hhmm(v) {
        if (typeof v === "string" && /(Z|[+-]\d\d:\d\d)$/.test(v)) {
            const ms = Date.parse(v)
            if (!isNaN(ms)) return new Date(ms + 8 * 3600000).toISOString().slice(11, 19)
        }
        return "--"
    }
    // 分→元换算工具：本页费用口径是"预约不扣费"固定文案（见弹层费用行），暂无调用点。
    function money(cents) { return ((cents || 0) / 100).toFixed(2) }
    // 归档状态文案/彩签（widgets completedStatusText 口径）
    function doneText(r) {
        const st = String(r.status).toLowerCase()
        if (st === "fulfilled") return "已完成"
        if (st === "cancelled") return r.lateCancelled ? "已取消·迟到" : "已取消"
        if (st === "expired") return "已过期"
        return "已结束"
    }
    function doneTone(r) {
        const st = String(r.status).toLowerCase()
        return ({ fulfilled: "success", cancelled: "warning", expired: "neutral" })[st] || "neutral"
    }

    P.NoticePanel {
        objectName: "completedEmptyNotice"
        anchors.fill: parent
        // 加载中不算空：等母页回执落地前不闪"暂无历史预约"吓用户。
        visible: records.length === 0 && !loading
        glyph: "📒"
        title: "暂无历史预约"
        description: "结束（完成 / 取消 / 过期）的预约会归档到这里，点击卡片可查看详情。"
        actionText: ""
    }

    ListView {
        objectName: "completedList"
        anchors.fill: parent
        spacing: P.Style.spaceSm
        clip: true
        visible: records.length > 0
        model: page.records
        delegate: P.ClickableCard {
            objectName: "completedCard"
            width: parent.width
            // 选中态只记本地 detailRecord：整条记录直接喂弹层，按 id 回查是多余的服务往返。
            onClicked: { page.detailRecord = modelData; detailPopup.open() }
            Row {
                width: parent.width            // Column 内容器：anchors.fill 被忽略且告警
                spacing: P.Style.spaceMd
                Column {
                    width: parent.width - 90
                    spacing: 2
                    Text {
                        width: parent.width; elide: Text.ElideRight
                        text: (modelData.stationName || "--") + " " + (modelData.chargerCode || "")
                        font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink
                    }
                    // 两条副行只复述服务端字段、页内零推算：桩行尾缀"服务端预约记录"
                    // 明示归档数据出处；保留窗口时间走上方 hhmm——时区不明即 "--"。
                    Text {
                        width: parent.width; elide: Text.ElideRight
                        text: "充电桩 " + (modelData.chargerCode || "--")
                              + " · " + (modelData.chargerSpec || "充电桩")
                              + " · 服务端预约记录"
                        font.pixelSize: P.Style.fontSm; color: P.Style.muted
                    }
                    Text {
                        text: "保留时间 " + page.hhmm(modelData.startAtUtc)
                              + "—" + page.hhmm(modelData.expiresAtUtc)
                              + "（北京时间）"
                        font.pixelSize: P.Style.fontSm; color: P.Style.faint
                    }
                    Text {
                        // widgets hintLabel 同文案（整卡可点→弹层，hint 提示入口）
                        width: parent.width; horizontalAlignment: Text.AlignRight
                        text: "点击查看预约详情 ›"
                        font.pixelSize: P.Style.fontSm; color: P.Style.faint
                    }
                }
                P.StatusTag {
                    objectName: "historyStatusTag"
                    anchors.verticalCenter: parent.verticalCenter
                    tone: page.doneTone(modelData)
                    text: page.doneText(modelData)
                }
            }
        }
    }

    // 详情弹层（widgets QDialog 直译）
    Popup {
        id: detailPopup
        objectName: "reservationDetailDialog"
        // 模态：详情是"读完就走"的场景，挡住列表避免连点别的卡。
        modal: true
        anchors.centerIn: parent
        width: parent ? Math.min(330, parent.width - P.Style.spaceXl) : 330
        padding: P.Style.spaceLg
        closePolicy: Popup.CloseOnPressOutside
        background: Rectangle {
            radius: P.Style.radiusLg; color: P.Style.surface
            border.color: P.Style.line; border.width: 1
        }
        Column {
            width: parent.width
            spacing: P.Style.spaceXs
            Text {
                objectName: "reservationDetailDialogText"
                width: parent.width; wrapMode: Text.WordWrap
                text: detailRecord ? (detailRecord.stationName || "预约详情") : "预约详情"
                font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink
            }
            Repeater {
                // 字段清单三元：detailRecord 缺位给空数组——弹层理论不可见，兜底不渲染半截。
                // 费用行固定口径"预约不扣费"：钱在充电订单结算，归档详情不定金额。
                model: detailRecord ? [
                    { k: "充电桩", v: (detailRecord.chargerCode || "--") + " · " + (detailRecord.chargerSpec || "") },
                    { k: "保留时间", v: page.hhmm(detailRecord.startAtUtc) + "—" + page.hhmm(detailRecord.expiresAtUtc) + "（北京时间）" },
                    { k: "费用", v: "预约不扣费，实际账单见充电订单" },
                    { k: "状态", v: page.doneText(detailRecord) }
                ] : []
                Row {
                    width: parent.width
                    spacing: P.Style.spaceMd
                    Text { width: 80; text: modelData.k; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                    Text { width: parent.width - 80 - parent.spacing; text: modelData.v
                        font.pixelSize: P.Style.fontSm; color: P.Style.ink; wrapMode: Text.WordWrap }
                }
            }
            P.ActionButton {
                objectName: "reservationDetailCloseButton"
                variant: "primary"; text: "关闭"
                width: parent.width
                onClicked: detailPopup.close()
            }
        }
    }
}
