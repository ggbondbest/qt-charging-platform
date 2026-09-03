#pragma once

#include "charging/client/widgets/status_tag.h"

#include <QString>

class QWidget;

namespace charging::client {

// Transient top-centre notification shown over the current window.
class Toast final
{
public:
    static void show(QWidget* anchor, const QString& text,
                     StatusTag::Tone tone = StatusTag::Tone::Neutral);

    Toast() = delete;
};

} // namespace charging::client
