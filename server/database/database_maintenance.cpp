#include "database_maintenance.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
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

QString pathIdentity(const QString& path)
{
    const QFileInfo fileInfo(path);
    QString identity = fileInfo.canonicalFilePath();
    if (identity.isEmpty()) {
        const QString parent = fileInfo.absoluteDir().canonicalPath();
        identity = QDir(parent.isEmpty() ? fileInfo.absolutePath() : parent)
                       .absoluteFilePath(fileInfo.fileName());
    }
    return QDir::cleanPath(identity);
}

bool sameFileIdentity(const QString& first, const QString& second)
{
#ifdef Q_OS_WIN
    return pathIdentity(first).compare(pathIdentity(second), Qt::CaseInsensitive) == 0;
#else
    return pathIdentity(first) == pathIdentity(second);
#endif
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
    for (const QString& connectionName : QSqlDatabase::connectionNames()) {
        const QSqlDatabase database = QSqlDatabase::database(connectionName, false);
        if (database.isValid() && database.isOpen() &&
            database.driverName() == QStringLiteral("QSQLITE") &&
            database.databaseName() != QStringLiteral(":memory:") &&
            sameFileIdentity(database.databaseName(), path)) {
            return true;
        }
    }
    return false;
}

bool rejectExistingSidecars(const QString& databasePath, QString* errorMessage)
{
    const QStringList sidecarPaths = {
        databasePath + QStringLiteral("-wal"),
        databasePath + QStringLiteral("-shm"),
        databasePath + QStringLiteral("-journal")
    };
    for (const QString& sidecarPath : sidecarPaths) {
        if (QFileInfo::exists(sidecarPath)) {
            *errorMessage = QStringLiteral(
                "Refusing to replace a database while a SQLite sidecar file exists: %1")
                                .arg(sidecarPath);
            return false;
        }
    }
    return true;
}

bool validateTableColumns(const QSqlDatabase& database, const QString& table,
                          const QStringList& expectedColumns, QString* errorMessage)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        *errorMessage = query.lastError().text();
        return false;
    }
    QStringList actualColumns;
    while (query.next()) {
        actualColumns.append(query.value(1).toString());
    }
    if (actualColumns != expectedColumns) {
        *errorMessage = QStringLiteral("Database table %1 does not match the supported schema")
                            .arg(table);
        return false;
    }
    return true;
}

bool validateForeignKey(const QSqlDatabase& database, const QString& table,
                        const QString& fromColumn, const QString& targetTable,
                        QString* errorMessage)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA foreign_key_list(%1)").arg(table))) {
        *errorMessage = query.lastError().text();
        return false;
    }
    while (query.next()) {
        if (query.value(2).toString() == targetTable && query.value(3).toString() == fromColumn) {
            return true;
        }
    }
    *errorMessage = QStringLiteral("Database table %1 is missing a required foreign key")
                        .arg(table);
    return false;
}

struct IndexDefinition
{
    QString name;
    QString table;
    QStringList columns;
    bool unique = false;
    QString whereClause;
};

QString normalizedSql(QString sql)
{
    QString normalized;
    normalized.reserve(sql.size());
    bool inString = false;
    for (qsizetype index = 0; index < sql.size(); ++index) {
        const QChar character = sql.at(index);
        if (inString) {
            normalized.append(character);
            if (character == QLatin1Char('\'')) {
                if (index + 1 < sql.size() && sql.at(index + 1) == QLatin1Char('\'')) {
                    normalized.append(sql.at(++index));
                } else {
                    inString = false;
                }
            }
        } else if (character == QLatin1Char('\'')) {
            inString = true;
            normalized.append(character);
        } else if (!character.isSpace()) {
            normalized.append(character.toUpper());
        }
    }
    return normalized;
}

