pragma Singleton
import QtQuick

// Design tokens mirrored from resources/qss/client_platform.qss and
// client/widgets/include/.../motion.h. Pages must use Style.* — no literals.
//
// 主题/字号（2026-09-08 批次A）：theme/fontScale 是可写入口属性（Shell 顶层
// 从 settingsService 同步），下列颜色/字号令牌皆为对它们的**绑定**——readonly
// 只禁外部赋值，不碍依赖重算。两个刻意不随主题走的例外：
// 1) 渐变令牌 heroFrom/heroTo/heroPhone——Qt6.2 的 GradientStop.color 绑外部
//    属性首评后不刷新（9/7 实测坑），且品牌绿渐变白字在暗底上本就成立，
//    所以定格为常量；
// 2) brand 主色系与 pressed/disabled 档——品牌色跨主题不变是设计口径。
QtObject {
    // —— 外观入口（默认亮色 + 标准字号；值域见 SettingsService 注释）——
    property string theme: "light"
    property string fontScale: "standard"
    readonly property bool dark: theme === "dark"
    readonly property real fontScaleFactor:
        fontScale === "large" ? 1.18 : fontScale === "extraLarge" ? 1.36 : 1.0

    // palette（body bg 对齐 QSS 主底色 #F4F6F8；dark 为中性蓝黑系）
    readonly property color ink: dark ? "#E6EAF0" : "#1F2937"
    readonly property color muted: dark ? "#9BA6B2" : "#6B7280"
    readonly property color faint: dark ? "#7C8794" : "#9CA3AF"
    readonly property color bg: dark ? "#15181D" : "#F4F6F8"
    readonly property color surface: dark ? "#1E242C" : "#FFFFFF"
    readonly property color line: dark ? "#2A313B" : "#E5E9EF"
    readonly property color lineStrong: dark ? "#3A424E" : "#D5DCE4"
    readonly property color ghost: dark ? "#242B34" : "#EEF2F6"
    readonly property color brand: "#00B578"
    readonly property color brandDeep: "#00A76D"
    readonly property color brandSoft: dark ? "#17322A" : "#EAF9F2"
    readonly property color brandEdge: dark ? "#234A3E" : "#B7E5D2"
    readonly property color brandBright: "#2BC98A"
    // 语义色：暗底上提亮保证文本对比度（soft 档同步换深色低饱和底）
    readonly property color danger: dark ? "#F16A6A" : "#DC2626"
    readonly property color dangerDeep: dark ? "#F16A6A" : "#B91C1C"
    readonly property color dangerSoft: dark ? "#3A2124" : "#FDEBEB"
    readonly property color warning: dark ? "#E9A23B" : "#D97706"
    readonly property color warningSoft: dark ? "#382B16" : "#FFF4E0"
    readonly property color info: dark ? "#5C9CE0" : "#1971C2"
    readonly property color infoSoft: dark ? "#1B2A3A" : "#E8F3FE"
    // QSS 三形态补全：solid 按下加深 / disabled 浅化 / hero 对角渐变端
    readonly property color brandPressed: "#009A66"
    readonly property color dangerPressed: "#B91C1C"
    readonly property color disabledFg: "#F2FBF7"
    readonly property color heroFrom: "#00A46C"   // 定格：不随主题（见头注）
    readonly property color heroTo: "#2BC98A"
    readonly property color heroPhone: "#D9F3E7"

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
