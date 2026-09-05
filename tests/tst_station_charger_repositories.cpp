#include "charger_repository.h"
#include "database_connection.h"
#include "station_repository.h"

#include <QtTest>

class StationChargerRepositoriesTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void stationListIncludesDerivedCounts();
    void stationListSupportsFiltersAndPagination();
    void chargerListSupportsFiltersAndPagination();
    void invalidQueriesFail();

private:
    charging::server::DatabaseConnection databaseConnection_;
};

void StationChargerRepositoriesTest::initTestCase()
{
    QString errorMessage;
    QVERIFY2(databaseConnection_.open(QStringLiteral(":memory:"), true, &errorMessage),
             qPrintable(errorMessage));
}

void StationChargerRepositoriesTest::cleanupTestCase()
{
    databaseConnection_.close();
}

void StationChargerRepositoriesTest::stationListIncludesDerivedCounts()
{
    charging::server::StationRepository repository(databaseConnection_.database());
    const auto result = repository.list({});
    QVERIFY2(result.ok, qPrintable(result.errorMessage));
    QCOMPARE(result.totalCount, 3);
    QCOMPARE(result.stations.size(), 3);
    QCOMPARE(result.stations.at(0).totalChargers, 3);
    QCOMPARE(result.stations.at(0).availableChargers, 2);
    QCOMPARE(result.stations.at(1).totalChargers, 2);
    QCOMPARE(result.stations.at(1).availableChargers, 2);
}

void StationChargerRepositoriesTest::stationListSupportsFiltersAndPagination()
{
    charging::server::StationRepository repository(databaseConnection_.database());
    charging::server::StationQuery keywordQuery;
    keywordQuery.keyword = QStringLiteral("海创");
    const auto keywordResult = repository.list(keywordQuery);
    QVERIFY2(keywordResult.ok, qPrintable(keywordResult.errorMessage));
    QCOMPARE(keywordResult.totalCount, 1);
    QCOMPARE(keywordResult.stations.first().id, qint64(2));

    charging::server::StationQuery statusQuery;
    statusQuery.status = charging::model::StationStatus::Inactive;
    const auto statusResult = repository.list(statusQuery);
    QVERIFY2(statusResult.ok, qPrintable(statusResult.errorMessage));
    QCOMPARE(statusResult.totalCount, 0);

    charging::server::StationQuery pageQuery;
    pageQuery.limit = 1;
    pageQuery.offset = 1;
    const auto pageResult = repository.list(pageQuery);
    QVERIFY2(pageResult.ok, qPrintable(pageResult.errorMessage));
    QCOMPARE(pageResult.totalCount, 3);
    QCOMPARE(pageResult.stations.size(), 1);
    QCOMPARE(pageResult.stations.first().id, qint64(2));
}

void StationChargerRepositoriesTest::chargerListSupportsFiltersAndPagination()
{
    charging::server::ChargerRepository repository(databaseConnection_.database());
    charging::server::ChargerQuery query;
    query.stationId = 1;
    const auto allResult = repository.listByStation(query);
    QVERIFY2(allResult.ok, qPrintable(allResult.errorMessage));
    QCOMPARE(allResult.totalCount, 3);
    QCOMPARE(allResult.chargers.size(), 3);

    query.status = charging::model::ChargerStatus::Available;
    query.type = charging::model::ChargerType::Fast;
    query.limit = 1;
    const auto filteredResult = repository.listByStation(query);
    QVERIFY2(filteredResult.ok, qPrintable(filteredResult.errorMessage));
    QCOMPARE(filteredResult.totalCount, 2);
    QCOMPARE(filteredResult.chargers.size(), 1);
    QVERIFY(filteredResult.chargers.first().status
            == charging::model::ChargerStatus::Available);
    QVERIFY(filteredResult.chargers.first().type == charging::model::ChargerType::Fast);

    query.offset = 2;
    const auto emptyPage = repository.listByStation(query);
    QVERIFY2(emptyPage.ok, qPrintable(emptyPage.errorMessage));
    QCOMPARE(emptyPage.totalCount, 2);
    QVERIFY(emptyPage.chargers.isEmpty());
}

void StationChargerRepositoriesTest::invalidQueriesFail()
{
    charging::server::StationRepository stationRepository(databaseConnection_.database());
    charging::server::StationQuery stationQuery;
    stationQuery.limit = 0;
    QVERIFY(!stationRepository.list(stationQuery).ok);

    charging::server::ChargerRepository chargerRepository(databaseConnection_.database());
    charging::server::ChargerQuery chargerQuery;
    QVERIFY(!chargerRepository.listByStation(chargerQuery).ok);
    chargerQuery.stationId = 1;
    chargerQuery.limit = 101;
    QVERIFY(!chargerRepository.listByStation(chargerQuery).ok);
}

QTEST_APPLESS_MAIN(StationChargerRepositoriesTest)

#include "tst_station_charger_repositories.moc"
