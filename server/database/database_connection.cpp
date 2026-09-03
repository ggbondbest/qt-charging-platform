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
    if (databasePath != QStringLiteral(":memory:")) {
        const QFileInfo fileInfo(databasePath);
        resolvedPath = fileInfo.absoluteFilePath();
        QDir parentDirectory = fileInfo.absoluteDir();
        if (!parentDirectory.exists() && !parentDirectory.mkpath(QStringLiteral("."))) {
            return fail(errorMessage,
                        QStringLiteral("Unable to create the database directory: %1")
                            .arg(parentDirectory.absolutePath()));
        }
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
        (loadDemoSeed &&
         !executeResourceScript(QStringLiteral(":/database/seed.sql"), errorMessage))) {
        close();
        return false;
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
