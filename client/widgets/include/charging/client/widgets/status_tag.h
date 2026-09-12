#pragma once

#include <QLabel>

namespace charging::client {

// Small coloured pill used for order / recharge / charger states.
// Styled through QSS via property "tone".
class StatusTag final : public QLabel
{
    Q_OBJECT

public:
    enum class Tone
    {
        Neutral,
        Success,
        Warning,
        Danger,
        Info
    };

    explicit StatusTag(const QString& text, Tone tone = Tone::Neutral,
                       QWidget* parent = nullptr);

    void setTone(Tone tone);

private:
    static QString toneName(Tone tone);
};

} // namespace charging::client
