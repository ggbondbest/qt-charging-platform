#include "management_page_widgets.h"

#include "dashboard_visual_widgets.h"

#include <QAbstractItemView>
#include <QFrame>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimeZone>
#include <QVBoxLayout>

namespace charging::server {

namespace {

class ComboArrow final : public QWidget
{
public:
    explicit ComboArrow(QComboBox* combo) : QWidget(combo)
    {
        setObjectName(QStringLiteral("managementComboArrow"));
        setFixedSize(16, 16);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        combo->installEventFilter(this);
        reposition();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Resize) reposition();
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(isEnabled() ? "#718098" : "#b7c1ce"),
                            1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(3, 6), QPointF(8, 11));
        painter.drawLine(QPointF(8, 11), QPointF(13, 6));
    }

private:
    void reposition()
    {
        move(parentWidget()->width() - width() - 10, (parentWidget()->height() - height()) / 2);
    }
};

QLabel* createLabel(const QString& text, const QString& style, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(style);
    return label;
}

} // namespace

void applyManagementLightPalette(QWidget* widget)
{
    Q_ASSERT(widget != nullptr);
    QPalette palette = widget->palette();
    for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const bool disabled = group == QPalette::Disabled;
        palette.setColor(group, QPalette::Window, QColor("#f5f7fb"));
        palette.setColor(group, QPalette::WindowText, QColor(disabled ? "#8995a7" : "#1d2c46"));
        palette.setColor(group, QPalette::Base, QColor(disabled ? "#f2f5f9" : "#ffffff"));
        palette.setColor(group, QPalette::AlternateBase, QColor("#f8fafd"));
        palette.setColor(group, QPalette::Text, QColor(disabled ? "#8995a7" : "#1d2c46"));
        palette.setColor(group, QPalette::PlaceholderText, QColor("#7a879b"));
        palette.setColor(group, QPalette::Button, QColor("#ffffff"));
        palette.setColor(group, QPalette::ButtonText, QColor(disabled ? "#8995a7" : "#1d2c46"));
        palette.setColor(group, QPalette::Highlight, QColor("#eaf3ff"));
        palette.setColor(group, QPalette::HighlightedText, QColor("#1d2c46"));
        palette.setColor(group, QPalette::ToolTipBase, QColor("#ffffff"));
        palette.setColor(group, QPalette::ToolTipText, QColor("#1d2c46"));
        palette.setColor(group, QPalette::Link, QColor("#2878f0"));
    }
    widget->setPalette(palette);
}

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

ManagementResponsiveLayout::ManagementResponsiveLayout(Mode mode, QWidget* parent)
    : QLayout(parent), mode_(mode)
{
    setContentsMargins(0, 0, 0, 0);
    setSpacing(12);
}

ManagementResponsiveLayout::~ManagementResponsiveLayout()
{
    while (auto* item = takeAt(0)) delete item;
}

void ManagementResponsiveLayout::addWidget(QWidget* widget, int stretch)
{
    addChildWidget(widget);
    entries_.append({new QWidgetItem(widget), stretch});
    invalidate();
}

void ManagementResponsiveLayout::addItem(QLayoutItem* item)
{
    entries_.append({item, 0});
    invalidate();
}

int ManagementResponsiveLayout::count() const { return entries_.size(); }
QLayoutItem* ManagementResponsiveLayout::itemAt(int index) const
{
    return index >= 0 && index < entries_.size() ? entries_.at(index).item : nullptr;
}
QLayoutItem* ManagementResponsiveLayout::takeAt(int index)
{
    if (index < 0 || index >= entries_.size()) return nullptr;
    auto* item = entries_.takeAt(index).item;
    invalidate();
    return item;
}

QSize ManagementResponsiveLayout::sizeHint() const
{
    return QSize(1100, heightForWidth(1100));
}

QSize ManagementResponsiveLayout::minimumSize() const
{
    QSize size;
    for (const auto& entry : entries_) {
        if (!entry.item->isEmpty()) size = size.expandedTo(entry.item->minimumSize());
    }
    const auto margins = contentsMargins();
    return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
}

Qt::Orientations ManagementResponsiveLayout::expandingDirections() const
{
    return mode_ == Mode::ListAndDetail ? Qt::Horizontal | Qt::Vertical : Qt::Horizontal;
}
bool ManagementResponsiveLayout::hasHeightForWidth() const { return true; }
int ManagementResponsiveLayout::heightForWidth(int width) const
{
    return arrange(QRect(0, 0, width, 0), false);
}
void ManagementResponsiveLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