bool validateIndex(const QSqlDatabase& database, const IndexDefinition& expected,
                   QString* errorMessage)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT tbl_name, sql FROM sqlite_master "
        "WHERE type = 'index' AND name = :name LIMIT 1"));
    query.bindValue(QStringLiteral(":name"), expected.name);
    if (!query.exec() || !query.next()) {
        *errorMessage = QStringLiteral("Database is missing required index %1").arg(expected.name);
        return false;
    }
    const QString table = query.value(0).toString();
    const QString sql = normalizedSql(query.value(1).toString());
    if (table != expected.table || sql.isEmpty()) {
        *errorMessage = QStringLiteral("Database index %1 does not match the supported schema")
                            .arg(expected.name);
        return false;
    }

    QSqlQuery listQuery(database);
    if (!listQuery.exec(QStringLiteral("PRAGMA index_list(%1)").arg(expected.table))) {
        *errorMessage = listQuery.lastError().text();
        return false;
    }
    bool listed = false;
    while (listQuery.next()) {
        if (listQuery.value(1).toString() == expected.name) {
            listed = true;
            if (listQuery.value(2).toBool() != expected.unique ||
                listQuery.value(4).toBool() != !expected.whereClause.isEmpty()) {
                *errorMessage = QStringLiteral(
                    "Database index %1 has incorrect uniqueness or partial-index flags")
                                    .arg(expected.name);
                return false;
            }
            break;
        }
    }
    if (!listed) {
        *errorMessage = QStringLiteral("Database index %1 belongs to the wrong table")
                            .arg(expected.name);
        return false;
    }

    QSqlQuery columnsQuery(database);
    if (!columnsQuery.exec(QStringLiteral("PRAGMA index_info(%1)")
                               .arg(sqlStringLiteral(expected.name)))) {
        *errorMessage = columnsQuery.lastError().text();
        return false;
    }
    QStringList columns;
    while (columnsQuery.next()) {
        columns.append(columnsQuery.value(2).toString());
    }
    if (columns != expected.columns) {
        *errorMessage = QStringLiteral("Database index %1 has incorrect columns")
                            .arg(expected.name);
        return false;
    }

    const qsizetype wherePosition = sql.indexOf(QStringLiteral("WHERE"));
    const QString actualWhere = wherePosition < 0 ? QString() : sql.mid(wherePosition + 5);
    if (actualWhere != normalizedSql(expected.whereClause)) {
        *errorMessage = QStringLiteral("Database index %1 has an incorrect WHERE condition")
                            .arg(expected.name);
        return false;
    }
    return true;
}

bool validateTableDefinition(const QSqlDatabase& database, const QString& table,
                             const QStringList& requiredFragments, QString* errorMessage)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = :name LIMIT 1"));
    query.bindValue(QStringLiteral(":name"), table);
    if (!query.exec() || !query.next()) {
        *errorMessage = QStringLiteral("Database is missing required table %1").arg(table);
        return false;
    }
    const QString definition = query.value(0).toString().simplified().toUpper();
    for (const QString& fragment : requiredFragments) {
        if (!definition.contains(fragment.simplified().toUpper())) {
            *errorMessage = QStringLiteral("Database table %1 is missing a required constraint")
                                .arg(table);
            return false;
        }
    }
    return true;
}

