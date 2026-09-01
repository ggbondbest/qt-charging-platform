#include "charging/common/protocol/protocol.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include <cmath>

namespace charging::protocol {

namespace {

void clearError(ProtocolError* error)
{
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(ProtocolError* error, const char* code, const QString& message,
          const QJsonObject& details = {})
{
    if (error != nullptr) {
        error->code = QString::fromLatin1(code);
        error->message = message;
        error->details = details;
    }
    return false;
}

bool parseRootObject(const QByteArray& payload, QJsonObject* outObject, ProtocolError* error)
{
    if (payload.size() > static_cast<int>(kMaxPayloadBytes)) {
        return fail(error, error_code::kPayloadTooLarge,
                    QStringLiteral("JSON payload exceeds the v1 size limit"));
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        QJsonObject details;
        details.insert(QStringLiteral("offset"), static_cast<double>(parseError.offset));
        return fail(error, error_code::kInvalidJson,
                    QStringLiteral("Payload is not valid JSON: %1").arg(parseError.errorString()),
                    details);
    }
    if (!document.isObject()) {
        return fail(error, error_code::kInvalidEnvelope,
                    QStringLiteral("The JSON root must be an object"));
    }
    *outObject = document.object();
    return true;
}

bool readProtocolVersion(const QJsonObject& object, int* outValue, ProtocolError* error)
{
    const QJsonValue value = object.value(QStringLiteral("protocolVersion"));
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble()) != value.toDouble()) {
        return fail(error, error_code::kInvalidEnvelope,
                    QStringLiteral("'protocolVersion' must be an integer"));
    }
    const int version = value.toInt(-1);
    if (version != kProtocolVersion) {
        QJsonObject details;
        details.insert(QStringLiteral("supportedVersion"), kProtocolVersion);
        details.insert(QStringLiteral("receivedVersion"), version);
        return fail(error, error_code::kUnsupportedProtocolVersion,
                    QStringLiteral("Unsupported protocol version"), details);
    }
    *outValue = version;
    return true;
}

bool readKind(const QJsonObject& object, MessageKind expected, MessageKind* outValue,
              ProtocolError* error)
{
    const QJsonValue value = object.value(QStringLiteral("kind"));
    MessageKind kind = MessageKind::Request;
    if (!value.isString() || !messageKindFromString(value.toString(), &kind)) {
        return fail(error, error_code::kInvalidEnvelope,
                    QStringLiteral("'kind' contains an unknown message kind"));
    }
    if (kind != expected) {
        return fail(error, error_code::kInvalidEnvelope,
                    QStringLiteral("Unexpected message kind '%1'").arg(toString(kind)));
    }
    *outValue = kind;
    return true;
}

bool readBoundedString(const QJsonObject& object, const char* key, int maximumLength,
                       QString* outValue, ProtocolError* error)
{
    const QString fieldName = QString::fromLatin1(key);
    const QJsonValue value = object.value(fieldName);
    if (!value.isString() || value.toString().isEmpty() ||
        value.toString().size() > maximumLength) {
        return fail(error, error_code::kInvalidEnvelope,
                    QStringLiteral("'%1' must be a non-empty string of at most %2 characters")
                        .arg(fieldName)
                        .arg(maximumLength));
    }
    *outValue = value.toString();
    return true;
}

bool readData(const QJsonObject& object, QJsonObject* outValue, ProtocolError* error)
{
    const QJsonValue value = object.value(QStringLiteral("data"));
    if (!value.isObject()) {
        return fail(error, error_code::kInvalidEnvelope,
                    QStringLiteral("'data' must be a JSON object"));
    }
    *outValue = value.toObject();
    return true;
}

QJsonObject errorToJson(const ProtocolError& error)
{
    QJsonObject object;
    object.insert(QStringLiteral("code"), error.code);
    object.insert(QStringLiteral("message"), error.message);
    object.insert(QStringLiteral("details"), error.details);
    return object;
}

bool readResponseError(const QJsonValue& value, bool success, ProtocolError* outValue,
                       ProtocolError* parseError)
{
    if (success) {
        if (!value.isNull()) {
            return fail(parseError, error_code::kInvalidEnvelope,
                        QStringLiteral("A successful response must set 'error' to null"));
        }
        *outValue = {};
        return true;
    }

    if (!value.isObject()) {
        return fail(parseError, error_code::kInvalidEnvelope,
                    QStringLiteral("A failed response must contain an 'error' object"));
    }
    const QJsonObject object = value.toObject();
    const QJsonValue code = object.value(QStringLiteral("code"));
    const QJsonValue message = object.value(QStringLiteral("message"));
    const QJsonValue details = object.value(QStringLiteral("details"));
    if (!code.isString() || code.toString().isEmpty() || !message.isString() ||
        !details.isObject()) {
        return fail(
            parseError, error_code::kInvalidEnvelope,
            QStringLiteral("'error' requires non-empty code, string message, and object details"));
    }
    outValue->code = code.toString();
    outValue->message = message.toString();
    outValue->details = details.toObject();
    return true;
}

} // namespace

