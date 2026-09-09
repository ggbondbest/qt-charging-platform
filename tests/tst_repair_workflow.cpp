#include "database_connection.h"
#include "repair_repository.h"
#include "repair_service.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QtTest>
#include <memory>

using namespace charging::server;
namespace {
bool ok(const QJsonObject& response) { return response.value("success").toBool(); }
QJsonObject data(const QJsonObject& response) { return response.value("data").toObject(); }
QJsonObject item(const QJsonObject& response) { return data(response).value("item").toObject(); }
QString code(const QJsonObject& response) { return response.value("error").toObject().value("code").toString(); }
}

class RepairWorkflowTest final : public QObject
{
    Q_OBJECT
    DatabaseConnection db_;
    std::unique_ptr<RepairRepository> repository_;
    std::unique_ptr<RepairService> service_;
    QDateTime now_;
    int operation_ = 0;

    bool sql(const QString& statement) { QSqlQuery q(db_.database()); return q.exec(statement); }
    QString scalar(const QString& statement)
    {
        QSqlQuery q(db_.database());
        return q.exec(statement) && q.next() ? q.value(0).toString() : QStringLiteral("QUERY_FAILED");
    }
    QJsonObject submit(qint64 userId = 1, const QString& charger = "1")
    {
        return service_->handle("REPAIR_SUBMIT", {{"chargerId", charger}, {"problemType", "CONNECTOR"},
                    {"description", QStringLiteral("充电枪锁扣松动，请核实")},
                    {"operationId", QStringLiteral("submit-%1").arg(++operation_)}}, userId);
    }
    QJsonObject transition(const QJsonObject& report, const QString& action, const QString& note = QStringLiteral("管理员核实并模拟处理"))
    {
        now_ = now_.addSecs(1);
        return service_->adminHandle(action, {{"id", report.value("id")}, {"note", note},
                        {"operationId", QStringLiteral("admin-%1").arg(++operation_)},
                        {"expectedUpdatedAt", report.value("updatedAt")}}, 1);
    }
    QJsonObject get(const QJsonObject& report, qint64 userId = 1)
    { return service_->handle("REPAIR_GET", {{"id", report.value("id")}}, userId); }
private slots:
    void init()
    {
        QString error;
        QVERIFY2(db_.open(":memory:", true, &error), qPrintable(error));
        QVERIFY(sql("INSERT INTO users(id,phone,nickname) VALUES(2,'13900139000','第二位用户')"));
        now_ = QDateTime::fromString("2026-09-09T10:00:00.000Z", Qt::ISODateWithMs);
        repository_ = std::make_unique<RepairRepository>(db_.database());
        service_ = std::make_unique<RepairService>(repository_.get(), [this] { return now_; });
        operation_ = 0;
    }
    void cleanup() { service_.reset(); repository_.reset(); db_.close(); }

    void submissionDoesNotDisableAndIsDurablyIdempotent()
    {
        const QJsonObject p{{"chargerId", "1"}, {"problemType", "SCREEN"},
                            {"description", QStringLiteral("屏幕没有反应")}, {"operationId", "same-submit"}};
        const auto response = service_->handle("REPAIR_SUBMIT", p, 1);
        QVERIFY(ok(response));
        QCOMPARE(item(response).value("status").toString(), QString("SUBMITTED"));
        QCOMPARE(scalar("SELECT status FROM chargers WHERE id=1"), QString("AVAILABLE"));
        QVERIFY(!item(response).value("maintenance").toBool());
        QVERIFY(!item(response).contains("user"));
        QCOMPARE(item(response).value("timeline").toArray().size(), 1);
        QCOMPARE(code(submit()), QString("ALREADY_EXISTS"));
        service_.reset(); repository_.reset();
        repository_ = std::make_unique<RepairRepository>(db_.database());
        service_ = std::make_unique<RepairService>(repository_.get(), [this] { return now_; });
        const auto replay = service_->handle("REPAIR_SUBMIT", p, 1);
        QVERIFY(ok(replay)); QVERIFY(data(replay).value("idempotent").toBool());
        QCOMPARE(item(replay), item(response));
        auto changed = p; changed.insert("description", "不同的问题");
        QCOMPARE(code(service_->handle("REPAIR_SUBMIT", changed, 1)), QString("CONFLICT"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM repair_reports"), QString("1"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM repair_timeline"), QString("1"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM operation_logs WHERE action='REPAIR_SUBMIT'"), QString("1"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM notifications WHERE type='REPAIR_UPDATED'"), QString("0"));
    }

