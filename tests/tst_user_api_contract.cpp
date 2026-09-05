#include "charging/common/model/model_json.h"
#include "charging/common/protocol/user_api_contract.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QtTest>

using namespace charging::protocol;
using namespace charging::protocol::user_api;

class UserApiContractTest final : public QObject
{
    Q_OBJECT
private slots:
    void documentedExamples();
    void defaultsAndIdentity();
    void invalidRequests_data();
    void invalidRequests();
    void boundariesAndNormalization();
    void unsupportedAction();
};

template <typename Model>
static void verifyModel(const QJsonObject& object)
{
    Model model;
    QString error;
    QVERIFY2(charging::model::fromJson(object, &model, &error), qPrintable(error));
    const QJsonObject encoded = charging::model::toJson(model);
    for (auto it = encoded.begin(); it != encoded.end(); ++it) {
        QCOMPARE(object.value(it.key()), it.value());
    }
}

void UserApiContractTest::documentedExamples()
{
    QFile file(QString::fromUtf8(USER_API_EXAMPLES_PATH));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonParseError jsonError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &jsonError);
    QCOMPARE(jsonError.error, QJsonParseError::NoError);
    QVERIFY(doc.isArray());
    const QSet<QString> expected{
        request_type::kGetStations, request_type::kGetChargers, request_type::kGetReservations,
        request_type::kGetUserInfo, request_type::kUpdateUserInfo, request_type::kRecharge,
        request_type::kGetRechargeRecords, request_type::kGetOrders};
    QSet<QString> seen;
    for (const QJsonValue& value : doc.array()) {
        const QJsonObject example = value.toObject();
        const QString type = example.value("type").toString();
        QVERIFY(!seen.contains(type));
        seen.insert(type);
        QVERIFY(example.value("requestData").isObject());
        QVERIFY(example.value("responseData").isObject());
        const QJsonObject input = example.value("requestData").toObject();
        const QJsonObject output = example.value("responseData").toObject();
        QJsonObject normalized;
        ProtocolError error;
        QVERIFY2(normalizeRequestData(type, input, &normalized, &error), qPrintable(error.message));
        QCOMPARE(normalized, input);

        RequestEnvelope request;
        request.type = type;
        request.requestId = QStringLiteral("example-1");
        request.data = input;
        RequestEnvelope parsedRequest;
        QVERIFY(parseRequestPayload(serializePayload(request), &parsedRequest, &error));
        QCOMPARE(parsedRequest.data, input);
        ResponseEnvelope parsedResponse;
        QVERIFY(parseResponsePayload(serializePayload(makeSuccessResponse(request, output)),
                                     &parsedResponse, &error));
        QCOMPARE(parsedResponse.data, output);

        QString arrayKey;
        if (type == QLatin1String(request_type::kGetStations)) arrayKey = "stations";
        if (type == QLatin1String(request_type::kGetChargers)) arrayKey = "chargers";
        if (type == QLatin1String(request_type::kGetReservations)) arrayKey = "reservations";
        if (type == QLatin1String(request_type::kGetOrders)) arrayKey = "orders";
        if (type == QLatin1String(request_type::kGetRechargeRecords)) arrayKey = "records";
        if (!arrayKey.isEmpty()) {
            QCOMPARE(output.value("page"), input.value("page"));
            QCOMPARE(output.value("pageSize"), input.value("pageSize"));
            QVERIFY(output.value(arrayKey).isArray());
            const QJsonArray items = output.value(arrayKey).toArray();
            QCOMPARE(output.value("total").toInt(), 1);
            QCOMPARE(items.size(), 1);
            const QJsonObject item = items.first().toObject();
            if (arrayKey == "stations") {
                verifyModel<charging::model::Station>(item);
                QCOMPARE(item.value("distanceMeters").toInt(), -1);
            } else if (arrayKey == "chargers") {
                verifyModel<charging::model::Charger>(item);
                QCOMPARE(item.value("stationId"), input.value("stationId"));
            } else if (arrayKey == "reservations") {
                verifyModel<charging::model::Reservation>(item);
                QCOMPARE(item.value("orderId").toString(), QStringLiteral("4"));
            } else if (arrayKey == "orders") {
                verifyModel<charging::model::Order>(item);
            } else {
                verifyModel<charging::model::RechargeRecord>(item);
            }
            if (arrayKey == "reservations" || arrayKey == "orders") {
                QVERIFY(!item.value("stationName").toString().isEmpty());
                QVERIFY(!item.value("chargerCode").toString().isEmpty());
                QCOMPARE(item.value("status"), input.value("status"));
            }
        } else if (type == QLatin1String(request_type::kRecharge)) {
            const QJsonObject record = output.value("record").toObject();
            verifyModel<charging::model::RechargeRecord>(record);
            QCOMPARE(record.value("transactionNo"), input.value("transactionNo"));
            QCOMPARE(record.value("amountCents"), input.value("amountCents"));
            QCOMPARE(record.value("status").toString(), QStringLiteral("SUCCESS"));
            QVERIFY(output.value("balanceCents").isDouble());
            QVERIFY(output.value("idempotent").isBool());
            QVERIFY(!output.value("idempotent").toBool());
            QVERIFY(!output.contains("balanceAfterCents"));
        } else {
            verifyModel<charging::model::User>(output.value("user").toObject());
        }
    }
    QCOMPARE(seen, expected);
}