int ManagementResponsiveLayout::arrange(const QRect& outer, bool apply) const
{
    const auto margins = contentsMargins();
    const QRect rect = outer.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
    const int width = qMax(1, rect.width());
    const int gap = qMax(0, spacing());
    QList<Entry> visible;
    for (const auto& entry : entries_) if (!entry.item->isEmpty()) visible.append(entry);
    if (visible.isEmpty()) return margins.top() + margins.bottom();
    const auto itemHeight = [](QLayoutItem* item, int itemWidth) {
        const int preferred = item->hasHeightForWidth() ? item->heightForWidth(itemWidth) : item->sizeHint().height();
        return qMax(item->minimumSize().height(), preferred);
    };
    int usedHeight = 0;
    if (mode_ == Mode::ListAndDetail && visible.size() == 2) {
        auto* table = visible.at(0).item;
        auto* detail = visible.at(1).item;
        const int detailWidth = qMax(kManagementDetailWidth, detail->minimumSize().width());
        if (width >= qMax(1100, table->minimumSize().width() + gap + detailWidth)) {
            const int tableWidth = width - gap - detailWidth;
            usedHeight = qMax(itemHeight(table, tableWidth), itemHeight(detail, detailWidth));
            if (apply) {
                const int height = qMax(usedHeight, rect.height());
                table->setGeometry(QRect(rect.x(), rect.y(), tableWidth, height));
                detail->setGeometry(QRect(rect.x() + tableWidth + gap, rect.y(), detailWidth, height));
            }
        } else {
            const int tableHeight = itemHeight(table, width);
            const int detailHeight = itemHeight(detail, width);
            usedHeight = tableHeight + gap + detailHeight;
            if (apply) {
                const int expandedTableHeight = tableHeight + qMax(0, rect.height() - usedHeight);
                table->setGeometry(QRect(rect.x(), rect.y(), width, expandedTableHeight));
                detail->setGeometry(QRect(rect.x(), rect.y() + expandedTableHeight + gap, width, detailHeight));
            }
        }
    } else if (mode_ == Mode::Metrics) {
        const int columns = width >= 4 * 250 + 3 * gap ? 4 : (width >= 2 * 250 + gap ? 2 : 1);
        const int itemWidth = (width - (columns - 1) * gap) / columns;
        for (int start = 0; start < visible.size(); start += columns) {
            int rowHeight = 0;
            for (int offset = 0; offset < columns && start + offset < visible.size(); ++offset)
                rowHeight = qMax(rowHeight, itemHeight(visible.at(start + offset).item, itemWidth));
            if (apply) {
                for (int offset = 0; offset < columns && start + offset < visible.size(); ++offset) {
                    const int x = rect.x() + offset * (itemWidth + gap);
                    const int actualWidth = offset == columns - 1 ? rect.right() - x + 1 : itemWidth;
                    visible.at(start + offset).item->setGeometry(QRect(x, rect.y() + usedHeight, actualWidth, rowHeight));
                }
            }
            usedHeight += rowHeight + gap;
        }
        usedHeight -= gap;
    } else {
        // Keep label/input groups intact while wrapping controls at their
        // natural widths. Only explicitly expanding fields take row slack.
        int start = 0;
        while (start < visible.size()) {
            QList<int> widths;
            int rowWidth = 0;
            int totalStretch = 0;
            int end = start;
            for (; end < visible.size(); ++end) {
                auto* item = visible.at(end).item;
                const int preferred = qMin(width, item->sizeHint().expandedTo(item->minimumSize()).boundedTo(item->maximumSize()).width());
                if (end > start && rowWidth + gap + preferred > width) break;
                widths.append(preferred);
                rowWidth += (end > start ? gap : 0) + preferred;
                totalStretch += visible.at(end).stretch;
            }
            int rowHeight = 0;
            for (int i = start; i < end; ++i) {
                if (totalStretch > 0) widths[i - start] += (width - rowWidth) * visible.at(i).stretch / totalStretch;
                rowHeight = qMax(rowHeight, itemHeight(visible.at(i).item, widths.at(i - start)));
            }
            if (apply) {
                int x = rect.x();
                for (int i = start; i < end; ++i) {
                    visible.at(i).item->setGeometry(QRect(x, rect.y() + usedHeight, widths.at(i - start), rowHeight));
                    x += widths.at(i - start) + gap;
                }
            }
            usedHeight += rowHeight + gap;
            start = end;
        }
        usedHeight -= gap;
    }
    return usedHeight + margins.top() + margins.bottom();
}

