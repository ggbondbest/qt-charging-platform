import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets FavoritesPage (objectName "favoritesPage").
// 列表源 = 全量搜索结果 ∩ favoritesService.favoriteIds()（与 widgets 同口径：
// 取消收藏即时从列表消失，靠 favoritesChanged 重算投影）。高级筛选复用
// StationFilterDialog；卡片副行"空闲 n/m / 桩位已满"与距离行逐字对齐。
Item {
    id: page
    objectName: "favoritesPage"
    property string route: "favorites"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property var raw: []
    property var criteria: ({ maxDistanceKm: 0, statuses: [], operators: [],
                              accessTypes: [], parkingFees: [], features: [],
                              chargerTypes: [], voltageBands: [] })
    property bool loading: false
    property bool loaded: false
    property bool failed: false
    property string failMessage: ""

    function money(c) { return ((c || 0) / 100).toFixed(2) }
    // 供壳顶栏漏斗接线（同 StationHomePage 口径）：成员3 一行接通即活。
    function openAdvancedFilter() { filterDialog.openDialog(page.criteria) }
    // 2026-09-08：页面右上"▽ 高级筛选"钮撤除（全局唯一入口=顶栏漏斗），
    // 激活组数经本属性由 Shell 绑到 nav.filterBadgeCount。
    function activeFilterCount() {
        const c = page.criteria
        let n = 0
        if (c.maxDistanceKm > 0) ++n
        n += (c.statuses.length > 0) + (c.operators.length > 0) + (c.accessTypes.length > 0)
           + (c.parkingFees.length > 0) + (c.features.length > 0)
           + (c.chargerTypes.length > 0) + (c.voltageBands.length > 0)
        return n
    }
    readonly property int activeFilterBadge: activeFilterCount()
    function isFav(id) {
        try { return favoritesService ? favoritesService.contains(id) : false }
        catch (e) { return false }
    }
    function favIds() {
        // TODO(contract): favoritesService.favoriteIds() invokable（桥未补 → 空集）。
        try { return (favoritesService && favoritesService.favoriteIds()) || [] }
        catch (e) { return [] }
    }
    function refresh() {
        loading = true; failed = false
        // 收藏页不带关键词——全量拉取后取交集（widgets 同做法）。
        try { stationQueryService.search("") }
        catch (e) { loading = false; failed = true; failMessage = "收藏桥未就绪" }
    }
    function project() {
        const ids = favIds()
        const c = page.criteria
        const rows = []
        for (const s of (page.raw || [])) {
            if (ids.indexOf(s.id) < 0) continue
            if (c.maxDistanceKm > 0 && !(s.distanceMeters >= 0 && s.distanceMeters <= c.maxDistanceKm * 1000)) continue
            if (c.statuses.length > 0) {
                const zh = String(s.status).toLowerCase() === "active" ? "营业中" : "暂停运营"
                if (c.statuses.indexOf(zh) < 0) continue
            }
            if (c.operators.length > 0 && c.operators.indexOf(s.operatorName) < 0) continue
            if (c.accessTypes.length > 0 && c.accessTypes.indexOf(s.accessType) < 0) continue
            if (c.parkingFees.length > 0 && c.parkingFees.indexOf(s.parkingFee) < 0) continue
            if (c.features.length > 0 && !(s.features || []).some(f => c.features.indexOf(f) >= 0)) continue
            if (c.chargerTypes.length > 0 && !(s.chargerTypes || []).some(t => c.chargerTypes.indexOf(t) >= 0)) continue
            if (c.voltageBands.length > 0) {
                const ok = (c.voltageBands.indexOf("低于700V") >= 0 && s.hasVoltageBelow700)
                           || (c.voltageBands.indexOf("700V及以上") >= 0 && s.hasVoltageAtLeast700)
                if (!ok) continue
            }
            rows.push(s)
        }
        favModel.clear()
        for (const s of rows) {
            favModel.append({
                stationId: s.id, name: s.name, address: s.address,
                priceCentsPerKwh: s.priceCentsPerKwh || 0,
                availableChargers: s.availableChargers || 0, totalChargers: s.totalChargers || 0,
                distanceMeters: s.distanceMeters === undefined ? -1 : s.distanceMeters,
                status: String(s.status).toLowerCase()
            })
        }
    }

    Connections {
        target: stationQueryService
        function onQueryStarted() { page.loading = true; page.failed = false }
        function onQuerySucceeded(stations) {
            page.loading = false; page.loaded = true; page.failed = false
            page.raw = stations || []
            page.project()
        }
        function onQueryFailed(message) {
            page.loading = false; page.loaded = false; page.failed = true; page.failMessage = message
        }
    }
    Connections {
        target: favoritesService
        function onFavoritesChanged() { page.project() }   // 取消收藏即时消失
    }
    Connections {
        target: filterDialog
        function onApplied(criteria) { page.criteria = criteria; page.project() }
    }
    Component.onCompleted: refresh()

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd

        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            objectName: "favoritesPageTitle"
            text: "我的收藏"; font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink
        }

        ListView {
            id: favList
            objectName: "favoritesList"
            width: parent.width
            height: parent.height - y
            clip: true
            spacing: P.Style.spaceSm
            visible: favModel.count > 0
            model: ListModel { id: favModel }
            delegate: P.ClickableCard {
                objectName: "favoriteCard"
                width: favList.width
                onClicked: {
                    if (App) App.navigate("station_detail", {
                        id: stationId, name: name, address: address,
                        priceCentsPerKwh: priceCentsPerKwh,
                        distanceMeters: distanceMeters, status: status })
                }
                Row {
                    width: parent.width        // Column 内容器：anchors.fill 被忽略且告警
                    spacing: P.Style.spaceMd
                    Column {
                        width: parent.width - 60
                        spacing: 2
                        Text { width: parent.width; elide: Text.ElideRight
                            text: name; font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
                        Text { width: parent.width; elide: Text.ElideRight
                            text: address; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                        Text {
                            text: "¥" + page.money(priceCentsPerKwh) + "/kWh · "
                                  + (availableChargers > 0 ? "空闲 " + availableChargers + "/" + totalChargers
                                                           : "桩位已满")
                                  + " · 距离 "
                                  + (distanceMeters < 0 ? "--"
                                        : distanceMeters >= 1000 ? (distanceMeters / 1000).toFixed(1) + "km"
                                        : distanceMeters + "m")
                            font.pixelSize: P.Style.fontSm; color: P.Style.brandDeep
                        }
                    }
                    MouseArea {
                        objectName: "unfavoriteStarButton"
                        width: 42; height: parent.height
                        Text {
                            anchors.centerIn: parent
                            text: "★"; font.pixelSize: 26
                            color: P.Style.starGold
                        }
                        onClicked: {   // 取消收藏
                            try { favoritesService.toggle(stationId) } catch (e) {}
                        }
                    }
                }
            }
        }

        P.NoticePanel {
            objectName: "favoritesNotice"
            width: parent.width
            height: parent.height - y
            visible: favModel.count === 0
            glyph: page.failed ? "⚠️" : "⭐"
            title: page.failed ? "收藏列表加载失败"
                 : !page.loaded ? "正在加载收藏站点…"
                 : "暂无收藏的充电站"
            description: page.failed ? page.failMessage
                 : !page.loaded ? ""
                 : (favIds().length > 0 && favModel.count === 0
                    ? "当前筛选条件下没有收藏电站命中，试试放宽或重置筛选条件。"
                    : "在找站页点击卡片右下角 ☆ 即可收藏，收藏后在这里集中管理。")
            actionText: page.failed ? "重试" : ""
            onActionTriggered: page.refresh()
        }
    }

    StationFilterDialog { id: filterDialog }
}
