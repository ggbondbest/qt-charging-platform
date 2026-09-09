#include "database_connection.h"
#include "database_maintenance.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace charging::server;
namespace {
QString scalar(const QSqlDatabase& db, const QString& sql)
{
    QSqlQuery q(db);
    return q.exec(sql) && q.next() ? q.value(0).toString() : QStringLiteral("QUERY_FAILED");
}
bool makeV4(const QString& path, QString* error)
{
    DatabaseConnection db;
    if (!db.open(path, true, error)) return false;
    QSqlQuery q(db.database());
    // Reconstruct the actual pre-workflow shape: 15 tables, no stop_reason,
    // and the old three-value notification CHECK. No v5 shadow tables remain.
    const QStringList statements{
        "DROP TABLE queue_entries", "DROP TABLE repair_timeline", "DROP TABLE repair_reports",
        "DROP TABLE repair_operations", "DROP TABLE order_charge_targets",
        "ALTER TABLE orders DROP COLUMN stop_reason", "DROP TABLE notifications",
        "CREATE TABLE notifications (id INTEGER PRIMARY KEY AUTOINCREMENT,user_id INTEGER NOT NULL,"
        "type TEXT NOT NULL CHECK (type IN ('CHARGING_STOPPED','ORDER_PAID','RESERVATION_EXPIRY_REMINDER')),"
        "title TEXT NOT NULL CHECK (length(trim(title)) BETWEEN 1 AND 64),"
        "body TEXT NOT NULL CHECK (length(trim(body)) BETWEEN 1 AND 512),"
        "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),read_at TEXT,"
        "FOREIGN KEY(user_id) REFERENCES users(id) ON UPDATE CASCADE ON DELETE RESTRICT)",
        "CREATE INDEX idx_notifications_user_created_at ON notifications(user_id,created_at DESC)",
        "CREATE INDEX fixture_notification_title ON notifications(title)",
        "INSERT INTO notifications(id,user_id,type,title,body,created_at,read_at) VALUES"
        "(42,1,'ORDER_PAID','旧通知标题','旧通知正文','2026-09-01T01:00:00.000Z','2026-09-01T01:05:00.000Z')",
        "INSERT INTO notifications(id,user_id,type,title,body) VALUES(99,1,'CHARGING_STOPPED','deleted','deleted')",
        "DELETE FROM notifications WHERE id=99",
        "CREATE TRIGGER fixture_notification_trigger AFTER INSERT ON notifications "
        "WHEN NEW.id > 99 BEGIN UPDATE users SET nickname='notification-trigger-fired' WHERE id=1; END",
        "INSERT INTO orders(id,order_no,user_id,charger_id,status,unit_price_cents_per_kwh,"
        "energy_wh,duration_seconds,amount_cents,created_at,started_at,stopped_at,paid_at,updated_at) "
        "VALUES(41,'LEGACY-PAID',1,1,'COMPLETED',120,1000,30,120,"
        "'2026-09-01T00:00:00.000Z','2026-09-01T00:01:00.000Z','2026-09-01T00:01:30.000Z',"
        "'2026-09-01T00:02:00.000Z','2026-09-01T00:02:00.000Z')",
        "INSERT INTO order_pricing_snapshots(order_id,version,unit_price_cents_per_kwh,captured_at) "
        "VALUES(41,'energy-only-v1',120,'2026-09-01T00:00:00.000Z')",
        "PRAGMA user_version=4"
    };
    for (const auto& statement : statements) {
        if (!q.exec(statement)) { *error = q.lastError().text(); return false; }
    }
    q.finish();
    db.close();
    return true;
}
QByteArray digest(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256) : QByteArray();
}
}

