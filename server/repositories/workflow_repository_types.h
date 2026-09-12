#pragma once

#include "charging/common/model/models.h"

#include <QString>

namespace charging::server {

// Repository errors are deliberately domain-oriented. diagnostic is for the
// server log only; Services never copy SQLite error text into a wire response.
enum class RepositoryError
{
    None,
    InvalidInput,
    NotFound,
    Unauthorized,
    UserFrozen,
    ChargerNotAvailable,
    ExistingUnfinishedOrder,
    InvalidStateTransition,
    InsufficientBalance,
    ArithmeticOverflow,
    Database
};

struct ChargingRepositoryResult
{
    bool ok = false;
    bool idempotent = false;
    RepositoryError error = RepositoryError::None;
    charging::model::Reservation reservation;
    charging::model::Order order;
    charging::model::Charger charger;
    QString diagnostic;
};

struct OrderRepositoryResult
{
    bool ok = false;
    bool idempotent = false;
    RepositoryError error = RepositoryError::None;
    charging::model::Order order;
    qint64 balanceCents = 0;
    QString diagnostic;
};

} // namespace charging::server