void UserApiContractTest::defaultsAndIdentity()
{
    for (const char* type : {request_type::kGetStations, request_type::kGetReservations,
                             request_type::kGetOrders, request_type::kGetRechargeRecords}) {
        QJsonObject output;
        QVERIFY(normalizeRequestData(type, {{"userId", "999"}, {"futureField", true}}, &output));
        QCOMPARE(output.value("page").toInt(), kDefaultPage);
        QCOMPARE(output.value("pageSize").toInt(), kDefaultPageSize);
        QVERIFY(!output.contains("userId"));
        QVERIFY(!output.contains("futureField"));
    }
    QJsonObject output;
    QVERIFY(normalizeRequestData(request_type::kGetUserInfo, {{"userId", "999"}}, &output));
    QVERIFY(output.isEmpty()); // Not an authentication test: Session is the caller's responsibility.
}

void UserApiContractTest::invalidRequests_data()
{
    QTest::addColumn<QString>("type");
    QTest::addColumn<QJsonObject>("input");
    for (const QJsonValue& value : {QJsonValue(0), QJsonValue(-1), QJsonValue(1.5),
                                   QJsonValue("1"), QJsonValue(true), QJsonValue(QJsonValue::Null),
                                   QJsonValue(2147483648.0)}) {
        const QByteArray tag = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
        QTest::newRow(("page-" + tag).constData())
            << QString(request_type::kGetOrders) << QJsonObject{{"page", value}};
    }
    QTest::newRow("page-size-limit") << QString(request_type::kGetOrders)
        << QJsonObject{{"pageSize", 101}};
    for (const QJsonValue& id : {QJsonValue(1), QJsonValue("0"), QJsonValue("01"),
                                QJsonValue("+1"), QJsonValue(" 1"), QJsonValue("1\n"),
                                QJsonValue("9223372036854775808"), QJsonValue(QJsonValue::Null)}) {
        const QByteArray tag = QJsonDocument(QJsonArray{id}).toJson(QJsonDocument::Compact);
        QTest::newRow(("id-" + tag).constData())
            << QString(request_type::kGetChargers) << QJsonObject{{"stationId", id}};
    }
    QTest::newRow("missing-id") << QString(request_type::kGetChargers) << QJsonObject{};
    QTest::newRow("status-all") << QString(request_type::kGetOrders) << QJsonObject{{"status", "ALL"}};
    QTest::newRow("wrong-status-domain") << QString(request_type::kGetReservations)
        << QJsonObject{{"status", "CHARGING"}};
    QTest::newRow("status-null") << QString(request_type::kGetOrders)
        << QJsonObject{{"status", QJsonValue::Null}};
    QTest::newRow("keyword-long") << QString(request_type::kGetStations)
        << QJsonObject{{"keyword", QString(65, QLatin1Char('x'))}};
    QTest::newRow("empty-update") << QString(request_type::kUpdateUserInfo) << QJsonObject{};
    QTest::newRow("protected-update") << QString(request_type::kUpdateUserInfo)
        << QJsonObject{{"balanceCents", 100}};
    QTest::newRow("empty-nickname") << QString(request_type::kUpdateUserInfo)
        << QJsonObject{{"nickname", "   "}};
    QTest::newRow("nickname-long") << QString(request_type::kUpdateUserInfo)
        << QJsonObject{{"nickname", QString(33, QLatin1Char('x'))}};
    QTest::newRow("avatar-path") << QString(request_type::kUpdateUserInfo)
        << QJsonObject{{"avatarKey", "../avatar.png"}};
    QTest::newRow("partial-update-invalid") << QString(request_type::kUpdateUserInfo)
        << QJsonObject{{"nickname", "valid"}, {"avatarKey", false}};
    QTest::newRow("missing-transaction") << QString(request_type::kRecharge)
        << QJsonObject{{"amountCents", 100}};
    for (const QJsonValue& amount : {QJsonValue(0), QJsonValue(-1), QJsonValue(0.5),
                                    QJsonValue("100"), QJsonValue(10000001)}) {
        const QByteArray tag = QJsonDocument(QJsonArray{amount}).toJson(QJsonDocument::Compact);
        QTest::newRow(("amount-" + tag).constData()) << QString(request_type::kRecharge)
            << QJsonObject{{"amountCents", amount}, {"transactionNo", "txn-1"}};
    }
    for (const QString& transaction : {QString(), QStringLiteral("txn\n"),
                                       QString(41, QLatin1Char('x'))}) {
        const QByteArray tag = transaction.toUtf8().toHex();
        QTest::newRow(("transaction-" + tag).constData()) << QString(request_type::kRecharge)
            << QJsonObject{{"amountCents", 100}, {"transactionNo", transaction}};
    }
}

