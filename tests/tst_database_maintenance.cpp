#include "database_connection.h"
#include "database_maintenance.h"

#include <QFile>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using charging::server::DatabaseConnection;
using charging::server::DatabaseMaintenance;

class DatabaseMaintenanceTest final : public QObject
{
    Q_OBJECT

private slots:
    void backupValidateAndRestore();
    void rejectsInvalidBackupAndOpenDestination();
    void backupRejectsSourceAliasesAndOpenDatabases();
    void backupReplacementIsSafe();
    void rejectsWrongSchemaWithoutChangingDestination();
    void restoreRejectsResidualWalWithoutChangingDestination();
    void backupRejectsResidualWalWithoutChangingDestination();
    void rejectsIndexesWithWrongColumnsOrPredicate();
    void upgradesLegacyAdminIndexesWithoutLosingData();
    void restoresLegacyBackupThenMigratesOnFirstOpen();
    void restoresRealPreExpansionV2Backup();
    void rejectsLegacyBackupWithInvalidUniqueIndex();
    void rejectsFutureSchemaVersionWithoutChangingIt();
};

void DatabaseMaintenanceTest::backupValidateAndRestore()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("source.sqlite"));
    const QString backupPath = directory.filePath(QStringLiteral("backup/db.sqlite"));
    const QString restoredPath = directory.filePath(QStringLiteral("restored/db.sqlite"));

    DatabaseConnection source;
    QString errorMessage;
    QVERIFY2(source.open(sourcePath, true, &errorMessage), qPrintable(errorMessage));
    QSqlQuery insert(source.database());
    QVERIFY2(insert.exec(QStringLiteral(
                 "INSERT INTO users(phone, nickname) VALUES('13900009999', 'backup-user')")),
             qPrintable(insert.lastError().text()));

    const auto backupResult = DatabaseMaintenance::backup(source.database(), backupPath);
    QVERIFY2(backupResult.ok, qPrintable(backupResult.errorMessage));
    const auto validationResult = DatabaseMaintenance::validate(backupPath);
    QVERIFY2(validationResult.ok, qPrintable(validationResult.errorMessage));

    const auto restoreResult = DatabaseMaintenance::restore(backupPath, restoredPath);
    QVERIFY2(restoreResult.ok, qPrintable(restoreResult.errorMessage));
    DatabaseConnection restored;
    QVERIFY2(restored.open(restoredPath, false, &errorMessage), qPrintable(errorMessage));
    QSqlQuery count(restored.database());
    QVERIFY(count.exec(QStringLiteral("SELECT COUNT(*) FROM users WHERE phone='13900009999'")));
    QVERIFY(count.next());
    QCOMPARE(count.value(0).toInt(), 1);
}

void DatabaseMaintenanceTest::rejectsInvalidBackupAndOpenDestination()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString invalidPath = directory.filePath(QStringLiteral("invalid.sqlite"));
    QFile invalidFile(invalidPath);
    QVERIFY(invalidFile.open(QIODevice::WriteOnly));
    QCOMPARE(invalidFile.write("not a database"), qint64(14));
    invalidFile.close();
    QVERIFY(!DatabaseMaintenance::validate(invalidPath).ok);

    const QString sourcePath = directory.filePath(QStringLiteral("source.sqlite"));
    const QString backupPath = directory.filePath(QStringLiteral("backup.sqlite"));
    DatabaseConnection source;
    QString errorMessage;
    QVERIFY2(source.open(sourcePath, true, &errorMessage), qPrintable(errorMessage));
    QVERIFY(DatabaseMaintenance::backup(source.database(), backupPath).ok);
    QVERIFY(!DatabaseMaintenance::restore(backupPath, sourcePath).ok);
}

