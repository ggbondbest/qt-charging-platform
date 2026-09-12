#pragma once

#include <QFrame>

class QVBoxLayout;

namespace charging::client {

// Generic elevated surface used by all profile/charging pages.
// Styled through QSS via objectName "uiCard".
class Card : public QFrame
{
    Q_OBJECT

public:
    explicit Card(QWidget* parent = nullptr);

    QVBoxLayout* bodyLayout() const;

private:
    QVBoxLayout* bodyLayout_ = nullptr;
};

} // namespace charging::client
