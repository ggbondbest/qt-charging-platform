#include "admin_request_gateway.h"
#include "dashboard_page.h"
#include "database_connection.h"
#include "delivery_dashboard_widgets.h"
#include "management_time_format.h"
#include "server_runtime.h"
#include "station_management_page.h"
#include "user_management_page.h"

#include <QChart>
#include <QDir>
#include <QLabel>
#include <QLineSeries>
#include <QPieSeries>
#include <QPieSlice>
#include <QPushButton>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QValueAxis>
#include <QtTest>

using namespace charging::server;

namespace {
QLineSeries* lineSeries(DeliveryRevenueTrendWidget& widget)
{
    return qobject_cast<QLineSeries*>(widget.chart()->series().value(0));
}
QPushButton* buttonWithText(QWidget& page, const QString& text)
{
    for (auto* button : page.findChildren<QPushButton*>())
        if (button->text() == text)
            return button;
    return nullptr;
}
void optionalSnapshot(QWidget& page, const QString& name)
{
    const QString directory = qEnvironmentVariable("DELIVERY_UI_SNAPSHOT_DIR");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    page.resize(1500, 960);
    page.show();
    QCoreApplication::processEvents();
    page.grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
}
} // namespace

class DeliveryAdminPagesTest final : public QObject
{
    Q_OBJECT
private slots:
    void registrationTimeIsExplicitlyBeijing();
    void chartsUseServiceValuesAndSwitchPeriods();
    void chartsClearInvalidAndMissingData();
    void fiveDeviceStatesNeverOverlap();
    void pagesReadRealGatewayAndDatabase();
};

void DeliveryAdminPagesTest::registrationTimeIsExplicitlyBeijing()
{
    QCOMPARE(managementRegistrationTime({{"createdAtUtc", "2026-09-01T23:30:00.000Z"}}),
             QString("2026-09-02 07:30:00"));
    QCOMPARE(managementRegistrationTime({{"createdAt", "2026-09-01T16:00:00.000Z"}}),
             QString("2026-09-02 00:00:00"));
    QCOMPARE(managementRegistrationTime({{"updatedAt", "2026-09-08T00:00:00.000Z"}}),
             QString("—"));
    QCOMPARE(managementBeijingTime("invalid"), QString("—"));
}

void DeliveryAdminPagesTest::chartsUseServiceValuesAndSwitchPeriods()
{
    DeliveryRevenueTrendWidget chart;
    QVERIFY(qobject_cast<QChartView*>(&chart));
    QStringList dates;
    QVector<qint64> revenue;
    QVector<int> orders;
    for (int i = 0; i < 30; ++i) {
        dates.append(QDate(2026, 8, 10).addDays(i).toString(Qt::ISODate));
        revenue.append(12345 + 100 * i);
        orders.append(i);
    }
    chart.setServiceSeries(dates, revenue, orders);
    QVERIFY(lineSeries(chart));
    QCOMPARE(lineSeries(chart)->count(), 7);
    QCOMPARE(lineSeries(chart)->at(0).y(), 146.45);
    QCOMPARE(lineSeries(chart)->at(6).y(), 152.45);
    chart.setPeriod(2);
    QCOMPARE(lineSeries(chart)->count(), 30);
    QCOMPARE(lineSeries(chart)->at(0).y(), 123.45);
    chart.setDisplayMode(1);
    QCOMPARE(lineSeries(chart)->at(29).y(), 29.0);
    auto* axis = qobject_cast<QValueAxis*>(chart.chart()->axes(Qt::Vertical).first());
    QVERIFY(axis);
    QCOMPARE(axis->titleText(), QString("完成订单（笔）"));
    chart.setCustomDateRange(QDate(2026, 8, 12), QDate(2026, 8, 16));
    QCOMPARE(lineSeries(chart)->count(), 5);
    QCOMPARE(lineSeries(chart)->at(0).y(), 2.0);
    chart.setDisplayMode(0);
    QCOMPARE(lineSeries(chart)->at(0).y(), 125.45);
    optionalSnapshot(chart, "revenue-chart");
}

void DeliveryAdminPagesTest::chartsClearInvalidAndMissingData()
{
    DeliveryRevenueTrendWidget chart;
    chart.setServiceSeries({"2026-09-07"}, {0}, {0});
    QCOMPARE(lineSeries(chart)->count(), 1);
    QCOMPARE(lineSeries(chart)->at(0).y(), 0.0);
    chart.setServiceSeries({"2026-09-07"}, {}, {});
    QCOMPARE(lineSeries(chart)->count(), 0);
    chart.setServiceSeries({"2026-09-07"}, {-1}, {0});
    QCOMPARE(lineSeries(chart)->count(), 0);
    chart.setServiceSeries({"invalid-date"}, {100}, {1});
    QCOMPARE(lineSeries(chart)->count(), 0);
    chart.setServiceSeries({"2026-09-08", "2026-09-07"}, {100, 200}, {1, 2});
    QCOMPARE(lineSeries(chart)->count(), 0);
    chart.setServiceSeries({}, {}, {});
    QCOMPARE(lineSeries(chart)->count(), 0);
    QVERIFY(!chart.chart()->title().isEmpty());
}

