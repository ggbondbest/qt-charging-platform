#pragma once

#include "charging/common/protocol/protocol.h"

#include <QtGlobal>

namespace charging::server {

struct BillingResult
{
    bool success = false;
    qint64 durationSeconds = 0;
    qint64 energyWh = 0;
    qint64 amountCents = 0;
    charging::protocol::ProtocolError error;
};

class BillingService final
{
public:
    BillingResult calculate(int powerWatts, qint64 durationSeconds,
                            qint64 unitPriceCentsPerKwh) const;
};

} // namespace charging::server
