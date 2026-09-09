import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// 会员等级页（2026-09-09 经验等级批）：从「我的」页经验卡整卡点击进入。
// 三段式：当前等级 hero（条+经验）→ 等级阶梯与权益 → 升级记录。
// 数据全在 ProgressService（App.progressService）；升级礼包积分 2026-09-09
// 拍板改真入账（推翻原"分账"设计）：bridge 发 CREDIT_LEVEL_REWARD 落服务端
// points_ledger，「积分」页流水可见"等级礼包"行；本页「升级记录」为展示镜像。
Item {
    id: page
    objectName: "levelPage"
    property string route: "level"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    readonly property var prog: (typeof App !== "undefined" && App && App.progressService)
                                ? App.progressService : null

    // 档位主题色：青铜/白银/黄金/铂金/黑金（页内字面量，同 ProfilePage chevron 口径）
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
            Item {
                width: parent.width
                height: 28
                Text {
                    objectName: "uiLevelPageTitle"
                    anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                    text: "会员等级"
                    font.pixelSize: P.Style.fontXl; font.weight: Font.Bold; color: P.Style.ink
                }
                Text {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: "做任务攒经验"
                    font.pixelSize: P.Style.fontSm; color: P.Style.muted
                }
            }

            // ---- hero：当前档位 ----
            Rectangle {
                objectName: "uiLevelHero"
                visible: !!page.prog
                width: parent.width
                height: 118
                radius: P.Style.radiusLg
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: page.prog
                                   ? page.tierColor(page.prog.level) : P.Style.heroFrom }
                    GradientStop { position: 1.0; color: Qt.lighter(page.prog
                                   ? page.tierColor(page.prog.level) : P.Style.heroTo, 1.35) }
                }
                Item {
                    anchors.fill: parent
                    anchors.leftMargin: 20; anchors.rightMargin: 20
                    anchors.topMargin: 14; anchors.bottomMargin: 14
                    Column {
                        anchors.left: parent.left; anchors.top: parent.top
                        anchors.right: parent.right
                        spacing: 3
                        // 档名行：白色奖牌线稿承托档位渐变（tierGlyph emoji 已退役，
                        // 图形与文字不再双重表意）。
                        Row {
                            spacing: P.Style.spaceSm
                            Image {
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.round(26 * P.Style.fontScaleFactor)
                                height: width
                                source: Glyphs.source("medal", "#FFFFFF")
                            }
                            Text {
                                objectName: "uiLevelHeroTier"
                                anchors.verticalCenter: parent.verticalCenter
                                text: page.prog ? (page.prog.tierName + " · Lv." + page.prog.level) : ""
                                font.pixelSize: P.Style.fontHero; font.weight: Font.ExtraBold
                                color: "white"
                            }
                        }
                        Text {
                            objectName: "uiLevelHeroXp"
                            text: page.prog
                                  ? (page.prog.xpToNext > 0
                                     ? "本级 " + page.prog.xpIntoLevel + "/" + page.prog.xpSpan
                                       + " XP · 距" + page.prog.nextTierName + "还需 "
                                       + page.prog.xpToNext + " XP（累计 " + page.prog.xp + "）"
                                     : "最高档位 · 累计 " + page.prog.xp + " XP · 全部权益已解锁")
                                  : ""
                            font.pixelSize: P.Style.fontXs
                            color: "#E6FFFFFF"
                        }
                    }
                    // 进度条压在 hero 底部
                    Rectangle {
                        objectName: "uiLevelHeroBar"
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 10
                        height: 8; radius: 4
                        color: "#40FFFFFF"
                        Rectangle {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            height: parent.height; radius: parent.radius
                            width: parent.width * (page.prog ? page.prog.progress : 0)
                            color: "white"
                            Behavior on width {
                                enabled: P.Style.motionEnabled
                                NumberAnimation { duration: P.Style.durValue }
                            }
                        }
                    }
                }
            }

            // ---- 等级阶梯与权益 ----
            Text {
                objectName: "uiLevelLadderTitle"
                text: "等级阶梯与权益"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
            Repeater {
                model: page.prog ? page.prog.tiers : []
                delegate: Rectangle {
                    objectName: "uiTierCard"
                    required property var modelData
                    width: parent.width
                    height: 84
                    radius: P.Style.radiusLg
                    color: P.Style.surface
                    border.width: 1
                    border.color: modelData.state === "current"
                                  ? page.tierColor(modelData.level) : P.Style.line
                    opacity: modelData.state === "locked" ? 0.55 : 1.0
                    Item {
                        anchors.fill: parent
                        anchors.leftMargin: 16; anchors.rightMargin: 14
                        Rectangle {
                            id: tierHub
                            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                            width: 42; height: 42; radius: 21
                            color: page.tierColor(modelData.level)
                            Image {
                                anchors.centerIn: parent
                                width: Math.round(22 * P.Style.fontScaleFactor)
                                height: width
                                source: Glyphs.source("medal", "#FFFFFF")
                            }
                        }
                        Column {
                            anchors.left: tierHub.right
                            anchors.right: tierTag.left
                            anchors.leftMargin: P.Style.spaceMd
                            anchors.rightMargin: P.Style.spaceSm
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                width: parent.width; elide: Text.ElideRight
                                objectName: "uiTierName"
                                text: "Lv." + modelData.level + " " + modelData.name
                                      + " · " + modelData.threshold + " XP 达成"
                                      + (modelData.giftPoints > 0
                                         ? " · 礼包 +" + modelData.giftPoints + " 积分" : "")
                                font.pixelSize: P.Style.fontMd; font.weight: Font.DemiBold
                                color: P.Style.ink
                            }
                            Text {
                                width: parent.width
                                objectName: "uiTierPerk"
                                text: modelData.perk
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                font.pixelSize: P.Style.fontXs; color: P.Style.muted
                            }
                        }
                        P.StatusTag {
                            id: tierTag
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            objectName: "uiTierStateTag"
                            tone: modelData.state === "current" ? "warning"
                                  : modelData.state === "reached" ? "success" : "neutral"
                            text: modelData.state === "current" ? "当前"
                                  : modelData.state === "reached" ? "已达成" : "未解锁"
                        }
                    }
                }
            }

            // ---- 升级记录 ----
            Text {
                objectName: "uiGiftsTitle"
                text: "升级礼包记录"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
            Repeater {
                model: page.prog ? page.prog.gifts : []
                delegate: Rectangle {
                    objectName: "uiGiftCard"
                    required property var modelData
                    width: parent.width
                    height: 58
                    radius: P.Style.radiusLg
                    color: P.Style.surface
                    border.width: 1
                    border.color: P.Style.line
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 16; anchors.rightMargin: 16
                        spacing: P.Style.spaceMd
                        Image {
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.round(22 * P.Style.fontScaleFactor)
                            height: width
                            source: Glyphs.source("medal", page.tierColor(modelData.level))
                        }
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text {
                                text: "升到 " + modelData.tier + " · 礼包 +"
                                    + modelData.points + " 积分"
                                font.pixelSize: P.Style.fontMd; font.weight: Font.DemiBold
                                color: P.Style.ink
                            }
                            Text {
                                text: modelData.date
                                font.pixelSize: P.Style.fontSm; color: P.Style.faint
                            }
                        }
                        Image {
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.round(20 * P.Style.fontScaleFactor)
                            height: width
                            source: Glyphs.source("gift", P.Style.warning)
                        }
                    }
                }
            }
            P.NoticePanel {
                objectName: "uiGiftsEmptyNotice"
                width: parent.width
                height: 150
                visible: !!page.prog && page.prog.gifts.length === 0
                glyph: "gift"
                title: "还没有升级记录"
                description: "去「每日任务」攒经验，跨过档位门槛即自动发放礼包。"
                actionText: ""
            }

            // ---- 到账口径说明（诚实标注）----
            Text {
                objectName: "uiLevelFootnote"
                width: parent.width
                wrapMode: Text.WordWrap
                text: "升级礼包积分已并入服务端积分账本（「积分」页流水可见「等级礼包」行）；经验与档位为客户端成长体系。"
                font.pixelSize: P.Style.fontXs; color: P.Style.faint
            }

            // ---- 无桥空态 ----
            P.NoticePanel {
                objectName: "uiLevelEmptyNotice"
                width: parent.width
                height: 180
                visible: !page.prog
                glyph: "medal"
                title: "等级系统未就绪"
                description: "登录后即可在这里查看会员等级与权益。"
                actionText: ""
            }
        }
    }
}