void DatabaseMaintenanceTest::backupRejectsSourceAliasesAndOpenDatabases()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("source.sqlite"));
    const QString otherPath = directory.filePath(QStringLiteral("other.sqlite"));
    DatabaseConnection source;
    DatabaseConnection other;
    QString errorMessage;
    QVERIFY2(source.open(sourcePath, true, &errorMessage), qPrintable(errorMessage));
    QVERIFY2(other.open(otherPath, true, &errorMessage), qPrintable(errorMessage));

    QVERIFY(!DatabaseMaintenance::backup(source.database(), sourcePath).ok);
    const QString aliasPath = QDir(directory.path()).filePath(QStringLiteral("./source.sqlite"));
    QVERIFY(!DatabaseMaintenance::backup(source.database(), aliasPath).ok);
    QVERIFY(!DatabaseMaintenance::backup(source.database(), otherPath).ok);

    QSqlQuery sourceWrite(source.database());
    QVERIFY2(sourceWrite.exec(QStringLiteral(
                 "INSERT INTO users(phone, nickname) VALUES('13900008888', 'source-still-open')")),
             qPrintable(sourceWrite.lastError().text()));
    QSqlQuery otherWrite(other.database());
    QVERIFY2(otherWrite.exec(QStringLiteral(
                 "INSERT INTO users(phone, nickname) VALUES('13900007777', 'other-still-open')")),
             qPrintable(otherWrite.lastError().text()));
}

void DatabaseMaintenanceTest::backupReplacementIsSafe()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("source.sqlite"));
    const QString backupPath = directory.filePath(QStringLiteral("backup.sqlite"));
    DatabaseConnection source;
    QString errorMessage;
    QVERIFY2(source.open(sourcePath, true, &errorMessage), qPrintable(errorMessage));

    QFile oldBackup(backupPath);
    QVERIFY(oldBackup.open(QIODevice::WriteOnly));
    QCOMPARE(oldBackup.write("old backup"), qint64(10));
    oldBackup.close();
    QVERIFY2(DatabaseMaintenance::backup(source.database(), backupPath).ok,
             "An existing backup should be replaced atomically");
    QVERIFY(DatabaseMaintenance::validate(backupPath).ok);

    const QString blockedDestination = directory.filePath(QStringLiteral("keep-directory"));
    QVERIFY(QDir().mkpath(blockedDestination));
    QFile marker(QDir(blockedDestination).filePath(QStringLiteral("marker")));
    QVERIFY(marker.open(QIODevice::WriteOnly));
    QCOMPARE(marker.write("keep"), qint64(4));
    marker.close();
    QVERIFY(!DatabaseMaintenance::backup(source.database(), blockedDestination).ok);
    QVERIFY(QFileInfo::exists(QDir(blockedDestination).filePath(QStringLiteral("marker"))));

#ifdef Q_OS_UNIX
    const QString readOnlyDirectory = directory.filePath(QStringLiteral("read-only"));
    QVERIFY(QDir().mkpath(readOnlyDirectory));
    const QString preservedPath = QDir(readOnlyDirectory).filePath(QStringLiteral("backup.sqlite"));
    QFile preserved(preservedPath);
    QVERIFY(preserved.open(QIODevice::WriteOnly));
    QCOMPARE(preserved.write("preserve old backup"), qint64(19));
    preserved.close();
    QVERIFY(QFile::setPermissions(readOnlyDirectory,
                                  QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const auto failedReplacement = DatabaseMaintenance::backup(source.database(), preservedPath);
    QVERIFY(QFile::setPermissions(readOnlyDirectory,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner));
    QVERIFY(!failedReplacement.ok);
    QVERIFY(preserved.open(QIODevice::ReadOnly));
    QCOMPARE(preserved.readAll(), QByteArray("preserve old backup"));
#endif
}

void DatabaseMaintenanceTest::rejectsWrongSchemaWithoutChangingDestination()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString wrongPath = directory.filePath(QStringLiteral("wrong.sqlite"));
    const QString connectionName = QStringLiteral("wrong-schema-test");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(wrongPath);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE marker(value INTEGER)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO marker VALUES(42)")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 1")));
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    QVERIFY(!DatabaseMaintenance::validate(wrongPath).ok);

    const QString destinationPath = directory.filePath(QStringLiteral("existing.sqlite"));
    QFile destination(destinationPath);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    QCOMPARE(destination.write("original target"), qint64(15));
    destination.close();
    QVERIFY(!DatabaseMaintenance::restore(wrongPath, destinationPath).ok);
    QVERIFY(destination.open(QIODevice::ReadOnly));
    QCOMPARE(destination.readAll(), QByteArray("original target"));
}

