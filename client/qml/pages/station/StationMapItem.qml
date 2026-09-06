import QtQuick
import "../../platform" as P

// Schematic station/route map — QML twin of the StationMapPanel *degraded* state
// (and the offscreen-safe stand-in for the real WebEngine map tonight).
// Decision record: the task book offered a QQuickPaintedItem C++ wrapper, but type
// registration would touch client/qml/main.cpp (member-3 territory) and the CMake
// mount only forwards .qml — so this sprint paints with Canvas (zero deps,
// grabToImage-safe). Real map = QtWebEngineQuick WebEngineView after cutover
// (separate binary ⇒ no WebEngineWidgets conflict).
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
            for (const p of (route || [])) {
                if (Array.isArray(p) && p.length >= 2) bump(+p[0], +p[1])
            }
            if (!isFinite(loLa)) { bump(centerLat, centerLng); bump(centerLat + 0.01, centerLng + 0.01) }
            if (hiLa - loLa < 1e-6) { loLa -= 0.005; hiLa += 0.005 }
            if (hiLn - loLn < 1e-6) { loLn -= 0.005; hiLn += 0.005 }
            const sx = ln => PAD + (ln - loLn) / (hiLn - loLn) * (W - 2 * PAD)
            const sy = la => H - PAD - (la - loLa) / (hiLa - loLa) * (H - 2 * PAD)

            // Schematic backdrop: soft grid + a diagonal "river".
            ctx.strokeStyle = Qt.alpha(P.Style.brand, 0.10)
            ctx.lineWidth = 1
            for (let gx = 0; gx < W; gx += 36) { ctx.beginPath(); ctx.moveTo(gx, 0); ctx.lineTo(gx, H); ctx.stroke() }
            for (let gy = 0; gy < H; gy += 36) { ctx.beginPath(); ctx.moveTo(0, gy); ctx.lineTo(W, gy); ctx.stroke() }
            ctx.strokeStyle = Qt.alpha(P.Style.info, 0.18)
            ctx.lineWidth = 10
            ctx.beginPath(); ctx.moveTo(0, H * 0.75); ctx.lineTo(W, H * 0.25); ctx.stroke()

            // Route polyline (navigation): brand line under a white halo.
            if (route && route.length >= 2) {
                ctx.lineCap = "round"; ctx.lineJoin = "round"
                ctx.strokeStyle = P.Style.surface
                ctx.lineWidth = 7
                ctx.beginPath()
                ctx.moveTo(sx(+route[0][1]), sy(+route[0][0]))
                for (let i = 1; i < route.length; ++i) ctx.lineTo(sx(+route[i][1]), sy(+route[i][0]))
                ctx.stroke()
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

            if (!hasContent) {
                ctx.fillStyle = P.Style.muted
                ctx.font = P.Style.fontSm + 'px sans-serif'
                ctx.textAlign = "center"
                ctx.fillText("地图示意（等待真实点位）", W / 2, H / 2)
            }
        }
    }

    // Click hit-test (topmost marker wins).
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
            if (!isFinite(loLa) || !mapItem.markers || mapItem.markers.length === 0) return
            if (hiLa - loLa < 1e-6) { loLa -= 0.005; hiLa += 0.005 }
            if (hiLn - loLn < 1e-6) { loLn -= 0.005; hiLn += 0.005 }
            for (let i = (mapItem.markers || []).length - 1; i >= 0; --i) {
                const m = mapItem.markers[i]
                const la = +m.lat, ln = +m.lng
                if (!isFinite(la) || !isFinite(ln)) continue   // 坐标坏点不参与点击
                const x = PAD + (ln - loLn) / (hiLn - loLn) * (W - 2 * PAD)
                const y = H - PAD - (la - loLa) / (hiLa - loLa) * (H - 2 * PAD)
                if (Math.hypot(mouse.x - x, mouse.y - y) <= 12) { mapItem.markerClicked(i); return }
            }
        }
    }
}
