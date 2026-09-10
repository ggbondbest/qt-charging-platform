.pragma library
// image://glyphs URL 单点（provider 见 client/qml/glyph_provider.h）：
// 主题 token 的 hex 原样透传给 GlyphProvider 染色（QColor 接受
// rrggbb / aarrggbb），明暗主题自动换色。
function source(name, colorValue) {
    return "image://glyphs/" + name + "/" + String(colorValue).replace("#", "")
}