void DatabaseMaintenanceTest::restoreRejectsResidualWalWithoutChangingDestination()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("source.sqlite"));
    const QString backupPath = directory.filePath(QStringLiteral("backup.sqlite"));
    const QString destinationPath = directory.filePath(QStringLiteral("destination.sqlite"));
    DatabaseConnection source;
    QString errorMessage;
    QVERIFY2(source.open(sourcePath, true, &errorMessage), qPrintable(errorMessage));
    QVERIFY(DatabaseMaintenance::backup(source.database(), backupPath).ok);

    QFile destination(destinationPath);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    QCOMPARE(destination.write("original database"), qint64(17));
    destination.close();
    QFile wal(destinationPath + QStringLiteral("-wal"));
    QVERIFY(wal.open(QIODevice::WriteOnly));
    QCOMPARE(wal.write("residual wal"), qint64(12));
    wal.close();

    QVERIFY(!DatabaseMaintenance::restore(backupPath, destinationPath).ok);
    QVERIFY(destination.open(QIODevice::ReadOnly));
    QCOMPARE(destination.readAll(), QByteArray("original database"));
    QVERIFY(QFileInfo::exists(destinationPath + QStringLiteral("-wal")));
}

void DatabaseMaintenanceTest::backupRejectsResidualWalWithoutChangingDestination()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("source.sqlite"));
    const QString destinationPath = directory.filePath(QStringLiteral("destination.sqlite"));
    DatabaseConnection source;
    QString errorMessage;
    QVERIFY2(source.open(sourcePath, true, &errorMessage), qPrintable(errorMessage));

    QFile destination(destinationPath);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    QCOMPARE(destination.write("previous backup"), qint64(15));
    destination.close();
    QFile wal(destinationPath + QStringLiteral("-wal"));
    QVERIFY(wal.open(QIODevice::WriteOnly));
    QCOMPARE(wal.write("residual wal"), qint64(12));
    wal.close();

    QVERIFY(!DatabaseMaintenance::backup(source.database(), destinationPath).ok);
    QVERIFY(destination.open(QIODevice::ReadOnly));
    QCOMPARE(destination.readAll(), QByteArray("previous backup"));
    QVERIFY(QFileInfo::exists(destinationPath + QStringLiteral("-wal")));
}

void DatabaseMaintenanceTest::rejectsIndexesWithWrongColumnsOrPredicate()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("source.sqlite"));
    const QString wrongColumnsPath = directory.filePath(QStringLiteral("wrong-columns.sqlite"));
    const QString wrongPredicatePath = directory.filePath(QStringLiteral("wrong-predicate.sqlite"));
    const QString wrongCasePath = directory.filePath(QStringLiteral("wrong-case.sqlite"));
    DatabaseConnection source;
    QString errorMessage;
    QVERIFY2(source.open(sourcePath, true, &errorMessage), qPrintable(errorMessage));
    QVERIFY(DatabaseMaintenance::backup(source.database(), wrongColumnsPath).ok);
    QVERIFY(DatabaseMaintenance::backup(source.database(), wrongPredicatePath).ok);
    QVERIFY(DatabaseMaintenance::backup(source.database(), wrongCasePath).ok);
    source.close();

    const auto mutateIndex = [](const QString& path, const QString& replacement) {
        const QString connectionName = QStringLiteral("index-mutation-%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        bool ok = false;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                              connectionName);
            database.setDatabaseName(path);
            if (database.open()) {
                QSqlQuery query(database);
                ok = query.exec(QStringLiteral("DROP INDEX ux_orders_unfinished_user")) &&
                     query.exec(replacement);
                database.close();
            }
        }
        QSqlDatabase::removeDatabase(connectionName);
        return ok;
    };

    QVERIFY(mutateIndex(wrongColumnsPath, QStringLiteral(
        "CREATE UNIQUE INDEX ux_orders_unfinished_user ON orders(id) "
        "WHERE status IN ('RESERVED', 'CHARGING', 'WAITING_PAYMENT')")));
    QVERIFY(!DatabaseMaintenance::validate(wrongColumnsPath).ok);

    QVERIFY(mutateIndex(wrongPredicatePath, QStringLiteral(
        "CREATE UNIQUE INDEX ux_orders_unfinished_user ON orders(user_id) "
        "WHERE status = 'COMPLETED'")));
    QVERIFY(!DatabaseMaintenance::validate(wrongPredicatePath).ok);

    QVERIFY(mutateIndex(wrongCasePath, QStringLiteral(
        "CREATE UNIQUE INDEX ux_orders_unfinished_user ON orders(user_id) "
        "WHERE status IN ('reserved', 'CHARGING', 'WAITING_PAYMENT')")));
    QVERIFY(!DatabaseMaintenance::validate(wrongCasePath).ok);
}

