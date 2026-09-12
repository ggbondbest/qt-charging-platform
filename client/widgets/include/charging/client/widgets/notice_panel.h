#pragma once

#include <QString>
#include <QWidget>

class QLabel;

namespace charging::client {

// Inline empty/error placeholder with an optional retry action.
class NoticePanel final : public QWidget
{
    Q_OBJECT

public:
    explicit NoticePanel(const QString& glyph, const QString& title, const QString& description,
                         const QString& actionText = QString(), QWidget* parent = nullptr);

    void setContent(const QString& glyph, const QString& title, const QString& description,
                    const QString& actionText = QString());

signals:
    void actionTriggered();

private:
    QLabel* glyphLabel_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    class ActionButton* actionButton_ = nullptr;
};

} // namespace charging::client
