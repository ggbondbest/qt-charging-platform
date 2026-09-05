#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QtGlobal>

namespace charging::protocol {

inline constexpr int kProtocolVersion = 1;
inline constexpr quint32 kMaxPayloadBytes = 1024U * 1024U;
inline constexpr int kMaxTypeLength = 64;
inline constexpr int kMaxRequestIdLength = 64;

namespace request_type {
inline constexpr char kUserLogin[] = "USER_LOGIN";
inline constexpr char kGetStations[] = "GET_STATIONS";
inline constexpr char kGetChargers[] = "GET_CHARGERS";
inline constexpr char kGetReservations[] = "GET_RESERVATIONS";
inline constexpr char kReserveCharger[] = "RESERVE_CHARGER";
inline constexpr char kCancelReservation[] = "CANCEL_RESERVATION";
inline constexpr char kStartCharging[] = "START_CHARGING";
inline constexpr char kGetChargingStatus[] = "GET_CHARGING_STATUS";
inline constexpr char kStopCharging[] = "STOP_CHARGING";
inline constexpr char kPayOrder[] = "PAY_ORDER";
inline constexpr char kGetUserInfo[] = "GET_USER_INFO";
inline constexpr char kUpdateUserInfo[] = "UPDATE_USER_INFO";
inline constexpr char kRecharge[] = "RECHARGE";
inline constexpr char kGetRechargeRecords[] = "GET_RECHARGE_RECORDS";
inline constexpr char kGetOrders[] = "GET_ORDERS";
inline constexpr char kAdminLogin[] = "ADMIN_LOGIN";
inline constexpr char kGetDashboard[] = "GET_DASHBOARD";
inline constexpr char kGetUsers[] = "GET_USERS";
inline constexpr char kFreezeUser[] = "FREEZE_USER";
inline constexpr char kUnfreezeUser[] = "UNFREEZE_USER";
inline constexpr char kRestartCharger[] = "RESTART_CHARGER";
inline constexpr char kCreateStation[] = "CREATE_STATION";
} // namespace request_type

namespace error_code {
inline constexpr char kInvalidFrame[] = "INVALID_FRAME";
inline constexpr char kPayloadTooLarge[] = "PAYLOAD_TOO_LARGE";
inline constexpr char kInvalidJson[] = "INVALID_JSON";
inline constexpr char kInvalidEnvelope[] = "INVALID_ENVELOPE";
inline constexpr char kInvalidArgument[] = "INVALID_ARGUMENT";
inline constexpr char kIdempotencyConflict[] = "IDEMPOTENCY_CONFLICT";
inline constexpr char kRechargeFailed[] = "RECHARGE_FAILED";
inline constexpr char kUnsupportedProtocolVersion[] = "UNSUPPORTED_PROTOCOL_VERSION";
inline constexpr char kUnknownRequestType[] = "UNKNOWN_REQUEST_TYPE";
inline constexpr char kInvalidPhone[] = "INVALID_PHONE";
inline constexpr char kUserFrozen[] = "USER_FROZEN";
inline constexpr char kUnauthorized[] = "UNAUTHORIZED";
inline constexpr char kChargerNotAvailable[] = "CHARGER_NOT_AVAILABLE";
inline constexpr char kInvalidStateTransition[] = "INVALID_STATE_TRANSITION";
inline constexpr char kInsufficientBalance[] = "INSUFFICIENT_BALANCE";
inline constexpr char kNotFound[] = "NOT_FOUND";
inline constexpr char kDatabaseError[] = "DATABASE_ERROR";
inline constexpr char kInternalError[] = "INTERNAL_ERROR";
inline constexpr char kConnectionError[] = "CONNECTION_ERROR";
inline constexpr char kRequestTimeout[] = "REQUEST_TIMEOUT";
} // namespace error_code

enum class MessageKind
{
    Request,
    Response,
    Event,
};

struct ProtocolError
{
    QString code;
    QString message;
    QJsonObject details;

    bool isEmpty() const
    {
        return code.isEmpty();
    }
};

struct RequestEnvelope
{
    int protocolVersion = kProtocolVersion;
    MessageKind kind = MessageKind::Request;
    QString type;
    QString requestId;
    QJsonObject data;
};

struct ResponseEnvelope
{
    int protocolVersion = kProtocolVersion;
    MessageKind kind = MessageKind::Response;
    QString type;
    QString requestId;
    bool success = false;
    QJsonObject data;
    ProtocolError error;
};

QString toString(MessageKind kind);
bool messageKindFromString(const QString& text, MessageKind* outValue);

QJsonObject toJson(const RequestEnvelope& request);
QJsonObject toJson(const ResponseEnvelope& response);

QByteArray serializePayload(const RequestEnvelope& request);
QByteArray serializePayload(const ResponseEnvelope& response);

// Decoders validate the complete v1 envelope. Unknown extra keys are ignored.
// On failure, outValue is unchanged and error receives a stable error code.
bool parseRequestPayload(const QByteArray& payload, RequestEnvelope* outValue,
                         ProtocolError* error = nullptr);
bool parseResponsePayload(const QByteArray& payload, ResponseEnvelope* outValue,
                          ProtocolError* error = nullptr);

ResponseEnvelope makeSuccessResponse(const RequestEnvelope& request, const QJsonObject& data = {});
ResponseEnvelope makeErrorResponse(const RequestEnvelope& request, const ProtocolError& error);

} // namespace charging::protocol
