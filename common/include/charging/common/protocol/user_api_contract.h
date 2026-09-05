#pragma once

#include "charging/common/protocol/protocol.h"

namespace charging::protocol::user_api {

inline constexpr int kDefaultPage = 1;
inline constexpr int kDefaultPageSize = 20;
inline constexpr int kMaximumPageSize = 100;
inline constexpr int kMaximumPage = 2147483647;
inline constexpr qint64 kMaximumRechargeCents = 10000000;

// Validates only the eight user API request-data contracts documented in
// docs/api/user_api_contract.md. This does NOT authenticate, query SQL, or
// register a Dispatcher handler. The caller must obtain identity from Session.
// On success, defaults are inserted, strings are normalized, and unknown keys
// (including userId) are discarded. On failure, normalized is unchanged.
// Both output pointers may be null. Unknown actions return UNKNOWN_REQUEST_TYPE.
bool normalizeRequestData(const QString& type, const QJsonObject& data,
                          QJsonObject* normalized, ProtocolError* error = nullptr);

} // namespace charging::protocol::user_api
