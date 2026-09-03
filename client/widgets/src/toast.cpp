#include "charging/client/widgets/toast.h"

#include <QFont>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPropertyAnimation>
#include <QTimer>
#include <QWidget>

namespace charging::client {

namespace {

QString toastToneName(StatusTag::Tone tone)
{
    switch (tone) {
    case StatusTag::Tone::Success:
        return QStringLiteral("success");
    case StatusTag::Tone::Danger:
        return QStringLiteral("danger");
    case StatusTag::Tone::Warning:
        return QStringLiteral("warning");
    case StatusTag::Tone::Info:
    case StatusTag::Tone::Neutral:
        break;
    }
    return QStringLiteral("neutral");
}

} // namespace

void Toast::show(QWidget* anchor, const QString& text, StatusTag::Tone tone)
{
    QWidget* host = anchor != nullptr ? anchor->window() : nullptr;
    if (host == nullptr || text.isEmpty()) {
        return;
    }

    // Only one toast is visible at a time; a new message replaces the old one.
    static QPointer<QFrame> activeToast;
    if (!activeToast.isNull()) {
        activeToast->deleteLater();
    }

    auto* frame = new QFrame(host);
    frame->setObjectName(QStringLiteral("uiToast"));
    frame->setProperty("tone", toastToneName(tone));

    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(18, 10, 18, 10);

    auto* label = new QLabel(text, frame);
    label->setObjectName(QStringLiteral("uiToastLabel"));
    QFont labelFont = label->font();
    labelFont.setPointSize(10);
    label->setFont(labelFont);
    layout->addWidget(label);

    auto* effect = new QGraphicsOpacityEffect(frame);
    effect->setOpacity(0.0);
    frame->setGraphicsEffect(effect);

    frame->adjustSize();
    frame->move((host->width() - frame->width()) / 2, 28);
    frame->show();
    frame->raise();
    activeToast = frame;

    auto* fadeIn = new QPropertyAnimation(effect, "opacity", frame);
    fadeIn->setDuration(140);
    fadeIn->setStartValue(0.0);
    fadeIn->setEndValue(1.0);
    fadeIn->start(QAbstractAnimation::DeleteWhenStopped);

    QTimer::singleShot(2200, frame, [frame, effect]() {
        auto* fadeOut = new QPropertyAnimation(effect, "opacity", frame);
        fadeOut->setDuration(220);
        fadeOut->setStartValue(effect->opacity());
        fadeOut->setEndValue(0.0);
        QObject::connect(fadeOut, &QPropertyAnimation::finished, frame, &QObject::deleteLater);
        fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
    });
}

} // namespace charging::client
