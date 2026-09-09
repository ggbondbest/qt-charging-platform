#include "database_maintenance.h"

#include "database_connection.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
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
                          const QStringList& expectedColumns, QString* errorMessage,
                          bool allowLegacyExtensions = false)
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
    if (allowLegacyExtensions) {
        const QStringList optional = table == QStringLiteral("stations")
            ? QStringList{QStringLiteral("city"), QStringLiteral("district"),
                          QStringLiteral("contact_name"), QStringLiteral("contact_phone")}
            : table == QStringLiteral("orders")
                ? QStringList{QStringLiteral("telemetry_captured_at"), QStringLiteral("telemetry_power_watts")}
                : QStringList{};
        for (const auto& column : optional) actualColumns.removeAll(column);
    }
    // ALTER TABLE appends columns, whereas fresh CREATE may group them by
    // meaning. Column order is not a schema contract; the exact set is.
    QStringList expected = expectedColumns;
    actualColumns.sort();
    expected.sort();
    if (actualColumns != expected) {
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

bool validatePlatformSchema(const QSqlDatabase& database, QString* errorMessage,
                            bool currentIndexes = true,
                            const QStringList* onlyTables = nullptr)
{
    const auto wanted = [onlyTables](const QString& table) {
        return onlyTables == nullptr || onlyTables->contains(table);
    };
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
            QStringLiteral("created_at")}},
        {QStringLiteral("notifications"), {QStringLiteral("id"), QStringLiteral("user_id"),
            QStringLiteral("type"), QStringLiteral("title"), QStringLiteral("body"),
            QStringLiteral("created_at"), QStringLiteral("read_at")}},
        {QStringLiteral("coupons"), {QStringLiteral("id"), QStringLiteral("user_id"),
            QStringLiteral("kind"), QStringLiteral("title"), QStringLiteral("value_cents"),
            QStringLiteral("discount_tenths"), QStringLiteral("threshold_cents"),
            QStringLiteral("status"), QStringLiteral("source"), QStringLiteral("expires_at"),
            QStringLiteral("created_at"), QStringLiteral("updated_at")}},
        {QStringLiteral("points_ledger"), {QStringLiteral("id"), QStringLiteral("user_id"),
            QStringLiteral("amount"), QStringLiteral("reason"), QStringLiteral("created_at")}},
        {QStringLiteral("user_checkins"), {QStringLiteral("user_id"), QStringLiteral("day"),
            QStringLiteral("created_at")}},
        {QStringLiteral("charger_ratings"), {QStringLiteral("id"), QStringLiteral("user_id"),
            QStringLiteral("charger_id"), QStringLiteral("order_id"), QStringLiteral("rating"),
            QStringLiteral("comment"), QStringLiteral("created_at")}},
        {QStringLiteral("order_pricing_snapshots"), {QStringLiteral("order_id"),
            QStringLiteral("version"), QStringLiteral("unit_price_cents_per_kwh"),
            QStringLiteral("captured_at")}},
        {QStringLiteral("charger_exceptions"), {QStringLiteral("id"), QStringLiteral("charger_id"),
            QStringLiteral("code"), QStringLiteral("severity"), QStringLiteral("safe_summary"),
            QStringLiteral("status"), QStringLiteral("occurred_at"), QStringLiteral("acknowledged_at"),
            QStringLiteral("recovered_at"), QStringLiteral("recoverable"), QStringLiteral("recovery_action"),
            QStringLiteral("recovered_by_admin_id"), QStringLiteral("recovery_command_id"),
            QStringLiteral("recovery_message"), QStringLiteral("updated_at")}}
    };
    for (const auto& table : tables) {
        if (!wanted(table.first)) {
            continue;
        }
        QStringList expected = table.second;
        if (onlyTables == nullptr && table.first == QStringLiteral("stations"))
            expected << QStringLiteral("city") << QStringLiteral("district")
                     << QStringLiteral("contact_name") << QStringLiteral("contact_phone");
        if (onlyTables == nullptr && table.first == QStringLiteral("orders"))
            expected << QStringLiteral("telemetry_captured_at") << QStringLiteral("telemetry_power_watts");
        if (!validateTableColumns(database, table.first, expected, errorMessage, onlyTables != nullptr)) {
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
            QStringLiteral("status IN ('SUCCESS', 'FAILED')")}},
        {QStringLiteral("notifications"), {
            QStringLiteral("type IN ('CHARGING_STOPPED', 'ORDER_PAID', 'RESERVATION_EXPIRY_REMINDER')")}},
        {QStringLiteral("coupons"), {
            QStringLiteral("kind IN ('CASH', 'DISCOUNT')"),
            QStringLiteral("status IN ('AVAILABLE', 'USED', 'EXPIRED')")}},
        {QStringLiteral("points_ledger"), {
            QStringLiteral("amount BETWEEN -9007199254740991 AND 9007199254740991"),
            QStringLiteral("length(trim(reason)) BETWEEN 1 AND 32")}},
        {QStringLiteral("user_checkins"), {
            QStringLiteral("day GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'")}},
        {QStringLiteral("charger_ratings"), {
            QStringLiteral("order_id INTEGER NOT NULL UNIQUE"),
            QStringLiteral("rating BETWEEN 1 AND 5"),
            QStringLiteral("length(comment) <= 140")}},
        {QStringLiteral("order_pricing_snapshots"), {
            QStringLiteral("order_id INTEGER PRIMARY KEY"),
            QStringLiteral("version = 'energy-only-v1'"),
            QStringLiteral("unit_price_cents_per_kwh BETWEEN 0 AND 9007199254740991")}},
        {QStringLiteral("charger_exceptions"), {
            QStringLiteral("code IN ('SIMULATED_FAULT', 'SIMULATED_OFFLINE')"),
            QStringLiteral("severity IN ('WARNING', 'CRITICAL')"),
            QStringLiteral("status IN ('ACTIVE', 'ACKNOWLEDGED', 'RECOVERING', 'RECOVERED')"),
            QStringLiteral("recoverable IN (0, 1)"),
            QStringLiteral("recovery_action = 'SIMULATE_RESTORE'")}}
    };
    for (const auto& table : tableConstraints) {
        if (!wanted(table.first)) {
            continue;
        }
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
        {QStringLiteral("operation_logs"), QStringLiteral("admin_id"), QStringLiteral("admins")},
        {QStringLiteral("notifications"), QStringLiteral("user_id"), QStringLiteral("users")},
        {QStringLiteral("coupons"), QStringLiteral("user_id"), QStringLiteral("users")},
        {QStringLiteral("points_ledger"), QStringLiteral("user_id"), QStringLiteral("users")},
        {QStringLiteral("user_checkins"), QStringLiteral("user_id"), QStringLiteral("users")},
        {QStringLiteral("charger_ratings"), QStringLiteral("user_id"), QStringLiteral("users")},
        {QStringLiteral("charger_ratings"), QStringLiteral("charger_id"), QStringLiteral("chargers")},
        {QStringLiteral("charger_ratings"), QStringLiteral("order_id"), QStringLiteral("orders")},
        {QStringLiteral("order_pricing_snapshots"), QStringLiteral("order_id"), QStringLiteral("orders")},
        {QStringLiteral("charger_exceptions"), QStringLiteral("charger_id"), QStringLiteral("chargers")},
        {QStringLiteral("charger_exceptions"), QStringLiteral("recovered_by_admin_id"), QStringLiteral("admins")}
    };
    for (const QStringList& foreignKey : foreignKeys) {
        if (!wanted(foreignKey.at(0))) {
            continue;
        }
        if (!validateForeignKey(database, foreignKey.at(0), foreignKey.at(1), foreignKey.at(2),
                                errorMessage)) {
            return false;
        }
    }

    QList<IndexDefinition> indexes = {
        {QStringLiteral("idx_charger_exceptions_charger_status_occurred"),
         QStringLiteral("charger_exceptions"),
         {QStringLiteral("charger_id"), QStringLiteral("status"), QStringLiteral("occurred_at"),
          QStringLiteral("id")}, false, {}},
        {QStringLiteral("idx_charger_exceptions_occurred"), QStringLiteral("charger_exceptions"),
         {QStringLiteral("occurred_at"), QStringLiteral("id")}, false, {}},
        {QStringLiteral("idx_stations_status"), QStringLiteral("stations"),
         {QStringLiteral("status")}, false, {}},
        {QStringLiteral("idx_chargers_station_status"), QStringLiteral("chargers"),
         {QStringLiteral("station_id"), QStringLiteral("status")}, false, {}},
        {QStringLiteral("idx_chargers_updated_at"), QStringLiteral("chargers"),
         {QStringLiteral("updated_at"), QStringLiteral("id")}, false, {}},
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
         currentIndexes
             ? QStringList{QStringLiteral("status"), QStringLiteral("created_at"),
                           QStringLiteral("id")}
             : QStringList{QStringLiteral("status"), QStringLiteral("created_at")},
         false, {}},
        {QStringLiteral("idx_orders_created_at"), QStringLiteral("orders"),
         {QStringLiteral("created_at"), QStringLiteral("id")}, false, {}},
        {QStringLiteral("idx_users_status_id"), QStringLiteral("users"),
         {QStringLiteral("status"), QStringLiteral("id")}, false, {}},
        {QStringLiteral("idx_recharge_records_user_created_at"),
         QStringLiteral("recharge_records"),
         {QStringLiteral("user_id"), QStringLiteral("created_at")}, false, {}},
        {QStringLiteral("idx_recharge_records_status_created_at"),
         QStringLiteral("recharge_records"),
         {QStringLiteral("status"), QStringLiteral("created_at"), QStringLiteral("id")}, false,
         {}},
        {QStringLiteral("idx_recharge_records_created_at"),
         QStringLiteral("recharge_records"),
         {QStringLiteral("created_at"), QStringLiteral("id")}, false, {}},
        {QStringLiteral("idx_operation_logs_admin_created_at"),
         QStringLiteral("operation_logs"),
         currentIndexes
             ? QStringList{QStringLiteral("admin_id"), QStringLiteral("created_at"),
                           QStringLiteral("id")}
             : QStringList{QStringLiteral("admin_id"), QStringLiteral("created_at")},
         false, {}},
        {QStringLiteral("idx_operation_logs_action_created_at"),
         QStringLiteral("operation_logs"),
         {QStringLiteral("action"), QStringLiteral("created_at"), QStringLiteral("id")}, false,
         {}},
        {QStringLiteral("idx_operation_logs_created_at"), QStringLiteral("operation_logs"),
         {QStringLiteral("created_at"), QStringLiteral("id")}, false, {}},
        {QStringLiteral("idx_notifications_user_created_at"), QStringLiteral("notifications"),
         {QStringLiteral("user_id"), QStringLiteral("created_at")}, false, {}},
        {QStringLiteral("idx_coupons_user_status"), QStringLiteral("coupons"),
         {QStringLiteral("user_id"), QStringLiteral("status")}, false, {}},
        {QStringLiteral("idx_coupons_user_created_at"), QStringLiteral("coupons"),
         {QStringLiteral("user_id"), QStringLiteral("created_at")}, false, {}},
        {QStringLiteral("idx_points_ledger_user_created_at"), QStringLiteral("points_ledger"),
         {QStringLiteral("user_id"), QStringLiteral("created_at")}, false, {}},
        {QStringLiteral("idx_charger_ratings_user_created_at"), QStringLiteral("charger_ratings"),
         {QStringLiteral("user_id"), QStringLiteral("created_at")}, false, {}},
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
    if (currentIndexes) {
        indexes.append({QStringLiteral("idx_chargers_abnormal_updated_at"),
                        QStringLiteral("chargers"),
                        {QStringLiteral("updated_at"), QStringLiteral("id")}, false,
                        QStringLiteral("status IN ('FAULT','OFFLINE')")});
    } else {
        indexes.append({QStringLiteral("idx_chargers_status_updated_at"),
                        QStringLiteral("chargers"),
                        {QStringLiteral("status"), QStringLiteral("updated_at"),
                         QStringLiteral("id")},
                        false, {}});
    }
    // A table-filtered run (legacy migration gate) checks shape only; index
    // generations differ across versions there, and the post-migration strict
    // validation covers them afterwards.
    if (onlyTables == nullptr) {
        for (const IndexDefinition& index : indexes) {
            if (!validateIndex(database, index, errorMessage)) {
                return false;
            }
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

// Shape gate for the migration path: the source must be a healthy platform
// database already carrying the eight core tables (any supported legacy
// user_version 1..3). Without this check, an unrelated or empty SQLite file
// would be "migrated" into a fresh empty schema and restored as if valid.
// Index shape is intentionally not gated (the two index generations differ;
// the post-migration strict validation covers it).
DatabaseMaintenanceResult validateMigratableRestoreSource(const QString& databasePath)
{
    const QString path = QFileInfo(databasePath).absoluteFilePath();
    const QFileInfo fileInfo(path);
    if (!fileInfo.isFile() || fileInfo.size() == 0) {
        return failure(QStringLiteral("Database backup does not exist or is empty: %1").arg(path));
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return failure(QStringLiteral("Qt SQLite driver QSQLITE is not available"));
    }

    const QStringList coreTables = {
        QStringLiteral("users"), QStringLiteral("admins"), QStringLiteral("stations"),
        QStringLiteral("chargers"), QStringLiteral("reservations"), QStringLiteral("orders"),
        QStringLiteral("recharge_records"), QStringLiteral("operation_logs")};

    const QString connectionName = QStringLiteral("legacy-restore-validation-%1").arg(
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
                !integrityQuery.next() ||
                integrityQuery.value(0).toString() != QStringLiteral("ok")) {
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
                 !versionQuery.next())) {
                result = failure(QStringLiteral("Unable to read database schema version: %1")
                                     .arg(versionQuery.lastError().text()));
            } else if (result.ok) {
                const int version = versionQuery.value(0).toInt();
                if (version < 1 || version > 3) {
                    result = failure(QStringLiteral("Unsupported database schema version"));
                }
            }
            if (result.ok) {
                QString schemaError;
                if (!validatePlatformSchema(database, &schemaError, true, &coreTables)) {
                    result = failure(schemaError);
                }
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return result;
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
                 !versionQuery.next() || versionQuery.value(0).toInt() != 4)) {
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

    // Strict path: the backup already carries the current schema. Otherwise it
    // may be an older supported version (user_version 1..3). Migrate a
    // temporary copy by applying
    // schema.sql — the original backup file is never modified — and only then
    // validate and restore the migrated copy.
    QString sourcePath = QFileInfo(backupPath).absoluteFilePath();
    const DatabaseMaintenanceResult currentValidation = validate(sourcePath);
    QString migratedPath;
    if (!currentValidation.ok) {
        const DatabaseMaintenanceResult legacyShape =
            validateMigratableRestoreSource(sourcePath);
        if (!legacyShape.ok) {
            return legacyShape;
        }
        migratedPath = QStringLiteral("%1.migrate-%2")
                           .arg(sourcePath, QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!QFile::copy(sourcePath, migratedPath)) {
            return failure(QStringLiteral("Unable to copy backup for migration: %1")
                               .arg(migratedPath));
        }
        QString migrationError;
        {
            DatabaseConnection migration;
            if (migration.open(migratedPath, false, &migrationError)) {
                migration.close();  // last connection checkpoints and drops WAL sidecars
            }
        }
        if (!migrationError.isEmpty()) {
            QFile::remove(migratedPath);
            return failure(QStringLiteral("Unable to migrate legacy backup: %1")
                               .arg(migrationError));
        }
        const DatabaseMaintenanceResult migratedValidation = validate(migratedPath);
        if (!migratedValidation.ok) {
            QFile::remove(migratedPath);
            return failure(QStringLiteral("Legacy backup did not become a valid database "
                                          "after migration: %1")
                               .arg(migratedValidation.errorMessage));
        }
        sourcePath = migratedPath;
    }

    const QString destination = QFileInfo(destinationPath).absoluteFilePath();
    auto cleanupMigratedCopy = [&migratedPath]() {
        if (!migratedPath.isEmpty()) {
            QFile::remove(migratedPath);
            QFile::remove(migratedPath + QStringLiteral("-wal"));
            QFile::remove(migratedPath + QStringLiteral("-shm"));
        }
    };
    if (isOpenDatabasePath(destination)) {
        cleanupMigratedCopy();
        return failure(QStringLiteral("Close the destination database before restoring it"));
    }
    QString errorMessage;
    if (!rejectExistingSidecars(destination, &errorMessage)) {
        cleanupMigratedCopy();
        return failure(errorMessage);
    }
    if (!ensureParentDirectory(destination, &errorMessage)) {
        cleanupMigratedCopy();
        return failure(errorMessage);
    }

    const DatabaseMaintenanceResult copied = copyAtomically(sourcePath, destination);
    cleanupMigratedCopy();
    return copied;
}

} // namespace charging::server
