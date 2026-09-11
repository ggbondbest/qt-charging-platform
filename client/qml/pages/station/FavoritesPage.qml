import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// QML twin of widgets FavoritesPage (objectName "favoritesPage").
// 列表源 = 全量搜索结果 ∩ favoritesService.favoriteIds()（与 widgets 同口径：
// 取消收藏即时从列表消失，靠 favoritesChanged 重算投影）。高级筛选复用
// StationFilterDialog；卡片副行"空闲 n/m / 桩位已满"与距离行逐字对齐。
// route "favorites"：从"我的"页 ⭐收藏 行（openFavoritesButton → App.navigate）进入，
// Shell migrated 白名单已收，路由翻真页。
// 数据流：stationQueryService.search("") → 成功信号存全量 raw → project() 做
// "收藏交集 + 八组筛选"投影灌 favModel；重算触发=query成功/筛选 onApplied/
// favoritesChanged/定位变化 四路同源。属 station 域 P0 六页批。
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
    // ---- 桥调用全走 try/catch：桥缺位=判否/空集/失败态，页面永不因服务缺席崩 ----
    // 供壳顶栏漏斗接线（同 StationHomePage 口径）：成员3 一行接通即活。
    function openAdvancedFilter() { filterDialog.openDialog(page.criteria) }
    function isFav(id) {
        // 判"在收藏"从严：桥没到位一律 false——宁可少亮星标不可假亮。
        try { return favoritesService ? favoritesService.contains(id) : false }
        catch (e) { return false }
    }
    // 桥未补时恒空集=交集判据全滤掉，列表诚实为空，不拿全量站点冒充收藏。
    function favIds() {
        // TODO(contract): favoritesService.favoriteIds() invokable（桥未补 → 空集）。
        try { return (favoritesService && favoritesService.favoriteIds()) || [] }
        catch (e) { return [] }
    }
    function refresh() {
        loading = true; failed = false
        // 收藏页不带关键词——全量拉取后取交集（widgets 同做法）。
        // 全量拉取是"发起"不是"等值"：真值走 onQuerySucceeded 信号，这里只拨状态。
        try { stationQueryService.search("") }
        catch (e) { loading = false; failed = true; failMessage = "收藏桥未就绪" }
    }
    // ---- 本地投影：收藏交集 ∩ 八组筛选（口径与 StationHomePage.project 同源）----
    function project() {
        // 收藏交集先行：非收藏条目直接短路，后面八组条件不必白算。
        const ids = favIds()
        const c = page.criteria
        const rows = []
        for (const source of (page.raw || [])) {
            const s = Object.assign({}, source)
            s.distanceMeters = typeof s.latitude === "number" && typeof s.longitude === "number"
                ? mapBridge.distanceMeters(s.latitude, s.longitude) : -1
            if (ids.indexOf(s.id) < 0) continue
            // 勾选距离档时 -1（未定位）一律出局：距离未知的站不配自称"在 5 公里内"。
            if (c.maxDistanceKm > 0 && !(s.distanceMeters >= 0 && s.distanceMeters <= c.maxDistanceKm * 1000)) continue
            // 服务侧状态是英文枚举，筛选键是中文选项字面量——先映射再比，键名两侧一致。
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
                stationId: String(s.id), name: s.name, address: s.address,
                latitude: s.latitude, longitude: s.longitude,
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
        target: mapBridge
        function onLocationChanged() { page.project() }
    }
    // 取消收藏即时消失：favoritesChanged 只重算投影不重拉——raw 仍是全量，
    // 交集判据变了就够，省一次服务往返。
    Connections {
        target: favoritesService
        function onFavoritesChanged() { page.project() }   // 取消收藏即时消失
    }
    Connections {
        target: filterDialog
        // 草稿整批提交后才重投影：勾选过程中列表不跟着抖，取消则 criteria 原样。
        function onApplied(criteria) { page.criteria = criteria; page.project() }
    }
    // 进页即拉全量；桥未就绪时 refresh 内 try/catch 落失败态，页不崩。
    Component.onCompleted: refresh()

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd

        Row {
            width: parent.width
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 110
                objectName: "favoritesPageTitle"
                text: "我的收藏"; font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink
            }
            P.ActionButton {
                objectName: "favoritesFilterButton"
                variant: "ghost"; text: "▽ 高级筛选"
                anchors.verticalCenter: parent.verticalCenter
                onClicked: filterDialog.openDialog(page.criteria)
            }
        }

        ListView {
            id: favList
            objectName: "favoritesList"
            width: parent.width
            // Column 不分高度：手拼"剩余高" = 父高 − 起点 y，否则列表按内容收缩滚不动。
            height: parent.height - y
            clip: true
            spacing: P.Style.spaceSm
            visible: favModel.count > 0
            model: ListModel { id: favModel }
            delegate: P.ClickableCard {
                objectName: "favoriteCard"
                width: favList.width
                onClicked: {
                    // App.navigate 是导航单点漏斗（带 arg 直达详情）；withArguments 仅给 C++ 孪生用。
                    if (App) App.navigate("station_detail", {
                        id: stationId, name: name, address: address,
                        priceCentsPerKwh: priceCentsPerKwh,
                        distanceMeters: distanceMeters, status: status,
                        latitude: latitude, longitude: longitude })
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
                        // 距离行：-1（未定位）显示"--"而非 0m，不骗"就在旁边"；≥1km 折一位小数。
                        Text {
                            text: "¥" + page.money(priceCentsPerKwh) + "/kWh · "
                                  + (availableChargers > 0 ? "空闲 " + availableChargers + "/" + totalChargers
                                                           : "桩位已满")
                                  + " · 直线距离 "
                                  + (distanceMeters < 0 ? "--"
                                        : distanceMeters >= 1000 ? (distanceMeters / 1000).toFixed(1) + "km"
                                        : distanceMeters + "m")
                            font.pixelSize: P.Style.fontSm; color: P.Style.brandDeep
                        }
                    }
                    MouseArea {
                        objectName: "unfavoriteStarButton"
                        width: 34; height: parent.height
                        Image {
                            anchors.centerIn: parent
                            width: Math.round(20 * P.Style.fontScaleFactor)
                            height: width
                            source: Glyphs.source("star-filled", P.Style.warning)
                        }
                        onClicked: {   // 取消收藏
                            // 吞异常：桥没就绪时点击无响应即可，页面不弹错不打断浏览。
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
            glyph: page.failed ? "triangle-alert" : "star"
            glyphColor: page.failed ? P.Style.warning : P.Style.ink
            title: page.failed ? "收藏列表加载失败"
                 : !page.loaded ? "正在加载收藏站点…"
                 : "暂无收藏的充电站"
            // 空态文案按"真空收藏 vs 筛选没命中"分支：有 favIds 却 0 命中时引导放宽
            // 筛选，而不是让用户误以为收藏丢了。
            description: page.failed ? page.failMessage
                 : !page.loaded ? ""
                 : (favIds().length > 0 && favModel.count === 0
                    ? "当前筛选条件下没有收藏电站命中，试试放宽或重置筛选条件。"
                    : "在找站页点击卡片右下角的收藏星标即可收藏，收藏后在这里集中管理。")
            actionText: page.failed ? "重试" : ""
            onActionTriggered: page.refresh()
        }
    }

    StationFilterDialog { id: filterDialog }
}
