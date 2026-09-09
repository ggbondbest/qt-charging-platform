import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// 每日任务区块（2026-09-09 会员中心批自 TasksPage 抽出）：任务卡列表 +
// 全勤行，TasksPage 与 LevelPage（会员中心页）共用同一份形态与逻辑。
// 自包含：经验引擎走 App.progressService，签到走 pointsService 真实入账
// （服务端 +10 积分）后回执 reportEvent 记经验；裸引擎/无桥场景整块隐身，
// 页面对测试上下文健壮。当日幂等由 ProgressService 裁决。
Item {
    id: section
    objectName: "uiTaskSection"
    implicitHeight: body.implicitHeight
    height: implicitHeight

    readonly property var prog: (typeof App !== "undefined" && App && App.progressService)
                                ? App.progressService : null
    property bool checkingIn: false

    function checkInNow() {
        if (!prog || section.checkingIn) return
        if (typeof pointsService === "undefined" || !pointsService) {
            if (App) App.showToast("积分通道未就绪，请到签到积分页重试", "warning")
            return
        }
        section.checkingIn = true
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
            section.checkingIn = false
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