bool validatePlatformSchema(const QSqlDatabase& database, QString* errorMessage)
{
    const QList<QPair<QString, QStringList>> tables = {
        {QStringLiteral("users"), {QStringLiteral("id"), QStringLiteral("phone"),
            QStringLiteral("nickname"), QStringLiteral("avatar_key"),
            QStringLiteral("balance_cents"), QStringLiteral("status"),
            QStringLiteral("created_at"), QStringLiteral("updated_at")}},
        {QStringLiteral("admins"), {QStringLiteral("id"), QStringLiteral("username"),
            QStringLiteral("display_name"), QStringLiteral("password_algorithm"),
            QStringLiteral("password_salt"), QStringLiteral("password_hash"),
            QStringLiteral("status"), QStringLiteral("last_login_at"),
            QStringLiteral("created_at"), QStringLiteral("updated_at")}},
        {QStringLiteral("stations"), {QStringLiteral("id"), QStringLiteral("code"),
            QStringLiteral("name"), QStringLiteral("address"), QStringLiteral("latitude"),
            QStringLiteral("longitude"), QStringLiteral("price_cents_per_kwh"),
            QStringLiteral("status"), QStringLiteral("created_at"), QStringLiteral("updated_at")}},
        {QStringLiteral("chargers"), {QStringLiteral("id"), QStringLiteral("station_id"),
            QStringLiteral("code"), QStringLiteral("type"), QStringLiteral("power_watts"),
            QStringLiteral("status"), QStringLiteral("total_charge_count"),
            QStringLiteral("total_charge_seconds"), QStringLiteral("created_at"),
            QStringLiteral("updated_at")}},
        {QStringLiteral("reservations"), {QStringLiteral("id"), QStringLiteral("user_id"),
            QStringLiteral("charger_id"), QStringLiteral("status"),
            QStringLiteral("reserved_at"), QStringLiteral("expires_at"),
            QStringLiteral("ended_at"), QStringLiteral("created_at"),
            QStringLiteral("updated_at")}},
        {QStringLiteral("orders"), {QStringLiteral("id"), QStringLiteral("order_no"),
            QStringLiteral("user_id"), QStringLiteral("charger_id"),
            QStringLiteral("reservation_id"), QStringLiteral("status"),
            QStringLiteral("unit_price_cents_per_kwh"), QStringLiteral("energy_wh"),
            QStringLiteral("duration_seconds"), QStringLiteral("amount_cents"),
            QStringLiteral("created_at"), QStringLiteral("started_at"),
            QStringLiteral("stopped_at"), QStringLiteral("paid_at"),
            QStringLiteral("updated_at")}},
        {QStringLiteral("recharge_records"), {QStringLiteral("id"),
            QStringLiteral("transaction_no"), QStringLiteral("user_id"),
            QStringLiteral("amount_cents"), QStringLiteral("balance_after_cents"),
            QStringLiteral("status"), QStringLiteral("created_at")}},
        {QStringLiteral("operation_logs"), {QStringLiteral("id"), QStringLiteral("admin_id"),
            QStringLiteral("action"), QStringLiteral("target_type"),
            QStringLiteral("target_id"), QStringLiteral("details_json"),
            QStringLiteral("created_at")}}
    };
    for (const auto& table : tables) {
        if (!validateTableColumns(database, table.first, table.second, errorMessage)) {
            return false;
        }
    }

    const QList<QPair<QString, QStringList>> tableConstraints = {
        {QStringLiteral("users"), {QStringLiteral("phone TEXT NOT NULL UNIQUE"),
            QStringLiteral("status IN ('ACTIVE', 'FROZEN')")}},
        {QStringLiteral("stations"), {QStringLiteral("code TEXT NOT NULL UNIQUE"),
            QStringLiteral("status IN ('ACTIVE', 'INACTIVE')")}},
        {QStringLiteral("chargers"), {QStringLiteral("code TEXT NOT NULL UNIQUE"),
            QStringLiteral("status IN ('AVAILABLE', 'RESERVED', 'CHARGING', 'FAULT', 'OFFLINE')")}},
        {QStringLiteral("orders"), {QStringLiteral("order_no TEXT NOT NULL UNIQUE"),
            QStringLiteral("reservation_id INTEGER UNIQUE")}},
        {QStringLiteral("recharge_records"), {
            QStringLiteral("transaction_no TEXT NOT NULL UNIQUE"),
            QStringLiteral("status IN ('SUCCESS', 'FAILED')")}}
    };
    for (const auto& table : tableConstraints) {
        if (!validateTableDefinition(database, table.first, table.second, errorMessage)) {
            return false;
        }
    }

    const QList<QStringList> foreignKeys = {
        {QStringLiteral("chargers"), QStringLiteral("station_id"), QStringLiteral("stations")},
        {QStringLiteral("reservations"), QStringLiteral("user_id"), QStringLiteral("users")},
        {QStringLiteral("reservations"), QStringLiteral("charger_id"), QStringLiteral("chargers")},
        {QStringLiteral("orders"), QStringLiteral("user_id"), QStringLiteral("users")},
        {QStringLiteral("orders"), QStringLiteral("charger_id"), QStringLiteral("chargers")},
        {QStringLiteral("orders"), QStringLiteral("reservation_id"), QStringLiteral("reservations")},
        {QStringLiteral("recharge_records"), QStringLiteral("user_id"), QStringLiteral("users")},
        {QStringLiteral("operation_logs"), QStringLiteral("admin_id"), QStringLiteral("admins")}
    };
    for (const QStringList& foreignKey : foreignKeys) {
        if (!validateForeignKey(database, foreignKey.at(0), foreignKey.at(1), foreignKey.at(2),
                                errorMessage)) {
            return false;
        }
    }

    const QList<IndexDefinition> indexes = {
        {QStringLiteral("idx_stations_status"), QStringLiteral("stations"),
         {QStringLiteral("status")}, false, {}},
        {QStringLiteral("idx_chargers_station_status"), QStringLiteral("chargers"),
         {QStringLiteral("station_id"), QStringLiteral("status")}, false, {}},
        {QStringLiteral("idx_reservations_user_status"), QStringLiteral("reservations"),
         {QStringLiteral("user_id"), QStringLiteral("status")}, false, {}},
        {QStringLiteral("idx_reservations_charger_status"), QStringLiteral("reservations"),
         {QStringLiteral("charger_id"), QStringLiteral("status")}, false, {}},
        {QStringLiteral("idx_reservations_expires_at"), QStringLiteral("reservations"),
         {QStringLiteral("expires_at")}, false, {}},
        {QStringLiteral("idx_orders_user_created_at"), QStringLiteral("orders"),
         {QStringLiteral("user_id"), QStringLiteral("created_at")}, false, {}},
        {QStringLiteral("idx_orders_charger_status"), QStringLiteral("orders"),
         {QStringLiteral("charger_id"), QStringLiteral("status")}, false, {}},
        {QStringLiteral("idx_orders_status_created_at"), QStringLiteral("orders"),
         {QStringLiteral("status"), QStringLiteral("created_at")}, false, {}},
        {QStringLiteral("idx_recharge_records_user_created_at"),
         QStringLiteral("recharge_records"),
         {QStringLiteral("user_id"), QStringLiteral("created_at")}, false, {}},
        {QStringLiteral("idx_operation_logs_admin_created_at"),
         QStringLiteral("operation_logs"),
         {QStringLiteral("admin_id"), QStringLiteral("created_at")}, false, {}},
        {QStringLiteral("ux_reservations_active_user"), QStringLiteral("reservations"),
         {QStringLiteral("user_id")}, true, QStringLiteral("status = 'ACTIVE'")},
        {QStringLiteral("ux_reservations_active_charger"), QStringLiteral("reservations"),
         {QStringLiteral("charger_id")}, true, QStringLiteral("status = 'ACTIVE'")},
        {QStringLiteral("ux_orders_unfinished_user"), QStringLiteral("orders"),
         {QStringLiteral("user_id")}, true,
         QStringLiteral("status IN ('RESERVED', 'CHARGING', 'WAITING_PAYMENT')")},
        {QStringLiteral("ux_orders_active_charger"), QStringLiteral("orders"),
         {QStringLiteral("charger_id")}, true,
         QStringLiteral("status IN ('RESERVED', 'CHARGING')")}
    };
    for (const IndexDefinition& index : indexes) {
        if (!validateIndex(database, index, errorMessage)) {
            return false;
        }
    }
    return true;
}

