#pragma once

#include <QSqlDatabase>
#include <QString>

namespace charging::server {

struct DatabaseMaintenanceResult
{
    bool ok = false;
    QString errorMessage;
};

class DatabaseMaintenance final
{
public:
    static DatabaseMaintenanceResult backup(const QSqlDatabase& database,
                                            const QString& destinationPath);
    static DatabaseMaintenanceResult validate(const QString& databasePath);
    static DatabaseMaintenanceResult restore(const QString& backupPath,
                                             const QString& destinationPath);
};

} // namespace charging::server
