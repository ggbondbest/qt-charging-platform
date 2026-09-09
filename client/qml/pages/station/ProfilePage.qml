import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// QML twin of widgets ProfilePage (objectName "profilePage").
// Skeleton mirrors pages/profile_charging/src/profile_page.cpp buildUi():
//   1) 150px edge-to-edge gradient hero（64px avatar / nickname+打码手机号 /
//      "编辑资料 ›"行内 / 右侧签到胶囊）
//   2) wallet float card: 余额 | 充值 | 充值记录 三列同卡（1×40 divider）
//   3) 双格入口：我的订单（待支付角标）/ 我的预约
//   4) 「账号与服务」分组：整齐方块宫格（4 列）——2026-09-09 按用户参考稿
//      由 52px 长行改宫格，收藏/通知/报告/券/积分/任务/等级/评价/设置九格，
//      通知/优惠券/积分角标走桥真实计数（无数据即不显示，不放假数值）
//   5) 退出登录 = 白卡红字 logout variant（不是 danger 红底）
// 结构由成员3 于 2026-09-07 按 widgets 原版重建（成员2 文件，PR 报备）。
// 2026-09-09 布局改版（成员2 文件，PR 报备沿承）：签到胶囊并入 hero 右侧
// （点击直调 pointsService.checkIn()，日粒度幂等由服务端裁决，页面只做镜像），
// 手机号打码（参考稿 187****6904 口径），"编辑资料 ›"下移到手机号行内。
Item {
    id: page
    objectName: "profilePage"
    property string route: "profile"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property var user: (App && App.currentUser) ? App.currentUser : null
    property int waitingCount: 0
    // 宫格角标数据面（service_bridges.h）：券有 couponCount() 即时读数；
    // 通知桥无计数接口 → notifications().length 真实条数；积分桥无缓存属性
    // → 进页 fetchPoints 等 pointsLoaded 回填（-1 = 未知，角标不显示）。
    property int couponTotal: 0
    property int notifTotal: 0
    property int pointsTotal: -1
    // 今日已签镜像（PointsPage todayCheckedIn 同款：成功与重放都进已签态）。
    // 页面会被 Tab 切换销毁重建——重建后属性丢失，由进页流水推导恢复
    // （refreshCheckedToday，用户反馈 2026-09-09：切走再回来胶囊复亮可点）。
    property bool checkedToday: false
    // 在途守卫（PointsPage checkingIn 同款）：服务端日粒度幂等只保分数的
    // 底线，不保连点——回执落地前胶囊必须自己挡住重复 checkIn（审查反馈 2026-09-09）。
    property bool checkingIn: false
    function money(cents) { return ((cents || 0) / 100).toFixed(2) }
    // image://glyphs URL 单点在 platform/Glyphs.js（provider 见 glyph_provider.cpp）。
    function glyphSource(name, colorValue) { return Glyphs.source(name, colorValue) }
    // 从流水推导"今天已签"：存在今日（UTC）"每日签到"行即已签。账本跨页面
    // 重建持久，是签到态唯一可恢复的数据源；单向置真（本地已签不因分页查
    // 不到而回亮）。day 口径与 checkInCompleted 的 UTC "yyyy-MM-dd" 一致，
    // 两通道流水 createdAtUtc 均为 ISO UTC 串，前 10 字符直接可比。
    function refreshCheckedToday(entries) {
        if (page.checkedToday) return
        const today = new Date().toISOString().slice(0, 10)
        for (const row of entries || []) {
            if (row.reason === "每日签到"
                && String(row.createdAtUtc || "").slice(0, 10) === today) {
                page.checkedToday = true
                return
            }
        }
    }
    function maskPhone(p) {
        const s = String(p || "")
        return s.length === 11 ? s.slice(0, 3) + "****" + s.slice(7) : (s || "--")
    }
    // 签到入口（PointsPage checkInNow 同款：测试与 onClicked 走同一代码路径）。
    // 在途/已签双闸挡住连点——服务端日粒度幂等只保分数不反悔，不保多发请求
    // 多弹 toast（审查反馈 2026-09-09）。
    function checkInNow() {
        if (page.checkedToday) {
            if (App) App.showToast("今天已经签过啦", "info")
            return
        }
        if (page.checkingIn) return
        try {
            if (pointsService.isBusy()) {   // 服务级单飞：GET_POINTS 在途时也挡下
                if (App) App.showToast("积分请求还在路上，稍等一下", "info")
                return
            }
            page.checkingIn = true
            pointsService.checkIn()
        } catch (e) {
            page.checkingIn = false
            if (App) App.showToast("签到桥未就绪", "warning")
        }
    }

    // 经验等级引擎（2026-09-09）：客户端本地成长系统，经 App.progressService
    // 透传；裸引擎/无 App 场景取 null，等级块整体隐藏（页面对测试上下文健壮）。
    readonly property var progress: (App && App.progressService) ? App.progressService : null

    // 档位主题色：青铜/白银/黄金/铂金/黑金（与 LevelPage.tierColor 逐字同表——
    // 两页各自页面内字面量，改档色须同批，LevelPage 头注已互指）。
    function tierColor(lv) {
        const c = ["#B0764A", "#8E9AAF", "#D9A32B", "#5FA8D3", "#3B3A52"]
        return c[Math.max(0, Math.min(4, (lv || 1) - 1))]
    }

    // 待支付角标数据（widgets ordersCell waitingBadge_ 同款）
    Connections {
        target: orderService
        function onStatusCountsUpdated(chargingCount, waitingPaymentCount, completedCount) {
            page.waitingCount = waitingPaymentCount
        }
    }
    Connections {
        target: couponService
        function onCouponsChanged() { try { page.couponTotal = couponService.couponCount() } catch (e) {} }
    }
    Connections {
        target: notificationService
        function onNotificationsChanged() { try { page.notifTotal = notificationService.notifications().length } catch (e) {} }
    }
    Connections {
        target: pointsService
        function onPointsLoaded(points, entries, total) {
            page.pointsTotal = points
            page.refreshCheckedToday(entries)
        }
        function onCheckInCompleted(day, points, gained, alreadyCheckedIn) {
            page.checkingIn = false
            page.pointsTotal = points
            page.checkedToday = true              // 成功与重放都进入"已签"态
            // 经验等级挂点（经验批原钉 PointsPage 回执，该面已随纯流水化删除——
            // 签到入口收口在本页胶囊与任务页，两处同报；当日幂等在服务内）。
            if (page.progress) page.progress.reportEvent("checkin")
            if (App) App.showToast(
                (alreadyCheckedIn || gained <= 0) ? "今天已经签过啦"
                                                  : "签到成功 +" + gained + " 积分",
                (alreadyCheckedIn || gained <= 0) ? "info" : "success")
        }
        function onOperationFailed(type, code, message) {
            if (type !== "CHECK_IN") return       // GET_POINTS 静默失败：积分角标不亮即可
            page.checkingIn = false
            if (App) App.showToast("签到失败：" + message, "danger")
        }
    }
    Component.onCompleted: {
        try { orderService.fetchStatusCounts() } catch (e) {}
        try { page.couponTotal = couponService.couponCount() } catch (e) {}
        try { page.notifTotal = notificationService.notifications().length } catch (e) {}
        // 拉首页流水：回填积分角标 + 推导今日已签（refreshCheckedToday）。
        try { pointsService.fetchPoints(1, 20) } catch (e) {}
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: col.implicitHeight
        clip: true

        Column {
            id: col
            width: parent.width
            padding: P.Style.spaceLg
            spacing: P.Style.spaceMd
            readonly property int contentW: width - padding * 2

            // ---------- ① 渐变头图（顶到边、150 高、64 头像） ----------
            Rectangle {
                objectName: "profileHeroButton"
                x: -col.padding               // 抵消 Column padding 顶到左右边
                width: col.width
                height: 150
                radius: 0
                gradient: Gradient {
                    orientation: Gradient.Horizontal   // Qt6.2 无 Diagonal
                    GradientStop { position: 0.0; color: P.Style.heroFrom }
                    GradientStop { position: 1.0; color: P.Style.heroTo }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { if (App) App.navigate("profile_edit") }
                }
                Item {
                    anchors.fill: parent
                    anchors.leftMargin: 22; anchors.topMargin: 28
                    anchors.rightMargin: 20; anchors.bottomMargin: 26
                    Rectangle {
                        id: avatarHub
                        objectName: "uiAvatarHub"
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 64; height: 64; radius: 32
                        color: "#9AA4B2"
                        border.width: 2; border.color: "#66FFFFFF"
                        Text {
                            anchors.centerIn: parent
                            text: page.user && page.user.avatarKey
                                  ? (page.user.nickname ? page.user.nickname[0] : "用") : "👤"
                            font.pixelSize: 22; font.bold: true; color: P.Style.surface
                            visible: profileAvatar.status !== Image.Ready
                        }
                        Image {
                            id: profileAvatar
                            objectName: "profileAvatarImage"
                            anchors.fill: parent
                            anchors.margins: 2
                            source: page.user && String(page.user.avatarKey || "").indexOf("data:image/png;base64,") === 0
                                    ? page.user.avatarKey : ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                    }
                    Column {
                        objectName: "profileIdentity"
                        anchors.left: avatarHub.right
                        anchors.leftMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 64 - 110 - 14 - 14   // 让位右侧签到胶囊
                        spacing: 4
                        Text {
                            objectName: "nicknameLabel"
                            width: parent.width; elide: Text.ElideRight
                            text: page.user ? (page.user.nickname || "未设置") : "未登录"
                            font.pixelSize: P.Style.fontHero; font.bold: true; color: P.Style.surface
                        }
                        // 经验等级三件套已于 2026-09-09 迁出 hero（见下方
                        // "①.5 经验等级卡"：名片卡与钱包卡之间）。
                        Row {
                            spacing: P.Style.spaceXs
                            Text {
                                objectName: "heroPhoneLabel"
                                anchors.verticalCenter: parent.verticalCenter
                                text: page.user ? maskPhone(page.user.phone) : "点这里登录"
                                font.pixelSize: P.Style.fontSm; color: P.Style.heroPhone
                            }
                            Text {
                                objectName: "profileEditHint"
                                anchors.verticalCenter: parent.verticalCenter
                                text: "编辑资料 ›"; font.pixelSize: P.Style.fontSm
                                font.weight: Font.DemiBold; color: P.Style.heroPhone
                            }
                        }
                    }
                    // ---------- 签到胶囊（参考稿"¥ 签到"位：名片卡右侧） ----------
                    // 整卡 MouseArea 先于本节点声明，点击被胶囊吞掉不会误入编辑页。
                    Rectangle {
                        objectName: "heroCheckInPill"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: checkInPillRow.implicitWidth + 24
                        height: Math.round(34 * P.Style.fontScaleFactor)
                        radius: P.Style.radiusPill
                        color: page.checkedToday ? "#59FFFFFF" : P.Style.surface
                        opacity: page.checkingIn ? 0.7 : 1.0   // 在途压暗作视觉回执
                        Behavior on color { ColorAnimation { duration: P.Style.durValue } }
                        Behavior on opacity { NumberAnimation { duration: P.Style.durValue } }
                        Row {
                            id: checkInPillRow
                            anchors.centerIn: parent
                            spacing: 5
                            Image { anchors.verticalCenter: parent.verticalCenter
                                width: Math.round(14 * P.Style.fontScaleFactor)
                                height: width
                                source: page.glyphSource("coin",
                                    page.checkedToday ? P.Style.heroPhone : P.Style.brandDeep) }
                            Text { anchors.verticalCenter: parent.verticalCenter
                                text: page.checkedToday ? "已签到"
                                    : page.checkingIn ? "签到中…" : "签到"
                                font.pixelSize: P.Style.fontSm; font.bold: true
                                color: page.checkedToday ? P.Style.heroPhone : P.Style.brandDeep }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.checkInNow()
                        }
                    }
                }
            }

            // ---------- ①.5 经验等级卡（2026-09-09 需求批二版：由 hero 迁入，
            // 用户指定放名片卡与钱包卡之间）。整卡点击进会员等级页——objectName
            // 沿用 uiLevelBadgeButton/uiLevelBar/uiLevelBarFill/uiLevelXpLabel
            // （station 端到端测试钉的是名字与行为，不是位置）。奖牌图形走
            // glyph 染色管线（medal 母版 × 档位色），替代原 emoji 徽章文本。 ----------
            Rectangle {
                objectName: "uiLevelBadgeButton"
                visible: page.progress !== null
                width: col.contentW
                height: levelCardCol.implicitHeight + 2 * P.Style.spaceMd
                radius: P.Style.radiusLg
                color: P.Style.surface
                border.width: 1
                border.color: P.Style.line
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { if (App) App.navigate("level") }
                }
                Column {
                    id: levelCardCol
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: P.Style.spaceMd
                    anchors.rightMargin: P.Style.spaceMd
                    spacing: P.Style.spaceSm
                    Row {
                        width: parent.width
                        spacing: P.Style.spaceMd
                        Rectangle {
                            id: levelMedalHub
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.round(44 * P.Style.fontScaleFactor)
                            height: width; radius: width / 2
                            color: P.Style.ghost
                            Image {
                                anchors.centerIn: parent
                                width: Math.round(26 * P.Style.fontScaleFactor)
                                height: width
                                source: page.glyphSource("medal",
                                    page.tierColor(page.progress ? page.progress.level : 1))
                            }
                        }
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - levelMedalHub.width - parent.spacing
                            spacing: 3
                            Text {
                                objectName: "uiLevelTierLine"
                                width: parent.width; elide: Text.ElideRight
                                text: "Lv." + (page.progress ? page.progress.level : 1)
                                      + " " + (page.progress ? page.progress.tierName : "") + " ›"
                                font.pixelSize: P.Style.fontMd; font.weight: Font.DemiBold
                                color: P.Style.ink
                            }
                            Text {
                                objectName: "uiLevelXpLabel"
                                width: parent.width; elide: Text.ElideRight
                                text: page.progress
                                      ? (page.progress.xpToNext > 0
                                         ? "当前 " + page.progress.xpIntoLevel + "/"
                                           + page.progress.xpSpan + " XP · 距"
                                           + page.progress.nextTierName + "还需 "
                                           + page.progress.xpToNext + " XP"
                                         : "当前累计 " + page.progress.xp + " XP · 已是最高等级")
                                      : ""
                                font.pixelSize: P.Style.fontXs; color: P.Style.muted
                            }
                        }
                    }
                    Rectangle {
                        objectName: "uiLevelBar"
                        width: parent.width; height: 6; radius: 3
                        color: P.Style.ghost
                        Rectangle {
                            objectName: "uiLevelBarFill"
                            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                            height: parent.height; radius: parent.radius
                            width: parent.width * (page.progress ? page.progress.progress : 0)
                            color: page.tierColor(page.progress ? page.progress.level : 1)
                            Behavior on width {
                                enabled: P.Style.motionEnabled
                                NumberAnimation { duration: P.Style.durValue }
                            }
                        }
                    }
                }
            }

            // ---------- ② 钱包三列浮卡（一张卡：余额 | 充值 | 充值记录） ----------
            Rectangle {
                objectName: "walletCard"
                width: col.contentW
                height: 86
                radius: P.Style.radiusLg
                color: P.Style.surface
                border.width: 1
                border.color: P.Style.line
                Row {
                    anchors.fill: parent
                    anchors.margins: 1
                    readonly property int cellW: (width - 2) / 3
                    // 余额列（值上注下，点击进钱包）
                    Item {
                        width: parent.cellW; height: parent.height
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: { if (App) App.navigate("wallet") } }
                        Column {
                            anchors.centerIn: parent
                            spacing: 4
                            Text {
                                objectName: "balanceLabel"
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "¥ " + (page.user ? money(page.user.balanceCents) : "--")
                                font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.brandDeep
                            }
                            Text { anchors.horizontalCenter: parent.horizontalCenter
                                text: "余额（元）"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                            }
                    }
                    Rectangle { width: 1; height: 40
                        y: (parent.height - height) / 2; color: P.Style.line }
                    Repeater {
                        model: [
                            { obj: "openRechargeButton", glyph: "credit-card", caption: "充值",     route: "recharge" },
                            { obj: "openWalletButton",   glyph: "receipt", caption: "充值记录", route: "wallet" }
                        ]
                        delegate: Item {
                            objectName: modelData.obj
                            width: parent.cellW; height: 84
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: { if (App) App.navigate(modelData.route) } }
                            Column {
                                anchors.centerIn: parent
                                spacing: 4
                                Image { anchors.horizontalCenter: parent.horizontalCenter
                                    width: Math.round(18 * P.Style.fontScaleFactor)
                                    height: width
                                    source: page.glyphSource(modelData.glyph, P.Style.muted) }
                                Text { anchors.horizontalCenter: parent.horizontalCenter
                                    text: modelData.caption; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                            }
                        }
                    }
                }
            }

            // ---------- ③ 双格入口：我的订单 / 我的预约 ----------
            Row {
                objectName: "profileCellsRow"
                width: col.contentW
                spacing: P.Style.spaceSm
                Repeater {
                    model: [
                        { obj: "openOrdersButton",       glyph: "clipboard", title: "我的订单",
                          caption: "全部充电订单", route: "order",           badge: true },
                        { obj: "openReservationsButton", glyph: "calendar-event", title: "我的预约",
                          caption: "时段预约记录", route: "reservation_module", badge: false }
                    ]
                    delegate: Rectangle {
                        objectName: modelData.obj
                        width: (col.contentW - P.Style.spaceSm) / 2
                        height: 78
                        radius: P.Style.radiusLg
                        color: P.Style.surface
                        border.width: 1
                        border.color: P.Style.line
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: { if (App) App.navigate(modelData.route) } }
                        Item {
                            anchors.fill: parent
                            anchors.margins: 16
                            Image {
                                id: cellIcon
                                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                                width: Math.round(22 * P.Style.fontScaleFactor)
                                height: width
                                source: page.glyphSource(modelData.glyph, P.Style.brandDeep)
                            }
                            Column {
                                anchors.left: cellIcon.right; anchors.leftMargin: P.Style.spaceSm
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3
                                Text { text: modelData.title
                                    font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink }
                                Text { text: modelData.caption
                                    font.pixelSize: P.Style.fontSm; color: P.Style.faint }
                            }
                            // 待支付角标（订单格）/ chevron（预约格）钉右缘
                            P.StatusTag {
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                visible: modelData.badge && page.waitingCount > 0
                                tone: "warning"; text: String(page.waitingCount)
                            }
                            Text {
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                visible: !modelData.badge
                                text: "›"; font.pixelSize: P.Style.fontLg2; color: P.Style.faint
                            }
                        }
                    }
                }
            }

            // ---------- ④ 账号与服务（整齐方块宫格，4 列） ----------
            Text {
                objectName: "sectionTitleAccount"
                text: "账号与服务"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
            Grid {
                objectName: "accountGrid"
                width: col.contentW
                columns: 4
                columnSpacing: P.Style.spaceSm
                rowSpacing: P.Style.spaceSm
                Repeater {
                    model: [
                        { obj: "openFavoritesButton",     glyph: "star", title: "收藏",
                          route: "favorites",    badge: "" },
                        // 2026-09-08 成员2 新增消息通知（fe325d3）与成员3 四入口合并；
                        // 2026-09-09 长行改宫格，路由与 objectName 逐格不变；
                        // 同日 emoji→Tabler glyph 名（染色 provider，见 glyph_provider.h）。
                        { obj: "openNotificationsButton", glyph: "bell", title: "消息通知",
                          route: "notifications", badge: "notif" },
                        { obj: "openStatsButton",   glyph: "chart-bar", title: "充电报告",
                          route: "stats",       badge: "" },
                        { obj: "openCouponButton",  glyph: "ticket", title: "优惠券",
                          route: "coupon",      badge: "coupon" },
                        { obj: "openPointsButton",  glyph: "coin", title: "积分",
                          route: "points",      badge: "points" },
                        // 经验等级/每日任务（2026-09-09 需求批）：任务做经验，等级看权益。
                        { obj: "openTasksButton",   glyph: "calendar-check", title: "每日任务",
                          route: "tasks",      badge: "" },
                        { obj: "openLevelButton",   glyph: "medal", title: "会员等级",
                          route: "level",      badge: "" },
                        { obj: "openRatingsButton", glyph: "star", title: "我的评价",
                          route: "ratings",     badge: "" },
                        { obj: "openSettingsButton", glyph: "settings", title: "设置",
                          route: "settings",    badge: "" }
                    ]
                    delegate: Rectangle {
                        objectName: modelData.obj
                        width: (col.contentW - 3 * P.Style.spaceSm) / 4
                        height: Math.round(86 * P.Style.fontScaleFactor)
                        radius: P.Style.radiusLg
                        color: P.Style.surface
                        border.width: 1
                        border.color: P.Style.line
                        // 角标读数（真实数据才显示：pointsTotal<0=未回填不亮）
                        readonly property int badgeCount:
                            modelData.badge === "coupon" ? page.couponTotal :
                            modelData.badge === "notif"  ? page.notifTotal :
                            modelData.badge === "points" ? Math.max(page.pointsTotal, 0) : 0
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: { if (App) App.navigate(modelData.route) } }
                        Column {
                            anchors.centerIn: parent
                            spacing: P.Style.spaceXs
                            Image { anchors.horizontalCenter: parent.horizontalCenter
                                width: Math.round(22 * P.Style.fontScaleFactor)
                                height: width
                                source: page.glyphSource(modelData.glyph, P.Style.ink) }
                            Text { anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.title
                                font.pixelSize: P.Style.fontSm; color: P.Style.ink }
                        }
                        P.StatusTag {
                            anchors.right: parent.right; anchors.top: parent.top
                            anchors.rightMargin: 3; anchors.topMargin: 3
                            visible: badgeCount > 0
                            tone: "warning"
                            text: badgeCount > 99 ? "99+" : String(badgeCount)
                        }
                    }
                }
            }

            // ---------- ⑤ 退出登录：白卡红字（QSS #logoutButton） ----------
            P.ActionButton {
                objectName: "logoutButton"
                variant: "logout"
                text: "退出登录"
                width: col.contentW
                height: 50
                visible: !!(App && App.loggedIn)
                onClicked: {
                    // TODO(contract): authService.logout() invokable；成功后壳收 loginStateChanged 自动翻页
                    if (authService) {
                        try { authService.logout() } catch (e) {
                            if (App) App.showToast("退出登录桥未就绪", "warning")
                        }
                        return
                    }
                    // mock 通道 authService==nullptr（app_bridge.cpp:99）：回退 App.logout()
                    if (App) App.logout()
                }
            }
        }
    }

    // 登录态翻转时重取 currentUser（绑定已跟 App.currentUser，这里兜壳重推）；
    // 登出顺手清"今日已签"镜像——换账号后签到态不可跨账号复用。
    Connections {
        target: App
        function onLoginStateChanged() {
            page.user = (App && App.currentUser) ? App.currentUser : null
            if (!(App && App.loggedIn)) page.checkedToday = false
        }
    }
}
