#include "database_connection.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace charging::server {

namespace {

void clearError(QString* errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
}

bool fail(QString* errorMessage, const QString& message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

QString normalizedSql(const QString& sql)
{
    QString normalized;
    normalized.reserve(sql.size());
    for (const QChar character : sql) {
        if (!character.isSpace() && character != QLatin1Char('"') &&
            character != QLatin1Char('`') && character != QLatin1Char('[') &&
            character != QLatin1Char(']') && character != QLatin1Char(';')) {
            normalized.append(character.toLower());
        }
    }
    return normalized;
}

// Splits the repository-controlled SQL resources at semicolons while respecting
// quoted strings and SQL comments. QSqlQuery intentionally receives one
// statement at a time because the SQLite driver does not accept whole scripts.
QStringList splitSqlStatements(const QString& script)
{
    QStringList statements;
    QString current;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    bool inLineComment = false;
    bool inBlockComment = false;

    for (qsizetype index = 0; index < script.size(); ++index) {
        const QChar character = script.at(index);
        const QChar next = index + 1 < script.size() ? script.at(index + 1) : QChar();

        if (inLineComment) {
            current.append(character);
            if (character == QLatin1Char('\n')) {
                inLineComment = false;
            }
            continue;
        }
        if (inBlockComment) {
            current.append(character);
            if (character == QLatin1Char('*') && next == QLatin1Char('/')) {
                current.append(next);
                ++index;
                inBlockComment = false;
            }
            continue;
        }

        if (!inSingleQuote && !inDoubleQuote && character == QLatin1Char('-') &&
            next == QLatin1Char('-')) {
            current.append(character);
            current.append(next);
            ++index;
            inLineComment = true;
            continue;
        }
        if (!inSingleQuote && !inDoubleQuote && character == QLatin1Char('/') &&
            next == QLatin1Char('*')) {
            current.append(character);
            current.append(next);
            ++index;
            inBlockComment = true;
            continue;
        }

        if (!inDoubleQuote && character == QLatin1Char('\'')) {
            current.append(character);
            if (inSingleQuote && next == QLatin1Char('\'')) {
                current.append(next);
                ++index;
            } else {
                inSingleQuote = !inSingleQuote;
            }
            continue;
        }
        if (!inSingleQuote && character == QLatin1Char('"')) {
            current.append(character);
            if (inDoubleQuote && next == QLatin1Char('"')) {
                current.append(next);
                ++index;
            } else {
                inDoubleQuote = !inDoubleQuote;
            }
            continue;
        }

        if (!inSingleQuote && !inDoubleQuote && character == QLatin1Char(';')) {
            const QString statement = current.trimmed();
            if (!statement.isEmpty()) {
                statements.append(statement);
            }
            current.clear();
            continue;
        }
        current.append(character);
    }

    const QString finalStatement = current.trimmed();
    if (!finalStatement.isEmpty()) {
        statements.append(finalStatement);
    }
    return statements;
}

} // namespace

DatabaseConnection::DatabaseConnection()
    : connectionName_(QStringLiteral("charging-server-%1").arg(
          QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

DatabaseConnection::~DatabaseConnection()
{
    close();
}

bool DatabaseConnection::open(const QString& databasePath, bool loadDemoSeed,
                              QString* errorMessage)
{
    clearError(errorMessage);
    close();

    if (databasePath.trimmed().isEmpty()) {
        return fail(errorMessage, QStringLiteral("Database path must not be empty"));
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return fail(errorMessage, QStringLiteral("Qt SQLite driver QSQLITE is not available"));
    }

    QString resolvedPath = databasePath;
    bool isNewDatabase = databasePath == QStringLiteral(":memory:");
    if (databasePath != QStringLiteral(":memory:")) {
        const QFileInfo fileInfo(databasePath);
        isNewDatabase = !fileInfo.exists() || fileInfo.size() == 0;
        resolvedPath = fileInfo.absoluteFilePath();
        QDir parentDirectory = fileInfo.absoluteDir();
        if (!parentDirectory.exists() && !parentDirectory.mkpath(QStringLiteral("."))) {
            return fail(errorMessage,
                        QStringLiteral("Unable to create the database directory: %1")
                            .arg(parentDirectory.absolutePath()));
        }
    }

    int schemaVersion = 0;
    if (!isNewDatabase) {
        const QString versionConnectionName = QStringLiteral("%1-version-check").arg(connectionName_);
        QString versionError;
        {
            QSqlDatabase versionDatabase =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), versionConnectionName);
            versionDatabase.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
            versionDatabase.setDatabaseName(resolvedPath);
            if (!versionDatabase.open()) {
                versionError = versionDatabase.lastError().text();
            } else {
                QSqlQuery versionQuery(versionDatabase);
                if (!versionQuery.exec(QStringLiteral("PRAGMA user_version")) ||
                    !versionQuery.next()) {
                    versionError = versionQuery.lastError().text();
                } else {
                    schemaVersion = versionQuery.value(0).toInt();
                }
                versionDatabase.close();
            }
        }
        QSqlDatabase::removeDatabase(versionConnectionName);
        if (!versionError.isEmpty()) {
            return fail(errorMessage, QStringLiteral("Unable to read database schema version: %1")
                                          .arg(versionError));
        }
    }
    // Supported on-disk versions: 1 and 2 (pre-user-domain tables, migrated in
    // place below) and 3 (current). 0 means an empty file (treated as new).
    if ((!isNewDatabase && schemaVersion == 0) || schemaVersion < 0 || schemaVersion > 3) {
        return fail(errorMessage, QStringLiteral("Unsupported database schema version: %1")
                                      .arg(schemaVersion));
    }

    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    database_.setDatabaseName(resolvedPath);
    if (!database_.open()) {
        const QString message = QStringLiteral("Unable to open SQLite database: %1")
                                    .arg(database_.lastError().text());
        close();
        return fail(errorMessage, message);
    }
    databasePath_ = resolvedPath;

    if (!executeResourceScript(QStringLiteral(":/database/schema.sql"), errorMessage) ||
        !migrateManagedIndexes(errorMessage) ||
        (loadDemoSeed &&
         !executeResourceScript(QStringLiteral(":/database/seed.sql"), errorMessage))) {
        close();
        return false;
    }
    return true;
}

bool DatabaseConnection::migrateManagedIndexes(QString* errorMessage)
{
    const QList<QPair<QString, QString>> managedIndexes = {
        {QStringLiteral("idx_orders_status_created_at"),
         QStringLiteral("CREATE INDEX idx_orders_status_created_at "
                        "ON orders(status, created_at DESC, id DESC)")},
        {QStringLiteral("idx_operation_logs_admin_created_at"),
         QStringLiteral("CREATE INDEX idx_operation_logs_admin_created_at "
                        "ON operation_logs(admin_id, created_at DESC, id DESC)")}
    };

    if (!database_.transaction()) {
        return fail(errorMessage, QStringLiteral("Unable to start database migration: %1")
                                      .arg(database_.lastError().text()));
    }

    const auto rollbackWithError = [this, errorMessage](const QString& message) {
        database_.rollback();
        return fail(errorMessage, message);
    };

    for (const auto& managedIndex : managedIndexes) {
        QSqlQuery definitionQuery(database_);
        definitionQuery.prepare(QStringLiteral(
            "SELECT sql FROM sqlite_master WHERE type = 'index' AND name = ?"));
        definitionQuery.addBindValue(managedIndex.first);
        if (!definitionQuery.exec()) {
            return rollbackWithError(QStringLiteral("Unable to inspect database index %1: %2")
                                         .arg(managedIndex.first,
                                              definitionQuery.lastError().text()));
        }

        const bool matches = definitionQuery.next() &&
                             normalizedSql(definitionQuery.value(0).toString()) ==
                                 normalizedSql(managedIndex.second);
        definitionQuery.finish();
        if (matches) {
            continue;
        }

        QSqlQuery migrationQuery(database_);
        if (!migrationQuery.exec(QStringLiteral("DROP INDEX IF EXISTS %1")
                                     .arg(managedIndex.first)) ||
            !migrationQuery.exec(managedIndex.second)) {
            return rollbackWithError(QStringLiteral("Unable to rebuild database index %1: %2")
                                         .arg(managedIndex.first,
                                              migrationQuery.lastError().text()));
        }
    }

    QSqlQuery migrationQuery(database_);
    if (!migrationQuery.exec(QStringLiteral("DROP INDEX IF EXISTS "
                                            "idx_chargers_status_updated_at")) ||
        !migrationQuery.exec(QStringLiteral("PRAGMA user_version = 3"))) {
        return rollbackWithError(QStringLiteral("Unable to finish database migration: %1")
                                     .arg(migrationQuery.lastError().text()));
    }
    if (!database_.commit()) {
        return rollbackWithError(QStringLiteral("Unable to commit database migration: %1")
                                     .arg(database_.lastError().text()));
    }
    return true;
}

void DatabaseConnection::close()
{
    databasePath_.clear();
    if (!database_.isValid()) {
        return;
    }

    database_.close();
    database_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName_);
}

bool DatabaseConnection::isOpen() const
{
    return database_.isValid() && database_.isOpen();
}

QString DatabaseConnection::databasePath() const
{
    return databasePath_;
}

QSqlDatabase DatabaseConnection::database() const
{
    return database_;
}

bool DatabaseConnection::executeResourceScript(const QString& resourcePath,
                                               QString* errorMessage)
{
    QFile resource(resourcePath);
    if (!resource.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return fail(errorMessage,
                    QStringLiteral("Unable to read database resource %1").arg(resourcePath));
    }

    const QString script = QString::fromUtf8(resource.readAll());
    const QStringList statements = splitSqlStatements(script);
    for (const QString& statement : statements) {
        QSqlQuery query(database_);
        if (!query.exec(statement)) {
            const QString message = QStringLiteral("Database initialization failed: %1")
                                        .arg(query.lastError().text());
            QSqlQuery rollbackQuery(database_);
            rollbackQuery.exec(QStringLiteral("ROLLBACK"));
            return fail(errorMessage, message);
        }
    }
    return true;
}

} // namespace charging::server
