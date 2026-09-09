#include "workflow_management_page.h"
#include "admin_request_gateway.h"
#include "server_runtime.h"

#include <QComboBox>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QtTest>
#include <memory>

namespace charging::server {
namespace {
QJsonObject success(const QJsonObject& data) { return {{"success", true}, {"data", data}}; }
QJsonObject failure(const QString& code)
{ return {{"success", false}, {"error", QJsonObject{{"code", code}, {"message", code}}}}; }
QJsonObject report(const QString& id, const QString& status = "SUBMITTED")
{
    return {{"id", id}, {"stationId", "1"}, {"stationName", "测试站"}, {"chargerId", id},
            {"chargerCode", "CHG-" + id}, {"description", "报障说明-" + id}, {"status", status},
            {"updatedAt", "2026-09-09T10:00:00.000Z"},
            {"user", QJsonObject{{"phone", "138****8000"}}},
            {"timeline", QJsonArray{QJsonObject{{"status", status}, {"createdAt", "2026-09-09T10:00:00.000Z"}, {"note", "当前处理说明"}}}}};
}
QJsonObject twoReports()
{ return success({{"items", QJsonArray{report("1"), report("2")}}, {"total", 2}}); }
QJsonObject command()
{
    return {{"id", "1"}, {"operationId", "immutable-original-operation"},
            {"expectedUpdatedAt", "2026-09-09T10:00:00.000Z"}, {"note", "确认故障，进入维护"}};
}
}

// Explicit response injection makes races deterministic; real database and
// gateway round-trips are separately covered by workflow_management_page.
class WorkflowManagementRacesTest final : public QObject
{
    Q_OBJECT
    void authenticate(ServerRuntime& runtime, AdminRequestGateway& gateway, const QString& adminId = "1")
    {
        const auto id = gateway.request("auth.login", {}, this);
        runtime.adminResponse(id, success({{"sessionToken", "test-capability"}, {"admin", QJsonObject{{"id", adminId}}}}));
        QCOMPARE(gateway.adminId(), adminId);
    }
    void populate(WorkflowManagementPage& page)
    {
        page.tabs_->setCurrentIndex(1);
        page.receive(page.listRequest_, twoReports());
        page.repairTable_->selectRow(0);
        page.receive(page.detailRequest_, success({{"item", report("1")}}));
        QCOMPARE(page.selected_.value("id").toString(), QString("1"));
        QVERIFY(page.accept_->isEnabled());
    }
    void write(WorkflowManagementPage& page)
    {
        // Begin at the point where the operator has accepted the note dialog.
        page.retryPayload_ = command(); page.retryAction_ = "repair_reports.accept";
        page.updateActions(); page.retry_->click();
        QVERIFY(!page.mutationRequest_.isEmpty());
    }
private slots:
    void timeoutKeepsOperationAndLateSuccessCannotReplaceSelectedReport()
    {
        ServerRuntime runtime; AdminRequestGateway gateway(&runtime); authenticate(runtime, gateway);
        WorkflowManagementPage page(&gateway); populate(page);
        page.refreshData(); const auto staleList = page.listRequest_;
        page.selectReport(); const auto staleDetail = page.detailRequest_;
        write(page);
        const auto original = page.mutationRequest_;
        QVERIFY(page.listRequest_.isEmpty()); QVERIFY(page.detailRequest_.isEmpty());
        QVERIFY(!page.accept_->isEnabled()); QVERIFY(!page.start_->isEnabled());
        page.receive(staleList, success({{"items", QJsonArray{report("3")}}, {"total", 1}}));
        QCOMPARE(page.repairTable_->rowCount(), 2);
        page.receive(staleDetail, success({{"item", report("1", "RESOLVED")}}));
        QCOMPARE(page.selected_.value("status").toString(), QString("SUBMITTED"));
        page.receive(original, failure("TIMEOUT"));
        QVERIFY(page.mutationRequest_.isEmpty());
        QCOMPARE(page.retryPayload_, command());
        QCOMPARE(page.retryAction_, QString("repair_reports.accept"));
        QVERIFY(page.retry_->isEnabled()); QVERIFY(!page.accept_->isEnabled());
        page.repairTable_->selectRow(1);
        page.receive(page.detailRequest_, success({{"item", report("2")}}));
        page.retry_->click();
        const auto replay = page.mutationRequest_;
        QVERIFY(!replay.isEmpty()); QVERIFY(replay != original);
        QCOMPARE(page.retryPayload_, command());
        page.receive(original, success({{"item", report("1", "ACCEPTED")}}));
        QCOMPARE(page.mutationRequest_, replay);
        QCOMPARE(page.selected_.value("id").toString(), QString("2"));
        page.receive(replay, success({{"item", report("1", "ACCEPTED")}, {"idempotent", true}}));
        QVERIFY(page.retryPayload_.isEmpty());
        // A failed refresh must leave row B and its actions coherent, never A's
        // successful result attached to B's highlighted row.
        page.receive(page.listRequest_, failure("DATABASE_ERROR"));
        QCOMPARE(page.repairTable_->currentRow(), 1);
        QCOMPARE(page.selected_.value("id").toString(), QString("2"));
        QVERIFY(page.timeline_->toPlainText().contains("报障 #2"));
        QVERIFY(page.accept_->isEnabled()); QVERIFY(!page.start_->isEnabled());
        page.repairTable_->clearSelection();
        QVERIFY(page.selected_.isEmpty()); QVERIFY(!page.accept_->isEnabled());
    }

