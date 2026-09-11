#include "charging/client/profile_charging/stats_service.h"

#include "charging/common/protocol/protocol.h"

#include <QJsonArray>

namespace charging::client {

StatsService::StatsService(IRequestTransport* transport, QObject* parent)
    : QObject(parent), transport_(transport)
{
}

bool StatsService::isFetchingStats() const
{
    return fetching_;
}

void StatsService::fetchStats(int months, const QString& period)
{
    if (transport_ == nullptr || fetching_) {
        return;
    }
    fetching_ = true;
    const QString type =
        QString::fromLatin1(charging::protocol::request_type::kGetUserStats);
    transport_->sendFor(this, type, {{QStringLiteral("months"), months},
                            {QStringLiteral("period"), period}},
                     [this, type](bool ok, const QJsonObject& data,
                                  const charging::protocol::ProtocolError& error) {
                         fetching_ = false;
                         if (!ok) {
                             emit operationFailed(type, error);
                             return;
                         }
                         emit statsLoaded(
                             data.value(QStringLiteral("months")).toArray().toVariantList());
                     });
}

} // namespace charging::client