void DatabaseMaintenanceTest::upgradesLegacyAdminIndexesWithoutLosingData()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("legacy.sqlite"));
    QString errorMessage;

    DatabaseConnection legacy;
    QVERIFY2(legacy.open(databasePath, true, &errorMessage), qPrintable(errorMessage));
    QSqlQuery mutation(legacy.database());
    QVERIFY(mutation.exec(QStringLiteral(
        "INSERT INTO users(phone, nickname) VALUES('13900006666', 'preserved-user')")));
    QVERIFY(mutation.exec(QStringLiteral("DROP INDEX idx_orders_status_created_at")));
    QVERIFY(mutation.exec(QStringLiteral(
        "CREATE INDEX idx_orders_status_created_at "
        "ON orders(status, created_at DESC)")));
    QVERIFY(mutation.exec(QStringLiteral("DROP INDEX idx_operation_logs_admin_created_at")));
    QVERIFY(mutation.exec(QStringLiteral(
        "CREATE INDEX idx_operation_logs_admin_created_at "
        "ON operation_logs(admin_id, created_at DESC)")));
    QVERIFY(mutation.exec(QStringLiteral(
        "CREATE INDEX idx_chargers_status_updated_at "
        "ON chargers(status, updated_at DESC, id DESC)")));
    QVERIFY(mutation.exec(QStringLiteral("PRAGMA user_version = 1")));
    legacy.close();

    DatabaseConnection upgraded;
    QVERIFY2(upgraded.open(databasePath, false, &errorMessage), qPrintable(errorMessage));

    QSqlQuery verification(upgraded.database());
    QVERIFY(verification.exec(QStringLiteral("PRAGMA user_version")));
    QVERIFY(verification.next());
    QCOMPARE(verification.value(0).toInt(), 4);

    QVERIFY(verification.exec(QStringLiteral(
        "SELECT COUNT(*) FROM users WHERE phone = '13900006666'")));
    QVERIFY(verification.next());
    QCOMPARE(verification.value(0).toInt(), 1);

    const auto indexSql = [&verification](const QString& indexName) {
        verification.prepare(QStringLiteral(
            "SELECT sql FROM sqlite_master WHERE type = 'index' AND name = ?"));
        verification.addBindValue(indexName);
        if (!verification.exec() || !verification.next()) {
            return QString();
        }
        return verification.value(0).toString();
    };
    QVERIFY(indexSql(QStringLiteral("idx_orders_status_created_at"))
                .contains(QStringLiteral("id DESC"), Qt::CaseInsensitive));
    QVERIFY(indexSql(QStringLiteral("idx_operation_logs_admin_created_at"))
                .contains(QStringLiteral("id DESC"), Qt::CaseInsensitive));
    QVERIFY(indexSql(QStringLiteral("idx_chargers_status_updated_at")).isEmpty());

    upgraded.close();
    const auto validation = DatabaseMaintenance::validate(databasePath);
    QVERIFY2(validation.ok, qPrintable(validation.errorMessage));
}