void DeliveryAdminPagesTest::fiveDeviceStatesNeverOverlap()
{
    DeliveryDeviceStatusWidget widget;
    widget.setCounts(5, 2, 1, 1, 1);
    auto* series = qobject_cast<QPieSeries*>(widget.chart()->series().first());
    QVERIFY(series);
    QCOMPARE(series->count(), 5);
    QCOMPARE(series->sum(), 10.0);
    const QStringList expected{"空闲", "在用", "故障", "预约", "离线"};
    double percentage = 0;
    for (int i = 0; i < 5; ++i) {
        QCOMPARE(series->slices().at(i)->label(), expected.at(i));
        percentage += series->slices().at(i)->percentage();
    }
    QVERIFY(qAbs(percentage - 1.0) < 0.000001);
    widget.setCounts(0, 0, 0, 0, 0);
    series = qobject_cast<QPieSeries*>(widget.chart()->series().first());
    QCOMPARE(series->count(), 0);
    QCOMPARE(series->sum(), 0.0);
}

void DeliveryAdminPagesTest::pagesReadRealGatewayAndDatabase()
{
    QTemporaryDir directory;
    const auto path = directory.filePath("admin-pages.sqlite");
    {
        DatabaseConnection prepare;
        QString diagnostic;
        QVERIFY2(prepare.open(path, true, &diagnostic), qPrintable(diagnostic));
        QSqlQuery query(prepare.database());
        QVERIFY(query.exec("UPDATE users SET created_at='2026-09-01T23:30:00.000Z', "
                           "updated_at='2026-09-05T02:00:00.000Z' WHERE id=1"));
    }
    ServerRuntime runtime;
    QSignalSpy ready(&runtime, &ServerRuntime::listening);
    QVERIFY(runtime.start(path, false, QHostAddress::LocalHost, 0));
    QTRY_COMPARE(ready.size(), 1);
    AdminRequestGateway gateway(&runtime);
    gateway.request("auth.login", {{"username", "admin"}, {"password", "123456"}}, this);
    QTRY_VERIFY(gateway.isAuthenticated());

    UserManagementPage users;
    users.setAdminGateway(&gateway);
    auto* userTable = users.findChild<QTableWidget*>("managementUsersTable");
    auto* userDetail = users.findChild<QLabel*>("managementUserDetails");
    QVERIFY(userTable && userDetail);
    QTRY_COMPARE(userTable->rowCount(), 1);
    QCOMPARE(userTable->item(0, 4)->text(), QString("2026-09-02 07:30:00"));
    QVERIFY(userTable->horizontalHeaderItem(4)->text().contains("北京时间"));
    QTRY_VERIFY(userDetail->text().contains("2026-09-02 07:30:00"));
    QTRY_VERIFY(userDetail->text().contains("2026-09-05 10:00:00"));
    optionalSnapshot(users, "users-page");

    StationManagementPage stations;
    stations.setAdminGateway(&gateway);
    auto* stationTable = stations.findChild<QTableWidget*>("managementStationsTable");
    auto* stationDetail = stations.findChild<QLabel*>("managementStationConfiguration");
    QVERIFY(stationTable && stationDetail);
    QTRY_COMPARE(stationTable->rowCount(), 3);
    QCOMPARE(stationTable->item(0, 7)->text(), QString("50.0% / 1 台"));
    QCOMPARE(stationTable->item(2, 7)->text(), QString("100.0% / 3 台"));
    QTRY_VERIFY(stationDetail->text().contains("在线率　50.0%"));
    QTRY_VERIFY(stationDetail->text().contains("在线电桩　1 台"));
    optionalSnapshot(stations, "stations-page");

    DashboardPage dashboard;
    dashboard.setAdminGateway(&gateway);
    auto* chart = dashboard.findChild<DeliveryRevenueTrendWidget*>();
    QVERIFY(chart);
    QTRY_COMPARE(lineSeries(*chart)->count(), 7);
    const QStringList expected{"5（71.4%）", "0（0.0%）", "1（14.3%）", "0（0.0%）", "1（14.3%）"};
    for (int i = 0; i < 5; ++i) {
        auto* value = dashboard.findChild<QLabel*>(QString("deviceStateCount%1").arg(i));
        QVERIFY(value);
        QCOMPARE(value->text(), expected.at(i));
    }
    auto* month = buttonWithText(dashboard, "近30天");
    auto* week = buttonWithText(dashboard, "近7天");
    QVERIFY(month && week);
    month->click();
    QTRY_COMPARE(lineSeries(*chart)->count(), 30);
    week->click();
    QTRY_COMPARE(lineSeries(*chart)->count(), 7);
    optionalSnapshot(dashboard, "dashboard-page");
    runtime.stop();
}

QTEST_MAIN(DeliveryAdminPagesTest)
#include "tst_delivery_admin_pages.moc"