    void authenticationOwnershipAndStrictInput()
    {
        QCOMPARE(code(service_->handle("REPAIR_GET_MINE", {}, 0)), QString("UNAUTHORIZED"));
        QCOMPARE(code(service_->adminHandle("repair_reports.list", {}, 999)), QString("UNAUTHORIZED"));
        const auto mine = item(submit());
        QVERIFY(!mine.isEmpty());
        QCOMPARE(code(get(mine, 2)), QString("NOT_FOUND"));
        QCOMPARE(data(service_->handle("REPAIR_GET_MINE", {}, 2)).value("total").toInt(), 0);
        QVERIFY(sql("UPDATE users SET status='FROZEN' WHERE id=1"));
        QCOMPARE(code(get(mine)), QString("USER_FROZEN"));
        QVERIFY(sql("UPDATE users SET status='ACTIVE' WHERE id=1"));
        QVERIFY(sql("UPDATE admins SET status='DISABLED' WHERE id=1"));
        QCOMPARE(code(transition(mine, "repair_reports.accept")), QString("UNAUTHORIZED"));
        for (const auto& p : {QJsonObject{{"id", 1}}, QJsonObject{{"id", "01"}},
                             QJsonObject{{"id", "1"}, {"userId", "2"}}})
            QCOMPARE(code(service_->handle("REPAIR_GET", p, 1)), QString("INVALID_ARGUMENT"));
        for (const auto& p : {QJsonObject{{"page", 0}}, QJsonObject{{"pageSize", 101}},
                             QJsonObject{{"status", "FAULT"}}, QJsonObject{{"userId", "2"}}})
            QCOMPARE(code(service_->handle("REPAIR_GET_MINE", p, 1)), QString("INVALID_ARGUMENT"));
        QJsonObject p{{"chargerId", "2"}, {"problemType", "OTHER"}, {"description", "issue"}, {"operationId", "valid"}};
        p.insert("description", QString(201, 'x'));
        QCOMPARE(code(service_->handle("REPAIR_SUBMIT", p, 1)), QString("INVALID_ARGUMENT"));
        p.insert("description", "  ");
        QCOMPARE(code(service_->handle("REPAIR_SUBMIT", p, 1)), QString("INVALID_ARGUMENT"));
        p.insert("description", "issue"); p.insert("problemType", "DROP_TABLE");
        QCOMPARE(code(service_->handle("REPAIR_SUBMIT", p, 1)), QString("INVALID_ARGUMENT"));
        p.insert("problemType", "OTHER"); p.insert("operationId", "invalid whitespace");
        QCOMPARE(code(service_->handle("REPAIR_SUBMIT", p, 1)), QString("INVALID_ARGUMENT"));
    }

