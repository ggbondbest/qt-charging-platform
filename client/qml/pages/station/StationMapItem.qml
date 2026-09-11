import QtQuick
import "../../platform" as P

// Coordinate overview only; it plots real station points without pretending to
// be a geographic basemap. NavigationPage owns the actual Tencent WebEngine map.
// 职责：零网络/零 key 的"坐标示意"绘图组件——markers（站点圆点）与 route
// （[lat,lng] 折线）按经纬度线性映射铺满画布（x/y 独立缩放，不锁纵横比），
// 只画真点位、绝不伪造底图路网，画不出内容就直说"等待真实点位"。
// 进用：曾嵌于 StationHomePage 的 map⇄list 分段（e8546fa 换腾讯真图后撤下）；
// 真底图归 StationMapView（WebEngine 壳），本组件留作无 key/断网降级示意。
// 数据流：纯 props 进（markers/route）、markerClicked(index) 信号出，无桥无服务。
Item {
    id: mapItem
    objectName: "stationMapItem"

    // [{lat, lng, label, selected}] — station dots (list page).
    property var markers: []
    // [[lat, lng], ...] — polyline (navigation page).
    property var route: []
    // Fallback center when nothing to fit (22.541,113.943 = panel's default).
    property real centerLat: 22.541
    property real centerLng: 113.943
    readonly property bool hasContent: (markers && markers.length > 0)
                                       || (route && route.length > 0)

    signal markerClicked(int index)

    // Canvas 内容不参与属性绑定：外部数据变了不会自动重画，必须手动 requestPaint
    // 排一帧 onPaint（Qt6.2 Canvas 语义，漏调就"数据对、画面旧"）。
    onMarkersChanged: canvas.requestPaint()
    onRouteChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        radius: P.Style.radiusMd
        color: P.Style.brandSoft
        border.color: P.Style.line; border.width: 1
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: 2
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const W = width, H = height, PAD = 14
            // 首帧/隐藏态尺寸可能为 0：直接返回，否则后面除法变 0 除产生 NaN 点。
            if (W <= 0 || H <= 0) return

            // Fit bounds over markers ∪ route.
            let loLa = Infinity, hiLa = -Infinity, loLn = Infinity, hiLn = -Infinity
            const bump = (la, ln) => {
                if (la < loLa) loLa = la
                if (la > hiLa) hiLa = la
                if (ln < loLn) loLn = ln
                if (ln > hiLn) hiLn = ln
            }
            for (const m of (markers || [])) bump(+m.lat, +m.lng)
            // Route points are [lat,lng] pairs.
            // 坏元素跳过而非画歪：路线来自桥侧，形状不保证全是二元数组。
            for (const p of (route || [])) {
                if (Array.isArray(p) && p.length >= 2) bump(+p[0], +p[1])
            }
            // 三行退化保护：没有任何点位→以默认中心撑 0.01° 小框；所有点同纬/同经→
            // 边界撑开 0.005°。否则 (hi-lo) 为 0，缩放除法出 NaN/Inf，整幅画布画空。
            if (!isFinite(loLa)) { bump(centerLat, centerLng); bump(centerLat + 0.01, centerLng + 0.01) }
            if (hiLa - loLa < 1e-6) { loLa -= 0.005; hiLa += 0.005 }
            if (hiLn - loLn < 1e-6) { loLn -= 0.005; hiLn += 0.005 }
            // 经纬→屏幕：y 轴翻转（屏幕 y 向下、纬度向上），保持"北在上"的地图直觉。
            const sx = ln => PAD + (ln - loLn) / (hiLn - loLn) * (W - 2 * PAD)
            const sy = la => H - PAD - (la - loLa) / (hiLa - loLa) * (H - 2 * PAD)

            // Coordinate grid; do not invent rivers, roads or route geometry.
            ctx.strokeStyle = Qt.alpha(P.Style.brand, 0.10)
            ctx.lineWidth = 1
            for (let gx = 0; gx < W; gx += 36) { ctx.beginPath(); ctx.moveTo(gx, 0); ctx.lineTo(gx, H); ctx.stroke() }
            for (let gy = 0; gy < H; gy += 36) { ctx.beginPath(); ctx.moveTo(0, gy); ctx.lineTo(W, gy); ctx.stroke() }

            // Route polyline (navigation): brand line under a white halo.
            if (route && route.length >= 2) {
                ctx.lineCap = "round"; ctx.lineJoin = "round"
                ctx.strokeStyle = P.Style.surface
                ctx.lineWidth = 7
                ctx.beginPath()
                ctx.moveTo(sx(+route[0][1]), sy(+route[0][0]))
                for (let i = 1; i < route.length; ++i) ctx.lineTo(sx(+route[i][1]), sy(+route[i][0]))
                ctx.stroke()
                // 同一 path 连描两遍（先宽白后窄品牌色）即"白描光晕"，省一次路径重建；
                // Canvas 的 current path 在 beginPath 前持续有效。
                ctx.strokeStyle = P.Style.brand
                ctx.lineWidth = 4
                ctx.stroke()
                // End pin.
                const last = route[route.length - 1]
                ctx.fillStyle = P.Style.danger
                ctx.beginPath(); ctx.arc(sx(+last[1]), sy(+last[0]), 6, 0, 2 * Math.PI); ctx.fill()
            }

            // Station dots (list page); selected enlarges + ring.
            for (let i = 0; i < (markers || []).length; ++i) {
                const m = markers[i]
                const x = sx(+m.lng), y = sy(+m.lat)
                if (m.selected) {
                    ctx.strokeStyle = P.Style.brandDeep; ctx.lineWidth = 3
                    ctx.beginPath(); ctx.arc(x, y, 11, 0, 2 * Math.PI); ctx.stroke()
                }
                ctx.fillStyle = m.selected ? P.Style.brandDeep : P.Style.brand
                ctx.beginPath(); ctx.arc(x, y, 6, 0, 2 * Math.PI); ctx.fill()
                ctx.fillStyle = P.Style.surface
                ctx.beginPath(); ctx.arc(x, y, 2.5, 0, 2 * Math.PI); ctx.fill()
            }

            // 没内容只写提示、不落一个假点——"示意"页的诚实口径。
            if (!hasContent) {
                ctx.fillStyle = P.Style.muted
                ctx.font = P.Style.fontSm + 'px sans-serif'
                ctx.textAlign = "center"
                ctx.fillText("地图示意（等待真实点位）", W / 2, H / 2)
            }
        }
    }

    // Click hit-test (topmost marker wins).
    Text {
        anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 6
        text: "站点坐标示意 · 导航请点卡片上的导航按钮"
        font.pixelSize: 11; color: P.Style.muted
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onClicked: mouse => {
            const W = width, H = height, PAD = 14
            // Re-derive the same fit transform as onPaint（与绘制端逐字同构：markers ∪ route
            // 一起入界；比较式天然跳 NaN——Math.min/max 会被 NaN 污染导致全组点击永久失效）。
            let loLa = Infinity, hiLa = -Infinity, loLn = Infinity, hiLn = -Infinity
            const bump = (la, ln) => {
                if (la < loLa) loLa = la
                if (la > hiLa) hiLa = la
                if (ln < loLn) loLn = ln
                if (ln > hiLn) hiLn = ln
            }
            for (const m of (mapItem.markers || [])) bump(+m.lat, +m.lng)
            for (const p of (mapItem.route || [])) {
                if (Array.isArray(p) && p.length >= 2) bump(+p[0], +p[1])
            }
            // 无 markers 或边界仍未撑起（全坏点）时直接短路：纯折线/空图不发明中信号。
            if (!isFinite(loLa) || !mapItem.markers || mapItem.markers.length === 0) return
            if (hiLa - loLa < 1e-6) { loLa -= 0.005; hiLa += 0.005 }
            if (hiLn - loLn < 1e-6) { loLn -= 0.005; hiLn += 0.005 }
            // 倒序遍历=后画的点压在上面，命中按"顶层先"，与视觉层序一致。
            for (let i = (mapItem.markers || []).length - 1; i >= 0; --i) {
                const m = mapItem.markers[i]
                const la = +m.lat, ln = +m.lng
                if (!isFinite(la) || !isFinite(ln)) continue   // 坐标坏点不参与点击
                const x = PAD + (ln - loLn) / (hiLn - loLn) * (W - 2 * PAD)
                const y = H - PAD - (la - loLa) / (hiLa - loLa) * (H - 2 * PAD)
                // 命中容差半径 12 > 视觉半径 6：手指戳小圆点也点得中。
                if (Math.hypot(mouse.x - x, mouse.y - y) <= 12) { mapItem.markerClicked(i); return }
            }
        }
    }
}
