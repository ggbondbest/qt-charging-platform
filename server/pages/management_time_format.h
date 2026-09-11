#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QTimeZone>

namespace charging::server {

inline QString managementBeijingTime(const QString& timestamp)
{
    const auto instant = QDateTime::fromString(timestamp, Qt::ISODateWithMs);
    const QTimeZone zone("Asia/Shanghai");
    if (!instant.isValid() || !zone.isValid())
        return QStringLiteral("—");
    return instant.toTimeZone(zone).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

inline QString managementRegistrationTime(const QJsonObject& user)
{
    const auto timestamp = user.value(QStringLiteral("createdAtUtc")).toString();
    return managementBeijingTime(timestamp.isEmpty()
                                    ? user.value(QStringLiteral("createdAt")).toString()
                                    : timestamp);
}

} // namespace charging::server
