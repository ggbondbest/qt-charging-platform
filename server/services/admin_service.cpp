#include "admin_service.h"

#include "admin_repository.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <cmath>

namespace charging::server {
namespace {
QString stamp(const QJsonObject& c)
{
    return c.value(QStringLiteral("password_algorithm")).toString() + QLatin1Char(':') +
           c.value(QStringLiteral("password_salt")).toString() + QLatin1Char(':') +
           c.value(QStringLiteral("password_hash")).toString();
}
void require(bool value)
{
    if (!value)
        throw AdminFailure("INVALID_ARGUMENT");
}
void textField(const QJsonObject& p, const QString& key, int minimum, int maximum)
{
    const auto v = p.value(key);
    require(v.isString() && v.toString() == v.toString().trimmed() &&
            v.toString().size() >= minimum && v.toString().size() <= maximum &&
            !v.toString().contains(QChar(0)));
}
void integer(const QJsonObject& p, const QString& key, double minimum, double maximum)
{
    const auto v = p.value(key);
    require(v.isDouble() && std::isfinite(v.toDouble()) &&
            std::floor(v.toDouble()) == v.toDouble() && v.toDouble() >= minimum &&
            v.toDouble() <= maximum);
}
void id(const QJsonObject& p, const QString& key)
{
    const auto v = p.value(key);
    bool ok = false;
    const qint64 number = v.toString().toLongLong(&ok);
    require(v.isString() && ok && number > 0 && QString::number(number) == v.toString());
}
void choice(const QJsonObject& p, const QString& key, const QStringList& values)
{
    require(p.value(key).isString() && values.contains(p.value(key).toString()));
}
void fields(const QJsonObject& p, const QStringList& allowed)
{
    for (auto it = p.begin(); it != p.end(); ++it)
        require(allowed.contains(it.key()));
}
void stationFields(const QJsonObject& p)
{
    textField(p, QStringLiteral("name"), 1, 64);
    textField(p, QStringLiteral("address"), 1, 255);
    for (const auto& key : {QStringLiteral("latitude"), QStringLiteral("longitude")}) {
        const auto v = p.value(key);
        const double bound = key == QStringLiteral("latitude") ? 90 : 180;
        require(v.isDouble() && std::isfinite(v.toDouble()) && std::abs(v.toDouble()) <= bound);
    }
    integer(p, QStringLiteral("priceCentsPerKwh"), 0, 9007199254740991.0);
}
QJsonObject success(const QJsonObject& data)
{
    return {{QStringLiteral("success"), true}, {QStringLiteral("data"), data}};
}
} // namespace

AdminService::AdminService(AdminRepository* repository, qint64 idleTimeoutMs)
    : repository_(repository), idleTimeoutMs_(idleTimeoutMs)
{
    clock_.start();
}

QJsonObject AdminService::failure(const QString& code)
{
    static const QHash<QString, QString> messages{
        {QStringLiteral("INVALID_CREDENTIALS"), QStringLiteral("账号或密码不正确，或账号不可用")},
        {QStringLiteral("UNAUTHORIZED"), QStringLiteral("管理员会话失效，请重新登录")},
        {QStringLiteral("RATE_LIMITED"), QStringLiteral("登录尝试过于频繁，请稍后重试")},
        {QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("请求参数不合法")},
        {QStringLiteral("NOT_FOUND"), QStringLiteral("记录不存在")},
        {QStringLiteral("RESOURCE_BUSY"),
         QStringLiteral("存在活动预约或未结束业务，无法执行此操作")},
        {QStringLiteral("CONFLICT"), QStringLiteral("数据已变更或操作编号冲突，请刷新核对")},
        {QStringLiteral("INVALID_STATE_TRANSITION"), QStringLiteral("当前状态不允许此操作")},
        {QStringLiteral("TIMEOUT"),
         QStringLiteral("请求超时；写操作结果未知，请使用原操作编号核对")},
        {QStringLiteral("UNAVAILABLE"), QStringLiteral("服务尚未就绪或已停止")},
        {QStringLiteral("DATABASE_ERROR"), QStringLiteral("数据操作失败，请稍后重试")}};
    const QString safeCode = messages.contains(code) ? code : QStringLiteral("DATABASE_ERROR");
    return {{QStringLiteral("success"), false},
            {QStringLiteral("error"),
             QJsonObject{{QStringLiteral("code"), safeCode},
                         {QStringLiteral("message"), messages.value(safeCode)}}}};
}

QJsonObject AdminService::login(const QJsonObject& p, const QString& channel)
{
    fields(p, {QStringLiteral("username"), QStringLiteral("password")});
    textField(p, QStringLiteral("username"), 3, 32);
    require(p.value(QStringLiteral("password")).isString() &&
            !p.value(QStringLiteral("password")).toString().isEmpty() &&
            p.value(QStringLiteral("password")).toString().size() <= 256);
    const qint64 now = clock_.elapsed();
    if (now < blockedUntil_)
        return failure(QStringLiteral("RATE_LIMITED"));
    const auto c = repository_->credentials(p.value(QStringLiteral("username")).toString());
    // Compatibility with the existing seed/schema contract. No plaintext
    // password, salt or hash is returned in the public administrator DTO.
    const auto actual =
        QCryptographicHash::hash((c.value(QStringLiteral("password_salt")).toString() +
                                  QLatin1Char(':') + p.value(QStringLiteral("password")).toString())
                                     .toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex();
    const auto expected = c.value(QStringLiteral("password_hash")).toString().toLatin1();
    unsigned char difference = expected.size() == actual.size() ? 0 : 1;
    for (int i = 0; i < actual.size(); ++i)
        difference |=
            static_cast<unsigned char>(actual.at(i) ^ (i < expected.size() ? expected.at(i) : 0));
    if (difference || c.value(QStringLiteral("status")).toString() != QStringLiteral("ACTIVE") ||
        c.value(QStringLiteral("password_algorithm")).toString() !=
            QStringLiteral("SHA256_SALTED")) {
        if (++loginFailures_ >= 5) {
            blockedUntil_ = now + 30000;
            loginFailures_ = 0;
        }
        return failure(QStringLiteral("INVALID_CREDENTIALS"));
    }
    if (sessions_.size() >= 32)
        return failure(QStringLiteral("RATE_LIMITED"));
    loginFailures_ = 0;
    const qint64 adminId = c.value(QStringLiteral("id")).toString().toLongLong();
    repository_->recordLogin(adminId);
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sessions_.insert(token, {adminId, stamp(c), now, now, channel});
    const QJsonObject admin{
        {QStringLiteral("id"), QString::number(adminId)},
        {QStringLiteral("username"), c.value(QStringLiteral("username"))},
        {QStringLiteral("displayName"), c.value(QStringLiteral("display_name"))},
        {QStringLiteral("permission"), QStringLiteral("ADMIN")}};
    return success({{QStringLiteral("sessionToken"), token}, {QStringLiteral("admin"), admin}});
}

QJsonObject AdminService::handle(const QString& action, const QJsonObject& p, const QString& token,
                                 const QString& channel)
{
    try {
        const qint64 now = clock_.elapsed();
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (now - it->touched >= idleTimeoutMs_ || now - it->created >= 8 * 60 * 60 * 1000)
                it = sessions_.erase(it);
            else
                ++it;
        }
        if (action == QStringLiteral("auth.close")) {
            require(!channel.isEmpty());
            for (auto it = sessions_.begin(); it != sessions_.end();) {
                if (it->channel == channel)
                    it = sessions_.erase(it);
                else
                    ++it;
            }
            return success({});
        }
        if (action == QStringLiteral("auth.logout")) {
            sessions_.remove(token);
            return success({});
        }
        if (action == QStringLiteral("auth.login")) {
            sessions_.remove(token);
            return login(p, channel);
        }
        auto session = sessions_.find(token);
        if (session == sessions_.end())
            return failure(QStringLiteral("UNAUTHORIZED"));
        if (session->channel != channel)
            return failure(QStringLiteral("UNAUTHORIZED"));
        const auto c = repository_->credentials(session->adminId);
        if (c.value(QStringLiteral("status")).toString() != QStringLiteral("ACTIVE") ||
            stamp(c) != session->credentialStamp) {
            sessions_.erase(session);
            return failure(QStringLiteral("UNAUTHORIZED"));
        }
        if (action == QStringLiteral("auth.check")) {
            fields(p, {});
            return success({});
        }
        session->touched = now;
        if (action == QStringLiteral("dashboard.get")) {
            fields(p, {QStringLiteral("days")});
            if (p.contains(QStringLiteral("days")))
                integer(p, QStringLiteral("days"), 7, 30);
            const int days = p.value(QStringLiteral("days")).toInt(7);
            require(days == 7 || days == 30);
            return success(repository_->dashboard(days));
        }
        const auto parts = action.split(QLatin1Char('.'));
        require(parts.size() == 2);
        const QString entity = parts.first(), operation = parts.last();
        if (operation == QStringLiteral("list") || operation == QStringLiteral("get")) {
            require(QStringList{QStringLiteral("stations"), QStringLiteral("chargers"),
                                QStringLiteral("users"), QStringLiteral("orders"),
                                QStringLiteral("recharges")}
                        .contains(entity));
            if (operation == QStringLiteral("get")) {
                fields(p, {QStringLiteral("id")});
                id(p, QStringLiteral("id"));
            } else {
                QStringList allowed{QStringLiteral("keyword"), QStringLiteral("status"),
                                    QStringLiteral("page"), QStringLiteral("pageSize"),
                                    QStringLiteral("sort")};
                if (entity == QStringLiteral("chargers"))
                    allowed << QStringLiteral("stationId") << QStringLiteral("type");
                if (entity == QStringLiteral("orders") || entity == QStringLiteral("recharges"))
                    allowed << QStringLiteral("userId");
                fields(p, allowed);
                if (p.contains(QStringLiteral("keyword")))
                    textField(p, QStringLiteral("keyword"), 0, 64);
                if (p.contains(QStringLiteral("page")))
                    integer(p, QStringLiteral("page"), 1, 1000000);
                if (p.contains(QStringLiteral("pageSize")))
                    integer(p, QStringLiteral("pageSize"), 1, 100);
                if (p.contains(QStringLiteral("sort")))
                    choice(p, QStringLiteral("sort"),
                           {QStringLiteral("idAsc"), QStringLiteral("idDesc")});
                for (const auto& key : {QStringLiteral("stationId"), QStringLiteral("userId")})
                    if (p.contains(key))
                        id(p, key);
                if (p.contains(QStringLiteral("type")))
                    choice(p, QStringLiteral("type"),
                           {QStringLiteral("FAST"), QStringLiteral("SLOW")});
                if (p.contains(QStringLiteral("status"))) {
                    const QHash<QString, QStringList> statuses{
                        {QStringLiteral("stations"),
                         {QStringLiteral("ACTIVE"), QStringLiteral("INACTIVE")}},
                        {QStringLiteral("chargers"),
                         {QStringLiteral("AVAILABLE"), QStringLiteral("RESERVED"),
                          QStringLiteral("CHARGING"), QStringLiteral("FAULT"),
                          QStringLiteral("OFFLINE")}},
                        {QStringLiteral("users"),
                         {QStringLiteral("ACTIVE"), QStringLiteral("FROZEN")}},
                        {QStringLiteral("orders"),
                         {QStringLiteral("RESERVED"), QStringLiteral("CHARGING"),
                          QStringLiteral("WAITING_PAYMENT"), QStringLiteral("COMPLETED"),
                          QStringLiteral("CANCELLED")}},
                        {QStringLiteral("recharges"),
                         {QStringLiteral("SUCCESS"), QStringLiteral("FAILED")}}};
                    choice(p, QStringLiteral("status"), statuses.value(entity));
                }
            }
            return success(repository_->read(entity, p));
        }
        require(QStringList{QStringLiteral("station.create"), QStringLiteral("station.edit"),
                            QStringLiteral("station.status"), QStringLiteral("user.status"),
                            QStringLiteral("charger.status"), QStringLiteral("charger.restart")}
                    .contains(action));
        textField(p, QStringLiteral("operationId"), 1, 64);
        require(QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]+$"))
                    .match(p.value(QStringLiteral("operationId")).toString())
                    .hasMatch());
        QStringList allowed{QStringLiteral("operationId")};
        if (action != QStringLiteral("station.create")) {
            allowed << QStringLiteral("id") << QStringLiteral("expectedUpdatedAt");
            id(p, QStringLiteral("id"));
            textField(p, QStringLiteral("expectedUpdatedAt"), 1, 40);
        }
        if (action == QStringLiteral("station.create") ||
            action == QStringLiteral("station.edit")) {
            allowed << QStringLiteral("name") << QStringLiteral("address")
                    << QStringLiteral("latitude") << QStringLiteral("longitude")
                    << QStringLiteral("priceCentsPerKwh");
            stationFields(p);
            if (action == QStringLiteral("station.create")) {
                allowed << QStringLiteral("code") << QStringLiteral("chargers");
                textField(p, QStringLiteral("code"), 1, 32);
                require(p.value(QStringLiteral("chargers")).isArray());
                const auto chargers = p.value(QStringLiteral("chargers")).toArray();
                require(!chargers.isEmpty() && chargers.size() <= 100);
                QSet<QString> codes;
                for (const auto& value : chargers) {
                    require(value.isObject());
                    const auto charger = value.toObject();
                    fields(charger, {QStringLiteral("code"), QStringLiteral("type"),
                                     QStringLiteral("powerWatts")});
                    textField(charger, QStringLiteral("code"), 1, 32);
                    choice(charger, QStringLiteral("type"),
                           {QStringLiteral("FAST"), QStringLiteral("SLOW")});
                    integer(charger, QStringLiteral("powerWatts"), 1, 1000000);
                    const QString code = charger.value(QStringLiteral("code")).toString();
                    require(!codes.contains(code));
                    codes.insert(code);
                }
            }
        } else if (action != QStringLiteral("charger.restart")) {
            allowed << QStringLiteral("status");
            if (entity == QStringLiteral("station"))
                choice(p, QStringLiteral("status"),
                       {QStringLiteral("ACTIVE"), QStringLiteral("INACTIVE")});
            else if (entity == QStringLiteral("user"))
                choice(p, QStringLiteral("status"),
                       {QStringLiteral("ACTIVE"), QStringLiteral("FROZEN")});
            else
                choice(p, QStringLiteral("status"),
                       {QStringLiteral("FAULT"), QStringLiteral("OFFLINE")});
        }
        fields(p, allowed);
        return success(repository_->mutate(session->adminId, session->credentialStamp, action, p));
    } catch (const AdminFailure& failure) {
        return AdminService::failure(QString::fromLatin1(failure.what()));
    } catch (...) {
        return failure(QStringLiteral("DATABASE_ERROR"));
    }
}
} // namespace charging::server
