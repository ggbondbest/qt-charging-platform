#include "charging/client/widgets/status_tag.h"

#include <QStyle>
#include <QVariant>

namespace charging::client {

StatusTag::StatusTag(const QString& text, Tone tone, QWidget* parent)
    : QLabel(text, parent)
{
    setObjectName(QStringLiteral("uiStatusTag"));
    setProperty("role", QStringLiteral("statusTag"));
    setTone(tone);
}

void StatusTag::setTone(Tone tone)
{
    setProperty("tone", toneName(tone));
    style()->unpolish(this);
    style()->polish(this);
    update();
}

QString StatusTag::toneName(Tone tone)
{
    switch (tone) {
    case Tone::Success:
        return QStringLiteral("success");
    case Tone::Warning:
        return QStringLiteral("warning");
    case Tone::Danger:
        return QStringLiteral("danger");
    case Tone::Info:
        return QStringLiteral("info");
    case Tone::Neutral:
        break;
    }
    return QStringLiteral("neutral");
}

} // namespace charging::client