QWidget* createManagementFilterField(const QString& label, QWidget* field, QWidget* parent)
{
    auto* group = new QWidget(parent);
    auto* layout = new QHBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(createLabel(label, QStringLiteral("color:#52617a; font-size:13px; font-weight:600;"), group));
    layout->addWidget(field, 1);
    return group;
}

void configureManagementResponsiveTable(QTableWidget* table)
{
    table->setMinimumWidth(0);
    table->setMinimumHeight(320);
    table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    table->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table->setWordWrap(false);
}

QWidget* createManagementActionGroup(const QList<QWidget*>& controls, QWidget* parent)
{
    auto* group = new QWidget(parent);
    group->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    for (auto* control : controls) layout->addWidget(control);
    return group;
}

void configureManagementDetailWrapping(QWidget* card)
{
    card->setMinimumWidth(0);
    card->setMaximumWidth(QWIDGETSIZE_MAX);
    for (auto* label : card->findChildren<QLabel*>()) {
        // Badges, avatars and chart keys intentionally retain their geometry.
        if (label->minimumWidth() == label->maximumWidth()) continue;
        label->setMinimumWidth(0);
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
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
    valueLabel->setWordWrap(true);
    valueLabel->setMinimumWidth(0);
    textLayout->addWidget(valueLabel);
    auto* hintLabel = createLabel(hint, QStringLiteral("color:#68758a; font-size:13px;"), card);
    hintLabel->setObjectName(QStringLiteral("managementMetricHint"));
    hintLabel->setWordWrap(true);
    hintLabel->setMinimumWidth(0);
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
    card->setMinimumWidth(0);
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
    // Loaded station names must not widen the entire page. The popup still
    // exposes full names; the closed control uses the available field width.
    comboBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    comboBox->setMinimumContentsLength(8);
    Q_ASSERT(comboBox != nullptr);
    if (comboBox->findChild<QWidget*>(QStringLiteral("managementComboArrow")) == nullptr) {
        new ComboArrow(comboBox);
    }
    // Keep the trigger as a light icon area rather than the platform's raised
    // button, while the popup itself remains styled independently below.
    comboBox->setStyleSheet(QStringLiteral(
        "QComboBox { background:#ffffff; color:#1d2c46; border:1px solid #dfe6f0; border-radius:9px;"
        " min-height:40px; padding:0 34px 0 12px; font-size:14px; }"
        "QComboBox:disabled { background:#f2f5f9; color:#8995a7; border-color:#e4e9f1; }"
        "QComboBox:focus { border:2px solid #2878d4; }"
        "QComboBox::drop-down { subcontrol-origin:padding; subcontrol-position:top right; width:30px;"
        " border:none; background:transparent; }"
        "QComboBox::drop-down:hover { background:#f4f7fb; border-radius:7px; }"
        "QComboBox::drop-down:pressed { background:#eaf3ff; }"
        // QSS border triangles are not consistently rendered as arrows.
        // Draw the chevron above in Qt, with no font or image dependency.
        "QComboBox::down-arrow { image:none; border:none; width:0; height:0; }"));
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

QString formatBeijingDateTime(const QString& utcIsoTimestamp)
{
    if (utcIsoTimestamp.isEmpty()) {
        return QStringLiteral("—");
    }
    QDateTime timestamp = QDateTime::fromString(utcIsoTimestamp, Qt::ISODateWithMs);
    if (!timestamp.isValid()) {
        timestamp = QDateTime::fromString(utcIsoTimestamp, Qt::ISODate);
    }
    if (!timestamp.isValid()) {
        return utcIsoTimestamp;
    }
    return timestamp.toTimeZone(QTimeZone("Asia/Shanghai"))
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QDateTime beijingDayStartUtc(const QDate& beijingDate)
{
    return QDateTime(beijingDate, QTime(0, 0), QTimeZone("Asia/Shanghai")).toUTC();
}

} // namespace charging::server
