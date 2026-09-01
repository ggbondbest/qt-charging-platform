#pragma once

#include "charging/common/protocol/protocol.h"

#include <QByteArray>
#include <QList>
#include <QtGlobal>

namespace charging::protocol {

// Produces: 4-byte unsigned big-endian payload length + raw payload bytes.
bool encodeFrame(const QByteArray& payload, QByteArray* outFrame, ProtocolError* error = nullptr,
                 quint32 maxPayloadBytes = kMaxPayloadBytes);

// Stateful TCP stream decoder. append() supports split headers, split bodies,
// and multiple frames in one read. A framing error resets the decoder; callers
// should close that socket because stream resynchronisation is not reliable.
class FrameDecoder final
{
public:
    explicit FrameDecoder(quint32 maxPayloadBytes = kMaxPayloadBytes);

    bool append(const QByteArray& bytes, QList<QByteArray>* completedPayloads,
                ProtocolError* error = nullptr);

    void reset();
    qint64 bufferedByteCount() const;
    quint32 maxPayloadBytes() const;

private:
    QByteArray buffer_;
    quint32 expectedPayloadBytes_ = 0;
    quint32 maxPayloadBytes_ = kMaxPayloadBytes;
};

} // namespace charging::protocol
