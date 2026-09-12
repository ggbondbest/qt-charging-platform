import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// QML twin of widgets NotificationPage (objectName "notificationPage").
// 数据源 = notificationService（桥缺位期盲写 notifications() / notificationsChanged，
// TODO(contract)：返回 [{type,title,body,createdAtUtc}] 新→旧，type 小写串）。
// 答辩补注：route="notifications"；入口=顶栏铃铛（Shell onNotificationsRequested → App.navigate）。
// 与券页同口径：登录会话 boot 拉一次 + 进页在 navigate() 漏斗强制 refresh（审查 P2#3，
// 停止/支付落通知后同会话进页不能看旧缓存）；本页只读 notifications() 缓存 + 被动重渲染。
Item {
    id: page
    objectName: "notificationPage"
    property string route: "notifications"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    // 服务返回消息缓存（新→旧）。页内零类型过滤：开关收敛在桥/服务侧
    //（NotificationService 持 settingsService，按「通知与提醒」开关出数据，caption 即用户口径）。
    property var items: []
    function load() {
        // TODO(contract): notificationService.notifications() invokable。
        // 整体包 try：桥未注册时裸引用抛 ReferenceError 也被吞成空列表 → 走空态，页面不炸。
        try { page.items = notificationService.notifications() || [] }
        catch (e) { page.items = [] }
    }
    // target 同样是裸引用：缺桥时该绑定报一次可容忍的求值错误（连接落空、不响应信号），
    // 首屏数据由 load() 兜底——与 widgets 孪生保持同名盲写，桥落地即通。
    Connections {
        target: notificationService
        function onNotificationsChanged() { page.load() }
    }
    Component.onCompleted: load()

    // 类型 → 线稿图标名（NoticePanel 同款 GlyphProvider 面）：type 按契约
    // 小写串再归一一次大小写；未知类型 glyphFor 返空串、图标整体隐藏。
    function glyphFor(type) {
        const t = String(type).toLowerCase()
        return t === "reservation_success_notice" ? "check"
             : t === "reservation_cancel_notice" ? "x"
             : t === "reservation_expiry_reminder" ? "bell" : ""
    }
    function glyphColorFor(type) {
        const t = String(type).toLowerCase()
        return t === "reservation_success_notice" ? P.Style.brandDeep
             : t === "reservation_cancel_notice" ? P.Style.danger
             : t === "reservation_expiry_reminder" ? P.Style.warning : P.Style.faint
    }
    // 双格式宽容：epoch 毫秒 / ISO 串都吃，只到分钟；解析失败返回空串——宁缺勿假。
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

        // 列表与空态面板按 items 长度互斥（与券页同一口径）。
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
                    // 左右两端先拿 id：中间标题列宽度按下文用两侧实测隐式宽扣减
                    // （放大字号不溢出）。glyphLbl 本体已是线稿 Image（emoji→glyph 批）。
                    Image { id: glyphLbl
                        anchors.verticalCenter: parent.verticalCenter
                        visible: page.glyphFor(modelData.type).length > 0
                        width: Math.round(P.Style.fontXl * P.Style.fontScaleFactor)
                        height: width
                        source: visible ? Glyphs.source(page.glyphFor(modelData.type),
                                                        page.glyphColorFor(modelData.type)) : ""
                    }
                    Column {
                        // 原 parent.width - 60 的魔数预留放不下放大档时间列 →
                        // 按两侧内容实宽扣减（字号档适配 2026-09-08，成员3 代修）
                        width: Math.max(40, parent.width - glyphLbl.width
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
            glyph: "bell"
            title: "暂无通知"
            description: "开启新的预约后，成功/取消/到期消息会出现在这里；也可到设置页检查“通知与提醒”开关。"
            actionText: ""
        }
    }
}
