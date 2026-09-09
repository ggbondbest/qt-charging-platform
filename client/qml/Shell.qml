import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import "platform" as P

// App shell — QML twin of widgets HomeShell (objectName "homeShell" kept).
// StackView routing + login gate + toast sink; tabs match the C++ ids.
Window {
    id: shell
    objectName: "homeShell"
    width: 420
    height: 860
    visible: true
    color: P.Style.bg

    property string route: typeof chargingView === "string" ? chargingView : "station"
    readonly property var tabIds: ["station", "order", "charging", "profile"]

    // Routes whose .qml already exist this sprint get real pages; the rest
    // render the placeholder until their owner's PR lands (then flip here).
    // PR #36 landed 成员2 的 station 域（docs/design/qml-station-mapping.md
    // HomeShell 节翻位清单）——station/login/profile + 7 条新路由全部翻页。
    readonly property var migrated: ["wallet", "recharge", "order", "order_detail",
                                     "charging", "charging_run", "settlement",
                                     "profile_edit",
                                     "station", "login", "profile",
                                     "station_detail", "reservation_confirm",
                                     "reservation_module", "navigation",
                                     "favorites", "notifications", "settings",
                                     "stats", "coupon", "points", "ratings", "scan",
                                     "tasks", "level"]
    // Route id → QML page source (relative to this file's directory tree).
    function pageSource(r) {
        const t = {
            "station":   "pages/station/StationHomePage.qml",
            "order":     "pages/profile_charging/OrderListPage.qml",
            "charging":  "pages/profile_charging/ChargingHomePage.qml",
            "profile":   "pages/station/ProfilePage.qml",
            "login":     "pages/station/LoginPage.qml",
            "wallet":    "pages/profile_charging/WalletPage.qml",
            "recharge":  "pages/profile_charging/RechargePage.qml",
            "order_detail": "pages/profile_charging/OrderDetailPage.qml",
            "charging_run": "pages/profile_charging/ChargingPage.qml",
            "settlement":   "pages/profile_charging/SettlementPage.qml",
            "profile_edit": "pages/profile_charging/ProfileEditPage.qml",
            "station_detail":      "pages/station/StationDetailPage.qml",
            "reservation_confirm": "pages/station/ReservationConfirmPage.qml",
            "reservation_module":  "pages/station/ReservationModulePage.qml",
            "navigation":          "pages/station/NavigationPage.qml",
            "favorites":           "pages/station/FavoritesPage.qml",
            "notifications":       "pages/station/NotificationPage.qml",
            "settings":            "pages/station/SettingsPage.qml",
            "stats":               "pages/profile_charging/StatsPage.qml",
            "coupon":              "pages/station/CouponPage.qml",
            "points":              "pages/profile_charging/PointsPage.qml",
            "ratings":             "pages/profile_charging/RatingsPage.qml",
            "scan":                "pages/profile_charging/ScanPage.qml",
            "tasks":               "pages/profile_charging/TasksPage.qml",
            "level":               "pages/profile_charging/LevelPage.qml",
        }
        if (!t[r]) return ""
        return migrated.indexOf(r) >= 0 ? t[r] : "pages/PlaceholderPage.qml"
    }
    function pushRoute(r, arg) {
        // Historical links share the same tab and the same tracking owner.
        if (r === "charging_run") r = "charging"
        if ((!App || !App.loggedIn) && r !== "login") r = "login"
        const src = pageSource(r)
        if (src.length === 0) return
        const url = Qt.resolvedUrl(src)
        const props = { route: r, arg: arg === undefined ? "" : arg }
        // Parallel tab switches replace/clear the stack instead of pushing:
        // widgets HomeShell used a QStackedWidget (switch = no hierarchy);
        // bare push piled page instances whose countdown Timers / polling
        // Connections / looping animations kept running forever → progressive
        // lag + "frozen" animations (2026-09-07 user feedback).
        const isTab = tabIds.indexOf(r) >= 0 || r === "login"
                      || r === "charging_run" || r === "settlement"
        if (isTab) {
            if (stack.currentItem && typeof stack.currentItem.releaseTracking === "function")
                stack.currentItem.releaseTracking()
            stack.clear(StackView.Immediate)
            if (stack.depth > 0) stack.replace(url, props)
            else stack.push(url, props)
        } else {
            stack.push(url, props)
        }
        for (var i = 0; i < tabIds.length; ++i)
            if (tabIds[i] === r) { tabBar.setCurrentTab(r); break }
    }
    function pop() { if (stack.depth > 1) stack.pop() }

    // Wire the contract's routing bridge (App.navigate / App.back).
    Connections {
        target: App
        function onNavigateRequested(route, arg) { shell.pushRoute(route, arg) }
        function onBackRequested() { shell.pop() }
        function onToastRequested(text, tone) { toast.show(text, tone) }
        function onLoginStateChanged() {
            P.TabCache.charging = null
            if (stack.currentItem && typeof stack.currentItem.releaseTracking === "function")
                stack.currentItem.releaseTracking()
            stack.clear(StackView.Immediate)
            if (!App.loggedIn) shell.pushRoute("login")
            else shell.pushRoute("station")
        }
    }

    // —— 外观同步（2026-09-08 批次A）——
    // Style 的色/字号令牌全是对 theme/fontScale 的绑定，这里只做"服务→单例"
    // 的值搬运；启动拉一次 + appearanceChanged 持续跟。桥缺位场景（单页测试
    // 上下文）用 typeof 守卫，Style 保持默认亮色/标准档。
    function syncAppearance() {
        if (typeof settingsService === "undefined" || !settingsService) return
        try {
            const t = settingsService.theme()
            if (typeof t === "string" && t.length > 0) P.Style.theme = t
            const f = settingsService.fontScale()
            if (typeof f === "string" && f.length > 0) P.Style.fontScale = f
        } catch (e) { /* 桥未实现外观方法：静默按默认档 */ }
    }
    Component.onCompleted: syncAppearance()
    Connections {
        target: typeof settingsService !== "undefined" ? settingsService : null
        function onAppearanceChanged() { shell.syncAppearance() }
    }

    Column {
        anchors.fill: parent
        P.TopNavBar {
            id: navBar
            width: parent.width
            user: App && App.currentUser ? App.currentUser : null
            backVisible: stack.depth > 1
            // 搜索框/铃铛随"当前页"显隐（原绑 shell.route——只在启动时求值一次，
            // 从登录进入 station 后不更新，顶栏搜索与通知图标消失；用户实测指定修复）。
            searchVisible: stack.currentItem && stack.currentItem.route === "station"
            onBackRequested: shell.pop()
            // 映射稿 HomeShell 节：搜索关键词经路由 arg 注入站点首页（arg 作关键词入口）。
            onSearchSubmitted: (keyword) => { if (App) App.navigate("station", keyword) }
            onLoginRequested: shell.pushRoute("login")
            onProfileRequested: { if (App) App.navigate("profile") }
            onFilterRequested: {
                // 找站页/收藏页均暴露 openAdvancedFilter()（station 域组件），
                // 漏斗即开当前页的 8 组高级筛选弹层。typeof 守卫取上游口径。
                if (stack.currentItem && typeof stack.currentItem.openAdvancedFilter === "function")
                    stack.currentItem.openAdvancedFilter()
            }
            onNotificationsRequested: { if (App) App.navigate("notifications") }
        }
        StackView {
            id: stack
            objectName: "pageStack"
            width: parent.width
            height: parent.height - navBar.height - (tabBar.visible ? tabBar.height : 0)
            // Login gate mirrors HomeShell: unauthenticated → LoginPage first.
            // chargingArg (preview CLI --arg=JSON) 作深链路由参数透传。
            Component.onCompleted: pushRoute(App && App.loggedIn ? shell.route : "login",
                (typeof chargingArg !== "undefined" && chargingArg !== null
                 && (typeof chargingArg === "string" ? chargingArg.length > 0 : true))
                    ? chargingArg : undefined)
            // Transitions kept to a bare cross-fade: x-sliding the whole page
            // forces full-subtree repaints per frame and crawls under the
            // software renderer on this VM (2026-09-07 user feedback).
            pushEnter: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1
                    duration: P.Style.motionEnabled ? P.Style.durEnter : 0 }
            }
            popExit: Transition {
                NumberAnimation { property: "opacity"; from: 1; to: 0
                    duration: P.Style.motionEnabled ? P.Style.durExit : 0 }
            }
            replaceEnter: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1
                    duration: P.Style.motionEnabled ? P.Style.durEnter : 0 }
            }
            replaceExit: Transition {
                NumberAnimation { property: "opacity"; from: 1; to: 0
                    duration: P.Style.motionEnabled ? P.Style.durExit : 0 }
            }
            onCurrentItemChanged: {
                if (currentItem && currentItem.route)
                    for (var i = 0; i < tabIds.length; ++i)
                        if (tabIds[i] === currentItem.route) { tabBar.currentTab = currentItem.route; break }
            }
        }
        P.BottomTabBar {
            id: tabBar
            width: parent.width
            // Login has no bottom navigation, including its reserved layout space.
            visible: !!(App && App.loggedIn && stack.currentItem
                        && stack.currentItem.route !== "login")
            tabs: [{ id: "station", text: "🔍 找站" }, { id: "order", text: "📋 订单" },
                   { id: "charging", text: "⚡ 充电" }, { id: "profile", text: "👤 我的" }]
            currentTab: "station"
            // pushRoute now clears/replaces for tab targets — no manual pop loop.
            enabled: visible
            onTabChanged: (id) => { if (App && App.loggedIn) App.navigate(id) }
        }
    }

    P.Toast { id: toast }

    P.LoadingOverlay { id: overlay; running: !!(App && App.checkingOrders) }
}
