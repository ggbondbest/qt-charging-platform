#include <QByteArray>
#include <QFile>
#include <QtTest>

class DatabaseResourcesTest final : public QObject
{
    Q_OBJECT

private slots:
    void schemaAndSeedAreEmbedded();
};

void DatabaseResourcesTest::schemaAndSeedAreEmbedded()
{
    QFile schemaFile(QStringLiteral(":/database/schema.sql"));
    QVERIFY2(schemaFile.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(schemaFile.errorString()));
    const QByteArray schema = schemaFile.readAll();
    QVERIFY(!schema.trimmed().isEmpty());
    QVERIFY(schema.contains("CREATE TABLE"));
    for (const auto* index : {"idx_chargers_abnormal_updated_at", "idx_chargers_updated_at",
                              "idx_users_status_id", "idx_orders_status_created_at",
                              "idx_orders_created_at",
                              "idx_recharge_records_status_created_at",
                              "idx_recharge_records_created_at",
                              "idx_operation_logs_action_created_at",
                              "idx_operation_logs_admin_created_at",
                              "idx_operation_logs_created_at"})
        QVERIFY2(schema.contains(index), index);

    QFile seedFile(QStringLiteral(":/database/seed.sql"));
    QVERIFY2(seedFile.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(seedFile.errorString()));
    QVERIFY(!seedFile.readAll().trimmed().isEmpty());
}

QTEST_GUILESS_MAIN(DatabaseResourcesTest)

#include "tst_database_resources.moc"
