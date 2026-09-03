#pragma once

#include <QSqlDatabase>
#include <QString>

namespace charging::server {

// Owns the Server's SQLite connection. Keep this object alive longer than all
// repositories that use database().
class DatabaseConnection final
{
public:
    DatabaseConnection();
    ~DatabaseConnection();

    DatabaseConnection(const DatabaseConnection&) = delete;
    DatabaseConnection& operator=(const DatabaseConnection&) = delete;

    bool open(const QString& databasePath, bool loadDemoSeed, QString* errorMessage = nullptr);
    void close();

    bool isOpen() const;
    QString databasePath() const;
    QSqlDatabase database() const;

private:
    bool executeResourceScript(const QString& resourcePath, QString* errorMessage);

    QString connectionName_;
    QString databasePath_;
    QSqlDatabase database_;
};

} // namespace charging::server
