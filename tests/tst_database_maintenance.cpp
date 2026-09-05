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

QTEST_GUILESS_MAIN(DatabaseMaintenanceTest)

#include "tst_database_maintenance.moc"
