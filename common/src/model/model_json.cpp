#include "charging/common/model/model_json.h"

#include <QJsonValue>

#include <cmath>
#include <limits>

namespace charging::model {

namespace {

bool fail(QString* errorMessage, const QString& message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

void clearError(QString* errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
}

bool requireString(const QJsonObject& object, const char* key, QString* outValue,
                   QString* errorMessage, bool allowEmpty = true)
{
    const QString fieldName = QString::fromLatin1(key);
    const QJsonValue jsonValue = object.value(fieldName);
    if (!jsonValue.isString()) {
        return fail(errorMessage, QStringLiteral("Field '%1' must be a string").arg(fieldName));
    }

    const QString value = jsonValue.toString();
    if (!allowEmpty && value.isEmpty()) {
        return fail(errorMessage, QStringLiteral("Field '%1' must not be empty").arg(fieldName));
    }
    *outValue = value;
    return true;
}

bool requireId(const QJsonObject& object, const char* key, qint64* outValue, QString* errorMessage)
{
    QString text;
    if (!requireString(object, key, &text, errorMessage, false)) {
        return false;
    }

    bool ok = false;
    const qint64 value = text.toLongLong(&ok, 10);
    if (!ok || value < 0) {
        return fail(errorMessage, QStringLiteral("Field '%1' must be a non-negative decimal ID")
                                      .arg(QString::fromLatin1(key)));
    }
    *outValue = value;
    return true;
}

bool requireNullableId(const QJsonObject& object, const char* key, qint64* outValue,
                       QString* errorMessage)
{
    const QString fieldName = QString::fromLatin1(key);
    if (object.value(fieldName).isNull()) {
        *outValue = 0;
        return true;
    }
    return requireId(object, key, outValue, errorMessage);
}

bool requireInteger(const QJsonObject& object, const char* key, qint64* outValue,
                    QString* errorMessage)
{
    const QString fieldName = QString::fromLatin1(key);
    const QJsonValue jsonValue = object.value(fieldName);
    if (!jsonValue.isDouble()) {
        return fail(errorMessage, QStringLiteral("Field '%1' must be an integer").arg(fieldName));
    }

    const double number = jsonValue.toDouble();
    // JSON numbers are IEEE-754 doubles. Keep integer-valued fields in the
    // exact range; larger SQLite IDs use decimal strings via requireId().
    constexpr double kMaximumSafeInteger = static_cast<double>(kMaximumJsonSafeInteger);
    constexpr double kMinimumSafeInteger = static_cast<double>(kMinimumJsonSafeInteger);
    if (!std::isfinite(number) || std::floor(number) != number || number < kMinimumSafeInteger ||
        number > kMaximumSafeInteger) {
        return fail(
            errorMessage,
            QStringLiteral("Field '%1' is outside the supported integer range").arg(fieldName));
    }

    *outValue = static_cast<qint64>(number);
    return true;
}

bool requireInt(const QJsonObject& object, const char* key, int* outValue, QString* errorMessage)
{
    qint64 value = 0;
    if (!requireInteger(object, key, &value, errorMessage)) {
        return false;
    }
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return fail(
            errorMessage,
            QStringLiteral("Field '%1' is outside the int range").arg(QString::fromLatin1(key)));
    }
    *outValue = static_cast<int>(value);
    return true;
}

bool requireDouble(const QJsonObject& object, const char* key, double* outValue,
                   QString* errorMessage)
{
    const QString fieldName = QString::fromLatin1(key);
    const QJsonValue jsonValue = object.value(fieldName);
    if (!jsonValue.isDouble() || !std::isfinite(jsonValue.toDouble())) {
        return fail(errorMessage,
                    QStringLiteral("Field '%1' must be a finite number").arg(fieldName));
    }
    *outValue = jsonValue.toDouble();
    return true;
}

bool requireDateTime(const QJsonObject& object, const char* key, QDateTime* outValue,
                     QString* errorMessage, bool nullable)
{
    const QString fieldName = QString::fromLatin1(key);
    const QJsonValue jsonValue = object.value(fieldName);
    if (nullable && jsonValue.isNull()) {
        *outValue = {};
        return true;
    }
    if (!jsonValue.isString()) {
        return fail(errorMessage,
                    QStringLiteral("Field '%1' must be an ISO-8601 string%2")
                        .arg(fieldName, nullable ? QStringLiteral(" or null") : QString()));
    }

    QDateTime value = QDateTime::fromString(jsonValue.toString(), Qt::ISODateWithMs);
    if (!value.isValid()) {
        value = QDateTime::fromString(jsonValue.toString(), Qt::ISODate);
    }
    if (!value.isValid()) {
        return fail(errorMessage,
                    QStringLiteral("Field '%1' is not a valid ISO-8601 time").arg(fieldName));
    }
    if (value.timeSpec() != Qt::UTC && value.timeSpec() != Qt::OffsetFromUTC) {
        return fail(errorMessage,
                    QStringLiteral("Field '%1' must include 'Z' or an explicit UTC offset")
                        .arg(fieldName));
    }
    *outValue = value.toUTC();
    return true;
}

template <typename Enum>
bool requireEnum(const QJsonObject& object, const char* key, Enum* outValue, QString* errorMessage)
{
    QString text;
    if (!requireString(object, key, &text, errorMessage, false)) {
        return false;
    }
    if (!fromString(text, outValue)) {
        return fail(errorMessage, QStringLiteral("Field '%1' contains an unknown enum value '%2'")
                                      .arg(QString::fromLatin1(key), text));
    }
    return true;
}

void putId(QJsonObject* object, const char* key, qint64 value)
{
    object->insert(QString::fromLatin1(key), QString::number(value));
}

void putNullableId(QJsonObject* object, const char* key, qint64 value)
{
    if (value == 0) {
        object->insert(QString::fromLatin1(key), QJsonValue::Null);
        return;
    }
    putId(object, key, value);
}

void putInteger(QJsonObject* object, const char* key, qint64 value)
{
    // Fail closed for an invalid in-memory model. Emitting null makes the
    // receiver reject the DTO instead of silently rounding a monetary or
    // counter value through double.
    if (value < kMinimumJsonSafeInteger || value > kMaximumJsonSafeInteger) {
        object->insert(QString::fromLatin1(key), QJsonValue::Null);
        return;
    }
    object->insert(QString::fromLatin1(key), static_cast<double>(value));
}

void putDateTime(QJsonObject* object, const char* key, const QDateTime& value,
                 bool nullable = false)
{
    if (!value.isValid() && nullable) {
        object->insert(QString::fromLatin1(key), QJsonValue::Null);
        return;
    }
    object->insert(QString::fromLatin1(key), value.toUTC().toString(Qt::ISODateWithMs));
}

} // namespace

QJsonObject toJson(const User& value)
{
    QJsonObject object;
    putId(&object, "id", value.id);
    object.insert(QStringLiteral("phone"), value.phone);
    object.insert(QStringLiteral("nickname"), value.nickname);
    object.insert(QStringLiteral("avatarKey"), value.avatarKey);
    putInteger(&object, "balanceCents", value.balanceCents);
    object.insert(QStringLiteral("status"), toString(value.status));
    putDateTime(&object, "createdAt", value.createdAtUtc);
    putDateTime(&object, "updatedAt", value.updatedAtUtc);
    return object;
}

QJsonObject toJson(const Admin& value)
{
    QJsonObject object;
    putId(&object, "id", value.id);
    object.insert(QStringLiteral("username"), value.username);
    object.insert(QStringLiteral("displayName"), value.displayName);
    object.insert(QStringLiteral("status"), toString(value.status));
    putDateTime(&object, "createdAt", value.createdAtUtc);
    return object;
}

QJsonObject toJson(const Station& value)
{
    QJsonObject object;
    putId(&object, "id", value.id);
    object.insert(QStringLiteral("code"), value.code);
    object.insert(QStringLiteral("name"), value.name);
    object.insert(QStringLiteral("address"), value.address);
    object.insert(QStringLiteral("latitude"), value.latitude);
    object.insert(QStringLiteral("longitude"), value.longitude);
    putInteger(&object, "priceCentsPerKwh", value.priceCentsPerKwh);
    object.insert(QStringLiteral("status"), toString(value.status));
    object.insert(QStringLiteral("totalChargers"), value.totalChargers);
    object.insert(QStringLiteral("availableChargers"), value.availableChargers);
    return object;
}

QJsonObject toJson(const Charger& value)
{
    QJsonObject object;
    putId(&object, "id", value.id);
    putId(&object, "stationId", value.stationId);
    object.insert(QStringLiteral("code"), value.code);
    object.insert(QStringLiteral("type"), toString(value.type));
    object.insert(QStringLiteral("powerWatts"), value.powerWatts);
    object.insert(QStringLiteral("status"), toString(value.status));
    object.insert(QStringLiteral("totalChargeCount"), value.totalChargeCount);
    putInteger(&object, "totalChargeSeconds", value.totalChargeSeconds);
    putDateTime(&object, "createdAt", value.createdAtUtc);
    putDateTime(&object, "updatedAt", value.updatedAtUtc);
    return object;
}

QJsonObject toJson(const Reservation& value)
{
    QJsonObject object;
    putId(&object, "id", value.id);
    putId(&object, "userId", value.userId);
    putId(&object, "chargerId", value.chargerId);
    object.insert(QStringLiteral("status"), toString(value.status));
    putDateTime(&object, "reservedAt", value.reservedAtUtc);
    putDateTime(&object, "expiresAt", value.expiresAtUtc);
    putDateTime(&object, "endedAt", value.endedAtUtc, true);
    return object;
}

QJsonObject toJson(const Order& value)
{
    QJsonObject object;
    putId(&object, "id", value.id);
    object.insert(QStringLiteral("orderNo"), value.orderNo);
    putId(&object, "userId", value.userId);
    putId(&object, "chargerId", value.chargerId);
    putNullableId(&object, "reservationId", value.reservationId);
    object.insert(QStringLiteral("status"), toString(value.status));
    putInteger(&object, "unitPriceCentsPerKwh", value.unitPriceCentsPerKwh);
    putInteger(&object, "energyWh", value.energyWh);
    putInteger(&object, "durationSeconds", value.durationSeconds);
    putInteger(&object, "amountCents", value.amountCents);
    putDateTime(&object, "createdAt", value.createdAtUtc);
    putDateTime(&object, "startedAt", value.startedAtUtc, true);
    putDateTime(&object, "stoppedAt", value.stoppedAtUtc, true);
    putDateTime(&object, "paidAt", value.paidAtUtc, true);
    putDateTime(&object, "updatedAt", value.updatedAtUtc);
    return object;
}

QJsonObject toJson(const RechargeRecord& value)
{
    QJsonObject object;
    putId(&object, "id", value.id);
    object.insert(QStringLiteral("transactionNo"), value.transactionNo);
    putId(&object, "userId", value.userId);
    putInteger(&object, "amountCents", value.amountCents);
    putInteger(&object, "balanceAfterCents", value.balanceAfterCents);
    object.insert(QStringLiteral("status"), toString(value.status));
    putDateTime(&object, "createdAt", value.createdAtUtc);
    return object;
}

QJsonObject toJson(const OperationLog& value)
{
    QJsonObject object;
    putId(&object, "id", value.id);
    putNullableId(&object, "adminId", value.adminId);
    object.insert(QStringLiteral("action"), value.action);
    object.insert(QStringLiteral("targetType"), value.targetType);
    object.insert(QStringLiteral("targetId"), value.targetId);
    object.insert(QStringLiteral("details"), value.details);
    putDateTime(&object, "createdAt", value.createdAtUtc);
    return object;
}

bool fromJson(const QJsonObject& object, User* outValue, QString* errorMessage)
{
    clearError(errorMessage);
    if (outValue == nullptr) {
        return fail(errorMessage, QStringLiteral("User output pointer is null"));
    }
    User value;
    if (!requireId(object, "id", &value.id, errorMessage) ||
        !requireString(object, "phone", &value.phone, errorMessage, false) ||
        !requireString(object, "nickname", &value.nickname, errorMessage, false) ||
        !requireString(object, "avatarKey", &value.avatarKey, errorMessage) ||
        !requireInteger(object, "balanceCents", &value.balanceCents, errorMessage) ||
        !requireEnum(object, "status", &value.status, errorMessage) ||
        !requireDateTime(object, "createdAt", &value.createdAtUtc, errorMessage, false) ||
        !requireDateTime(object, "updatedAt", &value.updatedAtUtc, errorMessage, false)) {
        return false;
    }
    if (value.balanceCents < 0) {
        return fail(errorMessage, QStringLiteral("Field 'balanceCents' must not be negative"));
    }
    *outValue = value;
    return true;
}

bool fromJson(const QJsonObject& object, Admin* outValue, QString* errorMessage)
{
    clearError(errorMessage);
    if (outValue == nullptr) {
        return fail(errorMessage, QStringLiteral("Admin output pointer is null"));
    }
    Admin value;
    if (!requireId(object, "id", &value.id, errorMessage) ||
        !requireString(object, "username", &value.username, errorMessage, false) ||
        !requireString(object, "displayName", &value.displayName, errorMessage, false) ||
        !requireEnum(object, "status", &value.status, errorMessage) ||
        !requireDateTime(object, "createdAt", &value.createdAtUtc, errorMessage, false)) {
        return false;
    }
    *outValue = value;
    return true;
}

bool fromJson(const QJsonObject& object, Station* outValue, QString* errorMessage)
{
    clearError(errorMessage);
    if (outValue == nullptr) {
        return fail(errorMessage, QStringLiteral("Station output pointer is null"));
    }
    Station value;
    if (!requireId(object, "id", &value.id, errorMessage) ||
        !requireString(object, "code", &value.code, errorMessage, false) ||
        !requireString(object, "name", &value.name, errorMessage, false) ||
        !requireString(object, "address", &value.address, errorMessage, false) ||
        !requireDouble(object, "latitude", &value.latitude, errorMessage) ||
        !requireDouble(object, "longitude", &value.longitude, errorMessage) ||
        !requireInteger(object, "priceCentsPerKwh", &value.priceCentsPerKwh, errorMessage) ||
        !requireEnum(object, "status", &value.status, errorMessage) ||
        !requireInt(object, "totalChargers", &value.totalChargers, errorMessage) ||
        !requireInt(object, "availableChargers", &value.availableChargers, errorMessage)) {
        return false;
    }
    if (value.latitude < -90.0 || value.latitude > 90.0 || value.longitude < -180.0 ||
        value.longitude > 180.0 || value.priceCentsPerKwh < 0 || value.totalChargers < 0 ||
        value.availableChargers < 0 || value.availableChargers > value.totalChargers) {
        return fail(errorMessage, QStringLiteral("Station contains an out-of-range value"));
    }
    *outValue = value;
    return true;
}

bool fromJson(const QJsonObject& object, Charger* outValue, QString* errorMessage)
{
    clearError(errorMessage);
    if (outValue == nullptr) {
        return fail(errorMessage, QStringLiteral("Charger output pointer is null"));
    }
    Charger value;
    if (!requireId(object, "id", &value.id, errorMessage) ||
        !requireId(object, "stationId", &value.stationId, errorMessage) ||
        !requireString(object, "code", &value.code, errorMessage, false) ||
        !requireEnum(object, "type", &value.type, errorMessage) ||
        !requireInt(object, "powerWatts", &value.powerWatts, errorMessage) ||
        !requireEnum(object, "status", &value.status, errorMessage) ||
        !requireInt(object, "totalChargeCount", &value.totalChargeCount, errorMessage) ||
        !requireInteger(object, "totalChargeSeconds", &value.totalChargeSeconds, errorMessage) ||
        !requireDateTime(object, "createdAt", &value.createdAtUtc, errorMessage, false) ||
        !requireDateTime(object, "updatedAt", &value.updatedAtUtc, errorMessage, false)) {
        return false;
    }
    if (value.powerWatts <= 0 || value.totalChargeCount < 0 || value.totalChargeSeconds < 0) {
        return fail(errorMessage, QStringLiteral("Charger contains an out-of-range value"));
    }
    *outValue = value;
    return true;
}

bool fromJson(const QJsonObject& object, Reservation* outValue, QString* errorMessage)
{
    clearError(errorMessage);
    if (outValue == nullptr) {
        return fail(errorMessage, QStringLiteral("Reservation output pointer is null"));
    }
    Reservation value;
    if (!requireId(object, "id", &value.id, errorMessage) ||
        !requireId(object, "userId", &value.userId, errorMessage) ||
        !requireId(object, "chargerId", &value.chargerId, errorMessage) ||
        !requireEnum(object, "status", &value.status, errorMessage) ||
        !requireDateTime(object, "reservedAt", &value.reservedAtUtc, errorMessage, false) ||
        !requireDateTime(object, "expiresAt", &value.expiresAtUtc, errorMessage, false) ||
        !requireDateTime(object, "endedAt", &value.endedAtUtc, errorMessage, true)) {
        return false;
    }
    *outValue = value;
    return true;
}

bool fromJson(const QJsonObject& object, Order* outValue, QString* errorMessage)
{
    clearError(errorMessage);
    if (outValue == nullptr) {
        return fail(errorMessage, QStringLiteral("Order output pointer is null"));
    }
    Order value;
    if (!requireId(object, "id", &value.id, errorMessage) ||
        !requireString(object, "orderNo", &value.orderNo, errorMessage, false) ||
        !requireId(object, "userId", &value.userId, errorMessage) ||
        !requireId(object, "chargerId", &value.chargerId, errorMessage) ||
        !requireNullableId(object, "reservationId", &value.reservationId, errorMessage) ||
        !requireEnum(object, "status", &value.status, errorMessage) ||
        !requireInteger(object, "unitPriceCentsPerKwh", &value.unitPriceCentsPerKwh,
                        errorMessage) ||
        !requireInteger(object, "energyWh", &value.energyWh, errorMessage) ||
        !requireInteger(object, "durationSeconds", &value.durationSeconds, errorMessage) ||
        !requireInteger(object, "amountCents", &value.amountCents, errorMessage) ||
        !requireDateTime(object, "createdAt", &value.createdAtUtc, errorMessage, false) ||
        !requireDateTime(object, "startedAt", &value.startedAtUtc, errorMessage, true) ||
        !requireDateTime(object, "stoppedAt", &value.stoppedAtUtc, errorMessage, true) ||
        !requireDateTime(object, "paidAt", &value.paidAtUtc, errorMessage, true) ||
        !requireDateTime(object, "updatedAt", &value.updatedAtUtc, errorMessage, false)) {
        return false;
    }
    if (value.unitPriceCentsPerKwh < 0 || value.energyWh < 0 || value.durationSeconds < 0 ||
        value.amountCents < 0) {
        return fail(errorMessage, QStringLiteral("Order contains an out-of-range value"));
    }
    *outValue = value;
    return true;
}

bool fromJson(const QJsonObject& object, RechargeRecord* outValue, QString* errorMessage)
{
    clearError(errorMessage);
    if (outValue == nullptr) {
        return fail(errorMessage, QStringLiteral("RechargeRecord output pointer is null"));
    }
    RechargeRecord value;
    if (!requireId(object, "id", &value.id, errorMessage) ||
        !requireString(object, "transactionNo", &value.transactionNo, errorMessage, false) ||
        !requireId(object, "userId", &value.userId, errorMessage) ||
        !requireInteger(object, "amountCents", &value.amountCents, errorMessage) ||
        !requireInteger(object, "balanceAfterCents", &value.balanceAfterCents, errorMessage) ||
        !requireEnum(object, "status", &value.status, errorMessage) ||
        !requireDateTime(object, "createdAt", &value.createdAtUtc, errorMessage, false)) {
        return false;
    }
    if (value.amountCents <= 0 || value.balanceAfterCents < 0) {
        return fail(errorMessage, QStringLiteral("RechargeRecord contains an out-of-range value"));
    }
    *outValue = value;
    return true;
}

bool fromJson(const QJsonObject& object, OperationLog* outValue, QString* errorMessage)
{
    clearError(errorMessage);
    if (outValue == nullptr) {
        return fail(errorMessage, QStringLiteral("OperationLog output pointer is null"));
    }
    OperationLog value;
    const QJsonValue details = object.value(QStringLiteral("details"));
    if (!requireId(object, "id", &value.id, errorMessage) ||
        !requireNullableId(object, "adminId", &value.adminId, errorMessage) ||
        !requireString(object, "action", &value.action, errorMessage, false) ||
        !requireString(object, "targetType", &value.targetType, errorMessage, false) ||
        !requireString(object, "targetId", &value.targetId, errorMessage) ||
        !requireDateTime(object, "createdAt", &value.createdAtUtc, errorMessage, false)) {
        return false;
    }
    if (!details.isObject()) {
        return fail(errorMessage, QStringLiteral("Field 'details' must be an object"));
    }
    value.details = details.toObject();
    *outValue = value;
    return true;
}

} // namespace charging::model
