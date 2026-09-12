#pragma once

#include <QPushButton>

namespace charging::client {

// Themed push button. The visual variant is fixed at construction and exposed
// to QSS through the "variant" property.
class ActionButton final : public QPushButton
{
    Q_OBJECT

public:
    enum class Variant
    {
        Primary,
        Secondary,
        Danger,
        Ghost,
        Chip
    };

    explicit ActionButton(Variant variant, const QString& text, QWidget* parent = nullptr);

private:
    static QString variantName(Variant variant);
};

} // namespace charging::client
