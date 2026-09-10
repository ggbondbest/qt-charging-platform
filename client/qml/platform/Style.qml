pragma Singleton
import QtQuick

// Design tokens mirrored from resources/qss/client_platform.qss and
// client/widgets/include/.../motion.h. Pages must use Style.* — no literals.
//
// 主题/字号（2026-09-08 批次A）：theme/fontScale 是可写入口属性（Shell 顶层
// 从 settingsService 同步），下列颜色/字号令牌皆为对它们的**绑定**——readonly
// 只禁外部赋值，不碍依赖重算。两个刻意不随主题走的例外：
// 1) 渐变令牌 heroFrom/heroTo/heroPhone——绑 pal.*（GradientStop.color 首评后
//    不刷新的 9/7 实测坑仍在：定格改为"启动期一次性定型"，运行中换配色档，
//    hero 渐变要等页面重建/重启才翻新——设置页文案已按此口径说明）；
// 2) brand 主色系与 pressed 档——同一配色档内跨明暗不变；跨配色档由下方
//    palette 令牌族整体换色（2026-09-10 配色批，键值域同 SettingsService）。
QtObject {
    // —— 外观入口（默认亮色 + 标准字号；值域见 SettingsService 注释）——
    property string theme: "light"
    property string fontScale: "standard"
    property string palette: "green"
    readonly property bool dark: theme === "dark"
    readonly property real fontScaleFactor:
        fontScale === "large" ? 1.18 : fontScale === "extraLarge" ? 1.36 : 1.0

    // —— 配色档（2026-09-10 配色批）：品牌令牌族按 palette 键查表，脏键回退
    // green。label 供设置页色板直读，色值全站单源于此表（页面零字面量口径）。
    readonly property var paletteKeys: ["green", "blue", "violet", "amber"]
    readonly property var paletteSpecs: ({
        green:  { label: "电动绿", brand: "#00B578", deep: "#00A76D", bright: "#2BC98A",
                  pressed: "#009A66", softLight: "#EAF9F2", softDark: "#17322A",
                  edgeLight: "#B7E5D2", edgeDark: "#234A3E",
                  heroFrom: "#00A46C", heroTo: "#2BC98A", heroPhone: "#D9F3E7" },
        blue:   { label: "海洋蓝", brand: "#2374C8", deep: "#1B63AB", bright: "#4E9BE0",
                  pressed: "#15518F", softLight: "#E9F3FD", softDark: "#16283C",
                  edgeLight: "#BCDCF7", edgeDark: "#2A4A6B",
                  heroFrom: "#1C65B0", heroTo: "#4E9BE0", heroPhone: "#DCECFB" },
        violet: { label: "暮紫", brand: "#7C5CDB", deep: "#6A4CC2", bright: "#9B82E8",
                  pressed: "#573E9E", softLight: "#F1EDFC", softDark: "#251F3C",
                  edgeLight: "#D5C9F5", edgeDark: "#43386B",
                  heroFrom: "#6A4CC2", heroTo: "#9B82E8", heroPhone: "#E7DFFA" },
        amber:  { label: "琥珀橙", brand: "#DE7C1F", deep: "#B86114", bright: "#F09B3F",
                  pressed: "#8F4B0F", softLight: "#FDF1E3", softDark: "#3A2A17",
                  edgeLight: "#F5D5AC", edgeDark: "#6B4A24",
                  heroFrom: "#C96A12", heroTo: "#F09B3F", heroPhone: "#FBE8CF" }
    })
    readonly property var pal: paletteSpecs[palette] !== undefined
        ? paletteSpecs[palette] : paletteSpecs.green

    // palette（body bg 对齐 QSS 主底色 #F4F6F8；dark 为中性蓝黑系）
    readonly property color ink: dark ? "#E6EAF0" : "#1F2937"
    readonly property color muted: dark ? "#9BA6B2" : "#6B7280"
    readonly property color faint: dark ? "#7C8794" : "#9CA3AF"
    readonly property color bg: dark ? "#15181D" : "#F4F6F8"
    readonly property color surface: dark ? "#1E242C" : "#FFFFFF"
    readonly property color line: dark ? "#2A313B" : "#E5E9EF"
    readonly property color lineStrong: dark ? "#3A424E" : "#D5DCE4"
    readonly property color ghost: dark ? "#242B34" : "#EEF2F6"
    readonly property color brand: pal.brand
    readonly property color brandDeep: pal.deep
    readonly property color brandSoft: dark ? pal.softDark : pal.softLight
    readonly property color brandEdge: dark ? pal.edgeDark : pal.edgeLight
    readonly property color brandBright: pal.bright
    // 语义色：暗底上提亮保证文本对比度（soft 档同步换深色低饱和底）
    readonly property color danger: dark ? "#F16A6A" : "#DC2626"
    readonly property color dangerDeep: dark ? "#F16A6A" : "#B91C1C"
    readonly property color dangerSoft: dark ? "#3A2124" : "#FDEBEB"
    readonly property color warning: dark ? "#E9A23B" : "#D97706"
    readonly property color warningSoft: dark ? "#382B16" : "#FFF4E0"
    readonly property color info: dark ? "#5C9CE0" : "#1971C2"
    readonly property color infoSoft: dark ? "#1B2A3A" : "#E8F3FE"
    readonly property color starGold: "#FBBF24"
    // QSS 三形态补全：solid 按下加深 / disabled 浅化 / hero 对角渐变端
    readonly property color brandPressed: pal.pressed
    readonly property color dangerPressed: "#B91C1C"
    readonly property color disabledFg: "#F2FBF7"
    readonly property color heroFrom: pal.heroFrom     // 一次性求值（见头注 1）
    readonly property color heroTo: pal.heroTo
    readonly property color heroPhone: pal.heroPhone

    // type scale (px, matches QSS font-size ladder) × 字号档位系数
    readonly property int fontXl: Math.round(21 * fontScaleFactor)
    readonly property int fontHero: Math.round(19 * fontScaleFactor)   // QSS hero nickname
    readonly property int fontGlyph: 34  // QSS chargingHero 空态大图标（emoji 不放大）
    readonly property int fontPower: Math.round(34 * fontScaleFactor)  // QSS powerValue 34/800 主视觉
    readonly property int fontStat: Math.round(20 * fontScaleFactor)   // QSS statValue 20/800 三列统计
    readonly property int fontLg2: Math.round(16 * fontScaleFactor)    // QSS heroTitle/cellChevron
    readonly property int fontLg: Math.round(15 * fontScaleFactor)
    readonly property int fontMd: Math.round(14 * fontScaleFactor)
    readonly property int fontSm: Math.round(12 * fontScaleFactor)
    readonly property int fontXs: Math.round(11 * fontScaleFactor)     // QSS caption/badge 档

    // geometry
    readonly property int radiusSm: 10
    readonly property int radiusMd: 12
    readonly property int radiusLg: 16
    readonly property int radiusChip: 24  // QSS primary/danger/secondary 按钮圆角
    readonly property int radiusTag: 9    // QSS uiStatusTag 小圆角标签
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
