#include "database_maintenance.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace charging::server {

namespace {

DatabaseMaintenanceResult success()
{
    return {true, {}};
}

DatabaseMaintenanceResult failure(const QString& message)
{
    return {false, message};
}

QString absolutePath(const QString& path)
{
    return QFileInfo(path).absoluteFilePath();
}

QString sqlStringLiteral(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral("'%1'").arg(value);
}

bool ensureParentDirectory(const QString& path, QString* errorMessage)
{
    QDir directory = QFileInfo(path).absoluteDir();
    if (directory.exists() || directory.mkpath(QStringLiteral("."))) {
        return true;
    }
    *errorMessage = QStringLiteral("Unable to create directory: %1")
                        .arg(directory.absolutePath());
    return false;
}

bool isOpenDatabasePath(const QString& path)
{
    const QString targetPath = absolutePath(path);
    for (const QString& connectionName : QSqlDatabase::connectionNames()) {
        const QSqlDatabase database = QSqlDatabase::database(connectionName, false);
        if (database.isValid() && database.isOpen() &&
            database.driverName() == QStringLiteral("QSQLITE") &&
            database.databaseName() != QStringLiteral(":memory:") &&
            absolutePath(database.databaseName()) == targetPath) {
            return true;
        }
    }
    return false;
}

} // namespace

DatabaseMaintenanceResult DatabaseMaintenance::backup(const QSqlDatabase& database,
                                                      const QString& destinationPath)
{
    if (!database.isValid() || !database.isOpen() ||
        database.driverName() != QStringLiteral("QSQLITE")) {
        return failure(QStringLiteral("Backup requires an open QSQLITE database"));
    }
    if (destinationPath.trimmed().isEmpty()) {
        return failure(QStringLiteral("Backup destination must not be empty"));
    }

    const QString destination = absolutePath(destinationPath);
    QString errorMessage;
    if (!ensureParentDirectory(destination, &errorMessage)) {
        return failure(errorMessage);
    }

    const QString temporaryPath = QStringLiteral("%1.tmp-%2")
                                      .arg(destination,
                                           QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("VACUUM INTO %1").arg(sqlStringLiteral(temporaryPath)))) {
        QFile::remove(temporaryPath);
        return failure(QStringLiteral("Unable to create database backup: %1")
                           .arg(query.lastError().text()));
    }

    const DatabaseMaintenanceResult validation = validate(temporaryPath);
    if (!validation.ok) {
        QFile::remove(temporaryPath);
        return failure(QStringLiteral("Created backup failed validation: %1")
                           .arg(validation.errorMessage));
    }

    QFile::remove(destination);
    if (!QFile::rename(temporaryPath, destination)) {
        QFile::remove(temporaryPath);
        return failure(QStringLiteral("Unable to move backup into place: %1").arg(destination));
    }
    return success();
}

DatabaseMaintenanceResult DatabaseMaintenance::validate(const QString& databasePath)
{
    const QString path = absolutePath(databasePath);
    const QFileInfo fileInfo(path);
    if (!fileInfo.isFile() || fileInfo.size() == 0) {
        return failure(QStringLiteral("Database backup does not exist or is empty: %1").arg(path));
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return failure(QStringLiteral("Qt SQLite driver QSQLITE is not available"));
    }

    const QString connectionName = QStringLiteral("database-validation-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    DatabaseMaintenanceResult result = success();
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(path);
        if (!database.open()) {
            result = failure(QStringLiteral("Unable to open backup: %1")
                                 .arg(database.lastError().text()));
        } else {
            QSqlQuery integrityQuery(database);
            if (!integrityQuery.exec(QStringLiteral("PRAGMA integrity_check")) ||
                !integrityQuery.next() || integrityQuery.value(0).toString() != QStringLiteral("ok")) {
                result = failure(QStringLiteral("SQLite integrity check failed"));
            }

            QSqlQuery foreignKeyQuery(database);
            if (result.ok &&
                (!foreignKeyQuery.exec(QStringLiteral("PRAGMA foreign_key_check")) ||
                 foreignKeyQuery.next())) {
                result = failure(QStringLiteral("SQLite foreign key check failed"));
            }

            QSqlQuery versionQuery(database);
            if (result.ok &&
                (!versionQuery.exec(QStringLiteral("PRAGMA user_version")) ||
                 !versionQuery.next() || versionQuery.value(0).toInt() != 1)) {
                result = failure(QStringLiteral("Unsupported database schema version"));
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return result;
}

DatabaseMaintenanceResult DatabaseMaintenance::restore(const QString& backupPath,
                                                       const QString& destinationPath)
{
    if (destinationPath.trimmed().isEmpty()) {
        return failure(QStringLiteral("Restore destination must not be empty"));
    }
    const DatabaseMaintenanceResult validation = validate(backupPath);
    if (!validation.ok) {
        return validation;
    }

    const QString destination = absolutePath(destinationPath);
    if (isOpenDatabasePath(destination)) {
        return failure(QStringLiteral("Close the destination database before restoring it"));
    }

    QString errorMessage;
    if (!ensureParentDirectory(destination, &errorMessage)) {
        return failure(errorMessage);
    }

    QFile source(absolutePath(backupPath));
    if (!source.open(QIODevice::ReadOnly)) {
        return failure(QStringLiteral("Unable to read backup: %1").arg(source.errorString()));
    }
    QSaveFile destinationFile(destination);
    if (!destinationFile.open(QIODevice::WriteOnly)) {
        return failure(QStringLiteral("Unable to open restore destination: %1")
                           .arg(destinationFile.errorString()));
    }
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            destinationFile.cancelWriting();
            return failure(QStringLiteral("Unable to read backup: %1").arg(source.errorString()));
        }
        if (destinationFile.write(chunk) != chunk.size()) {
            destinationFile.cancelWriting();
            return failure(QStringLiteral("Unable to write restored database: %1")
                               .arg(destinationFile.errorString()));
        }
    }
    if (!destinationFile.commit()) {
        return failure(QStringLiteral("Unable to commit restored database: %1")
                           .arg(destinationFile.errorString()));
    }
    return success();
}

} // namespace charging::server
