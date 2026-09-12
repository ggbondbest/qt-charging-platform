#pragma once

#include <QDateTime>

#include <functional>

namespace charging::server {

using UtcClock = std::function<QDateTime()>;

inline QDateTime utcNow(const UtcClock& clock)
{
    return clock ? clock().toUTC() : QDateTime::currentDateTimeUtc();
}

} // namespace charging::server
