import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// 每日任务页（2026-09-09 经验等级批）：任务清单 + 当日完成态 + 全勤进度。
// 数据全在 ProgressService（App.progressService）：QSettings 本地持久化、
// 日粒度幂等（经验记账纯本地；签到积分仍走 pointsService 真实入账，升级礼包
// 由 bridge 收 levelUp 后发 CREDIT_LEVEL_REWARD 入账——2026-09-09 拍板批）。
// 裸引擎/无桥场景 prog 为 null：渲染空态提示，页面不炸（成员3 页测试口径）。
Item {
    id: page
    objectName: "tasksPage"
    property string route: "tasks"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    readonly property var prog: (typeof App !== "undefined" && App && App.progressService)
                                ? App.progressService : null
    property bool checkingIn: false

    // 经验域图形单表意（2026-09-09 emoji→glyph 批）：服务层 tasks[].glyph 仍存
    // emoji（导出形状不动），页面按 id 自映射母版名，染色随完成态。
    function taskGlyph(id) {
        const m = { checkin: "calendar-check", search: "search", detail: "eye",
                    route: "compass", stats: "chart-bar" }
        return m[id] || "check"
    }
    // 档位主题色（与 ProfilePage.tierColor / LevelPage.tierColor 同表三处互指）。
    function tierColor(lv) {
        const c = ["#B0764A", "#8E9AAF", "#D9A32B", "#5FA8D3", "#3B3A52"]
        return c[Math.max(0, Math.min(4, (lv || 1) - 1))]
    }

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    function checkInNow() {
        if (!prog || page.checkingIn) return
        if (typeof pointsService === "undefined" || !pointsService) {
            if (App) App.showToast("积分通道未就绪，请到签到积分页重试", "warning")
            return
        }
        checkingIn = true
        pointsService.checkIn()
    }
    function goTask(modelData) {
        if (!App) return
        if (modelData.action === "checkin") { checkInNow(); return }
        if (modelData.action === "stats") { App.navigate("stats"); return }
        // 搜索/详情/路线任务都从找站页出发：带关键词入口即命中「搜索」判据。
        App.navigate("station")
    }

    Connections {
        target: typeof pointsService !== "undefined" ? pointsService : null
        function onCheckInCompleted(day, points, gained, alreadyCheckedIn) {
            page.checkingIn = false
            if (page.prog) page.prog.reportEvent("checkin")
        }
        function onOperationFailed(type, code, message) {
            if (type !== "CHECK_IN") return
            page.checkingIn = false
            if (App) App.showToast("签到失败：" + message, "danger")
        }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight + 2 * P.Style.spaceXl
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Column {
            id: content
            x: P.Style.spaceXl; y: P.Style.spaceXl
            width: parent.width - 2 * P.Style.spaceXl
            spacing: P.Style.spaceMd

            // ---- header ----
            Row {
                width: parent.width
                spacing: P.Style.spaceSm
                Text {
                    objectName: "uiTasksTitle"
                    anchors.verticalCenter: parent.verticalCenter
                    text: "每日任务"
                    font.pixelSize: P.Style.fontXl; font.weight: Font.Bold; color: P.Style.ink
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    objectName: "uiTasksCaption"
                    text: page.prog ? ("今日 " + page.prog.doneTaskCount + "/5 · 全勤 +30") : ""
                    font.pixelSize: P.Style.fontSm; color: P.Style.muted
                }
            }

            // ---- hero：今日经验 + 等级快照（点击进等级页）----
            Rectangle {
                objectName: "uiTasksHero"
                visible: !!page.prog
                width: parent.width
                height: 92
                radius: P.Style.radiusLg
                gradient: Gradient {
                    orientation: Gradient.Horizontal   // Qt6.2 无 Diagonal
                    GradientStop { position: 0.0; color: P.Style.heroFrom }
                    GradientStop { position: 1.0; color: P.Style.heroTo }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { if (App) App.navigate("level") }
                }
                Item {
                    anchors.fill: parent
                    anchors.leftMargin: 20; anchors.rightMargin: 16
                    anchors.topMargin: 12; anchors.bottomMargin: 10
                    Column {
                        anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        // 等级行：档位色圆托 + 白色奖牌线稿 + 文字（emoji tierGlyph 已退役）。
                        Row {
                            spacing: P.Style.spaceXs
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.round(24 * P.Style.fontScaleFactor)
                                height: width; radius: width / 2
                                color: page.tierColor(page.prog ? page.prog.level : 1)
                                Image {
                                    anchors.centerIn: parent
                                    width: Math.round(16 * P.Style.fontScaleFactor)
                                    height: width
                                    source: Glyphs.source("medal", "#FFFFFF")
                                }
                            }
                            Text {
                                objectName: "uiTasksLevelLine"
                                anchors.verticalCenter: parent.verticalCenter
                                text: page.prog ? ("Lv." + page.prog.level + " " + page.prog.tierName) : ""
                                font.pixelSize: P.Style.fontLg2; font.weight: Font.Bold
                                color: P.Style.surface
                            }
                        }
                        Text {
                            objectName: "uiTasksXpLine"
                            text: page.prog
                                  ? (page.prog.xpToNext > 0
                                     ? "升级还需 " + page.prog.xpToNext + " XP · 点任务攒经验"
                                     : "已达最高等级 · 累计 " + page.prog.xp + " XP")
                                  : ""
                            font.pixelSize: P.Style.fontSm; color: P.Style.heroPhone
                        }
                    }
                    Text {
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        objectName: "uiTasksHeroArrow"
                        text: "等级详情 ›"
                        font.pixelSize: P.Style.fontSm
                        font.weight: Font.DemiBold; color: P.Style.heroPhone
                    }
                }
            }

            // ---- 任务卡列表 ----
            Repeater {
                model: page.prog ? page.prog.tasks : []
                delegate: Rectangle {
                    objectName: "uiTaskCard"
                    required property var modelData
                    width: parent.width
                    height: 70
                    radius: P.Style.radiusLg
                    color: P.Style.surface
                    border.width: 1
                    border.color: modelData.done ? P.Style.brandEdge : P.Style.line
                    Item {
                        anchors.fill: parent
                        anchors.leftMargin: 16; anchors.rightMargin: 12
                        Rectangle {
                            id: hub
                            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                            width: 40; height: 40; radius: 20
                            color: modelData.done ? P.Style.brandSoft : P.Style.ghost
                            Image {
                                anchors.centerIn: parent
                                width: Math.round(20 * P.Style.fontScaleFactor)
                                height: width
                                source: Glyphs.source(page.taskGlyph(modelData.id),
                                    modelData.done ? P.Style.brandDeep : P.Style.ink)
                            }
                        }
                        Column {
                            anchors.left: hub.right
                            anchors.right: taskButton.left
                            anchors.leftMargin: P.Style.spaceMd
                            anchors.rightMargin: P.Style.spaceMd
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                width: parent.width; elide: Text.ElideRight
                                objectName: "uiTaskTitle"
                                text: modelData.title        // 完成态由描边/按钮/图色三重表达，"✓" 文本已退役
                                font.pixelSize: P.Style.fontLg2; font.weight: Font.DemiBold
                                color: modelData.done ? P.Style.brandDeep : P.Style.ink
                            }
                            Text {
                                width: parent.width; elide: Text.ElideRight
                                text: modelData.desc + " · +" + modelData.xp + " XP"
                                font.pixelSize: P.Style.fontSm; color: P.Style.faint
                            }
                        }
                        P.ActionButton {
                            id: taskButton
                            objectName: "uiTaskAction"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            variant: modelData.done ? "chip" : "secondary"
                            selected: modelData.done
                            enabled: !modelData.done
                                     && (modelData.action !== "checkin" || !page.checkingIn)
                            text: modelData.done ? "已完成"
                                  : modelData.action === "checkin"
                                    ? (page.checkingIn ? "签到中…" : "签到")
                                    : "去完成"
                            onClicked: page.goTask(modelData)
                        }
                    }
                }
            }

            // ---- 全勤奖励行 ----
            Rectangle {
                objectName: "uiTaskBonusRow"
                visible: !!page.prog
                width: parent.width
                height: 56
                radius: P.Style.radiusLg
                color: page.prog && page.prog.allTasksDone ? P.Style.warningSoft : P.Style.surface
                border.width: 1
                border.color: page.prog && page.prog.allTasksDone ? P.Style.starGold : P.Style.line
                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 16; anchors.rightMargin: 16
                    spacing: P.Style.spaceMd
                    Image {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.round(20 * P.Style.fontScaleFactor)
                        height: width
                        source: Glyphs.source("flame",
                            page.prog && page.prog.allTasksDone ? P.Style.warning : P.Style.muted)
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            objectName: "uiTaskBonusTitle"
                            text: "今日全勤奖励"
                            font.pixelSize: P.Style.fontMd; font.weight: Font.DemiBold
                            color: P.Style.ink
                        }
                        Text {
                            objectName: "uiTaskBonusState"
                            text: page.prog && page.prog.allTasksDone
                                  ? "全部完成 · +30 XP 已到账" : "完成今日全部任务自动发放"
                            font.pixelSize: P.Style.fontSm
                            color: page.prog && page.prog.allTasksDone
                                   ? P.Style.warning : P.Style.muted
                        }
                    }
                }
            }

            // ---- 口径说明（经验本地、积分入账）----
            Text {
                objectName: "uiTasksFootnote"
                width: parent.width
                wrapMode: Text.WordWrap
                text: "经验与等级为客户端成长体系；签到积分与升级礼包积分入账服务端，见「积分」页流水。"
                font.pixelSize: P.Style.fontXs; color: P.Style.faint
            }

            // ---- 无桥空态 ----
            P.NoticePanel {
                objectName: "uiTasksEmptyNotice"
                width: parent.width
                height: 180
                visible: !page.prog
                glyph: "calendar-check"
                title: "任务系统未就绪"
                description: "登录后即可开始攒经验、升会员等级。"
                actionText: ""
            }
        }
    }
}
