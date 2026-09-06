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
    readonly property var migrated: ["wallet", "recharge", "order", "order_detail",
                                     "charging", "charging_run", "settlement",
                                     "profile_edit"]
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
        }
        if (!t[r]) return ""
        return migrated.indexOf(r) >= 0 ? t[r] : "pages/PlaceholderPage.qml"
    }
    function pushRoute(r) {
        const src = pageSource(r)
        if (src.length === 0) return
        stack.push(Qt.resolvedUrl(src), { route: r })
        for (var i = 0; i < tabIds.length; ++i)
            if (tabIds[i] === r) { tabBar.setCurrentTab(r); break }
    }
    function pop() { if (stack.depth > 1) stack.pop() }

    // Wire the contract's routing bridge (App.navigate / App.back).
    Connections {
        target: App
        function onNavigateRequested(route) { shell.pushRoute(route) }
        function onBackRequested() { shell.pop() }
        function onToastRequested(text, tone) { toast.show(text, tone) }
        function onLoginStateChanged() {
            if (!App.loggedIn) shell.pushRoute("login")
            else shell.pushRoute("station")
        }
    }

    Column {
        anchors.fill: parent
        P.TopNavBar {
            id: navBar
            width: parent.width
            user: App && App.currentUser ? App.currentUser : null
            backVisible: stack.depth > 1
            searchVisible: shell.route === "station"
            onBackRequested: shell.pop()
            onSearchSubmitted: (keyword) => { if (App) App.navigate("station") }
            onLoginRequested: shell.pushRoute("login")
            onProfileRequested: { if (App) App.navigate("profile") }
            onFilterRequested: { /* StationFilterDialog.qml — member 2's domain */ }
            onNotificationsRequested: { if (App) App.navigate("notifications") }
        }
        StackView {
            id: stack
            width: parent.width
            height: parent.height - navBar.height - tabBar.height
            // Login gate mirrors HomeShell: unauthenticated → LoginPage first.
            Component.onCompleted: pushRoute(App && App.loggedIn ? shell.route : "login")
            pushEnter: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1
                    duration: P.Style.motionEnabled ? P.Style.durEnter : 0 }
            }
            popExit: Transition {
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
            tabs: [{ id: "station", text: "🔍 找站" }, { id: "order", text: "📋 订单" },
                   { id: "charging", text: "⚡ 充电" }, { id: "profile", text: "👤 我的" }]
            currentTab: "station"
            onTabChanged: (id) => { while (stack.depth > 1) stack.pop(); shell.pushRoute(id) }
        }
    }

    P.Toast { id: toast }

    P.LoadingOverlay { id: overlay; running: false }
}
