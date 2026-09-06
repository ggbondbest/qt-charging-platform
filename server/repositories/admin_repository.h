#pragma once

#include <QJsonObject>
#include <QSqlDatabase>
#include <QVariantList>
#include <stdexcept>

namespace charging::server {

// Internal failure category only; SQL diagnostics never leave this layer.
struct AdminFailure : std::runtime_error
{
    explicit AdminFailure(const char* code) : std::runtime_error(code) {}
};

class AdminRepository final
{
public:
    explicit AdminRepository(const QSqlDatabase& database);
    QJsonObject credentials(const QString& username) const;
    QJsonObject credentials(qint64 id) const;
    void recordLogin(qint64 id) const;
    QJsonObject read(const QString& entity, const QJsonObject& parameters) const;
    QJsonObject dashboard(int days) const;
    // Rechecks account status inside the write transaction. Audit + mutation +
    // durable idempotency result either all commit or all roll back.
    QJsonObject mutate(qint64 adminId, const QString& credentialStamp, const QString& action,
                       const QJsonObject& parameters) const;

private:
    QJsonObject readRows(const QString& entity, const QJsonObject& parameters) const;
    QJsonObject credentialQuery(const QString& predicate, const QVariant& value) const;
    QSqlDatabase database_;
};

} // namespace charging::server