class WorkflowMigrationTest final : public QObject
{
    Q_OBJECT
private slots:
    void v4MigrationPreservesNotificationsReadStateSequenceAndDependencies()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("legacy-v4.sqlite");
        QString error; QVERIFY2(makeV4(path, &error), qPrintable(error));
        DatabaseConnection migrated;
        QVERIFY2(migrated.open(path, false, &error), qPrintable(error));
        auto db = migrated.database();
        QCOMPARE(scalar(db, "PRAGMA user_version"), QString("5"));
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"), QString("20"));
        QSqlQuery q(db);
        QVERIFY(q.exec("SELECT user_id,type,title,body,created_at,read_at FROM notifications WHERE id=42"));
        QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 1);
        QCOMPARE(q.value(1).toString(), QString("ORDER_PAID"));
        QCOMPARE(q.value(2).toString(), QString("旧通知标题"));
        QCOMPARE(q.value(3).toString(), QString("旧通知正文"));
        QCOMPARE(q.value(4).toString(), QString("2026-09-01T01:00:00.000Z"));
        QCOMPARE(q.value(5).toString(), QString("2026-09-01T01:05:00.000Z"));
        q.finish();
        QCOMPARE(scalar(db, "SELECT seq FROM sqlite_sequence WHERE name='notifications'"), QString("99"));
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE name IN ('fixture_notification_trigger','fixture_notification_title')"), QString("2"));
        QCOMPARE(scalar(db, "SELECT nickname FROM users WHERE id=1"), QString("用户8000"));
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM orders WHERE id=41 AND status='COMPLETED' AND amount_cents=120 AND stop_reason IS NULL"), QString("1"));
        QCOMPARE(scalar(db, "SELECT COUNT(*) FROM order_charge_targets"), QString("0"));
        QCOMPARE(scalar(db, "SELECT unit_price_cents_per_kwh FROM order_pricing_snapshots WHERE order_id=41"), QString("120"));
        QVERIFY(q.exec("INSERT INTO notifications(user_id,type,title,body) VALUES(1,'REPAIR_UPDATED','维修更新','模拟维修完成')"));
        QCOMPARE(q.lastInsertId().toInt(), 100);
        QCOMPARE(scalar(db, "SELECT nickname FROM users WHERE id=1"), QString("notification-trigger-fired"));
        QVERIFY(q.exec("INSERT INTO notifications(user_id,type,title,body) VALUES(1,'QUEUE_CALLED','排队叫号','请确认')"));
        QVERIFY(q.exec("INSERT INTO notifications(user_id,type,title,body) VALUES(1,'QUEUE_EXPIRED','叫号超时','机会已释放')"));
        QVERIFY(!q.exec("INSERT INTO notifications(user_id,type,title,body) VALUES(1,'UNSUPPORTED','bad','bad')"));
        q.finish(); db = {}; migrated.close();
        QVERIFY2(migrated.open(path, false, &error), qPrintable(error));
        QCOMPARE(scalar(migrated.database(), "SELECT COUNT(*) FROM notifications"), QString("4"));
        migrated.close();
        const auto checked = DatabaseMaintenance::validate(path);
        QVERIFY2(checked.ok, qPrintable(checked.errorMessage));
    }

    void notificationMigrationFailureRollsBackEverySchemaChange()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("rollback-v4.sqlite");
        QString error; QVERIFY2(makeV4(path, &error), qPrintable(error));
        const auto name = QStringLiteral("workflow-inject-failure");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE VIEW notifications_v5_upgrade AS SELECT 1 AS id"));
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
        DatabaseConnection rejected;
        QVERIFY(!rejected.open(path, false, &error));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(path); QVERIFY(db.open());
            QCOMPARE(scalar(db, "PRAGMA user_version"), QString("4"));
            QCOMPARE(scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"), QString("15"));
            QCOMPARE(scalar(db, "SELECT COUNT(*) FROM pragma_table_info('orders') WHERE name='stop_reason'"), QString("0"));
            QCOMPARE(scalar(db, "SELECT read_at FROM notifications WHERE id=42"), QString("2026-09-01T01:05:00.000Z"));
            QCOMPARE(scalar(db, "SELECT seq FROM sqlite_sequence WHERE name='notifications'"), QString("99"));
            QSqlQuery q(db);
            QVERIFY(!q.exec("INSERT INTO notifications(user_id,type,title,body) VALUES(1,'REPAIR_UPDATED','bad','bad')"));
            QCOMPARE(scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE name='fixture_notification_trigger'"), QString("1"));
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }

    void restoringV4BackupUpgradesOnlyTemporaryCopy()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto source = dir.filePath("backup-v4.sqlite");
        const auto destination = dir.filePath("restored.sqlite");
        QString error; QVERIFY2(makeV4(source, &error), qPrintable(error));
        const auto originalDigest = digest(source); QVERIFY(!originalDigest.isEmpty());
        const auto restored = DatabaseMaintenance::restore(source, destination);
        QVERIFY2(restored.ok, qPrintable(restored.errorMessage));
        QCOMPARE(digest(source), originalDigest);
        const auto checked = DatabaseMaintenance::validate(destination);
        QVERIFY2(checked.ok, qPrintable(checked.errorMessage));
        DatabaseConnection db;
        QVERIFY2(db.open(destination, false, &error), qPrintable(error));
        QCOMPARE(scalar(db.database(), "SELECT COUNT(*) FROM notifications WHERE id=42"), QString("1"));
        QCOMPARE(scalar(db.database(), "SELECT COUNT(*) FROM orders WHERE id=41 AND amount_cents=120"), QString("1"));
    }

    void strictValidationRejectsMissingWorkflowUniqueness()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("invalid.sqlite");
        DatabaseConnection db; QString error;
        QVERIFY2(db.open(path, true, &error), qPrintable(error));
        QSqlQuery q(db.database());
        QVERIFY(q.exec("DROP INDEX ux_queue_called_charger"));
        QVERIFY(q.exec("CREATE INDEX ux_queue_called_charger ON queue_entries(charger_id) WHERE status='CALLED'"));
        q.finish(); db.close();
        const auto checked = DatabaseMaintenance::validate(path);
        QVERIFY(!checked.ok);
        QVERIFY(checked.errorMessage.contains("ux_queue_called_charger"));
        const auto restored = DatabaseMaintenance::restore(path, dir.filePath("must-not-create.sqlite"));
        QVERIFY(!restored.ok);
        QVERIFY(!QFile::exists(dir.filePath("must-not-create.sqlite")));
    }
};

QTEST_GUILESS_MAIN(WorkflowMigrationTest)
#include "tst_workflow_migration.moc"
