#include "admin_request_gateway.h"
#include "charger_management_page.h"
#include "dashboard_page.h"
#include "delivery_dashboard_widgets.h"
#include "database_connection.h"
#include "server_runtime.h"
#include "station_management_page.h"

#include <QApplication>
#include <QChart>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPieSeries>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>

#include <algorithm>

using namespace charging::server;

class AdminManagementPagesTest final : public QObject
{
    Q_OBJECT

private slots:
    void stationCreatePersistsParsedCoordinatesAndPrice()
    {
        QTemporaryDir directory;
        const QString databasePath = directory.filePath(QStringLiteral("admin-pages.sqlite"));
        ServerRuntime runtime;
        QSignalSpy listening(&runtime, &ServerRuntime::listening);
        QVERIFY(runtime.start(databasePath, true, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(listening.size(), 1);

        AdminRequestGateway gateway(&runtime);
        gateway.request(QStringLiteral("auth.login"),
                        {{QStringLiteral("username"), QStringLiteral("admin")},
                         {QStringLiteral("password"), QStringLiteral("123456")}},
                        this, QStringLiteral("admin-pages-login"));
        QTRY_VERIFY(gateway.isAuthenticated());

        StationManagementPage page;
        page.setAdminGateway(&gateway);
        page.show();
        const auto pageButtons = page.findChildren<QPushButton*>();
        const auto addButton = std::find_if(pageButtons.cbegin(), pageButtons.cend(),
                                            [](QPushButton* button) {
                                                return button->text() == QObject::tr("新增电站");
                                            });
        QVERIFY(addButton != pageButtons.cend());

        bool dialogSubmitted = false;
        QTimer::singleShot(0, &page, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog == nullptr) return;
            const auto set = [dialog](const char* objectName, const QString& value) {
                auto* edit = dialog->findChild<QLineEdit*>(QString::fromLatin1(objectName));
                if (edit == nullptr) return false;
                edit->setText(value);
                return true;
            };
            const bool hasFields = set("stationCodeLineEdit", QStringLiteral("STA-UI-RELEASE-001"))
                && set("stationNameLineEdit", QStringLiteral("管理端参数回归站"))
                && set("stationAddressLineEdit", QStringLiteral("测试路 1 号"))
                && set("stationDistrictLineEdit", QStringLiteral("高新区"))
                && set("stationContactNameLineEdit", QStringLiteral("负责人"))
                && set("stationContactPhoneLineEdit", QStringLiteral("13800138000"))
                && set("stationLatitudeLineEdit", QStringLiteral("30.274100"))
                && set("stationLongitudeLineEdit", QStringLiteral("120.155100"))
                && set("stationPriceLineEdit", QStringLiteral("1.28"));
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            if (!hasFields || buttons == nullptr || buttons->button(QDialogButtonBox::Ok) == nullptr) {
                dialog->reject();
                return;
            }
            dialogSubmitted = true;
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        (*addButton)->click();
        QVERIFY(dialogSubmitted);

        const QString connectionName = QStringLiteral("admin-management-pages-query");
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            database.setDatabaseName(databasePath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            query.prepare(QStringLiteral("SELECT latitude, longitude, price_cents_per_kwh FROM stations WHERE code=?"));
            query.addBindValue(QStringLiteral("STA-UI-RELEASE-001"));
            QTRY_VERIFY(query.exec() && query.next());
            QCOMPARE(query.value(0).toDouble(), 30.274100);
            QCOMPARE(query.value(1).toDouble(), 120.155100);
            QCOMPARE(query.value(2).toLongLong(), qint64(128));
            database.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
        runtime.stop();
    }

    void stationEditUsesSnapshotAcrossDelayedRefreshAndReorder()
    {
        QTemporaryDir directory;
        const QString databasePath = directory.filePath(QStringLiteral("admin-refresh.sqlite"));
        ServerRuntime runtime;
        QSignalSpy listening(&runtime, &ServerRuntime::listening);
        QVERIFY(runtime.start(databasePath, true, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(listening.size(), 1);

        AdminRequestGateway gateway(&runtime);
        gateway.request(QStringLiteral("auth.login"),
                        {{QStringLiteral("username"), QStringLiteral("admin")},
                         {QStringLiteral("password"), QStringLiteral("123456")}},
                        this, QStringLiteral("admin-refresh-login"));
        QTRY_VERIFY(gateway.isAuthenticated());

        StationManagementPage page;
        page.setAdminGateway(&gateway);
        page.show();
        auto* table = page.findChild<QTableWidget*>(QStringLiteral("stationManagementTable"));
        QVERIFY(table != nullptr);
        QTRY_VERIFY(table->rowCount() > 0 && table->item(0, 0) != nullptr);
        const QString originalCode = table->item(0, 0)->text();
        table->cellClicked(0, 0);
        const auto pageButtons = page.findChildren<QPushButton*>();
        const auto editButton = std::find_if(pageButtons.cbegin(), pageButtons.cend(),
                                             [](QPushButton* button) {
                                                 return button->text() == QObject::tr("编辑电站");
                                             });
        QVERIFY(editButton != pageButtons.cend());
        // Editing needs the authorized, unmasked stations.get snapshot, which
        // arrives asynchronously after the list selection.
        QTRY_VERIFY((*editButton)->isEnabled());

        const QString injectedCode = QStringLiteral("STA-REFRESH-NEW-001");
        const QString editedName = QStringLiteral("延迟刷新后仍应编辑原电站");
        bool injectedCreated = false;
        QString injectedRequestId;
        const auto responseConnection = connect(
            &gateway, &AdminRequestGateway::finished, &page,
            [&page, &injectedCreated, &injectedRequestId](const QString& id,
                                                                      const QJsonObject& response) {
                if (id != injectedRequestId) return;
                injectedCreated = response.value(QStringLiteral("success")).toBool();
                if (injectedCreated) page.refreshData();
            });
        QTimer::singleShot(0, &page, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog == nullptr) return;
            auto* nameEdit = dialog->findChild<QLineEdit*>(QStringLiteral("stationNameLineEdit"));
            if (nameEdit != nullptr) nameEdit->setText(editedName);
            if (auto* edit = dialog->findChild<QLineEdit*>(QStringLiteral("stationDistrictLineEdit"))) edit->setText(QStringLiteral("高新区"));
            if (auto* edit = dialog->findChild<QLineEdit*>(QStringLiteral("stationContactNameLineEdit"))) edit->setText(QStringLiteral("负责人"));
            if (auto* edit = dialog->findChild<QLineEdit*>(QStringLiteral("stationContactPhoneLineEdit"))) edit->setText(QStringLiteral("13800138000"));
            if (auto* city = dialog->findChild<QComboBox*>(QStringLiteral("stationCityComboBox"))) city->setCurrentText(QStringLiteral("大连市"));
        });
        // MainWindow's ten-second timer calls this same refreshData() entry
        // point.  Create a newer station after that boundary so id-desc sorting
        // moves the edited station to a different list index while the dialog
        // remains open.
        QTimer::singleShot(11000, &page, [&] {
            const QJsonArray chargers{QJsonObject{{QStringLiteral("code"), injectedCode + QStringLiteral("-C01")},
                                                   {QStringLiteral("type"), QStringLiteral("FAST")},
                                                   {QStringLiteral("powerWatts"), 60000}}};
            injectedRequestId = gateway.request(
                QStringLiteral("station.create"),
                {{QStringLiteral("operationId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                 {QStringLiteral("code"), injectedCode}, {QStringLiteral("name"), QStringLiteral("刷新重排站")},
                 {QStringLiteral("address"), QStringLiteral("测试路 2 号")},
                 {QStringLiteral("city"), QStringLiteral("大连市")},
                 {QStringLiteral("district"), QStringLiteral("高新区")},
                 {QStringLiteral("contactName"), QStringLiteral("刷新回归负责人")},
                 {QStringLiteral("contactPhone"), QStringLiteral("13800138000")},
                 {QStringLiteral("latitude"), 30.0}, {QStringLiteral("longitude"), 120.0},
                 {QStringLiteral("priceCentsPerKwh"), 100}, {QStringLiteral("chargers"), chargers}},
                this, QStringLiteral("admin-refresh-reorder"));
        });
        QTimer::singleShot(14000, &page, [] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog == nullptr) return;
            if (auto* buttons = dialog->findChild<QDialogButtonBox*>()) {
                if (auto* accept = buttons->button(QDialogButtonBox::Ok)) accept->click();
            }
        });
        (*editButton)->click();
        disconnect(responseConnection);
        QVERIFY(injectedCreated);

        const QString connectionName = QStringLiteral("admin-refresh-query");
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            database.setDatabaseName(databasePath);
            QVERIFY(database.open());
            const auto nameForCode = [&database](const QString& code) {
                QSqlQuery query(database);
                query.prepare(QStringLiteral("SELECT name FROM stations WHERE code=?"));
                query.addBindValue(code);
                return query.exec() && query.next() ? query.value(0).toString() : QString();
            };
            QTRY_COMPARE(nameForCode(originalCode), editedName);
            QCOMPARE(nameForCode(injectedCode), QStringLiteral("刷新重排站"));
            database.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
        runtime.stop();
    }

    void stationJumpThenManualStationSelectionUsesComboValue()
    {
        QTemporaryDir directory;
        ServerRuntime runtime;
        QSignalSpy listening(&runtime, &ServerRuntime::listening);
        QVERIFY(runtime.start(directory.filePath(QStringLiteral("admin-filter.sqlite")), true,
                              QHostAddress::LocalHost, 0));
        QTRY_COMPARE(listening.size(), 1);

        AdminRequestGateway gateway(&runtime);
        gateway.request(QStringLiteral("auth.login"),
                        {{QStringLiteral("username"), QStringLiteral("admin")},
                         {QStringLiteral("password"), QStringLiteral("123456")}},
                        this, QStringLiteral("admin-filter-login"));
        QTRY_VERIFY(gateway.isAuthenticated());

        ChargerManagementPage page;
        page.setAdminGateway(&gateway);
        page.show();
        auto* stationFilter = page.findChild<QComboBox*>(QStringLiteral("chargerStationFilterComboBox"));
        auto* table = page.findChild<QTableWidget*>(QStringLiteral("chargerManagementTable"));
        QVERIFY(stationFilter != nullptr && table != nullptr);
        QTRY_VERIFY(stationFilter->count() >= 3 && !stationFilter->itemData(1).toString().isEmpty());
        const int stationAIndex = 1;
        const int stationBIndex = 2;
        const QString stationAName = stationFilter->itemText(stationAIndex);
        const QString stationBName = stationFilter->itemText(stationBIndex);
        page.showStationRecords(stationFilter->itemData(stationAIndex).toString());
        QTRY_VERIFY(table->rowCount() > 0 && table->item(0, 1) != nullptr);
        QTRY_COMPARE(table->item(0, 1)->text(), stationAName);

        stationFilter->setCurrentIndex(stationBIndex);
        const auto pageButtons = page.findChildren<QPushButton*>();
        const auto queryButton = std::find_if(pageButtons.cbegin(), pageButtons.cend(),
                                              [](QPushButton* button) {
                                                  return button->text() == QObject::tr("查询");
                                              });
        QVERIFY(queryButton != pageButtons.cend());
        (*queryButton)->click();
        QTRY_VERIFY(table->rowCount() > 0 && table->item(0, 1) != nullptr);
        QTRY_COMPARE(table->item(0, 1)->text(), stationBName);
        runtime.stop();
    }

    void stationJumpBeyondFirstOptionsPageUsesTargetLookup()
    {
        QTemporaryDir directory;
        const QString databasePath = directory.filePath(QStringLiteral("admin-large-options.sqlite"));
        ServerRuntime runtime;
        QSignalSpy listening(&runtime, &ServerRuntime::listening);
        QVERIFY(runtime.start(databasePath, true, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(listening.size(), 1);

        const QString lastCode = QStringLiteral("STA-LARGE-101");
        const QString lastName = QStringLiteral("第 101 个回归电站");
        qint64 lastStationId = 0;
        {
            DatabaseConnection databaseConnection;
            QVERIFY(databaseConnection.open(databasePath, false));
            QSqlQuery insertStation(databaseConnection.database());
            insertStation.prepare(QStringLiteral(
                "INSERT INTO stations(code,name,address,latitude,longitude,price_cents_per_kwh,status) "
                "VALUES(?,?,?,?,?,?,'ACTIVE')"));
            for (int index = 4; index <= 101; ++index) {
                const QString code = index == 101
                    ? lastCode : QStringLiteral("STA-LARGE-%1").arg(index, 3, 10, QLatin1Char('0'));
                const QString name = index == 101
                    ? lastName : QStringLiteral("批量回归电站 %1").arg(index);
                insertStation.bindValue(0, code);
                insertStation.bindValue(1, name);
                insertStation.bindValue(2, QStringLiteral("测试路 %1 号").arg(index));
                insertStation.bindValue(3, 30.0);
                insertStation.bindValue(4, 120.0);
                insertStation.bindValue(5, 100);
                QVERIFY2(insertStation.exec(), qPrintable(insertStation.lastError().text()));
            }
            QSqlQuery stationIdQuery(databaseConnection.database());
            stationIdQuery.prepare(QStringLiteral("SELECT id FROM stations WHERE code=?"));
            stationIdQuery.addBindValue(lastCode);
            QVERIFY(stationIdQuery.exec() && stationIdQuery.next());
            lastStationId = stationIdQuery.value(0).toLongLong();
            QSqlQuery insertCharger(databaseConnection.database());
            insertCharger.prepare(QStringLiteral(
                "INSERT INTO chargers(station_id,code,type,power_watts,status) VALUES(?,?, 'FAST',60000,'AVAILABLE')"));
            insertCharger.addBindValue(lastStationId);
            insertCharger.addBindValue(QStringLiteral("CHG-LARGE-101-A1"));
            QVERIFY2(insertCharger.exec(), qPrintable(insertCharger.lastError().text()));
            databaseConnection.close();
        }

        AdminRequestGateway gateway(&runtime);
        gateway.request(QStringLiteral("auth.login"),
                        {{QStringLiteral("username"), QStringLiteral("admin")},
                         {QStringLiteral("password"), QStringLiteral("123456")}},
                        this, QStringLiteral("admin-large-options-login"));
        QTRY_VERIFY(gateway.isAuthenticated());

        ChargerManagementPage page;
        page.setAdminGateway(&gateway);
        page.show();
        auto* stationFilter = page.findChild<QComboBox*>(QStringLiteral("chargerStationFilterComboBox"));
        auto* table = page.findChild<QTableWidget*>(QStringLiteral("chargerManagementTable"));
        QVERIFY(stationFilter != nullptr && table != nullptr);
        QTRY_COMPARE(stationFilter->count(), 52); // All + 50 options + explicit next-page action.
        QVERIFY(stationFilter->findData(QStringLiteral("__load_more__")) >= 0);
        const QString lastStationIdText = QString::number(lastStationId);
        QCOMPARE(stationFilter->findData(lastStationIdText), -1);

        page.showStationRecords(lastStationIdText);
        QTRY_VERIFY(stationFilter->findData(lastStationIdText) >= 0);
        QTRY_VERIFY(table->rowCount() > 0 && table->item(0, 1) != nullptr);
        QTRY_COMPARE(table->item(0, 1)->text(), lastName);

        stationFilter->setCurrentIndex(1);
        const auto pageButtons = page.findChildren<QPushButton*>();
        const auto queryButton = std::find_if(pageButtons.cbegin(), pageButtons.cend(),
                                              [](QPushButton* button) {
                                                  return button->text() == QObject::tr("查询");
                                              });
        QVERIFY(queryButton != pageButtons.cend());
        const QString manuallySelectedName = stationFilter->currentText();
        (*queryButton)->click();
        QTRY_VERIFY(table->rowCount() > 0 && table->item(0, 1) != nullptr);
        QTRY_COMPARE(table->item(0, 1)->text(), manuallySelectedName);
        runtime.stop();
    }

    void dashboardUsesContractOnlineDefinition()
    {
        QTemporaryDir directory;
        ServerRuntime runtime;
        QSignalSpy listening(&runtime, &ServerRuntime::listening);
        QVERIFY(runtime.start(directory.filePath(QStringLiteral("admin-dashboard.sqlite")), true,
                              QHostAddress::LocalHost, 0));
        QTRY_COMPARE(listening.size(), 1);

        AdminRequestGateway gateway(&runtime);
        gateway.request(QStringLiteral("auth.login"),
                        {{QStringLiteral("username"), QStringLiteral("admin")},
                         {QStringLiteral("password"), QStringLiteral("123456")}},
                        this, QStringLiteral("admin-dashboard-login"));
        QTRY_VERIFY(gateway.isAuthenticated());

        DashboardPage page;
        page.setAdminGateway(&gateway);
        page.show();
        auto* value = page.findChild<QLabel*>(QStringLiteral("dashboardOnlineChargersValue"));
        auto* hint = page.findChild<QLabel*>(QStringLiteral("dashboardOnlineChargersHint"));
        QVERIFY(value != nullptr && hint != nullptr);
        // Runtime demo catalogue: 75 chargers, preserving 1 OFFLINE and 1
        // FAULT from the original seed. FAULT remains online under the
        // frozen contract, so the headline must be 74 / 98.7%.
        QTRY_COMPARE(value->text(), QStringLiteral("74 台"));
        QTRY_COMPARE(hint->text(), QStringLiteral("在线率 98.7%（北京时间快照）"));
        // Online includes FAULT, while the delivery chart splits all five
        // charger states without counting any charger twice.
        auto* distribution = page.findChild<DeliveryDeviceStatusWidget*>();
        auto* trend = page.findChild<DeliveryRevenueTrendWidget*>();
        QVERIFY(distribution && trend);
        auto* pie = qobject_cast<QPieSeries*>(distribution->chart()->series().first());
        QVERIFY(pie);
        QCOMPARE(pie->sum(), 75.0);
        const QStringList expected{"73（97.3%）", "0（0.0%）", "1（1.3%）", "0（0.0%）", "1（1.3%）"};
        for (int index = 0; index < expected.size(); ++index) {
            auto* legend = page.findChild<QLabel*>(QStringLiteral("deviceStateCount%1").arg(index));
            QVERIFY(legend);
            QCOMPARE(legend->text(), expected.at(index));
        }
        page.refreshCurrent();
        QCOMPARE(value->text(), QStringLiteral("74 台")); // timer refresh preserves the confirmed snapshot
        page.refreshCurrent(true);
        QCOMPARE(value->text(), QStringLiteral("—")); // a new admin session explicitly clears it
        pie = qobject_cast<QPieSeries*>(distribution->chart()->series().first());
        QVERIFY(pie);
        QCOMPARE(pie->sum(), 0.0);
        QTRY_COMPARE(value->text(), QStringLiteral("74 台"));
        runtime.stop();
    }
};

QTEST_MAIN(AdminManagementPagesTest)
#include "tst_admin_management_pages.moc"
