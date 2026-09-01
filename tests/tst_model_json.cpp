#include "charging/common/model/enums.h"
#include "charging/common/model/model_json.h"
#include "charging/common/model/models.h"

#include <QDateTime>
#include <QJsonObject>
#include <QtTest>

class ModelJsonTest final : public QObject
{
    Q_OBJECT

private slots:
    void enumValuesRoundTrip();
    void userWithUnicodeRoundTrips();
    void jsonIntegerBoundariesAreStrict();
    void dateTimesRequireExplicitTimezone();
    void invalidEnumDoesNotOverwriteOutput();
};

void ModelJsonTest::enumValuesRoundTrip()
{
    charging::model::ChargerStatus chargerStatus = charging::model::ChargerStatus::Offline;
    QVERIFY(charging::model::fromString(
        charging::model::toString(charging::model::ChargerStatus::Charging), &chargerStatus));
    QVERIFY(chargerStatus == charging::model::ChargerStatus::Charging);

    charging::model::OrderStatus orderStatus = charging::model::OrderStatus::Cancelled;
    QVERIFY(charging::model::fromString(
        charging::model::toString(charging::model::OrderStatus::WaitingPayment), &orderStatus));
    QVERIFY(orderStatus == charging::model::OrderStatus::WaitingPayment);

    chargerStatus = charging::model::ChargerStatus::Fault;
    QVERIFY(!charging::model::fromString(QStringLiteral("UNKNOWN"), &chargerStatus));
    QVERIFY(chargerStatus == charging::model::ChargerStatus::Fault);
}

void ModelJsonTest::userWithUnicodeRoundTrips()
{
    charging::model::User original;
    original.id = 9007199254740991LL;
    original.phone = QStringLiteral("13800138000");
    original.nickname = QStringLiteral("测试用户");
    original.avatarKey = QStringLiteral("avatar/default");
    original.balanceCents = 12345;
    original.status = charging::model::UserStatus::Active;
    original.createdAtUtc =
        QDateTime::fromString(QStringLiteral("2023-11-14T22:13:20.123Z"), Qt::ISODateWithMs);
    original.updatedAtUtc =
        QDateTime::fromString(QStringLiteral("2023-11-14T22:13:20.456Z"), Qt::ISODateWithMs);

    const QJsonObject json = charging::model::toJson(original);
    QCOMPARE(json.value(QStringLiteral("id")).toString(), QStringLiteral("9007199254740991"));
    QCOMPARE(json.value(QStringLiteral("nickname")).toString(), original.nickname);

    charging::model::User restored;
    QString errorMessage;
    QVERIFY2(charging::model::fromJson(json, &restored, &errorMessage), qPrintable(errorMessage));
    QCOMPARE(restored.id, original.id);
    QCOMPARE(restored.nickname, original.nickname);
    QCOMPARE(restored.balanceCents, original.balanceCents);
    QVERIFY(restored.status == original.status);
    QCOMPARE(restored.createdAtUtc, original.createdAtUtc);
    QCOMPARE(restored.updatedAtUtc, original.updatedAtUtc);
}

void ModelJsonTest::jsonIntegerBoundariesAreStrict()
{
    charging::model::User original;
    original.id = 1;
    original.phone = QStringLiteral("13800138000");
    original.nickname = QStringLiteral("边界用户");
    original.balanceCents = charging::model::kMaximumJsonSafeInteger;
    original.createdAtUtc =
        QDateTime::fromString(QStringLiteral("2026-09-01T00:00:00.000Z"), Qt::ISODateWithMs);
    original.updatedAtUtc = original.createdAtUtc;

    QJsonObject json = charging::model::toJson(original);
    charging::model::User restored;
    QString errorMessage;
    QVERIFY2(charging::model::fromJson(json, &restored, &errorMessage), qPrintable(errorMessage));
    QCOMPARE(restored.balanceCents, charging::model::kMaximumJsonSafeInteger);

    original.balanceCents = charging::model::kMaximumJsonSafeInteger + 1;
    json = charging::model::toJson(original);
    QVERIFY(json.value(QStringLiteral("balanceCents")).isNull());
    QVERIFY(!charging::model::fromJson(json, &restored, &errorMessage));

    json = charging::model::toJson(original);
    json.insert(QStringLiteral("balanceCents"),
                static_cast<double>(charging::model::kMaximumJsonSafeInteger) + 1.0);
    QVERIFY(!charging::model::fromJson(json, &restored, &errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("supported integer range")));
}

void ModelJsonTest::dateTimesRequireExplicitTimezone()
{
    charging::model::User valid;
    valid.id = 1;
    valid.phone = QStringLiteral("13800138000");
    valid.nickname = QStringLiteral("时区用户");
    valid.createdAtUtc =
        QDateTime::fromString(QStringLiteral("2026-09-01T00:00:00.000Z"), Qt::ISODateWithMs);
    valid.updatedAtUtc = valid.createdAtUtc;

    QJsonObject json = charging::model::toJson(valid);
    json.insert(QStringLiteral("createdAt"), QStringLiteral("2026-09-01T08:30:00.123"));

    charging::model::User output;
    output.id = 77;
    QString errorMessage;
    QVERIFY(!charging::model::fromJson(json, &output, &errorMessage));
    QCOMPARE(output.id, 77);
    QVERIFY(errorMessage.contains(QStringLiteral("explicit UTC offset")));

    json.insert(QStringLiteral("createdAt"), QStringLiteral("2026-09-01T08:30:00.123+08:00"));
    QVERIFY2(charging::model::fromJson(json, &output, &errorMessage), qPrintable(errorMessage));
    const QDateTime expectedUtc =
        QDateTime::fromString(QStringLiteral("2026-09-01T00:30:00.123Z"), Qt::ISODateWithMs);
    QCOMPARE(output.createdAtUtc, expectedUtc);
}

void ModelJsonTest::invalidEnumDoesNotOverwriteOutput()
{
    charging::model::User valid;
    valid.id = 1;
    valid.phone = QStringLiteral("13800138000");
    valid.nickname = QStringLiteral("用户8000");
    valid.balanceCents = 10000;
    valid.createdAtUtc =
        QDateTime::fromString(QStringLiteral("2023-11-14T22:13:20.000Z"), Qt::ISODateWithMs);
    valid.updatedAtUtc = valid.createdAtUtc;

    QJsonObject invalidJson = charging::model::toJson(valid);
    invalidJson.insert(QStringLiteral("status"), QStringLiteral("UNKNOWN"));

    charging::model::User output;
    output.id = 77;
    QString errorMessage;
    QVERIFY(!charging::model::fromJson(invalidJson, &output, &errorMessage));
    QCOMPARE(output.id, 77);
    QVERIFY(errorMessage.contains(QStringLiteral("unknown enum value")));
}

QTEST_GUILESS_MAIN(ModelJsonTest)

#include "tst_model_json.moc"
