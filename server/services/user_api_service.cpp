#include "user_api_service.h"
#include "user_api_repository.h"
#include "charging/common/model/model_json.h"
#include "charging/common/protocol/user_api_contract.h"
#include <QJsonArray>
#include <QSet>
#include <QBuffer>
#include <QImageReader>
#include <QImage>

namespace charging::server {
namespace {
using namespace charging::protocol;
bool validAvatar(const QString& value)
{
    static const QSet<QString> presets{"", "bolt", "plug", "car", "leaf", "cat", "panda", "moon", "rocket"};
    if (presets.contains(value)) return true;
    const QString prefix = QStringLiteral("data:image/png;base64,");
    if (!value.startsWith(prefix)) return false;
    const QByteArray encoded = value.mid(prefix.size()).toLatin1();
    QByteArray bytes = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (bytes.isEmpty() || bytes.size() > user_api::kMaximumAvatarBytes
        || bytes.toBase64() != encoded) return false;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return false;
    QImageReader reader(&buffer, "png");
    const QSize size = reader.size();
    // Check the header BEFORE decoding: a compressed image must not request an
    // unbounded allocation on the service worker.
    if (!size.isValid() || size.width() > user_api::kMaximumAvatarDimension
        || size.height() > user_api::kMaximumAvatarDimension) return false;
    return !reader.read().isNull();
}
const QMap<QString, UserApiAction> actions{
    {request_type::kGetStations, UserApiAction::Stations},
    {request_type::kGetChargers, UserApiAction::Chargers},
    {request_type::kGetReservations, UserApiAction::Reservations},
    {request_type::kGetUserInfo, UserApiAction::Profile},
    {request_type::kUpdateUserInfo, UserApiAction::UpdateProfile},
    {request_type::kRecharge, UserApiAction::Recharge},
    {request_type::kGetRechargeRecords, UserApiAction::RechargeRecords},
    {request_type::kGetOrders, UserApiAction::Orders},
    {request_type::kGetUserStats, UserApiAction::Stats},
    {request_type::kGetCoupons, UserApiAction::Coupons},
    {request_type::kGetNotifications, UserApiAction::Notifications},
    {request_type::kCheckIn, UserApiAction::CheckIn},
    {request_type::kGetPoints, UserApiAction::GetPoints},
    {request_type::kCreditLevelReward, UserApiAction::CreditLevelReward},
    {request_type::kSubmitChargerRating, UserApiAction::SubmitRating},
    {request_type::kGetMyRatings, UserApiAction::GetMyRatings}
};
UserApiReply fail(const char* code, const QString& message)
{
    UserApiReply r;
    r.error.code = QString::fromLatin1(code);
    r.error.message = message;
    return r;
}
QJsonObject wireRow(const QVariantMap& row)
{
    QJsonObject object;
    for (auto it = row.begin(); it != row.end(); ++it) {
        const QStringList parts = it.key().split('_');
        QString key = parts.first();
        for (int i = 1; i < parts.size(); ++i) {
            QString part = parts.at(i);
            if (!part.isEmpty()) part[0] = part[0].toUpper();
            key += part;
        }
        if (it.value().isNull()) object.insert(key, QJsonValue::Null);
        else if (it.key() == "id" || it.key().endsWith("_id"))
            object.insert(key, QString::number(it.value().toLongLong()));
        else object.insert(key, QJsonValue::fromVariant(it.value()));
    }
    return object;
}
template <typename Model>
bool canonical(const QJsonObject& source, QJsonObject* result)
{
    Model value;
    if (!charging::model::fromJson(source, &value)) return false;
    *result = charging::model::toJson(value);
    return true;
}
} // namespace

UserApiService::UserApiService(UserApiRepository* repository, UtcClock clock)
    : repository_(repository), clock_(clock) {}

bool UserApiService::handles(const QString& type) { return actions.contains(type); }

UserApiReply UserApiService::handle(const QString& type, const QJsonObject& data,
                                   qint64 sessionUserId) const
{
    using namespace charging::protocol;
    if (!handles(type)) return fail(error_code::kUnknownRequestType, QStringLiteral("未知请求类型"));
    if (sessionUserId <= 0) return fail(error_code::kUnauthorized, QStringLiteral("请重新登录"));
    QJsonObject input;
    UserApiReply reply;
    if (!user_api::normalizeRequestData(type, data, &input, &reply.error)) return reply;
    if (input.contains("avatarKey") && !validAvatar(input.value("avatarKey").toString())) {
        reply = fail(error_code::kInvalidArgument,
                     QStringLiteral("头像须为内置头像或不超过128KiB、512×512的PNG图片"));
        reply.error.details.insert("field", "avatarKey");
        return reply;
    }
    if (repository_ == nullptr) return fail(error_code::kInternalError, QStringLiteral("用户服务未就绪"));
    UserApiQuery query;
    query.action = actions.value(type);
    query.userId = sessionUserId; // Never read identity from request data.
    query.stationId = input.value("stationId").toString().toLongLong();
    query.page = input.value("page").toInt(1);
    query.pageSize = input.value("pageSize").toInt(20);
    query.keyword = input.value("keyword").toString();
    query.status = input.value("status").toString();
    query.updateNickname = input.contains("nickname");
    query.updateAvatar = input.contains("avatarKey");
    query.nickname = input.value("nickname").toString();
    query.avatarKey = input.value("avatarKey").toString();
    query.amountCents = static_cast<qint64>(input.value("amountCents").toDouble());
    query.transactionNo = input.value("transactionNo").toString();
    query.months = input.value("months").toInt(6);
    // 批次B：normalize 缺省注入 "month"（=冻结行为），此处透传给 repo 选聚合档。
    query.period = input.value("period").toString(QStringLiteral("month"));
    // 批次E：SUBMIT_CHARGER_RATING 参数（已过 normalize 形态校验；comment
    // 缺省注入空串，未知键照例丢弃）。
    query.orderId = input.value("orderId").toString().toLongLong();
    query.rating = input.value("rating").toInt();
    query.comment = input.value("comment").toString();
    // 2026-09-09 需求批：CREDIT_LEVEL_REWARD 档位（金额在服务端推导，无传额入参）。
    query.level = input.value("level").toInt();
    query.nowUtc = clock_ ? clock_().toUTC() : QDateTime::currentDateTimeUtc();
    const UserApiResult result = repository_->execute(query);
    switch (result.error) {
    case UserApiError::None: break;
    case UserApiError::Unauthorized: return fail(error_code::kUnauthorized, QStringLiteral("请重新登录"));
    case UserApiError::Frozen: return fail(error_code::kUserFrozen, QStringLiteral("用户已被冻结"));
    case UserApiError::NotFound: return fail(error_code::kNotFound, QStringLiteral("资源不存在或不可访问"));
    case UserApiError::Conflict: return fail(error_code::kIdempotencyConflict, QStringLiteral("充值流水号与原交易不符"));
    case UserApiError::RechargeFailed: return fail(error_code::kRechargeFailed, QStringLiteral("该笔充值已失败，请重新发起充值"));
    case UserApiError::Invalid:
        reply = fail(error_code::kInvalidArgument, QStringLiteral("参数无效或余额超出上限"));
        reply.error.details.insert("field", query.action == UserApiAction::Recharge ? "amountCents" : "data");
        return reply;
    case UserApiError::TooManyRows: return fail(error_code::kInternalError, QStringLiteral("查询结果超出支持范围"));
    case UserApiError::Database: return fail(error_code::kDatabaseError, QStringLiteral("数据库操作失败，请稍后重试"));
    }
    // ---- new read actions: raw wire rows (no model::canonical counterparts) ----
    if (query.action == UserApiAction::Stats) {
        // co2 = energyWh × 0.5568 g/Wh (national grid average emission factor
        // 0.5568 tCO2/MWh). TODO(contract): factor & rounding business sign-off.
        QJsonArray months;
        for (const auto& row : result.rows) {
            QJsonObject item = wireRow(row);
            item.insert("co2Grams", qRound64(item.value("energyWh").toDouble() * 0.5568));
            months.append(item);
        }
        reply.data.insert("months", months);
        reply.success = true;
        return reply;
    }
    if (query.action == UserApiAction::Coupons) {
        QJsonArray coupons;
        for (const auto& row : result.rows) {
            QJsonObject item = wireRow(row);
            item.insert("kind", item.value("kind").toString().toLower());
            item.insert("status", item.value("status").toString().toLower());
            const QDateTime expires = QDateTime::fromString(
                item.value("expiresAt").toString(), Qt::ISODateWithMs);
            item.insert("expiresAtUtc", expires.isValid()
                ? static_cast<double>(expires.toMSecsSinceEpoch()) : 0.0);
            const qint64 threshold = item.value("thresholdCents").toVariant().toLongLong();
            item.insert("condition", threshold > 0
                ? QStringLiteral("充电满 ¥%1 可用").arg(threshold / 100.0, 0, 'f', 0)
                : QStringLiteral("无门槛"));
            coupons.append(item);
        }
        reply.data.insert("coupons", coupons);
        reply.data.insert("page", query.page);
        reply.data.insert("pageSize", query.pageSize);
        reply.data.insert("total", result.total);
        reply.success = true;
        return reply;
    }
    if (query.action == UserApiAction::Notifications) {
        QJsonArray notifications;
        for (const auto& row : result.rows) {
            QJsonObject item = wireRow(row);
            item.insert("type", item.value("type").toString().toLower());
            item.insert("createdAtUtc", item.take("createdAt"));  // page contract key
            item.remove("userId");    // never echo internal identity columns
            item.remove("readAt");    // read-state sync is phase 2, TODO(contract)
            notifications.append(item);
        }
        reply.data.insert("notifications", notifications);
        reply.data.insert("page", query.page);
        reply.data.insert("pageSize", query.pageSize);
        reply.data.insert("total", result.total);
        reply.success = true;
        return reply;
    }
    // ---- 批次C（2026-09-08）：签到 / 积分流水（无 model::canonical 对应）----
    if (query.action == UserApiAction::CheckIn) {
        // day 与 repo 写入用同一个 query.nowUtc——单点换算，不会出现
        // 响应 day 与落库 day 跨 UTC 午夜的漂移。
        reply.data.insert("day", query.nowUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd")));
        reply.data.insert("points", static_cast<double>(result.points));
        reply.data.insert("gained", static_cast<double>(result.pointsGained));
        reply.data.insert("alreadyCheckedIn", result.alreadyCheckedIn);
        reply.success = true;
        return reply;
    }
    // 2026-09-09 需求批：升级礼包入账（形同 CheckIn：总分+本次所得+重放标记）。
    if (query.action == UserApiAction::CreditLevelReward) {
        reply.data.insert("points", static_cast<double>(result.points));
        reply.data.insert("gained", static_cast<double>(result.pointsGained));
        reply.data.insert("alreadyCredited", result.alreadyCredited);
        reply.success = true;
        return reply;
    }
    if (query.action == UserApiAction::GetPoints) {
        // 响应形冻结为 {points, entries:[{id, amount, reason, createdAtUtc}],
        // page, pageSize, total}；reason 词表映射 CHECK_IN/SETTLEMENT/LEVEL_GIFT，
        // 其余运营文案原样透传（TODO(contract): 词表评审）。
        QJsonArray entries;
        for (const auto& row : result.rows) {
            QJsonObject item = wireRow(row);
            if (item.value("reason").toString() == QLatin1String("CHECK_IN"))
                item.insert("reason", QStringLiteral("每日签到"));
            else if (item.value("reason").toString() == QLatin1String("SETTLEMENT"))
                item.insert("reason", QStringLiteral("消费返积分"));
            else if (item.value("reason").toString() == QLatin1String("LEVEL_GIFT"))
                item.insert("reason", QStringLiteral("等级礼包"));
            item.insert("createdAtUtc", item.take("createdAt"));
            item.remove("userId");    // never echo internal identity columns
            entries.append(item);
        }
        reply.data.insert("points", static_cast<double>(result.points));
        reply.data.insert("entries", entries);
        reply.data.insert("page", query.page);
        reply.data.insert("pageSize", query.pageSize);
        reply.data.insert("total", result.total);
        reply.success = true;
        return reply;
    }
    // ---- 批次E（2026-09-08）：电桩评价（无 model::canonical 对应）----
    if (query.action == UserApiAction::SubmitRating) {
        // rows[0]=当前落库行（重放亦返回首评原值，alreadyRated 区分）。
        QJsonObject item = wireRow(result.rows.first());
        item.insert("createdAtUtc", item.take("createdAt"));
        item.remove("userId");    // never echo internal identity columns
        reply.data.insert("rating", item);
        reply.data.insert("alreadyRated", result.alreadyRated);
        reply.success = true;
        return reply;
    }
    if (query.action == UserApiAction::GetMyRatings) {
        // 响应形冻结为 {ratings:[{id, orderId, chargerId, chargerCode,
        // stationName, rating, comment, createdAtUtc}], page, pageSize, total}。
        QJsonArray ratings;
        for (const auto& row : result.rows) {
            QJsonObject item = wireRow(row);
            item.insert("createdAtUtc", item.take("createdAt"));
            item.remove("userId");
            ratings.append(item);
        }
        reply.data.insert("ratings", ratings);
        reply.data.insert("page", query.page);
        reply.data.insert("pageSize", query.pageSize);
        reply.data.insert("total", result.total);
        reply.success = true;
        return reply;
    }
    QJsonArray items;
    for (const auto& row : result.rows) {
        const QJsonObject source = wireRow(row);
        QJsonObject item;
        bool ok = false;
        switch (query.action) {
        case UserApiAction::Stations:
            ok = canonical<charging::model::Station>(source, &item);
            item.insert("distanceMeters", -1);
            break;
        case UserApiAction::Chargers: ok = canonical<charging::model::Charger>(source, &item); break;
        case UserApiAction::Reservations:
            ok = canonical<charging::model::Reservation>(source, &item);
            item.insert("stationName", source.value("stationName"));
            item.insert("chargerCode", source.value("chargerCode"));
            item.insert("orderId", source.value("orderId"));
            break;
        case UserApiAction::Orders:
            ok = canonical<charging::model::Order>(source, &item);
            item.insert("stationName", source.value("stationName"));
            item.insert("chargerCode", source.value("chargerCode"));
            break;
        case UserApiAction::Profile:
        case UserApiAction::UpdateProfile: ok = canonical<charging::model::User>(source, &item); break;
        case UserApiAction::Recharge:
        case UserApiAction::RechargeRecords: ok = canonical<charging::model::RechargeRecord>(source, &item); break;
        default: break;   // Stats/Coupons/Notifications returned above
        }
        if (!ok) return fail(error_code::kDatabaseError, QStringLiteral("存储的数据无效"));
        items.append(item);
    }
    if (query.action == UserApiAction::Profile || query.action == UserApiAction::UpdateProfile)
        reply.data.insert("user", items.first());
    else if (query.action == UserApiAction::Recharge) {
        reply.data.insert("record", items.first());
        reply.data.insert("balanceCents", static_cast<double>(result.balanceCents));
        reply.data.insert("idempotent", result.idempotent);
    } else {
        const QString key = query.action == UserApiAction::Stations ? "stations"
            : query.action == UserApiAction::Chargers ? "chargers"
            : query.action == UserApiAction::Reservations ? "reservations"
            : query.action == UserApiAction::Orders ? "orders" : "records";
        reply.data.insert(key, items);
        reply.data.insert("page", query.page);
        reply.data.insert("pageSize", query.pageSize);
        reply.data.insert("total", result.total);
    }
    reply.success = true;
    return reply;
}
} // namespace charging::server