void UserApiContractTest::invalidRequests()
{
    QFETCH(QString, type);
    QFETCH(QJsonObject, input);
    const QJsonObject sentinel{{"unchanged", true}};
    QJsonObject output = sentinel;
    ProtocolError error;
    QVERIFY(!normalizeRequestData(type, input, &output, &error));
    QCOMPARE(output, sentinel);
    QCOMPARE(error.code, QString(error_code::kInvalidArgument));
    QVERIFY(!error.details.value("field").toString().isEmpty());
}

void UserApiContractTest::boundariesAndNormalization()
{
    QJsonObject output;
    ProtocolError error;
    error.code = "stale";
    QVERIFY(normalizeRequestData(request_type::kGetChargers,
        {{"stationId", "9223372036854775807"}, {"page", kMaximumPage}, {"pageSize", 100}},
        &output, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(output.value("stationId").toString(), QStringLiteral("9223372036854775807"));
    QCOMPARE((qint64(output.value("page").toInt()) - 1) * output.value("pageSize").toInt(),
             214748364600LL);
    QVERIFY(normalizeRequestData(request_type::kUpdateUserInfo,
        {{"nickname", " 小明 "}, {"avatarKey", ""}, {"balanceCents", 999}}, &output));
    QCOMPARE(output, QJsonObject({{"nickname", "小明"}, {"avatarKey", ""}}));
    QVERIFY(normalizeRequestData(request_type::kRecharge,
        {{"amountCents", double(kMaximumRechargeCents)}, {"transactionNo", QString(40, 'x')}},
        nullptr, nullptr));
    QVERIFY(normalizeRequestData(request_type::kRecharge,
        {{"amountCents", 1}, {"transactionNo", "x"}}, &output));
    for (const char* status : {"RESERVED", "CHARGING", "WAITING_PAYMENT", "COMPLETED", "CANCELLED", ""}) {
        QVERIFY(normalizeRequestData(request_type::kGetOrders, {{"status", status}}, &output));
    }
    for (const char* status : {"ACTIVE", "FULFILLED", "CANCELLED", "EXPIRED", ""}) {
        QVERIFY(normalizeRequestData(request_type::kGetReservations, {{"status", status}}, &output));
    }
}

void UserApiContractTest::unsupportedAction()
{
    ProtocolError error;
    QVERIFY(!normalizeRequestData(request_type::kUserLogin, {}, nullptr, &error));
    QCOMPARE(error.code, QString(error_code::kUnknownRequestType));
}

QTEST_GUILESS_MAIN(UserApiContractTest)
#include "tst_user_api_contract.moc"
