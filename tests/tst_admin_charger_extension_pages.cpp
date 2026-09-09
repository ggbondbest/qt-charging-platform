#include "admin_request_gateway.h"
#include "charger_management_page.h"
#include "server_runtime.h"

#include <QComboBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

namespace charging::server {
namespace {
QJsonObject success(const QJsonObject& data) { return {{"success", true}, {"data", data}}; }
QJsonObject failure(const QString& code)
{
    return {{"success", false}, {"error", QJsonObject{{"code", code}, {"message", code}}}};
}
QJsonObject event()
{
    return {{"id", "31"}, {"chargerId", "1"}, {"status", "ACTIVE"}, {"recoverable", true},
            {"safeSummary", "模拟故障事件"}, {"recoveryAction", "SIMULATE_RESTORE"},
            {"occurredAt", "2026-09-09T00:00:00.000Z"}, {"updatedAt", "2026-09-09T00:00:00.000Z"}};
}
QJsonObject charger(const QString& id, const QString& status, const QJsonObject& exception = {})
{
    return {{"id", id}, {"stationId", "1"}, {"stationName", "测试站"}, {"code", "TEST-" + id},
            {"status", status}, {"type", "FAST"}, {"powerWatts", 120000}, {"totalChargeCount", 0},
            {"totalChargeSeconds", 0}, {"updatedAt", "2026-09-09T00:00:00.000Z"}, {"activeException", exception}};
}
QJsonObject list(const QString& firstStatus = "FAULT")
{
    return success({{"items", QJsonArray{charger("1", firstStatus, event()), charger("2", "AVAILABLE")}},
                    {"total", 2}, {"page", 1}, {"pageSize", 10}});
}
QJsonObject recoverRequest()
{
    return {{"id", "31"}, {"operationId", "original-operation"}, {"recoveryAction", "SIMULATE_RESTORE"},
            {"expectedUpdatedAt", "2026-09-09T00:00:00.000Z"}};
}
} // namespace

// Deterministic presenter/gateway tests inject explicit envelopes; service and
// SQLite transaction coverage lives in tst_admin_charger_extensions.cpp.
class AdminChargerExtensionPagesTest final : public QObject
{
    Q_OBJECT
    void authenticate(ServerRuntime& runtime, AdminRequestGateway& gateway, const QString& adminId = "1")
    {
        const auto id = gateway.request("auth.login", {}, this);
        runtime.adminResponse(id, success({{"sessionToken", "test-only-capability"},
                                            {"admin", QJsonObject{{"id", adminId}}}}));
        QCOMPARE(gateway.adminId(), adminId);
    }
private slots:
    void manualRefreshRetriesInitiallyFailedOptions_data()
    {
        QTest::addColumn<bool>("detailRefresh");
        QTest::newRow("page-refresh") << false;
        QTest::newRow("detail-refresh") << true;
    }
    void manualRefreshRetriesInitiallyFailedOptions()
    {
        QFETCH(bool, detailRefresh);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ServerRuntime runtime;
        QSignalSpy ready(&runtime, &ServerRuntime::listening);
        const QString path = directory.filePath(QStringLiteral("options-retry.sqlite"));
        QVERIFY(runtime.start(path, true, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(ready.size(), 1);
        AdminRequestGateway gateway(&runtime);
        gateway.request("auth.login", {{"username", "admin"}, {"password", "123456"}}, this);
        QTRY_VERIFY(gateway.isAuthenticated());
        const QString connectionName = "charger-options-failure-injection";
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connectionName);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery query(database);
            // Temporary isolated-test schema failure, not a fake UI error.
            QVERIFY(query.exec("ALTER TABLE stations RENAME TO stations_unavailable_fixture"));
            ChargerManagementPage page;
            page.setAdminGateway(&gateway);
            QTRY_VERIFY(page.stationComboBox_->toolTip().contains(QStringLiteral("加载失败")));
            QVERIFY(query.exec("ALTER TABLE stations_unavailable_fixture RENAME TO stations"));
            if (detailRefresh) page.refreshSelectedStatus(); else page.refreshData();
            QTRY_VERIFY(page.stationComboBox_->findData(QStringLiteral("1")) >= 0);
            QVERIFY(!page.stationComboBox_->toolTip().contains(QStringLiteral("加载失败")));
            database.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
        runtime.stop();
    }
    void refreshCannotReenableOrReplacePendingWrite()
    {
        ServerRuntime runtime;
        AdminRequestGateway gateway(&runtime);
        authenticate(runtime, gateway);
        ChargerManagementPage page;
        page.setAdminGateway(&gateway);
        page.handleListResponse(list());
        page.showChargerDetails(0, false);
        QVERIFY(page.clearAlertButton_->isEnabled());
        page.submitWrite("charger_exceptions.recover", recoverRequest());
        const auto requestId = page.writeRequestId_;
        QVERIFY(!requestId.isEmpty());
        QVERIFY(page.writeInFlight_);
        page.handleListResponse(list());
        page.updateDetailActions();
        QVERIFY(!page.clearAlertButton_->isEnabled());
        QVERIFY(!page.editButton_->isEnabled());
        QVERIFY(!page.restartButton_->isEnabled());
        page.submitWrite("charger.status", {{"operationId", "replacement"}, {"id", "2"}});
        QCOMPARE(page.writeRequestId_, requestId);
        QCOMPARE(page.pendingWriteParameters_, recoverRequest());
    }
    void periodicRefreshKeepsSuccessfulOptionSearch()
    {
        QTemporaryDir directory;
        ServerRuntime runtime;
        QSignalSpy ready(&runtime, &ServerRuntime::listening);
        QVERIFY(runtime.start(directory.filePath(QStringLiteral("options-search.sqlite")), true, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(ready.size(), 1);
        AdminRequestGateway gateway(&runtime);
        gateway.request("auth.login", {{"username", "admin"}, {"password", "123456"}}, this);
        QTRY_VERIFY(gateway.isAuthenticated());
        ChargerManagementPage page;
        page.setAdminGateway(&gateway);
        QTRY_VERIFY(page.stationComboBox_->findData(QStringLiteral("1")) >= 0 && page.stationComboBox_->count() > 3);
        page.stationComboBox_->lineEdit()->setText(QStringLiteral("海创"));
        page.stationComboBox_->lineEdit()->textEdited(QStringLiteral("海创"));
        QTRY_COMPARE(page.stationComboBox_->count(), 2);
        QCOMPARE(page.stationComboBox_->itemData(1).toString(), QString("2"));
        QCOMPARE(page.stationComboBox_->currentText(), QStringLiteral("海创"));
        QSignalSpy responses(&gateway, &AdminRequestGateway::finished);
        page.refreshData();
        QTRY_VERIFY(responses.size() >= 2); // List + summary completed.
        QTest::qWait(100);
        QCOMPARE(page.stationComboBox_->count(), 2);
        QCOMPARE(page.stationComboBox_->currentText(), QStringLiteral("海创"));
        QCOMPARE(page.stationComboBox_->itemData(1).toString(), QString("2"));
        runtime.stop();
    }
    void timeoutReplayKeepsOriginalTargetAndRejectsLateResponse()
    {
        ServerRuntime runtime;
        AdminRequestGateway gateway(&runtime);
        authenticate(runtime, gateway);
        ChargerManagementPage page;
        page.setAdminGateway(&gateway);
        page.handleListResponse(list());
        page.submitWrite("charger_exceptions.recover", recoverRequest());
        const auto originalRequestId = page.writeRequestId_;
        page.handleWriteResponse(failure("TIMEOUT"));
        QVERIFY(page.writeOutcomeUnknown_);
        QVERIFY(!page.writeInFlight_);
        QVERIFY(page.retryWriteButton_->isEnabled());
        QVERIFY(page.writeStatusLabel_->text().contains(QStringLiteral("结果未知")));
        page.showChargerDetails(1, false);
        QVERIFY(!page.editButton_->isEnabled());
        page.retryWriteButton_->click();
        QVERIFY(page.writeInFlight_);
        QVERIFY(page.writeRequestId_ != originalRequestId);
        QCOMPARE(page.pendingWriteParameters_, recoverRequest());
        QCOMPARE(page.pendingWriteAction_, QString("charger_exceptions.recover"));
        gateway.finished(originalRequestId, success({{"commandId", "stale"}}));
        QVERIFY(page.writeInFlight_);
        QCOMPARE(page.pendingWriteParameters_, recoverRequest());
        const auto newRequestId = page.writeRequestId_;
        const QString message = QStringLiteral("当前异常已完成受控模拟恢复；电桩仍有其他活动异常。未发送硬件命令。");
        gateway.finished(newRequestId, success({{"commandId", "confirmed"}, {"idempotent", true},
                                                {"item", QJsonObject{{"recoveryMessage", message}}}}));
        QVERIFY(page.pendingWriteAction_.isEmpty());
        QVERIFY(!page.writeOutcomeUnknown_);
        QCOMPARE(page.writeStatusLabel_->text(), message);
        page.handleListResponse(list());
        QCOMPARE(page.writeStatusLabel_->text(), message);
    }
    void onlyOriginalAdministratorCanReplayUnknownOutcome()
    {
        ServerRuntime runtime;
        AdminRequestGateway gateway(&runtime);
        authenticate(runtime, gateway);
        ChargerManagementPage page;
        page.setAdminGateway(&gateway);
        page.handleListResponse(list());
        page.submitWrite("charger_exceptions.recover", recoverRequest());
        gateway.logout();
        QVERIFY(gateway.adminId().isEmpty());
        QVERIFY(page.writeOutcomeUnknown_);
        QVERIFY(!page.retryWriteButton_->isEnabled());
        authenticate(runtime, gateway, "2");
        QVERIFY(!page.retryWriteButton_->isEnabled());
        page.retryPendingWrite();
        QVERIFY(!page.writeInFlight_);
        QCOMPARE(page.pendingWriteAdminId_, QString("1"));
        QVERIFY(page.writeStatusLabel_->text().contains(QStringLiteral("原管理员")));
        authenticate(runtime, gateway, "1");
        QVERIFY(page.retryWriteButton_->isEnabled());
        page.retryPendingWrite();
        QVERIFY(page.writeInFlight_);
        QCOMPARE(page.pendingWriteParameters_, recoverRequest());
    }
    void busyFailureReleasesPendingButDoesNotClaimSuccess()
    {
        ServerRuntime runtime;
        AdminRequestGateway gateway(&runtime);
        authenticate(runtime, gateway);
        ChargerManagementPage page;
        page.setAdminGateway(&gateway);
        page.handleListResponse(list());
        page.submitWrite("charger_exceptions.recover", recoverRequest());
        page.handleWriteResponse(failure("RESOURCE_BUSY"));
        QVERIFY(page.pendingWriteAction_.isEmpty());
        QVERIFY(!page.writeInFlight_);
        QVERIFY(!page.writeOutcomeUnknown_);
        QVERIFY(page.writeStatusLabel_->text().contains(QStringLiteral("拒绝")));
        QVERIFY(!page.writeStatusLabel_->text().contains(QStringLiteral("已恢复")));
    }
    void runtimeExplainsSimulationAndInvalidatesOlderSession()
    {
        ServerRuntime runtime;
        AdminRequestGateway gateway(&runtime);
        authenticate(runtime, gateway);
        ChargerManagementPage page;
        page.setAdminGateway(&gateway);
        page.handleListResponse(list("CHARGING"));
        page.showChargerDetails(0, false);
        page.runtimeExpectedServerId_ = "1";
        const auto snapshot = QJsonObject{{"chargerId", "1"}, {"status", "CHARGING"}, {"sessionId", "91"},
            {"source", "SIMULATED_METER"}, {"simulated", true}, {"estimated", true},
            {"capturedAt", "2026-09-09T00:01:00.000Z"}, {"currentPowerWatts", 120000},
            {"energyWh", 2000}, {"chargeSeconds", 60}, {"currentAmountCents", 240}, {"lastHeartbeatAt", QJsonValue::Null}};
        page.handleRuntimeResponse(success({{"item", snapshot}}));
        QVERIFY(page.detailRuntimeInfoLabel_->text().contains(QStringLiteral("服务端模拟采样")));
        QVERIFY(page.detailRuntimeInfoLabel_->text().contains(QStringLiteral("非硬件遥测")));
        QVERIFY(page.detailRuntimeInfoLabel_->text().contains(QStringLiteral("暂估金额")));
        QVERIFY(page.detailRuntimeInfoLabel_->text().contains(QStringLiteral("¥ 2.40")));
        page.handleRuntimeResponse(failure("TIMEOUT"));
        QCOMPARE(page.runtimeSnapshots_.value("1"), snapshot);
        auto updated = charger("1", "CHARGING");
        updated.insert("updatedAt", "2026-09-09T00:02:00.000Z");
        page.detailExpectedServerId_ = "1";
        page.handleDetailResponse(success({{"item", updated}}));
        QVERIFY(!page.runtimeSnapshots_.contains("1"));
        QVERIFY(!page.detailRuntimeInfoLabel_->text().contains(QStringLiteral("¥ 2.40")));
        page.runtimeExpectedServerId_ = "2";
        page.handleRuntimeResponse(success({{"item", snapshot}}));
        QVERIFY(!page.runtimeSnapshots_.contains("1"));
    }
};
} // namespace charging::server

QTEST_MAIN(charging::server::AdminChargerExtensionPagesTest)
#include "tst_admin_charger_extension_pages.moc"
