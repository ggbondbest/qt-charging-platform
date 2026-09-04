#include "management_page_widgets.h"

#include "dashboard_visual_widgets.h"

#include <QAbstractItemView>
#include <QFrame>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QVBoxLayout>

namespace charging::server {

namespace {

QLabel* createLabel(const QString& text, const QString& style, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(style);
    return label;
}

} // namespace

QFrame* createManagementMetricCard(const QString& title, const QString& value, const QString& unit,
                                   const QString& hint, const QColor& accent, int iconType,
                                   QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("summaryCard"));
    card->setMinimumHeight(132);
    auto* layout = new QHBoxLayout(card);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(16);
    layout->addWidget(new MetricIconWidget(accent, iconType, card), 0, Qt::AlignTop);

    auto* textLayout = new QVBoxLayout();
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(5);
    textLayout->addWidget(createLabel(title, QStringLiteral("color:#627089; font-size:13px;"), card));
    textLayout->addWidget(createLabel(value + unit,
                                      QStringLiteral("color:#1b2a45; font-size:24px; font-weight:700;"),
                                      card));
    textLayout->addWidget(createLabel(hint, QStringLiteral("color:#68758a; font-size:13px;"), card));
    layout->addLayout(textLayout, 1);
    return card;
}

QFrame* createManagementDetailCard(const QString& title, QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("contentCard"));
    card->setMinimumWidth(292);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);
    layout->addWidget(createLabel(title,
                                  QStringLiteral("color:#1d2c46; font-size:18px; font-weight:700;"),
                                  card));
    return card;
}

QWidget* createManagementTableCell(QWidget* content, QWidget* parent)
{
    Q_ASSERT(content != nullptr);
    auto* cell = new QWidget(parent);
    auto* layout = new QHBoxLayout(cell);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(0);
    layout->addWidget(content, 0, Qt::AlignCenter);
    return cell;
}

void configureManagementComboBox(QComboBox* comboBox)
{
    Q_ASSERT(comboBox != nullptr);
    // Keep the trigger as a light icon area rather than the platform's raised
    // button, while the popup itself remains styled independently below.
    comboBox->setStyleSheet(QStringLiteral(
        "QComboBox { background:#ffffff; border:1px solid #dfe6f0; border-radius:9px;"
        " min-height:40px; padding:0 34px 0 12px; font-size:14px; }"
        "QComboBox:focus { border:2px solid #2878d4; }"
        "QComboBox::drop-down { subcontrol-origin:padding; subcontrol-position:top right; width:30px;"
        " border:none; background:transparent; }"
        "QComboBox::drop-down:hover { background:#f4f7fb; border-radius:7px; }"
        "QComboBox::drop-down:pressed { background:#eaf3ff; }"
        "QComboBox::down-arrow { width:0; height:0; margin-right:11px;"
        " border-left:4px solid transparent; border-right:4px solid transparent;"
        " border-top:5px solid #718098; }"));
    auto* popupView = comboBox->view();
    QPalette popupPalette = popupView->palette();
    popupPalette.setColor(QPalette::Active, QPalette::Highlight, QColor("#eaf3ff"));
    popupPalette.setColor(QPalette::Active, QPalette::HighlightedText, QColor("#1d2c46"));
    popupPalette.setColor(QPalette::Inactive, QPalette::Highlight, QColor("#eaf3ff"));
    popupPalette.setColor(QPalette::Inactive, QPalette::HighlightedText, QColor("#1d2c46"));
    popupView->setPalette(popupPalette);
    popupView->setStyleSheet(QStringLiteral(
        "QAbstractItemView { background: #ffffff; color: #1d2c46;"
        " border: 1px solid #dfe6f0; outline: 0; font-size: 14px;"
        " selection-background-color: #eaf3ff; selection-color: #1d2c46; }"
        "QAbstractItemView::item { min-height: 38px; padding: 0 12px; }"
        "QAbstractItemView::item:hover { background: #f7f9fc; color: #1d2c46; }"
        "QAbstractItemView::item:selected { background: #eaf3ff; color: #1d2c46;"
        " font-weight: 600; }"));
}

} // namespace charging::server
