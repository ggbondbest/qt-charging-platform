pragma Singleton
import QtQuick

// Design tokens mirrored from resources/qss/client_platform.qss and
// client/widgets/include/.../motion.h. Pages must use Style.* — no literals.
QtObject {
    // palette
    readonly property color ink: "#1F2937"
    readonly property color muted: "#6B7280"
    readonly property color faint: "#9CA3AF"
    readonly property color bg: "#F2F5F8"
    readonly property color surface: "#FFFFFF"
    readonly property color line: "#E5E9EF"
    readonly property color lineStrong: "#D5DCE4"
    readonly property color ghost: "#EEF2F6"
    readonly property color brand: "#00B578"
    readonly property color brandDeep: "#00A76D"
    readonly property color brandSoft: "#EAF9F2"
    readonly property color brandEdge: "#B7E5D2"
    readonly property color brandBright: "#2BC98A"
    readonly property color danger: "#DC2626"
    readonly property color dangerDeep: "#B91C1C"
    readonly property color dangerSoft: "#FDEBEB"
    readonly property color warning: "#D97706"
    readonly property color warningSoft: "#FFF4E0"
    readonly property color info: "#1971C2"
    readonly property color infoSoft: "#E8F3FE"

    // type scale (px, matches QSS font-size ladder)
    readonly property int fontXl: 21
    readonly property int fontLg: 15
    readonly property int fontMd: 14
    readonly property int fontSm: 12

    // geometry
    readonly property int radiusSm: 10
    readonly property int radiusMd: 12
    readonly property int radiusLg: 16
    readonly property int radiusChip: 24
    readonly property int radiusPill: 999
    readonly property int spaceXs: 6
    readonly property int spaceSm: 8
    readonly property int spaceMd: 12
    readonly property int spaceLg: 14
    readonly property int spaceXl: 20

    // motion tokens (microseconds→ms values from motion.h)
    readonly property int durMicro: 80
    readonly property int durEnter: 180
    readonly property int durExit: 120
    readonly property int durValue: 140
    readonly property int durBreathe: 1600
    readonly property int staggerStep: 40
    readonly property int staggerMax: 8
    // Reduced-motion / offscreen gate: wrap every duration as
    //   motionEnabled ? Style.durEnter : 0
    // main.cpp sets this after engine load (env MOTION_REDUCED / offscreen qpa).
    property bool motionEnabled: true

    // pull-to-refresh geometry (PullToRefreshArea constants)
    readonly property int pullActivatePx: 8
    readonly property int pullThresholdPx: 56
    readonly property int pullRestGapPx: 44
}
