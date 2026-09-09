import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// 每日任务页（2026-09-09 经验等级批）：任务清单 + 当日完成态 + 全勤进度。
// 会员中心批起任务卡形态抽到 TaskSection.qml（与 LevelPage 共用）；本页
// 保留独立路由（深链/测试入口）与等级 hero。数据全在 ProgressService
// （App.progressService）：QSettings 本地持久化、日粒度幂等（经验记账纯
// 本地；签到积分仍走 pointsService 真实入账，升级礼包由 bridge 收 levelUp
// 后发 CREDIT_LEVEL_REWARD 入账——2026-09-09 拍板批）。
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
    // 测试/外层消费位保持原样（别名转发进区块）：签到在途镜像 + 手动触发。
    property alias checkingIn: taskSection.checkingIn
    function checkInNow() { taskSection.checkInNow() }

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

            // ---- 任务卡列表 + 全勤行（会员中心批抽为共享区块）----
            TaskSection {
                id: taskSection
                width: parent.width
                visible: !!page.prog
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
