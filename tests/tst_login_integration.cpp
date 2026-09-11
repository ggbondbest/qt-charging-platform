#include "charging/common/model/enums.h"
#include "charging/common/protocol/protocol.h"
#include "charging_server.h"
#include "database_connection.h"
#include "network/client_connection.h"
#include "request_dispatcher.h"
#include "services/station/auth_service.h"
#include "user_repository.h"
#include "user_service.h"

#include <QHostAddress>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

class LoginServerFixture final
{
public:
    bool start(QString* errorMessage)
    {
        if (!temporaryDirectory_.isValid()) {
            *errorMessage = QStringLiteral("Unable to create the temporary directory");
            return false;
        }

        const QString databasePath =
            temporaryDirectory_.filePath(QStringLiteral("login-integration.sqlite3"));
        if (!database_.open(databasePath, false, errorMessage)) {
            return false;
        }

        repository_ = std::make_unique<charging::server::UserRepository>(database_.database());
        service_ = std::make_unique<charging::server::UserService>(repository_.get());
        dispatcher_ =
            std::make_unique<charging::server::RequestDispatcher>(service_.get());
        server_ = std::make_unique<charging::server::ChargingServer>();
        server_->setRequestDispatcher(dispatcher_.get());
        if (!server_->listen(QHostAddress::LocalHost, 0)) {
            *errorMessage = server_->errorString();
            return false;
        }
        return true;
    }

    quint16 port() const
    {
        return server_->serverPort();
    }

    QSqlDatabase database() const
    {
        return database_.database();
    }

private:
    QTemporaryDir temporaryDirectory_;
    charging::server::DatabaseConnection database_;
    std::unique_ptr<charging::server::UserRepository> repository_;
    std::unique_ptr<charging::server::UserService> service_;
    std::unique_ptr<charging::server::RequestDispatcher> dispatcher_;
    std::unique_ptr<charging::server::ChargingServer> server_;
};

} // namespace

class LoginIntegrationTest final : public QObject
{
    Q_OBJECT

private slots:
    void clientRejectsInvalidPhoneBeforeNetwork();
    void firstLoginCreatesUser();
    void repeatedLoginReturnsSameUser();
    void invalidPhoneIsRejectedByServer();
    void frozenUserIsRejected();
};

void LoginIntegrationTest::clientRejectsInvalidPhoneBeforeNetwork()
{
    charging::client::network::ClientConnection connection(
        QStringLiteral("127.0.0.1"), 1);
    charging::client::services::station::AuthService authService(&connection);

    QString failureMessage;
    connect(&authService, &charging::client::services::station::AuthService::loginFailed,
            this, [&](const QString& message) { failureMessage = message; });

    authService.login(QStringLiteral("23800138000"));

    QVERIFY(!failureMessage.isEmpty());
    QVERIFY(!authService.isLoginPending());
    QVERIFY(!connection.isConnected());
}

void LoginIntegrationTest::firstLoginCreatesUser()
{
    LoginServerFixture fixture;
    QString startError;
    QVERIFY2(fixture.start(&startError), qPrintable(startError));

    charging::client::network::ClientConnection connection(
        QStringLiteral("127.0.0.1"), fixture.port());
    charging::client::services::station::AuthService authService(&connection);

    bool completed = false;
    bool created = false;
    charging::model::User user;
    QString failureMessage;
    connect(&authService,
            &charging::client::services::station::AuthService::loginSucceeded,
            this, [&](const charging::model::User& value, bool wasCreated) {
                user = value;
                created = wasCreated;
                completed = true;
            });
    connect(&authService, &charging::client::services::station::AuthService::loginFailed,
            this, [&](const QString& message) { failureMessage = message; });

    authService.login(QStringLiteral("13912345678"));
    QTRY_VERIFY(completed || !failureMessage.isEmpty());
    QVERIFY2(failureMessage.isEmpty(), qPrintable(failureMessage));
    QVERIFY(created);
    QVERIFY(user.id > 0);
    QCOMPARE(user.phone, QStringLiteral("13912345678"));
    QCOMPARE(user.nickname, QStringLiteral("用户5678"));
    QCOMPARE(user.balanceCents, 0);
    QVERIFY(user.status == charging::model::UserStatus::Active);
    QVERIFY(user.createdAtUtc.isValid());
    QVERIFY(user.updatedAtUtc.isValid());

    QSqlQuery query(fixture.database());
    query.prepare(QStringLiteral(
        "SELECT COUNT(*), nickname, balance_cents, status FROM users WHERE phone = :phone"));
    query.bindValue(QStringLiteral(":phone"), user.phone);
    QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    QCOMPARE(query.value(1).toString(), QStringLiteral("用户5678"));
    QCOMPARE(query.value(2).toLongLong(), 0);
    QCOMPARE(query.value(3).toString(), QStringLiteral("ACTIVE"));
}

