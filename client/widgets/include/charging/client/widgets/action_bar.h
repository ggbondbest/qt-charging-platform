#pragma once

#include <QString>
#include <QWidget>

class QLabel;

namespace charging::client {

class ActionButton;

// Bottom action bar: a fixed-height strip that pins a page's primary action
// (and optional secondary hint text) to the bottom of the content area, so
// tall pages never leave the main button floating in empty space. Styled
// through the "uiActionBar" object name in the platform QSS (white surface,
// hairline top border); children use existing button variants.
class ActionBar final : public QWidget
{
    Q_OBJECT

public:
    enum class Variant
    {
        Primary,
        Danger,
    };

    explicit ActionBar(Variant variant, const QString& text, QWidget* parent = nullptr);

    ActionButton* actionButton() const;
    void setActionText(const QString& text);
    // Optional caption above the button (e.g. “当前余额 ¥100.00” on pay flows).
    void setCaption(const QString& text);

private:
    QLabel* captionLabel_ = nullptr;
    ActionButton* button_ = nullptr;
};

} // namespace charging::client
