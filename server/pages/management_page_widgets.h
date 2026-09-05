#pragma once

#include <QColor>
#include <QFrame>
#include <QString>

class QFrame;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidgetItem;
class QWidget;

namespace charging::server {

// All six management pages share one desktop content grid: a flexible list area
// and a fixed-width detail panel.  Keeping these values together prevents pages
// from gradually drifting into visually incompatible layouts.
inline constexpr int kManagementPageMinimumWidth = 1180;
inline constexpr int kManagementTableMinimumWidth = 820;
inline constexpr int kManagementDetailWidth = 312;
inline constexpr int kManagementStatusColumnWidth = 80;

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

// Native item tooltips provide the complete value whenever a responsive column
// still has to elide long text.  This keeps rows compact without hiding data.
QTableWidgetItem* createManagementTableItem(const QString& text);

// QComboBox popup views are top-level Qt popups, so they must be styled on the
// view itself rather than relying on the management page's descendant QSS.
void configureManagementComboBox(QComboBox* comboBox);

} // namespace charging::server
