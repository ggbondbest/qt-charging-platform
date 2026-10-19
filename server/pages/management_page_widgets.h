#pragma once

#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QFrame>
#include <QLayout>
#include <QList>
#include <QString>

class QFrame;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidgetItem;
class QTableWidget;
class QWidget;

namespace charging::server {

// Only the table's own viewport scrolls horizontally. Pages and detail cards
// reflow within the available management-shell width.
inline constexpr int kManagementPageMinimumWidth = 0;
inline constexpr int kManagementTableMinimumWidth = 0;
inline constexpr int kManagementDetailWidth = 312;
inline constexpr int kManagementStatusColumnWidth = 80;

class ManagementResponsiveLayout final : public QLayout
{
public:
    enum class Mode { Controls, Metrics, ListAndDetail };
    explicit ManagementResponsiveLayout(Mode mode, QWidget* parent = nullptr);
    ~ManagementResponsiveLayout() override;
    void addWidget(QWidget* widget, int stretch = 0);
    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    void setGeometry(const QRect& rect) override;

private:
    struct Entry { QLayoutItem* item; int stretch; };
    int arrange(const QRect& rect, bool apply) const;
    Mode mode_;
    QList<Entry> entries_;
};

QWidget* createManagementFilterField(const QString& label, QWidget* field, QWidget* parent);
QWidget* createManagementActionGroup(const QList<QWidget*>& controls, QWidget* parent);
void configureManagementResponsiveTable(QTableWidget* table);
void configureManagementDetailWrapping(QWidget* card);

// List pages must keep their card geometry while data is loading, empty, or
// temporarily unavailable.  This compact panel deliberately lives inside the
// existing table card rather than replacing the whole page with a dialog.
enum class ManagementListState { Hidden, Loading, EmptyFiltered, EmptyInitial, LoadError };

class ManagementStatePanel final : public QFrame
{
    Q_OBJECT

public:
    explicit ManagementStatePanel(QWidget* parent = nullptr);

    void setState(ManagementListState state, const QString& detail = {});

signals:
    void resetRequested();
    void retryRequested();

private:
    class StateGlyph;

    StateGlyph* glyph_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* detailLabel_ = nullptr;
    QPushButton* actionButton_ = nullptr;
};

QFrame* createManagementMetricCard(const QString& title, const QString& value, const QString& unit,
                                   const QString& hint, const QColor& accent, int iconType,
                                   QWidget* parent);

// Page-level aggregates must never retain decorative Mock numbers after a page
// starts using the real administrator gateway.  A page may later replace a
// value only when the service contract supplies that exact aggregate.
void setManagementMetricCardsUnavailable(QWidget* parent, const QString& hint);

void setManagementMetricCardValue(QWidget* parent, int index, const QString& value,
                                  const QString& hint);

QFrame* createManagementDetailCard(const QString& title, QWidget* parent);

// QTableWidget expands a cell widget to the full cell rectangle.  Wrap status
// tags and compact actions so their visual surface follows the content instead.
QWidget* createManagementTableCell(QWidget* content, QWidget* parent);

// A table column must accommodate the status tag, the cell wrapper and the
// platform's table frame.  This gives short and three-character Chinese states
// one consistent visual size instead of letting a page-specific fixed column cut them off.
int managementStatusTagWidth(const QString& text);

// The operations UI is intentionally light. Set every input/table palette role
// explicitly so a dark desktop palette cannot leak into unstyled subcontrols.
void applyManagementLightPalette(QWidget* widget);

// Native item tooltips provide the complete value whenever a responsive column
// still has to elide long text.  This keeps rows compact without hiding data.
QTableWidgetItem* createManagementTableItem(const QString& text);

// QComboBox popup views are top-level Qt popups, so they must be styled on the
// view itself rather than relying on the management page's descendant QSS.
void configureManagementComboBox(QComboBox* comboBox);

// All storage and gateway timestamps remain UTC, but operators work in China
// Standard Time.  Keep that conversion in one place so tables, details and
// date-range filters cannot silently use different calendar days.
QString formatBeijingDateTime(const QString& utcIsoTimestamp);
QDateTime beijingDayStartUtc(const QDate& beijingDate);

} // namespace charging::server
