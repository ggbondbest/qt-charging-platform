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
    void settlementRewardPointsRule();
    void levelRewardPointsRule();
    void unsupportedAction();
};

// 结算返积分规则单点（2026-09-09）：floor(amount/100)，负/零一律 0。
// 服务端事务与 mock 都调此函数，钉住它即钉住两端一致（TODO(contract) 比例）。
void UserApiContractTest::settlementRewardPointsRule()
{
    QCOMPARE(settlementRewardPoints(0), 0);
    QCOMPARE(settlementRewardPoints(99), 0);        // 不足 1 元不返
    QCOMPARE(settlementRewardPoints(100), 1);
    QCOMPARE(settlementRewardPoints(199), 1);       // floor 向下取整
    QCOMPARE(settlementRewardPoints(549), 5);
    QCOMPARE(settlementRewardPoints(5000), 50);
    QCOMPARE(settlementRewardPoints(-100), 0);
}

// 升级礼包单点表（2026-09-09 需求批）：表外档位一律 0（normalize 已挡 2..5，
// 这里是兜底防线——repo 侧 gift<=0 直接 Invalid）。四档金额互不相同是
// (user_id,'LEVEL_GIFT',amount) 流水去重幂等成立的前提，一并钉住。
// 与客户端经验引擎 kTiers 的一致性钉在 tst_qml_client_pages（跨层镜像）。
void UserApiContractTest::levelRewardPointsRule()
{
    QCOMPARE(levelRewardPoints(0), 0);
    QCOMPARE(levelRewardPoints(1), 0);        // 青铜初始档无礼包
    QCOMPARE(levelRewardPoints(2), 100);      // 白银
    QCOMPARE(levelRewardPoints(3), 150);      // 黄金
    QCOMPARE(levelRewardPoints(4), 200);      // 铂金
    QCOMPARE(levelRewardPoints(5), 300);      // 黑金
    QCOMPARE(levelRewardPoints(6), 0);
    QCOMPARE(levelRewardPoints(-3), 0);
    const QSet<qint64> distinct{levelRewardPoints(2), levelRewardPoints(3),
                                levelRewardPoints(4), levelRewardPoints(5)};
    QCOMPARE(distinct.size(), 4);             // 幂等键可辨性
}

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
        request_type::kGetRechargeRecords, request_type::kGetOrders, request_type::kGetUserStats,
        request_type::kGetCoupons, request_type::kGetNotifications,
        request_type::kCheckIn, request_type::kGetPoints, request_type::kCreditLevelReward,
        request_type::kSubmitChargerRating, request_type::kGetMyRatings};
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
        } else if (type == QLatin1String(request_type::kGetUserStats)) {
            const QJsonArray rows = output.value("months").toArray();
            QCOMPARE(rows.size(), 1);
            const QJsonObject row = rows.first().toObject();
            QCOMPARE(row.value("monthKey").toString().size(), 7);
            for (const char* key : {"orderCount", "energyWh", "amountCents",
                                    "durationSeconds", "co2Grams"}) {
                QVERIFY(row.value(QLatin1String(key)).isDouble());
            }
            // 碳排公式与服务端单点对拍：co2 = round(energyWh × 0.5568)。
            QCOMPARE(row.value("co2Grams").toDouble(),
                     qRound64(row.value("energyWh").toDouble() * 0.5568));
        } else if (type == QLatin1String(request_type::kGetCoupons)) {
            QCOMPARE(output.value("page"), input.value("page"));
            QCOMPARE(output.value("pageSize"), input.value("pageSize"));
            QCOMPARE(output.value("total").toInt(), 1);
            const QJsonArray items = output.value("coupons").toArray();
            QCOMPARE(items.size(), 1);
            const QJsonObject item = items.first().toObject();
            QVERIFY(item.value("kind").toString() == QLatin1String("cash")
                    || item.value("kind").toString() == QLatin1String("discount"));
            QVERIFY(item.value("status").toString() == QLatin1String("available")
                    || item.value("status").toString() == QLatin1String("used")
                    || item.value("status").toString() == QLatin1String("expired"));
            QVERIFY(item.value("expiresAtUtc").isDouble());   // 页面契约：epoch ms 数字
            QVERIFY(!item.value("condition").toString().isEmpty());
        } else if (type == QLatin1String(request_type::kGetNotifications)) {
            QCOMPARE(output.value("page"), input.value("page"));
            QCOMPARE(output.value("pageSize"), input.value("pageSize"));
            QCOMPARE(output.value("total").toInt(), 1);
            const QJsonArray items = output.value("notifications").toArray();
            QCOMPARE(items.size(), 1);
            const QJsonObject item = items.first().toObject();
            QVERIFY(item.value("type").toString() == QLatin1String("charging_stopped")
                    || item.value("type").toString() == QLatin1String("order_paid"));
            QVERIFY(!item.value("title").toString().isEmpty());
            QVERIFY(!item.value("body").toString().isEmpty());
            QVERIFY(item.value("createdAtUtc").isString());   // 页面契约 key（非 createdAt）
            QVERIFY(!item.contains("userId"));                // 内部身份列不回显
            QVERIFY(!item.contains("readAt"));
        } else if (type == QLatin1String(request_type::kCheckIn)) {
            // CHECK_IN 无参：input 必须显式 {}（normalize 对空 result，注入即挂）。
            QVERIFY(input.isEmpty());
            QCOMPARE(output.value("day").toString().size(), 10);   // "YYYY-MM-DD"
            QVERIFY(output.value("points").isDouble());
            QVERIFY(output.value("gained").isDouble());
            QVERIFY(output.value("alreadyCheckedIn").isBool());
        } else if (type == QLatin1String(request_type::kGetPoints)) {
            QCOMPARE(output.value("page"), input.value("page"));
            QCOMPARE(output.value("pageSize"), input.value("pageSize"));
            QVERIFY(output.value("points").isDouble());   // SUM 总分与流水同响应
            const QJsonArray items = output.value("entries").toArray();
            QCOMPARE(items.size(), 1);
            QCOMPARE(output.value("total").toInt(), 1);
            const QJsonObject item = items.first().toObject();
            QVERIFY(item.value("amount").isDouble());
            QVERIFY(!item.value("reason").toString().isEmpty());
            QVERIFY(item.value("createdAtUtc").isString());
            QVERIFY(!item.contains("userId"));
        } else if (type == QLatin1String(request_type::kCreditLevelReward)) {
            // 写型响应（2026-09-09 需求批）：{points, gained, alreadyCredited}；
            // 请求只带 level（金额服务端推导），示例=首次入账白银档。
            QCOMPARE(input.value("level").toInt(), 2);
            QVERIFY(output.value("points").isDouble());
            QCOMPARE(output.value("gained").toDouble(),
                     static_cast<double>(levelRewardPoints(2)));   // 与服务端单点对拍
            QVERIFY(output.value("alreadyCredited").isBool());
            QVERIFY(!output.value("alreadyCredited").toBool());
        } else if (type == QLatin1String(request_type::kSubmitChargerRating)) {
            // 写型响应：rating 行 = 服务端回读行（重放=首评原值），alreadyRated 旗标。
            const QJsonObject row = output.value("rating").toObject();
            QVERIFY(row.value("id").isString());
            QCOMPARE(row.value("orderId"), input.value("orderId"));
            QVERIFY(row.value("chargerId").isString());
            QVERIFY(!row.value("chargerCode").toString().isEmpty());
            QVERIFY(!row.value("stationName").toString().isEmpty());
            QCOMPARE(row.value("rating"), input.value("rating"));
            QVERIFY(row.value("createdAtUtc").isString());
            QVERIFY(!row.contains("userId"));
            QVERIFY(output.value("alreadyRated").isBool());
            QVERIFY(!output.value("alreadyRated").toBool());   // 示例=首次提交
        } else if (type == QLatin1String(request_type::kGetMyRatings)) {
            QCOMPARE(output.value("page"), input.value("page"));
            QCOMPARE(output.value("pageSize"), input.value("pageSize"));
            QCOMPARE(output.value("total").toInt(), 1);
            const QJsonArray items = output.value("ratings").toArray();
            QCOMPARE(items.size(), 1);
            const QJsonObject item = items.first().toObject();
            QVERIFY(item.value("id").isString());
            QVERIFY(item.value("orderId").isString());
            QVERIFY(item.value("rating").isDouble());
            QVERIFY(item.value("createdAtUtc").isString());
            QVERIFY(!item.contains("userId"));
        } else {
            verifyModel<charging::model::User>(output.value("user").toObject());
        }
    }
    QCOMPARE(seen, expected);
}