QString toString(MessageKind kind)
{
    switch (kind) {
    case MessageKind::Request:
        return QStringLiteral("REQUEST");
    case MessageKind::Response:
        return QStringLiteral("RESPONSE");
    case MessageKind::Event:
        return QStringLiteral("EVENT");
    }
    return {};
}

bool messageKindFromString(const QString& text, MessageKind* outValue)
{
    if (outValue == nullptr) {
        return false;
    }
    if (text == QStringLiteral("REQUEST")) {
        *outValue = MessageKind::Request;
        return true;
    }
    if (text == QStringLiteral("RESPONSE")) {
        *outValue = MessageKind::Response;
        return true;
    }
    if (text == QStringLiteral("EVENT")) {
        *outValue = MessageKind::Event;
        return true;
    }
    return false;
}

QJsonObject toJson(const RequestEnvelope& request)
{
    QJsonObject object;
    object.insert(QStringLiteral("protocolVersion"), request.protocolVersion);
    object.insert(QStringLiteral("kind"), toString(request.kind));
    object.insert(QStringLiteral("type"), request.type);
    object.insert(QStringLiteral("requestId"), request.requestId);
    object.insert(QStringLiteral("data"), request.data);
    return object;
}

QJsonObject toJson(const ResponseEnvelope& response)
{
    QJsonObject object;
    object.insert(QStringLiteral("protocolVersion"), response.protocolVersion);
    object.insert(QStringLiteral("kind"), toString(response.kind));
    object.insert(QStringLiteral("type"), response.type);
    object.insert(QStringLiteral("requestId"), response.requestId);
    object.insert(QStringLiteral("success"), response.success);
    object.insert(QStringLiteral("data"), response.data);
    if (response.success) {
        object.insert(QStringLiteral("error"), QJsonValue::Null);
    } else {
        object.insert(QStringLiteral("error"), errorToJson(response.error));
    }
    return object;
}

QByteArray serializePayload(const RequestEnvelope& request)
{
    return QJsonDocument(toJson(request)).toJson(QJsonDocument::Compact);
}

QByteArray serializePayload(const ResponseEnvelope& response)
{
    return QJsonDocument(toJson(response)).toJson(QJsonDocument::Compact);
}

bool parseRequestPayload(const QByteArray& payload, RequestEnvelope* outValue, ProtocolError* error)
{
    clearError(error);
    if (outValue == nullptr) {
        return fail(error, error_code::kInternalError,
                    QStringLiteral("Request output pointer is null"));
    }

    QJsonObject object;
    RequestEnvelope request;
    if (!parseRootObject(payload, &object, error) ||
        !readProtocolVersion(object, &request.protocolVersion, error) ||
        !readKind(object, MessageKind::Request, &request.kind, error) ||
        !readBoundedString(object, "type", kMaxTypeLength, &request.type, error) ||
        !readBoundedString(object, "requestId", kMaxRequestIdLength, &request.requestId, error) ||
        !readData(object, &request.data, error)) {
        return false;
    }
    *outValue = request;
    return true;
}

bool parseResponsePayload(const QByteArray& payload, ResponseEnvelope* outValue,
                          ProtocolError* error)
{
    clearError(error);
    if (outValue == nullptr) {
        return fail(error, error_code::kInternalError,
                    QStringLiteral("Response output pointer is null"));
    }

    QJsonObject object;
    ResponseEnvelope response;
    if (!parseRootObject(payload, &object, error) ||
        !readProtocolVersion(object, &response.protocolVersion, error) ||
        !readKind(object, MessageKind::Response, &response.kind, error) ||
        !readBoundedString(object, "type", kMaxTypeLength, &response.type, error) ||
        !readBoundedString(object, "requestId", kMaxRequestIdLength, &response.requestId, error) ||
        !object.value(QStringLiteral("success")).isBool() ||
        !readData(object, &response.data, error)) {
        if (error != nullptr && error->isEmpty()) {
            fail(error, error_code::kInvalidEnvelope,
                 QStringLiteral("'success' must be a boolean"));
        }
        return false;
    }

    response.success = object.value(QStringLiteral("success")).toBool();
    if (!readResponseError(object.value(QStringLiteral("error")), response.success, &response.error,
                           error)) {
        return false;
    }
    *outValue = response;
    return true;
}

ResponseEnvelope makeSuccessResponse(const RequestEnvelope& request, const QJsonObject& data)
{
    ResponseEnvelope response;
    response.type = request.type;
    response.requestId = request.requestId;
    response.success = true;
    response.data = data;
    return response;
}

ResponseEnvelope makeErrorResponse(const RequestEnvelope& request, const ProtocolError& error)
{
    ResponseEnvelope response;
    response.type = request.type;
    response.requestId = request.requestId;
    response.success = false;
    response.error = error;
    return response;
}

} // namespace charging::protocol
