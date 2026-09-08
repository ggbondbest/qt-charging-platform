#include "charging/common/protocol/user_api_contract.h"

#include "charging/common/model/enums.h"

#include <QRegularExpression>
#include <cmath>

namespace charging::protocol::user_api {
namespace {

bool integerInRange(const QJsonValue& value, qint64 minimum, qint64 maximum)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number
        && number >= static_cast<double>(minimum) && number <= static_cast<double>(maximum);
}

bool positiveId(const QJsonValue& value)
{
    if (!value.isString()
        || !QRegularExpression(QStringLiteral("\\A[1-9][0-9]*\\z"))
                .match(value.toString()).hasMatch()) {
        return false;
    }
    bool ok = false;
    value.toString().toLongLong(&ok);
    return ok;
}

template <typename Enum>
bool validStatus(const QString& value)
{
    Enum status;
    return value.isEmpty() || charging::model::fromString(value, &status);
}

} // namespace

bool normalizeRequestData(const QString& type, const QJsonObject& data,
                          QJsonObject* normalized, ProtocolError* error)
{
    using namespace request_type;
    const bool stations = type == QLatin1String(kGetStations);
    const bool chargers = type == QLatin1String(kGetChargers);
    const bool reservations = type == QLatin1String(kGetReservations);
    const bool orders = type == QLatin1String(kGetOrders);
    const bool records = type == QLatin1String(kGetRechargeRecords);
    const bool profile = type == QLatin1String(kGetUserInfo);
    const bool update = type == QLatin1String(kUpdateUserInfo);
    const bool recharge = type == QLatin1String(kRecharge);
    const bool stats = type == QLatin1String(kGetUserStats);
    const bool coupons = type == QLatin1String(kGetCoupons);
    const bool notices = type == QLatin1String(kGetNotifications);
    const auto fail = [error](const char* code, const QString& field) {
        if (error != nullptr) {
            *error = {};
            error->code = QString::fromLatin1(code);
            error->message = QStringLiteral("Invalid user API request: %1").arg(field);
            error->details.insert(QStringLiteral("field"), field);
        }
        return false;
    };
    if (!(stations || chargers || reservations || orders || records || profile || update
          || recharge || stats || coupons || notices)) {
        return fail(error_code::kUnknownRequestType, QStringLiteral("type"));
    }

    QJsonObject result;
    if (stations || chargers || reservations || orders || records || coupons || notices) {
        for (const QString& key : {QStringLiteral("page"), QStringLiteral("pageSize")}) {
            const bool isPage = key == QLatin1String("page");
            const QJsonValue value = data.contains(key)
                ? data.value(key) : QJsonValue(isPage ? kDefaultPage : kDefaultPageSize);
            if (!integerInRange(value, 1, isPage ? kMaximumPage : kMaximumPageSize)) {
                return fail(error_code::kInvalidArgument, key);
            }
            result.insert(key, value);
        }
    }
    if (stations) {
        const QString key = QStringLiteral("keyword");
        const QJsonValue value = data.contains(key) ? data.value(key) : QJsonValue(QString());
        if (!value.isString() || value.toString().trimmed().size() > 64) {
            return fail(error_code::kInvalidArgument, key);
        }
        result.insert(key, value.toString().trimmed());
    }
    if (chargers) {
        const QString key = QStringLiteral("stationId");
        if (!positiveId(data.value(key))) {
            return fail(error_code::kInvalidArgument, key);
        }
        result.insert(key, data.value(key));
    }
    if (reservations || orders) {
        const QString key = QStringLiteral("status");
        const QJsonValue value = data.contains(key) ? data.value(key) : QJsonValue(QString());
        if (!value.isString()
            || (reservations && !validStatus<charging::model::ReservationStatus>(value.toString()))
            || (orders && !validStatus<charging::model::OrderStatus>(value.toString()))) {
            return fail(error_code::kInvalidArgument, key);
        }
        result.insert(key, value);
    }
    if (stats) {
        const QString key = QStringLiteral("months");
        const QJsonValue value = data.contains(key)
            ? data.value(key) : QJsonValue(6);
        if (!integerInRange(value, 1, kMaximumStatsMonths)) {
            return fail(error_code::kInvalidArgument, key);
        }
        result.insert(key, value);
        // 2026-09-08 批次B 追加：period ∈ "week"|"month"|"year"，缺省注入
        // "month"（即冻结时行为——monthKey 恒 "YYYY-MM"）。week 档 monthKey 为
        // ISO 周 "YYYY-Www"，year 档为 "YYYY"；字段名沿用 monthKey 不动。
        const QString periodKey = QStringLiteral("period");
        const QJsonValue periodValue = data.contains(periodKey)
            ? data.value(periodKey) : QJsonValue(QStringLiteral("month"));
        const QString period = periodValue.toString();
        if (!periodValue.isString()
            || (period != QLatin1String("week") && period != QLatin1String("month")
                && period != QLatin1String("year"))) {
            return fail(error_code::kInvalidArgument, periodKey);
        }
        result.insert(periodKey, periodValue);
    }
    if (coupons) {
        // Wire statuses are the CouponPage contract's lowercase trio; empty = all.
        const QString key = QStringLiteral("status");
        const QJsonValue value = data.contains(key) ? data.value(key) : QJsonValue(QString());
        const QString status = value.toString();
        if (!value.isString()
            || (!status.isEmpty() && status != QLatin1String("available")
                && status != QLatin1String("used") && status != QLatin1String("expired"))) {
            return fail(error_code::kInvalidArgument, key);
        }
        result.insert(key, value);
    }
    if (update) {
        const QString nickname = QStringLiteral("nickname");
        const QString avatar = QStringLiteral("avatarKey");
        if (!data.contains(nickname) && !data.contains(avatar)) {
            return fail(error_code::kInvalidArgument, QStringLiteral("nickname/avatarKey"));
        }
        if (data.contains(nickname)) {
            const QJsonValue value = data.value(nickname);
            const QString trimmed = value.toString().trimmed();
            if (!value.isString() || trimmed.isEmpty() || trimmed.size() > 32) {
                return fail(error_code::kInvalidArgument, nickname);
            }
            result.insert(nickname, trimmed);
        }
        if (data.contains(avatar)) {
            const QJsonValue value = data.value(avatar);
            if (!value.isString()
                || !QRegularExpression(QStringLiteral("\\A[A-Za-z0-9_-]{0,64}\\z"))
                        .match(value.toString()).hasMatch()) {
                return fail(error_code::kInvalidArgument, avatar);
            }
            result.insert(avatar, value);
        }
    }
    if (recharge) {
        const QString amount = QStringLiteral("amountCents");
        const QString transaction = QStringLiteral("transactionNo");
        if (!integerInRange(data.value(amount), 1, kMaximumRechargeCents)) {
            return fail(error_code::kInvalidArgument, amount);
        }
        const QJsonValue value = data.value(transaction);
        if (!value.isString()
            || !QRegularExpression(QStringLiteral("\\A[A-Za-z0-9_-]{1,40}\\z"))
                    .match(value.toString()).hasMatch()) {
            return fail(error_code::kInvalidArgument, transaction);
        }
        result.insert(amount, data.value(amount));
        result.insert(transaction, value);
    }
    if (normalized != nullptr) {
        *normalized = result;
    }
    if (error != nullptr) {
        *error = {};
    }
    return true;
}

} // namespace charging::protocol::user_api
