#pragma once
#include <QHash>
#include <QQuickImageProvider>
#include <QPixmap>

namespace charging::qml {

// image://glyphs/<name>/<hexcolor> — 单色 glyph 染色 provider。
// 母版为 128px 黑底 alpha PNG（qrc:/charging/assets/glyphs/，Tabler outline
// 集离线烘焙，MIT 署名同目录），此处按需缩放到 requestedSize 并把黑色像素
// 替换为指定色（Painter CompositionMode_SourceIn，保留母版 alpha），带缓存：
// 同一"母版×颜色×尺寸"只烘一次。QML 端把主题 token 的 hex 直接拼进 URL，
// 暗/亮主题自动换色。
class GlyphProvider final : public QQuickImageProvider
{
public:
    GlyphProvider();
    QPixmap requestPixmap(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    QPixmap master(const QString& name);
    QHash<QString, QPixmap> masters_;   // name -> 黑底母版（空图也缓存，不反复开文件）
    QHash<QString, QPixmap> tinted_;    // name|color|edge -> 染色成品
};

} // namespace charging::qml
