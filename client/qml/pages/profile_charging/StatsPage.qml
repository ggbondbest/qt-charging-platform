import QtQuick
import "../../platform" as P

// 充电月报（成员3 新页，2026-09-08）：GET_USER_STATS 的展示端。
// 数据 = 近 6 个月已完成订单的服务端聚合（statsService 桥，monthsLoaded 事件），
// 卡面沿用订单页设计语言：hero 总计 + 月份卡 + 40ms 级联入场 + 下拉刷新。
// 碳减排 = 电量 × 0.5568 kg/kWh（全国电网平均因子，服务端单点注入，
// TODO(contract): 因子业务终确认）。
Item {
    id: page
    objectName: "statsPage"
    property string route: "stats"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    // 在途身份镜像（服务层单飞静默丢弃同款口径）：本页面同时至多一条在途。
    property bool reqActive: false
    property bool loadedOnce: false

    ListModel { id: monthsModel; objectName: "uiStatsModel" }

    function sumField(name) {
        var s = 0
        for (var i = 0; i < monthsModel.count; ++i)
            s += monthsModel.get(i)[name] || 0
        return s
    }
    // 绑定随 ListModel 变更自动重算（hero 总计）。
    readonly property var totals: ({
        wh: sumField("energyWh"), cents: sumField("amountCents"),
        count: sumField("orderCount"), co2Kg: sumField("co2Grams") / 1000,
        hours: sumField("durationSeconds") / 3600
    })

    function monthLabel(key) {
        const s = String(key || "")
        if (!/^\d{4}-\d{2}$/.test(s)) return "更早"
        const p = s.split("-")
        return p[0] + "年" + parseInt(p[1], 10) + "月"
    }
    function load() {
        if (reqActive) return               // 自己那路在途：不发第二条
        if (statsService.isFetchingStats()) {
            // 他页在途同服务：本请求会被静默丢弃，直接放行刷新胶囊。
            listScroll.setRefreshing(false)
            return
        }
        reqActive = true
        statsService.fetchStats(6)
    }

    Connections {
        target: statsService
        function onStatsLoaded(rows) {
            monthsModel.clear()
            for (var i = 0; i < rows.length; ++i)
                monthsModel.append(rows[i])
            reqActive = false
            loadedOnce = true
            listScroll.setRefreshing(false)
        }
        function onOperationFailed(type, code, message) {
            if (type !== "GET_USER_STATS") return
            reqActive = false
            listScroll.setRefreshing(false)
            if (App) App.showToast("月报加载失败：" + message, "danger")
        }
    }
    Component.onCompleted: load()

    P.PullToRefreshArea {
        id: listScroll
        objectName: "uiStatsListStack"
        anchors.fill: parent
        anchors.margins: P.Style.spaceXl
        spacingHint: P.Style.spaceMd
        onRefreshRequested: load()

        // ---- header（Item 承载 anchors 布局：6.2 Row 子项 anchors 是 UB）----
        Item {
            width: listScroll.width
            height: 40
            Text {
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                text: "充电月报"
                font.pixelSize: P.Style.fontXl; font.weight: Font.Bold; color: P.Style.ink
            }
            Text {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                objectName: "uiStatsCaption"
                text: "近 6 个月 · 已完成订单"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
        }

        // ---- hero 总计卡（渐变：电量大数 + 三小格）----
        Rectangle {
            objectName: "uiStatsHero"
            width: listScroll.width
            height: 104
            radius: P.Style.radiusLg
            gradient: Gradient {
                orientation: Gradient.Horizontal   // Qt6.2 无 Diagonal
                GradientStop { position: 0.0; color: P.Style.heroFrom }
                GradientStop { position: 1.0; color: P.Style.heroTo }
            }
            Item {
                anchors.fill: parent
                anchors.leftMargin: 20; anchors.rightMargin: 20
                anchors.topMargin: 14; anchors.bottomMargin: 12
                Column {
                    anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text {
                        objectName: "uiStatsHeroEnergy"
                        text: (page.totals.wh / 1000).toFixed(1)
                        font.pixelSize: P.Style.fontHero; font.weight: Font.ExtraBold
                        color: P.Style.surface
                    }
                    Text {
                        text: "累计电量（kWh）· " + page.totals.count + " 单"
                        font.pixelSize: P.Style.fontSm; color: P.Style.heroPhone
                    }
                }
                Column {
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    width: 160; spacing: 6
                    // 注意：Column 必须显式 width——Text.width 绑 parent.width 而
                    // Column 宽度又由 Text 决定会成 binding loop（首版三行重叠的根因）。
                    Repeater {
                        model: [
                            { v: "¥" + (page.totals.cents / 100).toFixed(2), k: "总花费" },
                            { v: page.totals.hours.toFixed(1) + " h",         k: "总时长" },
                            { v: page.totals.co2Kg.toFixed(1) + " kg",         k: "碳减排" }
                        ]
                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignRight
                            text: modelData.k + "  " + modelData.v
                            font.pixelSize: P.Style.fontSm; font.weight: Font.DemiBold
                            color: P.Style.surface
                        }
                    }
                }
            }
        }

        // ---- 月份卡列表（级联入场，与订单页同款节拍）----
        Repeater {
            model: monthsModel
            Loader {
                width: listScroll.width
                height: item ? item.height : 0
                sourceComponent: monthCardComp
                onLoaded: {
                    item.row = monthsModel.get(index)
                    if (P.Style.motionEnabled) rowDelay.start()
                    else opacity = 1.0
                }
                opacity: 0
                Timer {
                    id: rowDelay
                    interval: Math.min(index, 8) * 40   // 40ms stagger, 8 行封顶
                    onTriggered: rowIn.start()
                }
                NumberAnimation on opacity {
                    id: rowIn
                    from: 0; to: 1; duration: P.Style.durEnter
                }
            }
        }

        Component {
            id: monthCardComp
            Rectangle {
                objectName: "uiStatsMonthCard"
                property var row: ({})
                width: listScroll.width
                height: 76
                radius: P.Style.radiusLg
                color: P.Style.surface
                border.width: 1
                border.color: P.Style.line
                Item {
                    anchors.fill: parent
                    anchors.leftMargin: 16; anchors.rightMargin: 16
                    Rectangle {
                        id: hub
                        anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                        width: 44; height: 44; radius: 22
                        color: P.Style.brandSoft
                        Text {
                            anchors.centerIn: parent
                            text: "📅"; font.pixelSize: 18
                        }
                    }
                    Column {
                        anchors.left: hub.right
                        anchors.right: rightCol.left
                        anchors.leftMargin: P.Style.spaceMd
                        anchors.rightMargin: P.Style.spaceMd
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            text: page.monthLabel(row.monthKey)
                            font.pixelSize: P.Style.fontLg2; font.weight: Font.DemiBold
                            color: P.Style.ink
                        }
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            text: (row.orderCount || 0) + " 单 · ¥"
                                  + ((row.amountCents || 0) / 100).toFixed(2)
                            font.pixelSize: P.Style.fontSm; color: P.Style.muted
                        }
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            objectName: "uiStatsMonthSummary"
                            text: "减碳 " + ((row.co2Grams || 0) / 1000).toFixed(1) + " kg"
                            font.pixelSize: P.Style.fontSm; color: P.Style.faint
                        }
                    }
                    // 右列焦点：月电量大字 + 单位时长小字（订单页金额焦点同款）
                    Column {
                        id: rightCol
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        width: 80; spacing: 3
                        Text {
                            width: parent.width; horizontalAlignment: Text.AlignRight
                            text: ((row.energyWh || 0) / 1000).toFixed(2)
                            font.pixelSize: 17; font.weight: Font.ExtraBold; color: P.Style.ink
                        }
                        Text {
                            width: parent.width; horizontalAlignment: Text.AlignRight
                            text: "kWh · " + ((row.durationSeconds || 0) / 3600).toFixed(1) + "h"
                            font.pixelSize: P.Style.fontSm; color: P.Style.faint
                        }
                    }
                }
            }
        }

        // ---- 空态（数据落定且确无月份）----
        P.NoticePanel {
            objectName: "uiStatsEmptyNotice"
            width: listScroll.width
            height: 200
            visible: page.loadedOnce && !page.reqActive && monthsModel.count === 0
            glyph: "📊"
            title: "近 6 个月还没有充电记录"
            description: "完成一次充电结算后，这里会按月汇总电量、花费与碳减排。"
            actionText: ""
        }
    }
}