void DatabaseMaintenanceTest::restoresLegacyBackupThenMigratesOnFirstOpen()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString legacyPath = directory.filePath(QStringLiteral("legacy-v1.sqlite"));
    const QString restoredPath = directory.filePath(QStringLiteral("restored.sqlite"));
    QString errorMessage;

    DatabaseConnection legacy;
    QVERIFY2(legacy.open(legacyPath, true, &errorMessage), qPrintable(errorMessage));
    QSqlQuery mutation(legacy.database());
    QVERIFY(mutation.exec(QStringLiteral(
        "INSERT INTO users(phone, nickname) VALUES('13900005555', 'legacy-restore-user')")));
    QVERIFY(mutation.exec(QStringLiteral("DROP INDEX idx_orders_status_created_at")));
    QVERIFY(mutation.exec(QStringLiteral(
        "CREATE INDEX idx_orders_status_created_at "
        "ON orders(status, created_at DESC)")));
    QVERIFY(mutation.exec(QStringLiteral("DROP INDEX idx_operation_logs_admin_created_at")));
    QVERIFY(mutation.exec(QStringLiteral(
        "CREATE INDEX idx_operation_logs_admin_created_at "
        "ON operation_logs(admin_id, created_at DESC)")));
    QVERIFY(mutation.exec(QStringLiteral(
        "CREATE INDEX idx_chargers_status_updated_at "
        "ON chargers(status, updated_at DESC, id DESC)")));
    QVERIFY(mutation.exec(QStringLiteral("PRAGMA user_version = 1")));
    legacy.close();

    const auto restoreResult = DatabaseMaintenance::restore(legacyPath, restoredPath);
    QVERIFY2(restoreResult.ok, qPrintable(restoreResult.errorMessage));

    DatabaseConnection restored;
    QVERIFY2(restored.open(restoredPath, false, &errorMessage), qPrintable(errorMessage));
    QSqlQuery verification(restored.database());
    QVERIFY(verification.exec(QStringLiteral("PRAGMA user_version")));
    QVERIFY(verification.next());
    QCOMPARE(verification.value(0).toInt(), 4);
    QVERIFY(verification.exec(QStringLiteral(
        "SELECT COUNT(*) FROM users WHERE phone = '13900005555'")));
    QVERIFY(verification.next());
    QCOMPARE(verification.value(0).toInt(), 1);
    restored.close();

    const auto validation = DatabaseMaintenance::validate(restoredPath);
    QVERIFY2(validation.ok, qPrintable(validation.errorMessage));
}

void DatabaseMaintenanceTest::restoresRealPreExpansionV2Backup()
{
    // A genuine pre-expansion v2 backup: the eight original tables with the
    // v2 index shapes and user_version = 2, as created before the five
    // user-domain tables shipped. Restore must migrate a temporary copy to
    // version 4 while the original backup file stays untouched.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString legacyPath = directory.filePath(QStringLiteral("preexpansion-v2.sqlite"));
    const QString restoredPath = directory.filePath(QStringLiteral("restored.sqlite"));
    QString errorMessage;

    DatabaseConnection seeded;
    QVERIFY2(seeded.open(legacyPath, true, &errorMessage), qPrintable(errorMessage));
    QSqlQuery mutation(seeded.database());
    QVERIFY(mutation.exec(QStringLiteral(
        "INSERT INTO users(phone, nickname) VALUES('13900007777', 'pre-expansion-user')")));
    // Children first so foreign keys cannot refuse the drop.
    for (const QString& table : {QStringLiteral("charger_ratings"), QStringLiteral("user_checkins"),
                                 QStringLiteral("points_ledger"), QStringLiteral("coupons"),
                                 QStringLiteral("notifications"), QStringLiteral("charger_exceptions"),
                                 QStringLiteral("order_pricing_snapshots")}) {
        QVERIFY(mutation.exec(QStringLiteral("DROP TABLE %1").arg(table)));
    }
    for (const auto* sql : {"ALTER TABLE stations DROP COLUMN city",
                           "ALTER TABLE stations DROP COLUMN district",
                           "ALTER TABLE stations DROP COLUMN contact_name",
                           "ALTER TABLE stations DROP COLUMN contact_phone",
                           "ALTER TABLE orders DROP COLUMN telemetry_captured_at",
                           "ALTER TABLE orders DROP COLUMN telemetry_power_watts"})
        QVERIFY2(mutation.exec(QString::fromLatin1(sql)), qPrintable(mutation.lastError().text()));
    QVERIFY(mutation.exec(QStringLiteral("PRAGMA user_version = 2")));
    seeded.close();

    const auto strictValidation = DatabaseMaintenance::validate(legacyPath);
    QVERIFY(!strictValidation.ok);  // eight tables are not the current schema

    const auto restoreResult = DatabaseMaintenance::restore(legacyPath, restoredPath);
    QVERIFY2(restoreResult.ok, qPrintable(restoreResult.errorMessage));

    const QString checkConnection = QStringLiteral("preexpansion-check-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase restored = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                          checkConnection);
        restored.setDatabaseName(restoredPath);
        QVERIFY(restored.open());
        QSqlQuery verification(restored);
        QVERIFY(verification.exec(QStringLiteral("PRAGMA user_version")));
        QVERIFY(verification.next());
        QCOMPARE(verification.value(0).toInt(), 4);
        QVERIFY(verification.exec(QStringLiteral(
            "SELECT COUNT(*) FROM users WHERE phone = '13900007777'")));
        QVERIFY(verification.next());
        QCOMPARE(verification.value(0).toInt(), 1);  // business data survived
        QVERIFY(verification.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
            "AND name IN ('notifications','coupons','points_ledger',"
            "'user_checkins','charger_ratings')")));
        QVERIFY(verification.next());
        QCOMPARE(verification.value(0).toInt(), 5);  // all user-domain tables added
        restored.close();
    }
    QSqlDatabase::removeDatabase(checkConnection);
    const auto restoredValidation = DatabaseMaintenance::validate(restoredPath);
    QVERIFY2(restoredValidation.ok, qPrintable(restoredValidation.errorMessage));

    // The original backup must still read as the untouched v2 database.
    const QString originalConnection = QStringLiteral("preexpansion-original-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase original = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                          originalConnection);
        original.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        original.setDatabaseName(legacyPath);
        QVERIFY(original.open());
        QSqlQuery verification(original);
        QVERIFY(verification.exec(QStringLiteral("PRAGMA user_version")));
        QVERIFY(verification.next());
        QCOMPARE(verification.value(0).toInt(), 2);
        QVERIFY(verification.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
            "AND name NOT LIKE 'sqlite_%'")));
        QVERIFY(verification.next());
        QCOMPARE(verification.value(0).toInt(), 8);
        original.close();
    }
    QSqlDatabase::removeDatabase(originalConnection);
}

