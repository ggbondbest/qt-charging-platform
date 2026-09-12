#include "app_bridge.h"
#include "workflow_bridge.h"
#include "admin_request_gateway.h"
#include "database_connection.h"
#include "repair_repository.h"
#include "repair_service.h"
#include "server_runtime.h"

#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

using namespace charging::qml;
using namespace charging::server;
namespace {
QVariant converted(const QVariant& value)
{ return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant() : value; }
QVariantMap mapProperty(QObject* object, const char* property)
{ return converted(object->property(property)).toMap(); }
QVariantList listProperty(QObject* object, const char* property)
{ return converted(object->property(property)).toList(); }
QJsonObject responseData(const QJsonObject& response) { return response.value("data").toObject(); }
QJsonObject responseItem(const QJsonObject& response) { return responseData(response).value("item").toObject(); }
}

class QmlWorkflowPagesTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir settings_;
    QStringList qmlWarnings_;
    void bind(QQmlEngine& engine, QmlApp& app)
    {
        engine.rootContext()->setContextProperty("App", &app);
        connect(&engine, &QQmlEngine::warnings, this, [this](const QList<QQmlError>& errors) {
            for (const auto& error : errors) qmlWarnings_.append(error.toString());
        });
    }
    QQuickItem* load(QQmlEngine& engine, QQuickWindow& window, const QString& filename,
                     const QVariantMap& arg = {})
    {
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(CHARGING_QML_SOURCE_DIR) + "/pages/station/" + filename));
        if (component.isError()) qmlWarnings_ << component.errorString();
        auto* item = qobject_cast<QQuickItem*>(component.createWithInitialProperties({{"arg", arg}}));
        if (item) item->setParentItem(window.contentItem());
        return item;
    }
    QJsonObject admin(AdminRequestGateway& gateway, const QString& action, const QJsonObject& p = {})
    {
        QSignalSpy replies(&gateway, &AdminRequestGateway::finished);
        const auto id = gateway.request(action, p, this);
        QElapsedTimer clock; clock.start();
        while (clock.elapsed() < 10000) {
            for (const auto& row : replies) if (row[0].toString() == id) return row[1].toJsonObject();
            replies.wait(100);
        }
        return {};
    }
    QJsonObject user(WorkflowBridge* bridge, const QString& type, const QVariantMap& p = {})
    {
        QSignalSpy replies(bridge, &WorkflowBridge::finished);
        const auto id = bridge->request(type, p);
        QElapsedTimer clock; clock.start();
        while (clock.elapsed() < 10000) {
            for (const auto& row : replies) if (row[0].toString() == id)
                return {{"success", row[2].toBool()}, {"data", QJsonObject::fromVariantMap(row[3].toMap())},
                        {"error", QJsonObject::fromVariantMap(row[4].toMap())}};
            replies.wait(100);
        }
        return {};
    }
