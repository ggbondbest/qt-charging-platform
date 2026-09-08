import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets NotificationPage (objectName "notificationPage").
// 数据源 = notificationService（桥缺位期盲写 notifications() / notificationsChanged，
// TODO(contract)：返回 [{type,title,body,createdAtUtc}] 新→旧，type 小写串）。
Item {
    id: page
    objectName: "notificationPage"
    property string route: "notifications"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property var items: []
    function load() {
        // TODO(contract): notificationService.notifications() invokable。
        try { page.items = notificationService.notifications() || [] }
        catch (e) { page.items = [] }
    }
    Connections {
        target: notificationService
        function onNotificationsChanged() { page.load() }
    }
    Component.onCompleted: load()

    function glyphFor(type) {
        const t = String(type).toLowerCase()
        return t === "reservation_success_notice" ? "✅"
             : t === "reservation_cancel_notice" ? "❌"
             : t === "reservation_expiry_reminder" ? "🔔" : "•"
    }
    function timeText(v) {
        if (typeof v === "number") return new Date(v).toLocaleTimeString([], {hour: "2-digit", minute: "2-digit"})
        if (typeof v === "string" && v.length) { const d = new Date(v); if (!isNaN(d.getTime())) return d.toLocaleTimeString([], {hour: "2-digit", minute: "2-digit"}) }
        return ""
    }

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd

        Text { objectName: "notificationPageTitle"; text: "消息通知"
            font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink }
        Text {
            objectName: "notificationPageCaption"
            width: parent.width; wrapMode: Text.WordWrap
            text: "展示预约相关的业务消息；类型开关位于「我的-设置-通知与提醒」，关闭的类型不在此展示。"
            font.pixelSize: P.Style.fontSm; color: P.Style.muted
        }

        ListView {
            objectName: "notificationStack"
            width: parent.width
            height: parent.height - y
            clip: true
            spacing: P.Style.spaceSm
            visible: page.items.length > 0
            model: page.items
            delegate: P.Card {
                objectName: "notificationCard"
                width: parent.width
                Row {
                    width: parent.width            // Card 内容进 Column 容器：anchors 被忽略且告警
                    spacing: P.Style.spaceSm
                    Text { id: glyphLbl
                        anchors.verticalCenter: parent.verticalCenter
                        text: page.glyphFor(modelData.type); font.pixelSize: P.Style.fontXl }
                    Column {
                        // 原 parent.width - 60 的魔数预留放不下放大档时间列 →
                        // 按两侧内容实宽扣减（字号档适配 2026-09-08，成员3 代修）
                        width: Math.max(40, parent.width - glyphLbl.implicitWidth
                                        - timeLbl.implicitWidth - parent.spacing * 2)
                        spacing: 2
                        Text { width: parent.width; elide: Text.ElideRight
                            text: modelData.title || ""
                            font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink }
                        Text { width: parent.width; elide: Text.ElideRight
                            text: modelData.body || ""
                            font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                    }
                    Text { id: timeLbl; objectName: "notificationTimeLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        text: page.timeText(modelData.createdAtUtc)
                        font.pixelSize: P.Style.fontSm; color: P.Style.faint }
                }
            }
        }

        P.NoticePanel {
            objectName: "notificationEmptyNotice"
            width: parent.width
            height: 160
            visible: page.items.length === 0
            glyph: "🔔"
            title: "暂无通知"
            description: "开启新的预约后，成功/取消/到期消息会出现在这里；也可到设置页检查“通知与提醒”开关。"
            actionText: ""
        }
    }
}
