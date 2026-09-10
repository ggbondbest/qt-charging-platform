#include "glyph_provider.h"

#include <QColor>
#include <QPainter>

namespace charging::qml {
namespace {
constexpr int kDefaultEdge = 22;   // 未给 requestedSize 时的回退尺寸
}

GlyphProvider::GlyphProvider()
    : QQuickImageProvider(QQuickImageProvider::Pixmap) {}

QPixmap GlyphProvider::master(const QString& name)
{
    if (!masters_.contains(name)) {
        QPixmap px(QStringLiteral(":/charging/assets/glyphs/%1.png").arg(name));
        masters_.insert(name, px);
    }
    return masters_.value(name);
}

QPixmap GlyphProvider::requestPixmap(const QString& id, QSize* size, const QSize& requestedSize)
{
    const int slash = id.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0 || slash == id.size() - 1)
        return QPixmap();
    const QString name = id.left(slash);
    // 色值接受 rrggbb / aarrggbb（QML color.toString() 原样透传，含 alpha）。
    QColor color;
    if (QColor parsed = QColor(QStringLiteral("#") + id.mid(slash + 1)); parsed.isValid())
        color = parsed;
    else
        color = QColor(0, 0, 0);
    int edge = requestedSize.width() > 0 ? requestedSize.width() : kDefaultEdge;
    if (requestedSize.height() > 0)
        edge = qMin(edge, requestedSize.height());

    const QString key = name + QLatin1Char('|') + color.name(QColor::HexArgb)
                        + QLatin1Char('|') + QString::number(edge);
    if (tinted_.contains(key)) {
        const QPixmap cached = tinted_.value(key);
        if (size)
            *size = cached.size();
        return cached;
    }

    const QPixmap base = master(name);
    if (base.isNull())
        return QPixmap();   // 缺母版：Image 侧自行报缺失，不在这里造假图

    QPixmap px(edge, edge);
    px.fill(Qt::transparent);
    {
        QPainter p(&px);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawPixmap(px.rect(),
                     base.scaled(px.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        // SourceIn：目标（母版）alpha 保留，RGB 全部换成染色值。
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(px.rect(), color);
    }
    tinted_.insert(key, px);
    if (size)
        *size = px.size();
    return px;
}

} // namespace charging::qml
