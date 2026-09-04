#pragma once

#include <QColor>
#include <QString>

class QFrame;
class QComboBox;
class QWidget;

namespace charging::server {

QFrame* createManagementMetricCard(const QString& title, const QString& value, const QString& unit,
                                   const QString& hint, const QColor& accent, int iconType,
                                   QWidget* parent);

QFrame* createManagementDetailCard(const QString& title, QWidget* parent);

// QTableWidget expands a cell widget to the full cell rectangle.  Wrap status
// tags and compact actions so their visual surface follows the content instead.
QWidget* createManagementTableCell(QWidget* content, QWidget* parent);

// QComboBox popup views are top-level Qt popups, so they must be styled on the
// view itself rather than relying on the management page's descendant QSS.
void configureManagementComboBox(QComboBox* comboBox);

} // namespace charging::server