void LoginIntegrationTest::repeatedLoginReturnsSameUser()
{
    LoginServerFixture fixture;
    QString startError;
    QVERIFY2(fixture.start(&startError), qPrintable(startError));

    charging::client::network::ClientConnection connection(
        QStringLiteral("127.0.0.1"), fixture.port());
    charging::client::services::station::AuthService authService(&connection);

    QList<charging::model::User> users;
    QList<bool> creationFlags;
    QStringList responseRequestIds;
    QString failureMessage;
    connect(&authService,
            &charging::client::services::station::AuthService::loginSucceeded,
            this, [&](const charging::model::User& user, bool created) {
                users.append(user);
                creationFlags.append(created);
            });
    connect(&authService, &charging::client::services::station::AuthService::loginFailed,
            this, [&](const QString& message) { failureMessage = message; });
    connect(&connection,
            &charging::client::network::ClientConnection::responseReceived,
            this, [&](const charging::protocol::ResponseEnvelope& response) {
                if (response.type == QString::fromLatin1(
                                         charging::protocol::request_type::kUserLogin)) {
                    responseRequestIds.append(response.requestId);
                }
            });

    authService.login(QStringLiteral("13912345679"));
    QTRY_VERIFY(users.size() == 1 || !failureMessage.isEmpty());
    QVERIFY2(failureMessage.isEmpty(), qPrintable(failureMessage));

    authService.login(QStringLiteral("13912345679"));
    QTRY_VERIFY(users.size() == 2 || !failureMessage.isEmpty());
    QVERIFY2(failureMessage.isEmpty(), qPrintable(failureMessage));

    QVERIFY(creationFlags.at(0));
    QVERIFY(!creationFlags.at(1));
    QCOMPARE(users.at(0).id, users.at(1).id);
    QCOMPARE(responseRequestIds.size(), 2);
    QVERIFY(responseRequestIds.at(0) != responseRequestIds.at(1));

    QSqlQuery query(fixture.database());
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM users WHERE phone = :phone"));
    query.bindValue(QStringLiteral(":phone"), QStringLiteral("13912345679"));
    QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
}

void LoginIntegrationTest::invalidPhoneIsRejectedByServer()
{
    LoginServerFixture fixture;
    QString startError;
    QVERIFY2(fixture.start(&startError), qPrintable(startError));

    charging::client::network::ClientConnection connection(
        QStringLiteral("127.0.0.1"), fixture.port());
    charging::protocol::ResponseEnvelope receivedResponse;
    bool responseReceived = false;
    QString transportFailure;
    QString requestId;
    connect(&connection,
            &charging::client::network::ClientConnection::responseReceived,
            this, [&](const charging::protocol::ResponseEnvelope& response) {
                if (response.requestId == requestId) {
                    receivedResponse = response;
                    responseReceived = true;
                }
            });
    connect(&connection,
            &charging::client::network::ClientConnection::requestFailed,
            this, [&](const QString& failedRequestId, const QString&, const QString& message) {
                if (failedRequestId == requestId) {
                    transportFailure = message;
                }
            });

    QJsonObject data;
    data.insert(QStringLiteral("phone"), QStringLiteral("23800138000"));
    requestId = connection.sendRequest(
        QString::fromLatin1(charging::protocol::request_type::kUserLogin), data);

    QTRY_VERIFY(responseReceived || !transportFailure.isEmpty());
    QVERIFY2(transportFailure.isEmpty(), qPrintable(transportFailure));
    QVERIFY(!receivedResponse.success);
    QCOMPARE(receivedResponse.requestId, requestId);
    QCOMPARE(receivedResponse.error.code,
             QString::fromLatin1(charging::protocol::error_code::kInvalidPhone));
    QCOMPARE(receivedResponse.error.details.value(QStringLiteral("field")).toString(),
             QStringLiteral("phone"));

    QSqlQuery query(fixture.database());
    QVERIFY2(query.exec(QStringLiteral("SELECT COUNT(*) FROM users")),
             qPrintable(query.lastError().text()));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
}

void LoginIntegrationTest::frozenUserIsRejected()
{
    LoginServerFixture fixture;
    QString startError;
    QVERIFY2(fixture.start(&startError), qPrintable(startError));

    QSqlQuery insertQuery(fixture.database());
    QVERIFY2(insertQuery.exec(QStringLiteral(
                 "INSERT INTO users (phone, nickname, status) "
                 "VALUES ('13700000000', '冻结测试用户', 'FROZEN')")),
             qPrintable(insertQuery.lastError().text()));

    charging::client::network::ClientConnection connection(
        QStringLiteral("127.0.0.1"), fixture.port());
    charging::client::services::station::AuthService authService(&connection);

    QString failureMessage;
    charging::protocol::ResponseEnvelope receivedResponse;
    bool responseReceived = false;
    connect(&authService, &charging::client::services::station::AuthService::loginFailed,
            this, [&](const QString& message) { failureMessage = message; });
    connect(&connection,
            &charging::client::network::ClientConnection::responseReceived,
            this, [&](const charging::protocol::ResponseEnvelope& response) {
                if (response.type == QString::fromLatin1(
                                         charging::protocol::request_type::kUserLogin)) {
                    receivedResponse = response;
                    responseReceived = true;
                }
            });

    authService.login(QStringLiteral("13700000000"));
    QTRY_VERIFY(responseReceived && !failureMessage.isEmpty());
    QVERIFY(!receivedResponse.success);
    QCOMPARE(receivedResponse.error.code,
             QString::fromLatin1(charging::protocol::error_code::kUserFrozen));

    QSqlQuery query(fixture.database());
    QVERIFY2(query.exec(QStringLiteral(
                 "SELECT COUNT(*), status FROM users WHERE phone = '13700000000'")),
             qPrintable(query.lastError().text()));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    QCOMPARE(query.value(1).toString(), QStringLiteral("FROZEN"));
}

QTEST_GUILESS_MAIN(LoginIntegrationTest)

#include "tst_login_integration.moc"
