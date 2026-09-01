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

    QFile seedFile(QStringLiteral(":/database/seed.sql"));
    QVERIFY2(seedFile.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(seedFile.errorString()));
    QVERIFY(!seedFile.readAll().trimmed().isEmpty());
}

QTEST_GUILESS_MAIN(DatabaseResourcesTest)

#include "tst_database_resources.moc"