private slots:
    void initTestCase()
    {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName("ChargingPlatformTests");
        QCoreApplication::setApplicationName("qml-workflow-pages");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings_.path());
    }
    void init() { qmlWarnings_.clear(); }
    void cleanup() { QVERIFY2(qmlWarnings_.isEmpty(), qPrintable(qmlWarnings_.join('\n'))); }

    void mockShowsUnavailableInsteadOfInventingQueueOrReports()
    {
        QmlApp app("127.0.0.1", 0, true);
        QVERIFY(app.login("13800138000"));
        QVERIFY(app.loggedIn());
        QQmlEngine engine; bind(engine, app);
        QQuickWindow window; window.resize(420, 860); window.show();
        std::unique_ptr<QQuickItem> queue(load(engine, window, "QueuePage.qml", {{"chargerId", "1"}, {"chargerCode", "CHG-DEMO-001-A1"}}));
        QVERIFY(queue);
        QTRY_VERIFY(queue->property("errorMessage").toString().contains("请连接服务器"));
        QVERIFY(mapProperty(queue.get(), "entry").isEmpty());
        queue.reset();
        std::unique_ptr<QQuickItem> repair(load(engine, window, "FaultReportsPage.qml", {{"chargerId", "1"}}));
        QVERIFY(repair);
        QTRY_VERIFY(repair->property("message").toString().contains("请连接服务器"));
        QVERIFY(listProperty(repair.get(), "reports").isEmpty());
        auto* input = repair->findChild<QObject*>("repairDescription");
        auto* submit = repair->findChild<QObject*>("repairSubmitButton");
        QVERIFY(input); QVERIFY(submit);
        QVERIFY(input->setProperty("text", "演示通道不可虚构报障"));
        QVERIFY(QMetaObject::invokeMethod(submit, "clicked"));
        QTRY_VERIFY(repair->property("submitRequest").toString().isEmpty());
        QVERIFY(mapProperty(repair.get(), "selected").isEmpty());
        QVERIFY(listProperty(repair.get(), "reports").isEmpty());
        QVERIFY(repair->property("message").toString().contains("请连接服务器"));
    }

    void realFormSubmissionAndAdminProgressArriveOverTcp()
    {
        QTemporaryDir files; QVERIFY(files.isValid());
        ServerRuntime runtime;
        QVERIFY(runtime.start(files.filePath("workflow.sqlite"), true, QHostAddress::LocalHost, 0));
        QTRY_VERIFY(runtime.isListening());
        AdminRequestGateway gateway(&runtime);
        QVERIFY(admin(gateway, "auth.login", {{"username", "admin"}, {"password", "123456"}}).value("success").toBool());
        QmlApp app("127.0.0.1", runtime.serverPort(), false);
        QVERIFY(app.login("13900139123")); QTRY_VERIFY(app.loggedIn());
        QTRY_VERIFY(!app.checkingOrders());
        auto* bridge = qobject_cast<WorkflowBridge*>(app.workflowService()); QVERIFY(bridge);
        QSignalSpy changed(bridge, &WorkflowBridge::changed);
        QQmlEngine engine; bind(engine, app);
        QQuickWindow window; window.resize(420, 860); window.show();
        std::unique_ptr<QQuickItem> repair(load(engine, window, "FaultReportsPage.qml", {
            {"chargerId", "1"}, {"chargerCode", "CHG-DEMO-001-A1"}, {"stationName", "高新园区示范充电站"}}));
        QVERIFY(repair);
        auto* description = repair->findChild<QObject*>("repairDescription");
        auto* problem = repair->findChild<QObject*>("repairProblemType");
        auto* submit = repair->findChild<QObject*>("repairSubmitButton");
        QVERIFY(description); QVERIFY(problem); QVERIFY(submit);
        QVERIFY(problem->setProperty("currentIndex", 2));
        QVERIFY(description->setProperty("text", QString(210, 'x')));
        QTRY_COMPARE(description->property("text").toString().size(), 200);
        QVERIFY(description->setProperty("text", "充电枪锁扣松动，请核实"));
        QTRY_VERIFY(submit->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(submit, "clicked"));
        QTRY_COMPARE_WITH_TIMEOUT(mapProperty(repair.get(), "selected").value("status").toString(), QString("SUBMITTED"), 10000);
        auto report = mapProperty(repair.get(), "selected");
        QCOMPARE(report.value("problemType").toString(), QString("CONNECTOR"));
        QCOMPARE(report.value("description").toString(), QString("充电枪锁扣松动，请核实"));
        QCOMPARE(report.value("timeline").toList().size(), 1);
        QVERIFY(!repair->property("formVisible").toBool());
        QCOMPARE(responseItem(admin(gateway, "chargers.get", {{"id", "1"}})).value("status").toString(), QString("AVAILABLE"));
        int operation = 0;
        for (const auto& action : {"repair_reports.accept", "repair_reports.start", "repair_reports.resolve"}) {
            const auto before = responseItem(admin(gateway, "repair_reports.get", {{"id", report.value("id").toString()}}));
            const auto updated = admin(gateway, action, {{"id", before.value("id")},
                {"operationId", QString("qml-progress-%1").arg(++operation)}, {"expectedUpdatedAt", before.value("updatedAt")},
                {"note", QStringLiteral("管理员已核实，执行模拟处理")}});
            QVERIFY2(updated.value("success").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(updated).toJson())));
            const auto expected = responseItem(updated).value("status").toString();
            QTRY_COMPARE_WITH_TIMEOUT(mapProperty(repair.get(), "selected").value("status").toString(), expected, 6000);
            QCOMPARE(mapProperty(repair.get(), "selected").value("timeline").toList().size(), operation + 1);
            const auto charger = responseItem(admin(gateway, "chargers.get", {{"id", "1"}}));
            QCOMPARE(charger.value("status").toString(), operation == 3 ? QString("AVAILABLE") : QString("OFFLINE"));
        }
        QVERIFY(!changed.isEmpty());
        const auto mine = user(bridge, "REPAIR_GET_MINE");
        QVERIFY(mine.value("success").toBool()); QCOMPARE(responseData(mine).value("total").toInt(), 1);
    }

    void reportPaginationAndAccountSwitchClearOldData()
    {
        QTemporaryDir files; QVERIFY(files.isValid());
        const auto path = files.filePath("pagination.sqlite");
        {
            DatabaseConnection db; QString error;
            QVERIFY2(db.open(path, true, &error), qPrintable(error));
            QVERIFY2(db.applyCityDemoSeed(&error), qPrintable(error));
            RepairRepository repository(db.database()); RepairService service(&repository);
            QSqlQuery q(db.database()); QVERIFY(q.exec("SELECT id FROM chargers ORDER BY id LIMIT 21"));
            QStringList ids; while (q.next()) ids << q.value(0).toString(); q.finish();
            QCOMPARE(ids.size(), 21);
            for (const auto& id : ids) QVERIFY(service.handle("REPAIR_SUBMIT", {{"chargerId", id},
                {"problemType", "OTHER"}, {"description", "分页回归测试报障"}, {"operationId", "page-" + id}}, 1).value("success").toBool());
        }
        ServerRuntime runtime; QVERIFY(runtime.start(path, false, QHostAddress::LocalHost, 0));
        QTRY_VERIFY(runtime.isListening());
        QmlApp app("127.0.0.1", runtime.serverPort(), false);
        QVERIFY(app.login("13800138000")); QTRY_VERIFY(app.loggedIn()); QTRY_VERIFY(!app.checkingOrders());
        QQmlEngine engine; bind(engine, app);
        QQuickWindow window; window.resize(420, 860); window.show();
        std::unique_ptr<QQuickItem> repair(load(engine, window, "FaultReportsPage.qml"));
        QVERIFY(repair);
        QTRY_COMPARE(repair->property("total").toInt(), 21);
        QCOMPARE(listProperty(repair.get(), "reports").size(), 20);
        auto* next = repair->findChild<QObject*>("repairNextPage"); QVERIFY(next);
        QTRY_VERIFY(next->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(next, "clicked"));
        QTRY_COMPARE(repair->property("currentPage").toInt(), 2);
        QTRY_COMPARE(listProperty(repair.get(), "reports").size(), 1);
        const auto oldId = repair->property("accountId").toString(); QVERIFY(!oldId.isEmpty());
        // Leave an old request in flight, then switch account while the page is
        // retained. A stale callback must not repopulate the new user's list.
        QVERIFY(QMetaObject::invokeMethod(repair.get(), "load"));
        app.logout();
        QVERIFY(listProperty(repair.get(), "reports").isEmpty());
        QVERIFY(mapProperty(repair.get(), "selected").isEmpty());
        QVERIFY(repair->property("accountId").toString().isEmpty());
        QVERIFY(app.login("13900139124")); QTRY_VERIFY(app.loggedIn());
        QTRY_VERIFY(repair->property("accountId").toString() != oldId);
        QTRY_COMPARE(repair->property("total").toInt(), 0);
        QTRY_VERIFY(repair->property("listRequest").toString().isEmpty());
        QVERIFY(listProperty(repair.get(), "reports").isEmpty());
        QCOMPARE(repair->property("currentPage").toInt(), 1);
    }

    void queueLoadsRealEmptyStateAndClearsAccountState()
    {
        QTemporaryDir files; ServerRuntime runtime;
        QVERIFY(runtime.start(files.filePath("queue.sqlite"), true, QHostAddress::LocalHost, 0));
        QTRY_VERIFY(runtime.isListening());
        QmlApp app("127.0.0.1", runtime.serverPort(), false);
        QVERIFY(app.login("13900139125")); QTRY_VERIFY(app.loggedIn()); QTRY_VERIFY(!app.checkingOrders());
        QQmlEngine engine; bind(engine, app);
        QQuickWindow window; window.resize(420, 860); window.show();
        std::unique_ptr<QQuickItem> queue(load(engine, window, "QueuePage.qml", {{"chargerId", "1"}}));
        QVERIFY(queue); QTRY_VERIFY(queue->property("loaded").toBool());
        QVERIFY(!queue->property("active").toBool()); QVERIFY(!queue->property("called").toBool());
        QVERIFY(mapProperty(queue.get(), "entry").isEmpty());
        QVERIFY(queue->property("errorMessage").toString().isEmpty());
        auto* join = queue->findChild<QObject*>("queueJoinButton"); QVERIFY(join);
        QVERIFY(QMetaObject::invokeMethod(join, "clicked"));
        QTRY_VERIFY(!queue->property("busy").toBool());
        // Station has available chargers: the real service refuses a virtual
        // queue instead of creating an unnecessary mock position.
        QVERIFY(!queue->property("errorMessage").toString().isEmpty());
        QVERIFY(!queue->property("active").toBool());
        app.logout();
        QVERIFY(mapProperty(queue.get(), "entry").isEmpty());
        QVERIFY(queue->property("retryType").toString().isEmpty());
        QVERIFY(!queue->property("busy").toBool());
    }
};

QTEST_MAIN(QmlWorkflowPagesTest)
#include "tst_qml_workflow_pages.moc"