void UserApiContractTest::defaultsAndIdentity()
{
    for (const char* type : {request_type::kGetStations, request_type::kGetReservations,
                             request_type::kGetOrders, request_type::kGetRechargeRecords,
                             request_type::kGetCoupons, request_type::kGetNotifications,
                             request_type::kGetPoints, request_type::kGetMyRatings}) {
        QJsonObject output;
        QVERIFY(normalizeRequestData(type, {{"userId", "999"}, {"futureField", true}}, &output));
        QCOMPARE(output.value("page").toInt(), kDefaultPage);
        QCOMPARE(output.value("pageSize").toInt(), kDefaultPageSize);
        QVERIFY(!output.contains("userId"));
        QVERIFY(!output.contains("futureField"));
    }
    QJsonObject output;
    QVERIFY(normalizeRequestData(request_type::kCheckIn, {{"userId", "999"}, {"futureField", true}},
                                 &output));
    QVERIFY(output.isEmpty());   // CHECK_IN 无参：身份来自 Session，未知键丢弃（同 GET_USER_INFO）。
    QVERIFY(normalizeRequestData(request_type::kGetUserInfo, {{"userId", "999"}}, &output));
    QVERIFY(output.isEmpty()); // Not an authentication test: Session is the caller's responsibility.
    QVERIFY(normalizeRequestData(request_type::kGetUserStats, {{"userId", "999"}, {"page", 2}},
                                 &output));
    // stats is unpaged; page is discarded; period 缺省注入 "month"（=冻结行为）。
    QCOMPARE(output, (QJsonObject{{QStringLiteral("months"), 6},
                                  {QStringLiteral("period"), QStringLiteral("month")}}));
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
    for (const QJsonValue& months : {QJsonValue(0), QJsonValue(-1), QJsonValue(1.5),
                                      QJsonValue("6"), QJsonValue(13)}) {
        const QByteArray tag = QJsonDocument(QJsonArray{months}).toJson(QJsonDocument::Compact);
        QTest::newRow(("stats-months-" + tag).constData())
            << QString(request_type::kGetUserStats) << QJsonObject{{"months", months}};
    }
    for (const QJsonValue& period : {QJsonValue("WEEK"), QJsonValue("daily"),
                                     QJsonValue(1), QJsonValue(QJsonValue::Null)}) {
        const QByteArray tag = QJsonDocument(QJsonArray{period}).toJson(QJsonDocument::Compact);
        QTest::newRow(("stats-period-" + tag).constData())
            << QString(request_type::kGetUserStats) << QJsonObject{{"period", period}};
    }
    for (const QJsonValue& status : {QJsonValue("AVAILABLE"), QJsonValue("pending"),
                                     QJsonValue(1), QJsonValue(QJsonValue::Null)}) {
        const QByteArray tag = QJsonDocument(QJsonArray{status}).toJson(QJsonDocument::Compact);
        QTest::newRow(("coupon-status-" + tag).constData())
            << QString(request_type::kGetCoupons) << QJsonObject{{"status", status}};
    }
    QTest::newRow("notifications-page-size-limit") << QString(request_type::kGetNotifications)
        << QJsonObject{{"pageSize", 101}};
    QTest::newRow("points-page-size-limit") << QString(request_type::kGetPoints)
        << QJsonObject{{"pageSize", 101}};
    QTest::newRow("points-page-zero") << QString(request_type::kGetPoints)
        << QJsonObject{{"page", 0}};
    // 批次E：SUBMIT_CHARGER_RATING 写型校验（orderId 正十进制串 + rating 1..5 +
    // comment ≤140 trimmed）。
    QTest::newRow("rating-missing-order") << QString(request_type::kSubmitChargerRating)
        << QJsonObject{{"rating", 5}, {"comment", ""}};
    QTest::newRow("rating-order-zero") << QString(request_type::kSubmitChargerRating)
        << QJsonObject{{"orderId", "0"}, {"rating", 5}, {"comment", ""}};
    for (const QJsonValue& rating : {QJsonValue(0), QJsonValue(6), QJsonValue("5"),
                                     QJsonValue(1.5), QJsonValue(QJsonValue::Null)}) {
        const QByteArray tag = QJsonDocument(QJsonArray{rating}).toJson(QJsonDocument::Compact);
        QTest::newRow(("rating-value-" + tag).constData())
            << QString(request_type::kSubmitChargerRating)
            << QJsonObject{{"orderId", "7"}, {"rating", rating}, {"comment", ""}};
    }
    QTest::newRow("rating-comment-long") << QString(request_type::kSubmitChargerRating)
        << QJsonObject{{"orderId", "7"}, {"rating", 5}, {"comment", QString(141, QLatin1Char('x'))}};
    QTest::newRow("myratings-page-size-limit") << QString(request_type::kGetMyRatings)
        << QJsonObject{{"pageSize", 101}};
    // 2026-09-09 需求批：CREDIT_LEVEL_REWARD 只带 level（金额非入参，客户端
    // 自报无效），2..5 之外的整数、非整数与缺省全拒。
    QTest::newRow("credit-level-missing") << QString(request_type::kCreditLevelReward)
        << QJsonObject{};
    for (const QJsonValue& level : {QJsonValue(0), QJsonValue(1), QJsonValue(6),
                                    QJsonValue("2"), QJsonValue(2.5),
                                    QJsonValue(QJsonValue::Null)}) {
        const QByteArray tag = QJsonDocument(QJsonArray{level}).toJson(QJsonDocument::Compact);
        QTest::newRow(("credit-level-" + tag).constData())
            << QString(request_type::kCreditLevelReward) << QJsonObject{{"level", level}};
    }
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
    for (const int months : {1, 6, kMaximumStatsMonths}) {
        QVERIFY(normalizeRequestData(request_type::kGetUserStats, {{"months", months}}, &output));
        QCOMPARE(output.value("months").toInt(), months);
    }
    for (const char* period : {"week", "month", "year"}) {
        QVERIFY(normalizeRequestData(request_type::kGetUserStats,
                                     {{"period", period}}, &output));
        QCOMPARE(output.value("period").toString(), QLatin1String(period));
        QCOMPARE(output.value("months").toInt(), 6);   // 缺省窗口随同注入
    }
    for (const char* status : {"available", "used", "expired", ""}) {
        QVERIFY(normalizeRequestData(request_type::kGetCoupons, {{"status", status}}, &output));
        QCOMPARE(output.value("status").toString(), QLatin1String(status));
    }
    for (const int stars : {1, 3, 5}) {   // 批次E：星级全域
        QVERIFY(normalizeRequestData(request_type::kSubmitChargerRating,
            {{"orderId", "7"}, {"rating", stars}, {"comment", ""}}, &output));
        QCOMPARE(output.value("rating").toInt(), stars);
    }
    QVERIFY(normalizeRequestData(request_type::kSubmitChargerRating,   // comment 缺省注入空串
        {{"orderId", "7"}, {"rating", 4}}, &output));
    QCOMPARE(output.value("comment").toString(), QString());
    QVERIFY(normalizeRequestData(request_type::kSubmitChargerRating,   // 提交前 trim
        {{"orderId", "7"}, {"rating", 4}, {"comment", "  很快  "}}, &output));
    QCOMPARE(output.value("comment").toString(), QStringLiteral("很快"));
    // 2026-09-09 需求批：level 全域合法；客户端妄图自报金额也照例丢弃
    // （金额非入参，服务端 levelRewardPoints() 单点推导）。
    for (const int level : {kMinimumLevel, 3, 4, kMaximumLevel}) {
        QVERIFY(normalizeRequestData(request_type::kCreditLevelReward,
            {{"level", level}, {"amount", 999999}}, &output));
        QCOMPARE(output, QJsonObject({{"level", level}}));
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
