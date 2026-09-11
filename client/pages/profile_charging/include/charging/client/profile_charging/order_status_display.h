#pragma once

#include "charging/client/widgets/status_tag.h"
#include "charging/common/model/enums.h"

#include <QString>

namespace charging::client {

// Presentation-only mapping from the shared OrderStatus enum to the
// Chinese label and pill tone used across order pages. Business decisions
// (can this order be paid, is it terminal) stay server-side; this file only
// decides how a server-provided status is displayed.
struct OrderStatusDisplay
{
    QString text;
    StatusTag::Tone tone = StatusTag::Tone::Neutral;
};

OrderStatusDisplay orderStatusDisplay(charging::model::OrderStatus status);

} // namespace charging::client
