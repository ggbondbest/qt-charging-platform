#include "database_connection.h"
#include "database_maintenance.h"

#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using charging::server::DatabaseConnection;
using charging::server::DatabaseMaintenance;

class DatabaseMaintenanceTest final : public QObject
{
    Q_OBJECT

private slots:
    void backupValidateAndRestore();
    void rejectsInvalidBackupAndOpenDestination();
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

QTEST_GUILESS_MAIN(DatabaseMaintenanceTest)

#include "tst_database_maintenance.moc"
