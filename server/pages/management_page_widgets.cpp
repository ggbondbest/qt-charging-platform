#include "management_page_widgets.h"

#include "dashboard_visual_widgets.h"

#include <QAbstractItemView>
#include <QFrame>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPainter>
#include <QPushButton>
#include <QTableWidgetItem>
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

class ManagementStatePanel::StateGlyph final : public QWidget
{
public:
    explicit StateGlyph(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(34, 34);
    }

    void setState(ManagementListState state)
    {
        state_ = state;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF frame(4.0, 4.0, 26.0, 26.0);
        QColor color = QColor("#718098");
        if (state_ == ManagementListState::Loading) {
            color = QColor("#2878d4");
            QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap);
            painter.setPen(pen);
            painter.drawArc(frame, 35 * 16, 250 * 16);
            painter.drawEllipse(QPointF(25.5, 9.0), 1.8, 1.8);
            return;
        }
        if (state_ == ManagementListState::LoadError) {
            color = QColor("#d84a4a");
            painter.setPen(QPen(color, 1.8));
            painter.setBrush(QColor("#fff4f4"));
            painter.drawEllipse(frame);
            painter.drawLine(QPointF(17, 10), QPointF(17, 19));
            painter.setBrush(color);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QPointF(17, 24), 1.4, 1.4);
            return;
        }
        painter.setPen(QPen(color, 1.7));
        painter.setBrush(QColor("#f7f9fc"));
        painter.drawRoundedRect(frame, 6, 6);
        painter.drawLine(QPointF(11, 14), QPointF(23, 14));
        painter.drawLine(QPointF(11, 19), QPointF(19, 19));
    }

private:
    ManagementListState state_ = ManagementListState::EmptyFiltered;
};

ManagementStatePanel::ManagementStatePanel(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("managementStatePanel"));
    setMinimumHeight(180);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 20, 18, 20);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignCenter);
    glyph_ = new StateGlyph(this);
    titleLabel_ = createLabel({}, QStringLiteral("color:#34435b; font-size:15px; font-weight:600;"), this);
    titleLabel_->setAlignment(Qt::AlignCenter);
    detailLabel_ = createLabel({}, QStringLiteral("color:#718098; font-size:13px;"), this);
    detailLabel_->setAlignment(Qt::AlignCenter);
    detailLabel_->setWordWrap(true);
    actionButton_ = new QPushButton(this);
    actionButton_->setObjectName(QStringLiteral("secondaryButton"));
    actionButton_->setFixedHeight(38);
    actionButton_->setVisible(false);
    layout->addWidget(glyph_, 0, Qt::AlignHCenter);
    layout->addWidget(titleLabel_, 0, Qt::AlignHCenter);
    layout->addWidget(detailLabel_, 0, Qt::AlignHCenter);
    layout->addWidget(actionButton_, 0, Qt::AlignHCenter);
    connect(actionButton_, &QPushButton::clicked, this, [this] {
        if (actionButton_->property("stateAction").toString() == QStringLiteral("reset"))
            emit resetRequested();
        else
            emit retryRequested();
    });
    setState(ManagementListState::Hidden);
}

void ManagementStatePanel::setState(ManagementListState state, const QString& detail)
{
    if (state == ManagementListState::Hidden) {
        setVisible(false);
        return;
    }
    setVisible(true);
    glyph_->setState(state);
    actionButton_->setVisible(false);
    if (state == ManagementListState::Loading) {
        titleLabel_->setText(tr("正在加载列表"));
        detailLabel_->setText(detail.isEmpty() ? tr("请稍候，数据准备完成后将显示在这里。") : detail);
    } else if (state == ManagementListState::EmptyInitial) {
        titleLabel_->setText(tr("暂未产生记录"));
        detailLabel_->setText(detail.isEmpty() ? tr("新的业务记录会在这里显示。") : detail);
    } else if (state == ManagementListState::LoadError) {
        titleLabel_->setText(tr("数据暂时无法加载"));
        detailLabel_->setText(detail.isEmpty() ? tr("请检查服务状态后重新加载。") : detail);
        actionButton_->setText(tr("重新加载"));
        actionButton_->setProperty("stateAction", QStringLiteral("retry"));
        actionButton_->setVisible(true);
    } else {
        titleLabel_->setText(tr("暂无符合条件的记录"));
        detailLabel_->setText(detail.isEmpty() ? tr("请调整筛选条件，或重置后查看全部记录。") : detail);
        actionButton_->setText(tr("重置筛选"));
        actionButton_->setProperty("stateAction", QStringLiteral("reset"));
        actionButton_->setVisible(true);
    }
}

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
    auto* valueLabel = createLabel(value + unit,
                                   QStringLiteral("color:#1b2a45; font-size:24px; font-weight:700;"),
                                   card);
    valueLabel->setObjectName(QStringLiteral("managementMetricValue"));
    textLayout->addWidget(valueLabel);
    auto* hintLabel = createLabel(hint, QStringLiteral("color:#68758a; font-size:13px;"), card);
    hintLabel->setObjectName(QStringLiteral("managementMetricHint"));
    textLayout->addWidget(hintLabel);
    layout->addLayout(textLayout, 1);
    return card;
}

void setManagementMetricCardsUnavailable(QWidget* parent, const QString& hint)
{
    Q_ASSERT(parent != nullptr);
    for (auto* value : parent->findChildren<QLabel*>(QStringLiteral("managementMetricValue"))) {
        value->setText(QStringLiteral("—"));
        value->setToolTip(hint);
    }
    for (auto* detail : parent->findChildren<QLabel*>(QStringLiteral("managementMetricHint"))) {
        detail->setText(hint);
        detail->setToolTip(hint);
    }
}

void setManagementMetricCardValue(QWidget* parent, int index, const QString& value,
                                  const QString& hint)
{
    Q_ASSERT(parent != nullptr);
    const auto values = parent->findChildren<QLabel*>(QStringLiteral("managementMetricValue"));
    const auto details = parent->findChildren<QLabel*>(QStringLiteral("managementMetricHint"));
    if (index < 0 || index >= values.size() || index >= details.size()) {
        return;
    }
    values.at(index)->setText(value);
    values.at(index)->setToolTip(hint);
    details.at(index)->setText(hint);
    details.at(index)->setToolTip(hint);
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
    layout->setContentsMargins(2, 0, 2, 0);
    layout->setSpacing(0);
    layout->addWidget(content, 0, Qt::AlignCenter);
    return cell;
}

int managementStatusTagWidth(const QString& text)
{
    return text.size() >= 3 ? 62 : 54;
}

QTableWidgetItem* createManagementTableItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setData(Qt::ToolTipRole, text);
    return item;
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