void DatabaseMaintenanceTest::rejectsLegacyBackupWithInvalidUniqueIndex()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString legacyPath = directory.filePath(QStringLiteral("invalid-legacy.sqlite"));
    const QString restoredPath = directory.filePath(QStringLiteral("restored.sqlite"));
    QString errorMessage;

    DatabaseConnection legacy;
    QVERIFY2(legacy.open(legacyPath, true, &errorMessage), qPrintable(errorMessage));
    QSqlQuery mutation(legacy.database());
    QVERIFY(mutation.exec(QStringLiteral("DROP INDEX ux_orders_unfinished_user")));
    QVERIFY(mutation.exec(QStringLiteral(
        "CREATE UNIQUE INDEX ux_orders_unfinished_user ON orders(user_id) "
        "WHERE status IN ('RESERVED', 'CHARGING')")));
    QVERIFY(mutation.exec(QStringLiteral("DROP INDEX idx_orders_status_created_at")));
    QVERIFY(mutation.exec(QStringLiteral(
        "CREATE INDEX idx_orders_status_created_at ON orders(status, created_at DESC)")));
    QVERIFY(mutation.exec(QStringLiteral("DROP INDEX idx_operation_logs_admin_created_at")));
    QVERIFY(mutation.exec(QStringLiteral(
        "CREATE INDEX idx_operation_logs_admin_created_at "
        "ON operation_logs(admin_id, created_at DESC)")));
    QVERIFY(mutation.exec(QStringLiteral(
        "CREATE INDEX idx_chargers_status_updated_at "
        "ON chargers(status, updated_at DESC, id DESC)")));
    QVERIFY(mutation.exec(QStringLiteral("PRAGMA user_version = 1")));
    legacy.close();

    const auto restoreResult = DatabaseMaintenance::restore(legacyPath, restoredPath);
    QVERIFY(!restoreResult.ok);
    QVERIFY(!QFileInfo::exists(restoredPath));
}

void DatabaseMaintenanceTest::rejectsFutureSchemaVersionWithoutChangingIt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("future.sqlite"));
    QString errorMessage;

    DatabaseConnection current;
    QVERIFY2(current.open(databasePath, true, &errorMessage), qPrintable(errorMessage));
    QSqlQuery mutation(current.database());
    QVERIFY(mutation.exec(QStringLiteral("PRAGMA user_version = 5")));
    current.close();

    DatabaseConnection oldApplication;
    QVERIFY(!oldApplication.open(databasePath, false, &errorMessage));

    const QString connectionName = QStringLiteral("future-version-check");
    {
        QSqlDatabase verification = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                               connectionName);
        verification.setDatabaseName(databasePath);
        QVERIFY(verification.open());
        QSqlQuery versionQuery(verification);
        QVERIFY(versionQuery.exec(QStringLiteral("PRAGMA user_version")));
        QVERIFY(versionQuery.next());
        QCOMPARE(versionQuery.value(0).toInt(), 5);
        verification.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

QTEST_GUILESS_MAIN(DatabaseMaintenanceTest)

#include "tst_database_maintenance.moc"
