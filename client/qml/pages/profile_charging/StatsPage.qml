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
    // requestedPeriod 记录在途请求归属档位：响应回来时若用户已切档，
    // 丢弃陈旧响应并在在途结束后补发当前档位（快速切换不串数据）。
    property bool reqActive: false
    property bool loadedOnce: false
    property string requestedPeriod: ""
    // 聚合档（批次B）：week=近8周 / month=近6月 / year=近5年。
    // 期数窗口是展示口径，数据契约只认 months=窗口内期数。
    property string period: "month"
    readonly property var periodTabs: [
        { key: "week",  obj: "uiPeriodWeekButton",  label: "周",   span: 8, caption: "近 8 周 · 已完成订单" },
        { key: "month", obj: "uiPeriodMonthButton", label: "月",   span: 6, caption: "近 6 个月 · 已完成订单" },
        { key: "year",  obj: "uiPeriodYearButton",  label: "年",   span: 5, caption: "近 5 年 · 已完成订单" }
    ]
    property string periodCaption: "近 6 个月 · 已完成订单"

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
        // 周档键："2026-W36"（%W 年度周序号，周一为始）→ "2026年 第36周"
        const wk = /^(\d{4})-W(\d{2})$/.exec(s)
        if (wk) return wk[1] + "年 第" + parseInt(wk[2], 10) + "周"
        if (/^\d{4}$/.test(s)) return s + "年 · 年度汇总"     // 年档
        if (!/^\d{4}-\d{2}$/.test(s)) return "更早"
        const p = s.split("-")
        return p[0] + "年" + parseInt(p[1], 10) + "月"
    }
    function periodTab(key) {
        for (var i = 0; i < periodTabs.length; ++i)
            if (periodTabs[i].key === key) return periodTabs[i]
        return null
    }
    function load() {
        if (reqActive) return               // 自己那路在途：不发第二条
        if (statsService.isFetchingStats()) {
            // 他页在途同服务：本请求会被静默丢弃，直接放行刷新胶囊。
            listScroll.setRefreshing(false)
            return
        }
        var tab = periodTab(period)
        if (tab === null) return
        reqActive = true
        requestedPeriod = period
        statsService.fetchStats(tab.span, period)
    }
    function switchPeriod(key) {
        if (key === period) return
        period = key
        // 在途时不发第二条：旧响应到达后按归属丢弃并立即补发当前档位。
        load()
    }

    Connections {
        target: statsService
        function onStatsLoaded(rows) {
            reqActive = false
            if (!page.periodTab(period)) return
            if (requestedPeriod !== period) {
                // 陈旧响应（用户已切档）：不入模型，按当前档位补发。
                requestedPeriod = ""
                load()
                return
            }
            requestedPeriod = ""
            monthsModel.clear()
            for (var i = 0; i < rows.length; ++i)
                monthsModel.append(rows[i])
            page.periodCaption = page.periodTab(period).caption
            loadedOnce = true
            listScroll.setRefreshing(false)
        }
        function onOperationFailed(type, code, message) {
            if (type !== "GET_USER_STATS") return
            reqActive = false
            if (requestedPeriod !== period) {
                // 与所选档位无关的失败：静默补发当前档位，不误弹旧档错误。
                requestedPeriod = ""
                load()
                return
            }
            requestedPeriod = ""
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
                objectName: "uiStatsTitle"
                text: "充电" + (page.period === "week" ? "周报"
                     : page.period === "year" ? "年报" : "月报")
                font.pixelSize: P.Style.fontXl; font.weight: Font.Bold; color: P.Style.ink
            }
            Text {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                objectName: "uiStatsCaption"
                text: page.periodCaption
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
        }

        // ---- 档位切换（批次B：周/月/年 三 chip，选中=primary）----
        Row {
            objectName: "uiPeriodRow"
            width: listScroll.width
            spacing: P.Style.spaceSm
            Repeater {
                model: page.periodTabs
                delegate: P.ActionButton {
                    objectName: modelData.obj
                    variant: page.period === modelData.key ? "primary" : "secondary"
                    text: modelData.label
                    onClicked: page.switchPeriod(modelData.key)
                }
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
        // delegate 直挂 ListModel：role 访问在 clear/重建期读回 undefined，
        // 没有 Loader+onLoaded 赋值时序窗口（初版 null TypeError 的根因）。
        Repeater {
            model: monthsModel
            Rectangle {
                objectName: "uiStatsMonthCard"
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
                            text: page.monthLabel(model.monthKey)
                            font.pixelSize: P.Style.fontLg2; font.weight: Font.DemiBold
                            color: P.Style.ink
                        }
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            text: (model.orderCount || 0) + " 单 · ¥"
                                  + ((model.amountCents || 0) / 100).toFixed(2)
                            font.pixelSize: P.Style.fontSm; color: P.Style.muted
                        }
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            objectName: "uiStatsMonthSummary"
                            text: "减碳 " + ((model.co2Grams || 0) / 1000).toFixed(1) + " kg"
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
                            text: ((model.energyWh || 0) / 1000).toFixed(2)
                            font.pixelSize: 17; font.weight: Font.ExtraBold; color: P.Style.ink
                        }
                        Text {
                            width: parent.width; horizontalAlignment: Text.AlignRight
                            text: "kWh · " + ((model.durationSeconds || 0) / 3600).toFixed(1) + "h"
                            font.pixelSize: P.Style.fontSm; color: P.Style.faint
                        }
                    }
                }
                Component.onCompleted: {
                    if (!P.Style.motionEnabled) {
                        opacity = 1.0
                        return
                    }
                    rowDelay.start()   // widgets motion parity: 40ms stagger, 8 行封顶
                }
                opacity: 0
                Timer {
                    id: rowDelay
                    interval: Math.min(index, 8) * 40
                    onTriggered: rowIn.start()
                }
                NumberAnimation on opacity {
                    id: rowIn
                    from: 0; to: 1; duration: P.Style.durEnter
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
