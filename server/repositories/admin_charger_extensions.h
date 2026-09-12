#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QSqlDatabase>

namespace charging::server {

// Safe management DTOs. The caller owns the read/write transaction, authentication,
// audit and durable operationId replay; these helpers never commit independently.
class AdminChargerExtensions final
{
public:
    static bool handlesRead(const QString& action);
    static void validateRead(const QString& action, const QJsonObject& parameters);
    static QJsonObject read(const QSqlDatabase& database, const QString& action,
                            const QJsonObject& parameters);
    static void validateRecover(const QJsonObject& parameters);
    static QJsonObject recover(const QSqlDatabase& database, qint64 adminId,
                               const QJsonObject& parameters, const QString& now);
    static QJsonValue activeException(const QSqlDatabase& database, const QString& chargerId);
    static void recordStatusChange(const QSqlDatabase& database, const QString& chargerId,
                                   const QString& previousStatus, const QString& targetStatus,
                                   const QString& now);
};

} // namespace charging::server
