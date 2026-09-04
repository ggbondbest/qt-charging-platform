#include "charging/client/profile_charging/avatar_library.h"

#include <QLabel>
#include <QPainter>
#include <QStyle>

namespace charging::client {

const QVector<AvatarSpec>& AvatarLibrary::all()
{
    // 色相彼此拉开、与平台主色（电动绿 #00B578）协调的一组圆底色。
    static const QVector<AvatarSpec> kAvatars = {
        {QStringLiteral("bolt"), QStringLiteral("⚡"), QRgb(0x00B578), QRgb(0x007A55)},
        {QStringLiteral("plug"), QStringLiteral("🔌"), QRgb(0x1971C2), QRgb(0x0F4C82)},
        {QStringLiteral("car"), QStringLiteral("🚗"), QRgb(0xD97706), QRgb(0x9A5403)},
        {QStringLiteral("leaf"), QStringLiteral("🌿"), QRgb(0x4CAF50), QRgb(0x2E7D32)},
        {QStringLiteral("cat"), QStringLiteral("🐱"), QRgb(0xE0855E), QRgb(0xB05A2E)},
        {QStringLiteral("panda"), QStringLiteral("🐼"), QRgb(0x607D8B), QRgb(0x37474F)},
        {QStringLiteral("moon"), QStringLiteral("🌙"), QRgb(0x7E57C2), QRgb(0x4527A0)},
        {QStringLiteral("rocket"), QStringLiteral("🚀"), QRgb(0xDC2626), QRgb(0x8F1D1D)},
    };
    return kAvatars;
}

bool AvatarLibrary::contains(const QString& key)
{
    for (const AvatarSpec& spec : all()) {
        if (spec.key == key) {
            return true;
        }
    }
    return false;
}

QPixmap AvatarLibrary::render(const QString& key, int sizePx)
{
    const AvatarSpec* found = nullptr;
    for (const AvatarSpec& spec : all()) {
        if (spec.key == key) {
            found = &spec;
            break;
        }
    }
    if (found == nullptr || sizePx <= 0) {
        return QPixmap();
    }

    // DPR=2 绘制保证小屏文字与圆边清晰；painter 坐标系仍是逻辑尺寸。
    constexpr int kDpr = 2;
    QPixmap pixmap(sizePx * kDpr, sizePx * kDpr);
    pixmap.setDevicePixelRatio(kDpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    QLinearGradient gradient(0, 0, sizePx, sizePx);
    gradient.setColorAt(0.0, found->startRgb);
    gradient.setColorAt(1.0, found->endRgb);
    painter.setBrush(gradient);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(0, 0, sizePx, sizePx);

    // 文字用画笔着色：NoPen 会连字形一起画成"隐形"，必须显式恢复。
    // 白色单色回退在深色渐变圆上对比足够；彩色 emoji（Noto Color Emoji）忽略画笔色。
    painter.setPen(QColor(0xFF, 0xFF, 0xFF));
    QFont font = painter.font();
    font.setPixelSize(static_cast<int>(sizePx * 0.52));
    painter.setFont(font);
    painter.drawText(QRect(0, 0, sizePx, sizePx), Qt::AlignCenter, found->glyph);
    painter.end();
    return pixmap;
}

void AvatarLibrary::applyToLabel(QLabel* label, const QString& key, const QString& fallbackText,
                                 int sizePx)
{
    if (label == nullptr) {
        return;
    }
    const QPixmap pixmap = render(key, sizePx);
    const bool hasAvatar = !pixmap.isNull();
    label->setProperty("hasAvatar", hasAvatar);
    if (hasAvatar) {
        label->clear();
        label->setPixmap(pixmap);
    } else {
        label->setText(fallbackText);
    }
    label->style()->unpolish(label);
    label->style()->polish(label);
}

} // namespace charging::client
