#include "user_api_service.h"
#include "user_api_repository.h"
#include "charging/common/model/model_json.h"
#include "charging/common/protocol/user_api_contract.h"
#include <QJsonArray>
#include <QSet>

namespace charging::server {
namespace {
using namespace charging::protocol;
const QMap<QString, UserApiAction> actions{
    {request_type::kGetStations, UserApiAction::Stations},
    {request_type::kGetChargers, UserApiAction::Chargers},
    {request_type::kGetReservations, UserApiAction::Reservations},
    {request_type::kGetUserInfo, UserApiAction::Profile},
    {request_type::kUpdateUserInfo, UserApiAction::UpdateProfile},
    {request_type::kRecharge, UserApiAction::Recharge},
    {request_type::kGetRechargeRecords, UserApiAction::RechargeRecords},
    {request_type::kGetOrders, UserApiAction::Orders}
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
    const QSet<QString> avatars{"", "bolt", "plug", "car", "leaf", "cat", "panda", "moon", "rocket"};
    if (input.contains("avatarKey") && !avatars.contains(input.value("avatarKey").toString())) {
        reply = fail(error_code::kInvalidArgument, QStringLiteral("请选择内置头像"));
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