    void progressRecordsTrueTimelineAndNotifications()
    {
        auto current = item(submit());
        const auto submittedAt = now_.toString(Qt::ISODateWithMs);
        QCOMPARE(code(transition(current, "repair_reports.resolve")), QString("INVALID_STATE_TRANSITION"));
        auto accepted = transition(current, "repair_reports.accept", "已核实锁扣损坏，暂停新预约");
        QVERIFY(ok(accepted)); current = item(accepted);
        QCOMPARE(current.value("status").toString(), QString("ACCEPTED"));
        QCOMPARE(scalar("SELECT status FROM chargers WHERE id=1"), QString("OFFLINE"));
        QVERIFY(current.value("maintenance").toBool());
        auto processing = transition(current, "repair_reports.start", "模拟更换锁扣并检查连接");
        QVERIFY(ok(processing)); current = item(processing);
        auto resolved = transition(current, "repair_reports.resolve", "模拟测试通过，维修完成");
        QVERIFY(ok(resolved)); current = item(resolved);
        QCOMPARE(current.value("status").toString(), QString("RESOLVED"));
        QCOMPARE(scalar("SELECT status FROM chargers WHERE id=1"), QString("AVAILABLE"));
        QVERIFY(!current.value("maintenance").toBool());
        QVERIFY(data(resolved).value("chargerAvailable").toBool());
        QVERIFY(data(resolved).value("simulated").toBool());
        QCOMPARE(current.value("resolvedAt").toString(), now_.toString(Qt::ISODateWithMs));
        const auto timeline = current.value("timeline").toArray();
        QCOMPARE(timeline.size(), 4);
        QCOMPARE(timeline[0].toObject().value("createdAt").toString(), submittedAt);
        const QStringList states{"SUBMITTED", "ACCEPTED", "PROCESSING", "RESOLVED"};
        for (int i = 0; i < states.size(); ++i) QCOMPARE(timeline[i].toObject().value("status").toString(), states[i]);
        QCOMPARE(timeline[3].toObject().value("note").toString(), QString("模拟测试通过，维修完成"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM notifications WHERE user_id=1 AND type='REPAIR_UPDATED'"), QString("3"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM notifications WHERE user_id=2"), QString("0"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM operation_logs WHERE action LIKE 'repair_reports.%'"), QString("3"));
        QVERIFY(ok(submit())); // A new issue may be reported after the old one is resolved.
    }

    void busyChargersCannotBeTakenFromUsersOrCalledQueue()
    {
        auto submitted = item(submit());
        for (const auto& status : {"RESERVED", "CHARGING"}) {
            QVERIFY(sql(QString("UPDATE chargers SET status='%1' WHERE id=1").arg(status)));
            QCOMPARE(code(transition(submitted, "repair_reports.accept")), QString("RESOURCE_BUSY"));
            QCOMPARE(item(get(submitted)).value("status").toString(), QString("SUBMITTED"));
        }
        QVERIFY(sql("UPDATE chargers SET status='AVAILABLE' WHERE id=1"));
        QVERIFY(sql("INSERT INTO reservations(user_id,charger_id,reserved_at,expires_at) "
                    "VALUES(2,1,'2026-09-09T10:00:00.000Z','2026-09-09T10:15:00.000Z')"));
        QCOMPARE(code(transition(submitted, "repair_reports.accept")), QString("RESOURCE_BUSY"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM notifications"), QString("0"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM repair_timeline"), QString("1"));
    }

    void multipleReportsDoNotReleaseOtherMaintenanceHolds()
    {
        auto one = item(submit(1));
        auto two = item(submit(2));
        one = item(transition(one, "repair_reports.accept")); QVERIFY(!one.isEmpty());
        two = item(transition(two, "repair_reports.accept")); QVERIFY(!two.isEmpty());
        one = item(transition(one, "repair_reports.start")); QVERIFY(!one.isEmpty());
        two = item(transition(two, "repair_reports.start")); QVERIFY(!two.isEmpty());
        const auto resultOne = transition(one, "repair_reports.resolve");
        QVERIFY(ok(resultOne));
        QCOMPARE(scalar("SELECT status FROM chargers WHERE id=1"), QString("OFFLINE"));
        QVERIFY(!data(resultOne).value("chargerAvailable").toBool());
        QVERIFY(item(resultOne).value("maintenance").toBool());
        QVERIFY(scalar("SELECT body FROM notifications WHERE user_id=1 ORDER BY id DESC LIMIT 1").contains("暂不可预约"));
        const auto resultTwo = transition(two, "repair_reports.resolve");
        QVERIFY(ok(resultTwo));
        QCOMPARE(scalar("SELECT status FROM chargers WHERE id=1"), QString("AVAILABLE"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM notifications WHERE user_id=1"), QString("3"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM notifications WHERE user_id=2"), QString("3"));
        QCOMPARE(item(get(item(resultOne), 1)).value("status").toString(), QString("RESOLVED"));
        QCOMPARE(item(get(item(resultTwo), 2)).value("status").toString(), QString("RESOLVED"));
    }

    void independentFaultOrClosedStationPreventsRelease_data()
    {
        QTest::addColumn<bool>("exception");
        QTest::newRow("active independent fault") << true;
        QTest::newRow("closed station") << false;
    }
    void independentFaultOrClosedStationPreventsRelease()
    {
        QFETCH(bool, exception);
        auto current = item(submit());
        current = item(transition(current, "repair_reports.accept"));
        current = item(transition(current, "repair_reports.start"));
        QVERIFY(!current.isEmpty());
        if (exception) {
            QVERIFY(sql("INSERT INTO charger_exceptions(charger_id,code,severity,safe_summary,status,occurred_at,updated_at) "
                        "VALUES(1,'SIMULATED_FAULT','CRITICAL','另一个独立故障','ACTIVE',"
                        "'2026-09-09T10:00:02.000Z','2026-09-09T10:00:02.000Z')"));
        } else QVERIFY(sql("UPDATE stations SET status='INACTIVE' WHERE id=1"));
        auto resolved = transition(current, "repair_reports.resolve");
        QVERIFY(ok(resolved));
        QVERIFY(!data(resolved).value("chargerAvailable").toBool());
        QCOMPARE(scalar("SELECT status FROM chargers WHERE id=1"), QString("OFFLINE"));
    }

    void writeReplayConflictAndConcurrentVersion()
    {
        auto current = item(submit());
        QJsonObject p{{"id", current.value("id")}, {"operationId", "same-admin-op"},
                      {"expectedUpdatedAt", current.value("updatedAt")}, {"note", "确认故障，进入维护"}};
        // The service clock can have the same millisecond as submission; the
        // concurrency version still advances while event time stays truthful.
        const auto accepted = service_->adminHandle("repair_reports.accept", p, 1);
        QVERIFY(ok(accepted));
        QVERIFY(item(accepted).value("updatedAt").toString() > current.value("updatedAt").toString());
        QCOMPARE(item(accepted).value("timeline").toArray()[1].toObject().value("createdAt").toString(), now_.toString(Qt::ISODateWithMs));
        auto duplicate = service_->adminHandle("repair_reports.accept", p, 1);
        QVERIFY(ok(duplicate)); QVERIFY(data(duplicate).value("idempotent").toBool());
        QCOMPARE(item(duplicate), item(accepted));
        auto changed = p; changed.insert("note", "不同说明");
        QCOMPARE(code(service_->adminHandle("repair_reports.accept", changed, 1)), QString("CONFLICT"));
        auto stale = p; stale.insert("operationId", "next-action");
        QCOMPARE(code(service_->adminHandle("repair_reports.start", stale, 1)), QString("CONFLICT"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM repair_timeline"), QString("2"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM notifications"), QString("1"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM operation_logs WHERE action='repair_reports.accept'"), QString("1"));
    }

    void auditFailureRollsBackUserSubmission()
    {
        QVERIFY(sql("CREATE TRIGGER fail_repair_audit BEFORE INSERT ON operation_logs "
                    "BEGIN SELECT RAISE(ABORT,'sensitive-database-detail'); END"));
        const auto failed = submit();
        QCOMPARE(code(failed), QString("DATABASE_ERROR"));
        QVERIFY(!QJsonDocument(failed).toJson().contains("sensitive-database-detail"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM repair_reports"), QString("0"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM repair_timeline"), QString("0"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM repair_operations"), QString("0"));
        QCOMPARE(scalar("SELECT status FROM chargers WHERE id=1"), QString("AVAILABLE"));
    }

    void auditAndNotificationFailureRollBackAdminMutation_data()
    {
        QTest::addColumn<QString>("table");
        QTest::newRow("audit") << QString("operation_logs");
        QTest::newRow("notification") << QString("notifications");
    }
    void auditAndNotificationFailureRollBackAdminMutation()
    {
        QFETCH(QString, table);
        const auto current = item(submit());
        QVERIFY(sql(QString("CREATE TRIGGER fail_admin_write BEFORE INSERT ON %1 BEGIN SELECT RAISE(ABORT,'failure'); END").arg(table)));
        QCOMPARE(code(transition(current, "repair_reports.accept")), QString("DATABASE_ERROR"));
        QCOMPARE(item(get(current)), current);
        QCOMPARE(scalar("SELECT status FROM chargers WHERE id=1"), QString("AVAILABLE"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM repair_timeline"), QString("1"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM repair_operations"), QString("1"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM notifications"), QString("0"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM operation_logs WHERE action='repair_reports.accept'"), QString("0"));
    }

    void adminListPaginationAndSafeUserSummary()
    {
        QVERIFY(ok(submit(1, "1"))); QVERIFY(ok(submit(2, "1"))); QVERIFY(ok(submit(1, "4")));
        const auto first = service_->adminHandle("repair_reports.list", {{"stationId", "1"}, {"status", "SUBMITTED"},
                                         {"page", 1}, {"pageSize", 1}, {"keyword", "001"}}, 1);
        QVERIFY(ok(first)); QCOMPARE(data(first).value("total").toInt(), 2);
        QCOMPARE(data(first).value("items").toArray().size(), 1);
        const auto row = data(first).value("items").toArray()[0].toObject();
        QCOMPARE(row.value("user").toObject().value("phone").toString(), QString("139****9000"));
        QVERIFY(!QJsonDocument(first).toJson().contains("13900139000"));
        QVERIFY(!row.contains("timeline"));
        const auto detail = service_->adminHandle("repair_reports.get", {{"id", row.value("id")}}, 1);
        QVERIFY(ok(detail)); QCOMPARE(item(detail).value("timeline").toArray().size(), 1);
        QCOMPARE(data(service_->handle("REPAIR_GET_MINE", {}, 1)).value("total").toInt(), 2);
        QCOMPARE(data(service_->handle("REPAIR_GET_MINE", {}, 2)).value("total").toInt(), 1);
    }
};

QTEST_GUILESS_MAIN(RepairWorkflowTest)
#include "tst_repair_workflow.moc"
