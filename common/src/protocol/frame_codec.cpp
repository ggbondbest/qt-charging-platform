#include "charging/common/protocol/frame_codec.h"

#include <limits>

namespace charging::protocol {

namespace {

void clearError(ProtocolError* error)
{
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(ProtocolError* error, const char* code, const QString& message)
{
    if (error != nullptr) {
        error->code = QString::fromLatin1(code);
        error->message = message;
        error->details = {};
    }
    return false;
}

quint32 readBigEndianLength(const QByteArray& bytes)
{
    const auto byte0 = static_cast<quint8>(bytes.at(0));
    const auto byte1 = static_cast<quint8>(bytes.at(1));
    const auto byte2 = static_cast<quint8>(bytes.at(2));
    const auto byte3 = static_cast<quint8>(bytes.at(3));
    return (static_cast<quint32>(byte0) << 24U) | (static_cast<quint32>(byte1) << 16U) |
           (static_cast<quint32>(byte2) << 8U) | static_cast<quint32>(byte3);
}

} // namespace

bool encodeFrame(const QByteArray& payload, QByteArray* outFrame, ProtocolError* error,
                 quint32 maxPayloadBytes)
{
    clearError(error);
    if (outFrame == nullptr) {
        return fail(error, error_code::kInternalError,
                    QStringLiteral("Frame output pointer is null"));
    }
    outFrame->clear();
    if (payload.isEmpty()) {
        return fail(error, error_code::kInvalidFrame,
                    QStringLiteral("A frame payload must not be empty"));
    }
    if (maxPayloadBytes == 0 || payload.size() > static_cast<qint64>(maxPayloadBytes)) {
        return fail(error, error_code::kPayloadTooLarge,
                    QStringLiteral("Frame payload exceeds the configured size limit"));
    }

    const quint32 length = static_cast<quint32>(payload.size());
    QByteArray frame;
    frame.resize(4);
    frame[0] = static_cast<char>((length >> 24U) & 0xffU);
    frame[1] = static_cast<char>((length >> 16U) & 0xffU);
    frame[2] = static_cast<char>((length >> 8U) & 0xffU);
    frame[3] = static_cast<char>(length & 0xffU);
    frame.append(payload);
    *outFrame = frame;
    return true;
}

FrameDecoder::FrameDecoder(quint32 maxPayloadBytes) : maxPayloadBytes_(maxPayloadBytes)
{
    const quint32 maximumByteArraySize = static_cast<quint32>(std::numeric_limits<int>::max());
    if (maxPayloadBytes_ == 0 || maxPayloadBytes_ > maximumByteArraySize) {
        maxPayloadBytes_ = kMaxPayloadBytes;
    }
}

bool FrameDecoder::append(const QByteArray& bytes, QList<QByteArray>* completedPayloads,
                          ProtocolError* error)
{
    clearError(error);
    if (completedPayloads == nullptr) {
        return fail(error, error_code::kInternalError,
                    QStringLiteral("Completed payload output pointer is null"));
    }

    buffer_.append(bytes);
    while (true) {
        if (expectedPayloadBytes_ == 0) {
            if (buffer_.size() < 4) {
                return true;
            }

            expectedPayloadBytes_ = readBigEndianLength(buffer_);
            buffer_.remove(0, 4);
            if (expectedPayloadBytes_ == 0) {
                reset();
                return fail(error, error_code::kInvalidFrame,
                            QStringLiteral("Zero-length frames are not allowed"));
            }
            if (expectedPayloadBytes_ > maxPayloadBytes_) {
                reset();
                return fail(error, error_code::kPayloadTooLarge,
                            QStringLiteral("Incoming frame exceeds the configured size limit"));
            }
        }

        if (buffer_.size() < static_cast<qint64>(expectedPayloadBytes_)) {
            return true;
        }

        completedPayloads->append(buffer_.left(static_cast<int>(expectedPayloadBytes_)));
        buffer_.remove(0, static_cast<int>(expectedPayloadBytes_));
        expectedPayloadBytes_ = 0;
    }
}

void FrameDecoder::reset()
{
    buffer_.clear();
    expectedPayloadBytes_ = 0;
}

qint64 FrameDecoder::bufferedByteCount() const
{
    return buffer_.size();
}

quint32 FrameDecoder::maxPayloadBytes() const
{
    return maxPayloadBytes_;
}

} // namespace charging::protocol
