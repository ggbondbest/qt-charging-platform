#include "charging/client/widgets/action_button.h"

#include "charging/client/widgets/motion.h"

#include <QVariant>

namespace charging::client {

ActionButton::ActionButton(Variant variant, const QString& text, QWidget* parent)
    : QPushButton(text, parent)
{
    setObjectName(QStringLiteral("uiActionButton"));
    setProperty("variant", variantName(variant));
    setCursor(Qt::PointingHandCursor);
    if (variant == Variant::Chip) {
        setCheckable(true);
    }
    motion::attachPressDip(this); // 全端统一按压手感（offscreen 自动免装）。
}

QString ActionButton::variantName(Variant variant)
{
    switch (variant) {
    case Variant::Primary:
        return QStringLiteral("primary");
    case Variant::Secondary:
        return QStringLiteral("secondary");
    case Variant::Danger:
        return QStringLiteral("danger");
    case Variant::Ghost:
        return QStringLiteral("ghost");
    case Variant::Chip:
        break;
    }
    return QStringLiteral("chip");
}

} // namespace charging::client
