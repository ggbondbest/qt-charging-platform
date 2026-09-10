import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// 每日任务区块（2026-09-09 会员中心批自 TasksPage 抽出）：任务卡列表 +
// 全勤行，TasksPage 与 LevelPage（会员中心页）共用同一份形态与逻辑。
// 自包含：经验引擎走 App.progressService，签到走 pointsService 真实入账
// （服务端 +10 积分）后回执 reportEvent 记经验；裸引擎/无桥场景整块隐身，
// 页面对测试上下文健壮。当日幂等由 ProgressService 裁决。
// 答辩补注：非路由页，被 TasksPage / LevelPage 双页复用；本区块唯一 XP 写入口
// （签到）走"真实积分入账→回执再记经验"两段式，其余四个浏览型任务 XP 不在本区块
//（在 QmlApp::navigate() 漏斗），goTask 只负责把用户送进漏斗。
Item {
    id: section
    objectName: "uiTaskSection"
    implicitHeight: body.implicitHeight
    height: implicitHeight

    // 桥守卫：裸引擎/桥未就绪 → prog=null，任务列表自然为空、全勤行自隐；宿主页再以 visible 整块隐藏。
    readonly property var prog: (typeof App !== "undefined" && App && App.progressService)
                                ? App.progressService : null
    // 在途单飞标志：按钮"签到中…"禁用与 checkInNow 重入挡双保险；成败回执都会复位。
    property bool checkingIn: false

    // 本区块唯一"写"入口：先发起真实积分签到请求，XP 留到回执里记——保证顺序恒为"积分在前、经验在后"。
    function checkInNow() {
        if (!prog || section.checkingIn) return
        // 积分通道缺位就不碰经验：服务端没入账，就没有可记的经验。
        if (typeof pointsService === "undefined" || !pointsService) {
            if (App) App.showToast("积分通道未就绪，请到签到积分页重试", "warning")
            return
        }
        section.checkingIn = true
        pointsService.checkIn()
    }
    // 三类动作分流：签到走真实入账；stats 直跳月报；搜索/详情/路线统一送去站页。
    function goTask(modelData) {
        if (!App) return
        if (modelData.action === "checkin") { checkInNow(); return }
        if (modelData.action === "stats") { App.navigate("stats"); return }
        // 搜索/详情/路线任务都从找站页出发：带关键词入口即命中「搜索」判据。
        App.navigate("station")
    }

    // 回执驱动。operationFailed 是全桥广播（多种请求共用一个信号），必须先按 type 认领自家事件。
    Connections {
        // 桥名由 app 启动注入为上下文属性；typeof 守卫取值，未注册时 target=null 静默挂空而非 ReferenceError。
        target: typeof pointsService !== "undefined" ? pointsService : null
        function onCheckInCompleted(day, points, gained, alreadyCheckedIn) {
            section.checkingIn = false
            // 刻意不消费回执参数（积分值归积分页讲）：这里只解在途 + 上报 checkin 经验；
            // 已签过/重放由引擎当日幂等丢弃，不重复加 XP。
            if (section.prog) section.prog.reportEvent("checkin")
        }
        function onOperationFailed(type, code, message) {
            if (type !== "CHECK_IN") return
            section.checkingIn = false
            if (App) App.showToast("签到失败：" + message, "danger")
        }
    }

    Column {
        id: body
        anchors.left: parent.left; anchors.right: parent.right
        spacing: P.Style.spaceMd

        // ---- 任务卡列表 ----
        // model 是引擎带的当日 done 快照（含 xp/文案/action 路由）：卡片只呈现不计算，页面不自记完成态。
        Repeater {
            model: section.prog ? section.prog.tasks : []
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
                        Text {
                            anchors.centerIn: parent
                            text: modelData.glyph; font.pixelSize: 17
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
                            text: modelData.title
                                + (modelData.done ? " ✓" : "")
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
                        // 三态：已完成→chip 不可点；签到在途→禁用 +"签到中…"防连点；其余→去完成可点。
                        variant: modelData.done ? "chip" : "secondary"
                        selected: modelData.done
                        enabled: !modelData.done
                                 && (modelData.action !== "checkin" || !section.checkingIn)
                        text: modelData.done ? "已完成"
                              : modelData.action === "checkin"
                                ? (section.checkingIn ? "签到中…" : "签到")
                                : "去完成"
                        onClicked: section.goTask(modelData)
                    }
                }
            }
        }

        // ---- 全勤奖励行 ----
        // 纯展示行：+30 由引擎在最后一条任务判定时自动发放并落 bonus 幂等旗标，行本身无按钮、页面不补算。
        Rectangle {
            objectName: "uiTaskBonusRow"
            visible: !!section.prog
            width: parent.width
            height: 56
            radius: P.Style.radiusLg
            color: section.prog && section.prog.allTasksDone ? P.Style.warningSoft : P.Style.surface
            border.width: 1
            border.color: section.prog && section.prog.allTasksDone ? P.Style.starGold : P.Style.line
            Row {
                anchors.fill: parent
                anchors.leftMargin: 16; anchors.rightMargin: 16
                spacing: P.Style.spaceMd
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "🔥"; font.pixelSize: 18
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
                        text: section.prog && section.prog.allTasksDone
                              ? "全部完成 · +30 XP 已到账" : "完成今日全部任务自动发放"
                        font.pixelSize: P.Style.fontSm
                        color: section.prog && section.prog.allTasksDone
                               ? P.Style.warning : P.Style.muted
                    }
                }
            }
        }
    }
}
