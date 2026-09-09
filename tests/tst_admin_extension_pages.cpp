#include "activity_records_page.h"
#include "admin_request_gateway.h"
#include "database_connection.h"
#include "order_management_page.h"
#include "server_runtime.h"
#include "station_management_page.h"
#include "user_management_page.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <memory>

using namespace charging::server;
namespace {
QPushButton* button(QWidget& page, const QString& text)
{
    for (auto* widget : page.findChildren<QPushButton*>())
        if (widget->text() == text) return widget;
    return nullptr;
}
QLineEdit* edit(QWidget& page, const QString& placeholder)
{
    for (auto* widget : page.findChildren<QLineEdit*>())
        if (widget->placeholderText() == placeholder) return widget;
    return nullptr;
}
QComboBox* combo(QWidget& page, const QString& choice)
{
    for (auto* widget : page.findChildren<QComboBox*>())
        if (widget->findText(choice) >= 0) return widget;
    return nullptr;
}
void activate(QComboBox* widget, int index)
{
    widget->setCurrentIndex(index);
    widget->activated(index);
}
}

class AdminExtensionPagesTest final : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> directory_;
    DatabaseConnection fixture_;
    std::unique_ptr<ServerRuntime> runtime_;
    std::unique_ptr<AdminRequestGateway> gateway_;
    void startServer()
    {
        runtime_ = std::make_unique<ServerRuntime>();
        QSignalSpy ready(runtime_.get(), &ServerRuntime::listening);
        QVERIFY(runtime_->start(fixture_.databasePath(), false, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(ready.size(), 1);
        gateway_ = std::make_unique<AdminRequestGateway>(runtime_.get());
        gateway_->request("auth.login", {{"username", "admin"}, {"password", "123456"}}, this);
        QTRY_VERIFY(gateway_->isAuthenticated());
    }
    QString scalar(const QString& sql)
    {
        QSqlQuery query(fixture_.database());
        return query.exec(sql) && query.next() ? query.value(0).toString() : QString();
    }
private slots:
    void init()
    {
        directory_ = std::make_unique<QTemporaryDir>();
        QVERIFY(directory_->isValid());
        QString diagnostic;
        QVERIFY2(fixture_.open(directory_->filePath("extension-pages.sqlite"), true, &diagnostic),
                 qPrintable(diagnostic));
    }
    void cleanup()
    {
        gateway_.reset();
        runtime_.reset();
        fixture_.close();
        directory_.reset();
    }
    void orderPageUploadsAllThreeConditions()
    {
        QSqlQuery query(fixture_.database());
        QVERIFY(query.exec("UPDATE users SET nickname='张三' WHERE id=1"));
        QVERIFY(query.exec("INSERT INTO users(id,phone,nickname) VALUES(2,'13900139000','李四')"));
        QVERIFY(query.exec("INSERT INTO orders(order_no,user_id,charger_id,unit_price_cents_per_kwh) "
                           "VALUES('PAGE-AND-1',1,1,120),('PAGE-AND-2',2,2,120)"));
        startServer();
        OrderManagementPage page;
        page.setAdminGateway(gateway_.get());
        auto* table = page.findChild<QTableWidget*>();
        auto* number = edit(page, "完整订单号（精确）");
        auto* name = edit(page, "用户昵称（包含）");
        auto* phone = edit(page, "完整手机号（精确）");
        auto* search = button(page, "查询");
        QVERIFY(table && number && name && phone && search);
        QTRY_COMPARE(table->rowCount(), 2);
        number->setText("PAGE-AND-1"); name->setText("张"); phone->setText("13900139000");
        search->click();
        QTRY_COMPARE(table->rowCount(), 0); // Matching order/name, conflicting phone.
        phone->setText("13800138000"); search->click();
        QTRY_COMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), QString("PAGE-AND-1"));
        name->setText("李"); search->click();
        QTRY_COMPARE(table->rowCount(), 0); // Matching order/phone, conflicting nickname.
        name->setText("张"); search->click();
        QTRY_COMPARE(table->rowCount(), 1);
        number->setText("PAGE-AND-2"); search->click();
        QTRY_COMPARE(table->rowCount(), 0);
    }
    void optionSearchAndLoadMoreDoNotResetSelectedStation()
    {
        QSqlQuery query(fixture_.database());
        QVERIFY(query.exec("BEGIN"));
        for (int i = 1; i <= 105; ++i) {
            query.prepare("INSERT INTO stations(code,name,address,latitude,longitude,price_cents_per_kwh) "
                          "VALUES(?,?,'测试路',38.8,121.5,120)");
            query.addBindValue(QString("PAGE-OPTION-%1").arg(i, 3, 10, QLatin1Char('0')));
            query.addBindValue(QString("选项站%1").arg(i));
            QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
            query.prepare("INSERT INTO chargers(station_id,code,type,power_watts) VALUES(1,?,'FAST',60000)");
            query.addBindValue(QString("PAGE-CHARGER-%1").arg(i, 3, 10, QLatin1Char('0')));
            QVERIFY(query.exec());
        }
        QVERIFY(query.exec("COMMIT"));
        startServer();
        OrderManagementPage page;
        page.setAdminGateway(gateway_.get());
        auto* stations = combo(page, "全部电站");
        auto* chargers = combo(page, "全部电桩");
        QVERIFY(stations && chargers);
        QTRY_VERIFY(stations->findData("1") >= 0);
        activate(stations, stations->findData("1"));
        QTRY_VERIFY(chargers->findData("__load_more__") >= 0);
        const QString selectedStation = stations->currentData().toString();
        activate(stations, stations->findData("__load_more__"));
        QTRY_VERIFY(stations->count() > 100);
        QCOMPARE(stations->currentData().toString(), selectedStation);
        // Load page three, then search the item which was not on page one.
        activate(stations, stations->findData("__load_more__"));
        QTRY_COMPARE(stations->findData("__load_more__"), -1);
        QCOMPARE(stations->count(), 109); // 108 stations plus no-filter item.
        QCOMPARE(stations->currentData().toString(), selectedStation);
        activate(chargers, chargers->findData("__load_more__"));
        QTRY_VERIFY(chargers->count() > 100);
        activate(chargers, chargers->findData("__load_more__"));
        QTRY_COMPARE(chargers->findData("__load_more__"), -1);
        QCOMPARE(chargers->count(), 109); // Three original + 105 new at station 1.
        chargers->lineEdit()->setText("PAGE-CHARGER-105");
        chargers->lineEdit()->textEdited("PAGE-CHARGER-105");
        QTRY_COMPARE(chargers->count(), 2);
        QVERIFY(chargers->findText("PAGE-CHARGER-105（高新园区示范充电站）") > 0);
        stations->lineEdit()->setText("PAGE-OPTION-105");
        stations->lineEdit()->textEdited("PAGE-OPTION-105");
        QTRY_COMPARE(stations->count(), 2);
        QVERIFY(stations->findText("选项站105") > 0);
        activate(stations, stations->findText("选项站105"));
        QTRY_COMPARE(chargers->count(), 1); // No chargers at this new station.
        auto* reset = button(page, "重置");
        QVERIFY(reset);
        reset->click();
        QTRY_VERIFY(stations->findData("1") >= 0);
        QTRY_VERIFY(chargers->findData("4") >= 0);
        QVERIFY(stations->currentData().toString().isEmpty());
        QVERIFY(chargers->currentData().toString().isEmpty());
        // Reset must clear both the old city search and the selected station's
        // stationId filter, not only change the visible combo captions.
        chargers->lineEdit()->setText("CHG-DEMO-002-A1");
        chargers->lineEdit()->textEdited("CHG-DEMO-002-A1");
        QTRY_COMPARE(chargers->count(), 2);
        QVERIFY(chargers->findData("4") > 0);
    }
    void orderManualRefreshReloadsOptions()
    {
        startServer();
        OrderManagementPage page;
        page.setAdminGateway(gateway_.get());
        auto* stations = combo(page, "全部电站");
        auto* chargers = combo(page, "全部电桩");
        auto* refresh = button(page, "手动刷新");
        QVERIFY(stations && chargers && refresh);
        QTRY_COMPARE(stations->count(), 4);
        QTRY_COMPARE(chargers->count(), 8);
        QSqlQuery query(fixture_.database());
        QVERIFY(query.exec("INSERT INTO stations(id,code,name,address,latitude,longitude,price_cents_per_kwh) "
                           "VALUES(4,'REFRESH-OPTION','刷新新增站','测试路',38.8,121.5,120)"));
        QVERIFY(query.exec("INSERT INTO chargers(id,station_id,code,type,power_watts) "
                           "VALUES(8,4,'REFRESH-CHARGER','FAST',60000)"));
        refresh->click();
        QTRY_VERIFY(stations->findData("4") >= 0);
        QTRY_VERIFY(chargers->findData("8") >= 0);
        QCOMPARE(stations->itemText(stations->findData("4")), QString("刷新新增站"));
        QCOMPARE(chargers->itemText(chargers->findData("8")), QString("REFRESH-CHARGER（刷新新增站）"));
    }
    void userTimeAndBalanceControlsFilterRealRows()
    {
        QSqlQuery query(fixture_.database());
        QVERIFY(query.exec("UPDATE users SET balance_cents=10000,created_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=1"));
        QVERIFY(query.exec("INSERT INTO users(phone,nickname,balance_cents,created_at) VALUES"
                           "('13900139000','新高余额',30000,strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
                           "('13700137000','旧低余额',10000,'2020-01-01T00:00:00.000Z')"));
        startServer();
        UserManagementPage page;
        page.setAdminGateway(gateway_.get());
        auto* table = page.findChild<QTableWidget*>("managementUsersTable");
        auto* minimum = edit(page, "余额最小值");
        auto* maximum = edit(page, "余额最大值");
        auto* registration = combo(page, "近 7 日");
        auto* search = button(page, "查询");
        QVERIFY(table && minimum && maximum && registration && search);
        QTRY_COMPARE(table->rowCount(), 3);
        QVERIFY(minimum->isEnabled() && maximum->isEnabled() && registration->isEnabled());
        minimum->setText("100.00"); maximum->setText("100.00");
        registration->setCurrentIndex(registration->findText("近 7 日")); search->click();
        QTRY_COMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 1)->text(), QString("用户8000"));
        registration->setCurrentIndex(0); search->click();
        QTRY_COMPARE(table->rowCount(), 2);
        minimum->setText("300"); maximum->setText("300"); search->click();
        QTRY_COMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 1)->text(), QString("新高余额"));
    }
    void legacyNullContactsCanBeCompletedInEditForm()
    {
        startServer();
        StationManagementPage page;
        page.setAdminGateway(gateway_.get());
        page.show();
        auto* table = page.findChild<QTableWidget*>("stationManagementTable");
        auto* editButton = button(page, "编辑电站");
        QVERIFY(table && editButton);
        QTRY_COMPARE(table->rowCount(), 3);
        const auto originalCode = table->item(0, 0)->text();
        table->cellClicked(0, 0);
        QTRY_VERIFY(editButton->isEnabled());
        bool submitted = false;
        bool legacyWasEmpty = false;
        QTimer rescue;
        rescue.setSingleShot(true);
        connect(&rescue, &QTimer::timeout, &page, [] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* dialog = qobject_cast<QDialog*>(widget)) dialog->reject();
        });
        rescue.start(4000);
        QTimer::singleShot(0, &page, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* city = dialog->findChild<QComboBox*>("stationCityComboBox");
            auto* district = dialog->findChild<QLineEdit*>("stationDistrictLineEdit");
            auto* contact = dialog->findChild<QLineEdit*>("stationContactNameLineEdit");
            auto* phone = dialog->findChild<QLineEdit*>("stationContactPhoneLineEdit");
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            if (!city || !district || !contact || !phone || !buttons) { dialog->reject(); return; }
            legacyWasEmpty = district->text().isEmpty() && contact->text().isEmpty() && phone->text().isEmpty();
            city->setCurrentText("大连市"); district->setText("高新区");
            contact->setText("回归负责人"); phone->setText("13900139000");
            submitted = true;
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        editButton->click();
        rescue.stop();
        QVERIFY(submitted && legacyWasEmpty);
        const auto predicate = QString(" FROM stations WHERE code='%1'").arg(originalCode);
        QTRY_COMPARE(scalar("SELECT contact_phone" + predicate), QString("13900139000"));
        QCOMPARE(scalar("SELECT city" + predicate), QString("大连市"));
        QCOMPARE(scalar("SELECT district" + predicate), QString("高新区"));
        QCOMPARE(scalar("SELECT contact_name" + predicate), QString("回归负责人"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM operation_logs WHERE action='station.edit'"), QString("1"));
    }
    void auditMetadataAdminSelectionAndRechargeStates()
    {
        QSqlQuery query(fixture_.database());
        QVERIFY(query.exec("INSERT INTO admins(id,username,display_name,password_algorithm,password_salt,password_hash) "
                           "SELECT 2,'review-admin','审核管理员',password_algorithm,password_salt,password_hash FROM admins WHERE id=1"));
        QVERIFY(query.exec("INSERT INTO operation_logs(admin_id,action,target_type,target_id) VALUES"
                           "(1,'station.edit','STATION','1'),(2,'user.status','USER','1')"));
        startServer();
        ActivityRecordsPage logs(ActivityRecordsMode::OperationLog);
        logs.setAdminGateway(gateway_.get());
        auto* actions = logs.findChild<QComboBox*>("operationActionComboBox");
        auto* admins = logs.findChild<QComboBox*>("operationAdminComboBox");
        auto* table = logs.findChild<QTableWidget*>();
        auto* search = button(logs, "查询");
        QVERIFY(actions && admins && table && search);
        QTRY_VERIFY(actions->findData("charger_exceptions.recover") >= 0);
        QTRY_VERIFY(admins->findData("2") >= 0);
        QTRY_COMPARE(table->rowCount(), 2);
        actions->setCurrentIndex(actions->findData("user.status"));
        admins->setCurrentIndex(admins->findData("1")); search->click();
        QTRY_COMPARE(table->rowCount(), 0);
        admins->setCurrentIndex(admins->findData("2")); search->click();
        QTRY_COMPARE(table->rowCount(), 1);
        ActivityRecordsPage recharges(ActivityRecordsMode::Recharge);
        recharges.setAdminGateway(gateway_.get());
        auto* states = combo(recharges, "充值状态");
        QVERIFY(states);
        QCOMPARE(states->findData("PROCESSING"), -1);
        QCOMPARE(states->findText("处理中"), -1);
        QCOMPARE(states->count(), 3);
    }
    void firstMetadataFailureCanRetryWithoutLosingConfirmedOptions()
    {
        QSqlQuery query(fixture_.database());
        QVERIFY(query.exec("INSERT INTO operation_logs(admin_id,action,target_type,target_id) "
                           "VALUES(1,'station.edit','STATION','1')"));
        startServer();
        ActivityRecordsPage logs(ActivityRecordsMode::OperationLog);
        auto* actions = logs.findChild<QComboBox*>("operationActionComboBox");
        auto* table = logs.findChild<QTableWidget*>();
        auto* refresh = button(logs, "手动刷新");
        QVERIFY(actions && table && refresh);
        const auto isMetadata = [](const QJsonObject& response) {
            const auto rows = response.value("data").toObject().value("items").toArray();
            return !rows.isEmpty() && rows.first().toObject().contains("valueLabel");
        };
        const QJsonObject unavailable{{"success", false},
            {"error", QJsonObject{{"code", "UNAVAILABLE"}, {"message", "Not available"}}}};
        bool firstFailureInjected = false;
        // Explicit response-failure injection at the GUI boundary. All requests
        // still run through the authenticated gateway and actual worker/SQLite.
        // Suppress the first normal delivery, then forward the real responses,
        // replacing only the first action-metadata result with a safe failure.
        QSignalBlocker delivery(gateway_.get());
        const auto faultConnection = connect(runtime_.get(), &ServerRuntime::adminResponse, &logs,
            [&](const QString& id, const QJsonObject& response) {
                const bool fail = !firstFailureInjected && isMetadata(response);
                if (fail) firstFailureInjected = true;
                delivery.unblock();
                gateway_->finished(id, fail ? unavailable : response);
                delivery.reblock();
            });
        logs.setAdminGateway(gateway_.get());
        QTRY_VERIFY(firstFailureInjected);
        QTRY_COMPARE(table->rowCount(), 1); // Concurrent list success must not erase the metadata error.
        QCOMPARE(actions->count(), 1);
        QVERIFY(actions->toolTip().contains("加载失败"));
        disconnect(faultConnection);
        delivery.unblock();

        QString metadataRequestId;
        const auto observed = connect(gateway_.get(), &AdminRequestGateway::finished, &logs,
            [&](const QString& id, const QJsonObject& response) {
                if (isMetadata(response)) metadataRequestId = id;
            });
        refresh->click();
        QTRY_VERIFY(actions->findData("charger_exceptions.recover") >= 0);
        QVERIFY(actions->toolTip().isEmpty());
        QVERIFY(!metadataRequestId.isEmpty());
        actions->setCurrentIndex(actions->findData("station.edit"));
        const int confirmedCount = actions->count();
        gateway_->finished(metadataRequestId, unavailable);
        QCOMPARE(actions->count(), confirmedCount);
        QCOMPARE(actions->currentData().toString(), QString("station.edit"));
        QVERIFY(actions->toolTip().contains("加载失败"));
        QSignalSpy subsequent(gateway_.get(), &AdminRequestGateway::finished);
        logs.refreshData();
        QTRY_VERIFY(subsequent.size() >= 2);
        QVERIFY(actions->toolTip().contains("加载失败"));
        refresh->click();
        QTRY_VERIFY(actions->toolTip().isEmpty());
        QCOMPARE(actions->currentData().toString(), QString("station.edit"));
        disconnect(observed);
    }
};

QTEST_MAIN(AdminExtensionPagesTest)
#include "tst_admin_extension_pages.moc"
