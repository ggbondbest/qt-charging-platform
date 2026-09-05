#include "admin_request_gateway.h"
#include "database_connection.h"
#include "server_runtime.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace charging::server;
namespace {
QJsonObject response(const QSignalSpy& spy, int index = 0)
{
    return spy.at(index).at(1).toJsonObject();
}
QString code(const QJsonObject& r)
{
    return r.value("error").toObject().value("code").toString();
}
QJsonObject login()
{
    return {{"username", "admin"}, {"password", "123456"}};
}
} // namespace
class AdminGatewayTest final : public QObject
{
    Q_OBJECT
private slots:
    void authenticationOwnershipSupersessionAndStop()
    {
        QTemporaryDir dir;
        ServerRuntime runtime;
        QSignalSpy ready(&runtime, &ServerRuntime::listening);
        QVERIFY(runtime.start(dir.filePath("admin.sqlite"), true, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(ready.size(), 1);
        AdminRequestGateway gateway(&runtime);
        QObject page;
        AdminRequestGateway otherWindow(&runtime); // must not revoke this window's login
        QSignalSpy done(&gateway, &AdminRequestGateway::finished);
        const auto id = gateway.request("auth.login", login(), &page, "login");
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.first().first().toString(), id);
        QVERIFY(response(done).value("success").toBool());
        QVERIFY(gateway.isAuthenticated());
        QVERIFY(!response(done).value("data").toObject().contains("sessionToken"));
        done.clear();
        gateway.request("stations.list", {}, &page, "refresh");
        const auto latest = gateway.request("stations.list", {{"pageSize", 1}}, &page, "refresh");
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.first().first().toString(), latest);
        auto* deleted = new QObject;
        gateway.request("dashboard.get", {}, deleted);
        delete deleted;
        QTest::qWait(50);
        QCOMPARE(done.size(), 1);
        gateway.logout();
        QVERIFY(!gateway.isAuthenticated());
        done.clear();
        gateway.request("users.list", {}, &page);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(code(response(done)), QString("UNAUTHORIZED"));
        done.clear();
        gateway.request("auth.login", login(), &page);
        gateway.logout();
        QTest::qWait(100);
        QVERIFY(!gateway.isAuthenticated());
        QCOMPARE(done.size(), 0);
        QSignalSpy stopped(&runtime, &ServerRuntime::stopped);
        runtime.stop();
        QTRY_COMPARE(stopped.size(), 1);
        gateway.request("dashboard.get", {}, &page);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(code(response(done)), QString("UNAVAILABLE"));
    }
    void timeoutDoesNotBlockGuiOrDeliverLateLogin()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("locked.sqlite");
        ServerRuntime runtime;
        QSignalSpy ready(&runtime, &ServerRuntime::listening);
        QVERIFY(runtime.start(path, true, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(ready.size(), 1);
        DatabaseConnection lock;
        QVERIFY(lock.open(path, false));
        QSqlQuery query(lock.database());
        QVERIFY(query.exec("BEGIN IMMEDIATE"));
        AdminRequestGateway gateway(&runtime);
        QObject page;
        QSignalSpy done(&gateway, &AdminRequestGateway::finished);
        int ticks = 0;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start();
        gateway.request("auth.login", login(), &page, "login", 30);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(code(response(done)), QString("TIMEOUT"));
        QVERIFY(ticks > 0);
        QVERIFY(query.exec("COMMIT"));
        QTest::qWait(150);
        QCOMPARE(done.size(), 1);
        QVERIFY(!gateway.isAuthenticated());
        done.clear();
        gateway.request("auth.login", login(), &page, "login");
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(gateway.isAuthenticated());
        runtime.stop();
    }
    void timedOutWriteCanBeReplayedWithoutRepeatingMutation()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("write-timeout.sqlite");
        ServerRuntime runtime;
        QSignalSpy ready(&runtime, &ServerRuntime::listening);
        QVERIFY(runtime.start(path, true, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(ready.size(), 1);
        AdminRequestGateway gateway(&runtime);
        QObject page;
        QSignalSpy done(&gateway, &AdminRequestGateway::finished);
        gateway.request("auth.login", login(), &page);
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(gateway.isAuthenticated());
        done.clear();
        gateway.request("users.get", {{"id", "1"}}, &page);
        QTRY_COMPARE(done.size(), 1);
        const auto user = response(done).value("data").toObject().value("item").toObject();
        const QJsonObject command{{"id", "1"},
                                  {"operationId", "freeze-timeout"},
                                  {"expectedUpdatedAt", user.value("updatedAt")},
                                  {"status", "FROZEN"}};
        DatabaseConnection lock;
        QVERIFY(lock.open(path, false));
        QSqlQuery query(lock.database());
        QVERIFY(query.exec("BEGIN IMMEDIATE"));
        done.clear();
        gateway.request("user.status", command, &page, {}, 30);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(code(response(done)), QString("TIMEOUT"));
        QVERIFY(query.exec("COMMIT"));
        QTest::qWait(100);
        QCOMPARE(done.size(), 1); // late response did not touch the page
        done.clear();
        gateway.request("user.status", command, &page);
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(response(done).value("success").toBool());
        // Whether the first request started before its deadline or not, the
        // original operation identifier must result in exactly one audit/write.
        QVERIFY(query.exec("SELECT COUNT(*) FROM operation_logs WHERE target_type='ADMIN_COMMAND' "
                           "AND target_id='freeze-timeout'"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        runtime.stop();
    }
};
QTEST_GUILESS_MAIN(AdminGatewayTest)
#include "tst_admin_gateway.moc"