DatabaseMaintenanceResult copyAtomically(const QString& sourcePath, const QString& destinationPath)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return failure(QStringLiteral("Unable to read database file: %1").arg(source.errorString()));
    }
    QSaveFile destination(destinationPath);
    if (!destination.open(QIODevice::WriteOnly)) {
        return failure(QStringLiteral("Unable to open destination: %1").arg(destination.errorString()));
    }
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            destination.cancelWriting();
            return failure(QStringLiteral("Unable to read database file: %1")
                               .arg(source.errorString()));
        }
        if (destination.write(chunk) != chunk.size()) {
            destination.cancelWriting();
            return failure(QStringLiteral("Unable to write destination: %1")
                               .arg(destination.errorString()));
        }
    }
    if (!destination.commit()) {
        return failure(QStringLiteral("Unable to commit destination: %1")
                           .arg(destination.errorString()));
    }
    return success();
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

    const QString destination = QFileInfo(destinationPath).absoluteFilePath();
    if (database.databaseName() != QStringLiteral(":memory:") &&
        sameFileIdentity(database.databaseName(), destination)) {
        return failure(QStringLiteral("Backup destination must not be the source database"));
    }
    if (isOpenDatabasePath(destination)) {
        return failure(QStringLiteral("Backup destination is currently open"));
    }
    QString errorMessage;
    if (!rejectExistingSidecars(destination, &errorMessage)) {
        return failure(errorMessage);
    }
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

    const DatabaseMaintenanceResult replacement = copyAtomically(temporaryPath, destination);
    QFile::remove(temporaryPath);
    return replacement;
}

DatabaseMaintenanceResult DatabaseMaintenance::validate(const QString& databasePath)
{
    const QString path = QFileInfo(databasePath).absoluteFilePath();
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
            if (result.ok) {
                QString schemaError;
                if (!validatePlatformSchema(database, &schemaError)) {
                    result = failure(schemaError);
                }
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

    const QString destination = QFileInfo(destinationPath).absoluteFilePath();
    if (isOpenDatabasePath(destination)) {
        return failure(QStringLiteral("Close the destination database before restoring it"));
    }
    QString errorMessage;
    if (!rejectExistingSidecars(destination, &errorMessage)) {
        return failure(errorMessage);
    }
    if (!ensureParentDirectory(destination, &errorMessage)) {
        return failure(errorMessage);
    }

    return copyAtomically(QFileInfo(backupPath).absoluteFilePath(), destination);
}

} // namespace charging::server