    void successfulWriteDoesNotJumpBackFromNewSelection()
    {
        ServerRuntime runtime; AdminRequestGateway gateway(&runtime); authenticate(runtime, gateway);
        WorkflowManagementPage page(&gateway); populate(page); write(page);
        const auto id = page.mutationRequest_;
        page.repairTable_->selectRow(1);
        page.receive(page.detailRequest_, success({{"item", report("2")}}));
        page.receive(id, success({{"item", report("1", "ACCEPTED")}}));
        QCOMPARE(page.selected_.value("id").toString(), QString("2"));
        QVERIFY(page.message_->text().contains("#1"));
        QVERIFY(page.timeline_->toPlainText().contains("报障 #2"));
        page.receive(page.listRequest_, twoReports());
        page.receive(page.detailRequest_, success({{"item", report("2")}}));
        QCOMPARE(page.repairTable_->currentRow(), 1);
        QCOMPARE(page.selected_.value("id").toString(), QString("2"));
    }

    void loadMoreIsNotAnAllStationsFilterChange()
    {
        ServerRuntime runtime; AdminRequestGateway gateway(&runtime); authenticate(runtime, gateway);
        WorkflowManagementPage page(&gateway); populate(page);
        {
            QSignalBlocker blocker(page.station_);
            page.station_->clear(); page.station_->addItem("全部", "");
            page.station_->addItem("第一站", "1"); page.station_->addItem("第二站", "2");
            page.station_->addItem("加载更多…", "__load_more__");
        }
        page.station_->setCurrentIndex(1);
        page.receive(page.listRequest_, twoReports());
        QVERIFY(page.listRequest_.isEmpty());
        page.station_->setCurrentIndex(3);
        QVERIFY(page.listRequest_.isEmpty()); // No accidental global query.
        QVERIFY(QMetaObject::invokeMethod(page.station_, "activated", Q_ARG(int, 3)));
        QCOMPARE(page.station_->currentData().toString(), QString("1"));
        QVERIFY(page.listRequest_.isEmpty());
        QCOMPARE(page.repairTable_->rowCount(), 2);
    }

    void ownerDestructionDropsPendingWriteResponse()
    {
        ServerRuntime runtime; AdminRequestGateway gateway(&runtime); authenticate(runtime, gateway);
        auto page = std::make_unique<WorkflowManagementPage>(&gateway);
        populate(*page); write(*page);
        const auto id = page->mutationRequest_;
        QSignalSpy replies(&gateway, &AdminRequestGateway::finished);
        page.reset();
        runtime.adminResponse(id, success({{"item", report("1", "ACCEPTED")}}));
        QCOMPARE(replies.size(), 0);
    }

    void accountSwitchInsideNoteDialogDoesNotSubmitOldCommand()
    {
        ServerRuntime runtime; AdminRequestGateway gateway(&runtime); authenticate(runtime, gateway);
        WorkflowManagementPage page(&gateway); populate(page);
        QTimer::singleShot(0, &page, [&] {
            gateway.logout(); authenticate(runtime, gateway, "2");
            for (auto* dialog : page.findChildren<QInputDialog*>()) {
                dialog->setTextValue("必须丢弃的旧账号操作"); dialog->accept();
            }
        });
        page.accept_->click();
        QCOMPARE(gateway.adminId(), QString("2"));
        QVERIFY(page.mutationRequest_.isEmpty()); QVERIFY(page.retryPayload_.isEmpty());
        QVERIFY(page.selected_.isEmpty());
    }

    void refreshRetriesFailedStationOptions()
    {
        QTemporaryDir files; ServerRuntime runtime;
        const auto path = files.filePath("options.sqlite");
        QVERIFY(runtime.start(path, true, QHostAddress::LocalHost, 0));
        QTRY_VERIFY(runtime.isListening());
        AdminRequestGateway gateway(&runtime);
        gateway.request("auth.login", {{"username", "admin"}, {"password", "123456"}}, this);
        QTRY_VERIFY(gateway.isAuthenticated());
        const auto connection = QStringLiteral("workflow-options-injection");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery q(db); QVERIFY(q.exec("ALTER TABLE stations RENAME TO temporarily_unavailable_stations"));
            WorkflowManagementPage page(&gateway);
            QTRY_VERIFY(page.station_->toolTip().contains("加载失败"));
            QTRY_VERIFY(page.listRequest_.isEmpty());
            QVERIFY(q.exec("ALTER TABLE temporarily_unavailable_stations RENAME TO stations"));
            page.refreshData();
            QTRY_VERIFY(page.station_->findData("1") >= 0);
            QVERIFY(!page.station_->toolTip().contains("加载失败"));
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
};
} // namespace charging::server

QTEST_MAIN(charging::server::WorkflowManagementRacesTest)
#include "tst_workflow_management_races.moc"
