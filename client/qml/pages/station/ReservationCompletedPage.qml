import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets ReservationCompletedPage (objectName "reservationCompletedPage").
// ReservationModulePage 的 Tab1 子视图：records 由母页喂入；点击卡片弹详情（全字段）。
Item {
    id: page
    objectName: "reservationCompletedPage"
    width: parent ? parent.width : 420
    height: parent ? parent.height : 500

    property var records: []
    property bool loading: false
    property var detailRecord: null

    function hhmm(v) {
        if (typeof v === "number") { const d = new Date(v); return ("0"+d.getHours()).slice(-2)+":"+("0"+d.getMinutes()).slice(-2) }
        if (typeof v === "string" && v.length) { const d = new Date(v); if (!isNaN(d)) return ("0"+d.getHours()).slice(-2)+":"+("0"+d.getMinutes()).slice(-2) }
        return "--"
    }
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
                    Text {
                        width: parent.width; elide: Text.ElideRight
                        text: "充电桩 " + (modelData.chargerCode || "--")
                              + " · " + (modelData.chargerSpec || "充电桩")
                              + " · 预约时长 " + (modelData.durationMinutes || 0) + " 分钟"
                        font.pixelSize: P.Style.fontSm; color: P.Style.muted
                    }
                    Text {
                        text: "预约 · 时段 " + page.hhmm(modelData.startAtUtc)
                              + "—" + page.hhmm(modelData.expiresAtUtc)
                              + " · 预估 ¥" + page.money(modelData.estimatedFeeCents)
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
                model: detailRecord ? [
                    { k: "充电桩", v: (detailRecord.chargerCode || "--") + " · " + (detailRecord.chargerSpec || "") },
                    { k: "预约时长", v: (detailRecord.durationMinutes || 0) + " 分钟" },
                    { k: "时段", v: page.hhmm(detailRecord.startAtUtc) + "—" + page.hhmm(detailRecord.expiresAtUtc) },
                    { k: "预估费用", v: "¥" + page.money(detailRecord.estimatedFeeCents) },
                    { k: "车辆", v: detailRecord.vehiclePlate || "未关联" },
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
