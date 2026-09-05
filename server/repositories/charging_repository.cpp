#include "charging_repository.h"

#include "charging/common/model/enums.h"
#include "charging/common/model/models.h"
#include "repository_row_mapper.h"

#include <QList>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <limits>

namespace charging::server {

namespace {

const QString kOrderColumns =
    QStringLiteral("id, order_no, user_id, charger_id, reservation_id, status, "
                   "unit_price_cents_per_kwh, energy_wh, duration_seconds, amount_cents, "
                   "created_at, started_at, stopped_at, paid_at, updated_at");
const QString kReservationColumns =
    QStringLiteral("id, user_id, charger_id, status, reserved_at, expires_at, ended_at");
const QString kChargerColumns =
    QStringLiteral("id, station_id, code, type, power_watts, status, total_charge_count, "
                   "total_charge_seconds, created_at, updated_at");

QString toStorageUtc(const QDateTime& value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

bool validUtcInstant(const QDateTime& value)
{
    return value.isValid() && value.toUTC().isValid();
}

class ImmediateTransaction final
{
public:
    explicit ImmediateTransaction(const QSqlDatabase& database) : database_(database) {}

    ~ImmediateTransaction()
    {
        if (active_) {
            QSqlQuery rollback(database_);
            rollback.exec(QStringLiteral("ROLLBACK"));
        }
    }

    bool begin(QString* diagnostic)
    {
        QSqlQuery query(database_);
        if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
            *diagnostic = query.lastError().text();
            return false;
        }
        active_ = true;
        return true;
    }

    bool commit(QString* diagnostic)
    {
        QSqlQuery query(database_);
        if (!query.exec(QStringLiteral("COMMIT"))) {
            *diagnostic = query.lastError().text();
            return false;
        }
        active_ = false;
        return true;
    }

private:
    QSqlDatabase database_;
    bool active_ = false;
};

ChargingRepositoryResult failure(RepositoryError error, const QString& diagnostic = {})
{
    ChargingRepositoryResult result;
    result.error = error;
    result.diagnostic = diagnostic;
    return result;
}

bool executeUpdate(QSqlQuery* query, QString* diagnostic)
{
    if (!query->exec()) {
        *diagnostic = query->lastError().text();
        return false;
    }
    if (query->numRowsAffected() != 1) {
        *diagnostic = QStringLiteral("A conditional workflow update affected %1 rows")
                          .arg(query->numRowsAffected());
        return false;
    }
    return true;
}

bool loadReservation(const QSqlDatabase& database, qint64 reservationId,
                     charging::model::Reservation* reservation, bool* found, QString* diagnostic)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT %1 FROM reservations WHERE id = :id LIMIT 1")
                      .arg(kReservationColumns));
    query.bindValue(QStringLiteral(":id"), reservationId);
    if (!query.exec()) {
        *diagnostic = query.lastError().text();
        return false;
    }
    if (!query.next()) {
        *found = false;
        return true;
    }
    if (!repository_detail::readReservation(query, reservation)) {
        *diagnostic = QStringLiteral("The stored reservation row contains invalid data");
        return false;
    }
    *found = true;
    return true;
}

bool loadOrderByReservation(const QSqlDatabase& database, qint64 reservationId,
                            charging::model::Order* order, bool* found, QString* diagnostic)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT %1 FROM orders WHERE reservation_id = :id LIMIT 1")
                      .arg(kOrderColumns));
    query.bindValue(QStringLiteral(":id"), reservationId);
    if (!query.exec()) {
        *diagnostic = query.lastError().text();
        return false;
    }
    if (!query.next()) {
        *found = false;
        return true;
    }
    if (!repository_detail::readOrder(query, order)) {
        *diagnostic = QStringLiteral("The stored order row contains invalid data");
        return false;
    }
    *found = true;
    return true;
}

bool loadOrder(const QSqlDatabase& database, qint64 orderId, charging::model::Order* order,
               bool* found, QString* diagnostic)
{
    QSqlQuery query(database);
    query.prepare(
        QStringLiteral("SELECT %1 FROM orders WHERE id = :id LIMIT 1").arg(kOrderColumns));
    query.bindValue(QStringLiteral(":id"), orderId);
    if (!query.exec()) {
        *diagnostic = query.lastError().text();
        return false;
    }
    if (!query.next()) {
        *found = false;
        return true;
    }
    if (!repository_detail::readOrder(query, order)) {
        *diagnostic = QStringLiteral("The stored order row contains invalid data");
        return false;
    }
    *found = true;
    return true;
}

bool loadCharger(const QSqlDatabase& database, qint64 chargerId, charging::model::Charger* charger,
                 bool* found, QString* diagnostic)
{
    QSqlQuery query(database);
    query.prepare(
        QStringLiteral("SELECT %1 FROM chargers WHERE id = :id LIMIT 1").arg(kChargerColumns));
    query.bindValue(QStringLiteral(":id"), chargerId);
    if (!query.exec()) {
        *diagnostic = query.lastError().text();
        return false;
    }
    if (!query.next()) {
        *found = false;
        return true;
    }
    if (!repository_detail::readCharger(query, charger)) {
        *diagnostic = QStringLiteral("The stored charger row contains invalid data");
        return false;
    }
    *found = true;
    return true;
}

bool workflowStateIsConsistent(const ChargingRepositoryResult& result, QString* diagnostic)
{
    using charging::model::ChargerStatus;
    using charging::model::OrderStatus;
    using charging::model::ReservationStatus;

    const bool identitiesMatch = result.order.reservationId == result.reservation.id &&
                                 result.order.userId == result.reservation.userId &&
                                 result.order.chargerId == result.reservation.chargerId &&
                                 result.charger.id == result.reservation.chargerId;
    bool statesMatch = false;
    switch (result.order.status) {
    case OrderStatus::Reserved:
        statesMatch = result.reservation.status == ReservationStatus::Active &&
                      result.charger.status == ChargerStatus::Reserved;
        break;
    case OrderStatus::Charging:
        statesMatch = result.reservation.status == ReservationStatus::Fulfilled &&
                      result.charger.status == ChargerStatus::Charging;
        break;
    case OrderStatus::WaitingPayment:
    case OrderStatus::Completed:
        // STOP releases the charger, so a historical order cannot constrain
        // the charger's current state after another user reserves it.
        statesMatch = result.reservation.status == ReservationStatus::Fulfilled;
        break;
    case OrderStatus::Cancelled:
        statesMatch = result.reservation.status == ReservationStatus::Cancelled ||
                      result.reservation.status == ReservationStatus::Expired;
        break;
    }

    if (!identitiesMatch || !statesMatch) {
        *diagnostic = QStringLiteral("The charging workflow rows have inconsistent state");
        return false;
    }
    return true;
}

bool loadWorkflowByReservation(const QSqlDatabase& database, qint64 reservationId,
                               ChargingRepositoryResult* result)
{
    bool found = false;
    if (!loadReservation(database, reservationId, &result->reservation, &found,
                         &result->diagnostic)) {
        return false;
    }
    if (!found) {
        result->error = RepositoryError::NotFound;
        return true;
    }
    if (!loadOrderByReservation(database, reservationId, &result->order, &found,
                                &result->diagnostic)) {
        return false;
    }
    if (!found || !loadCharger(database, result->reservation.chargerId, &result->charger, &found,
                               &result->diagnostic)) {
        if (!found && result->diagnostic.isEmpty()) {
            result->diagnostic = QStringLiteral("Workflow references a missing row");
        }
        return false;
    }
    return workflowStateIsConsistent(*result, &result->diagnostic);
}

bool loadWorkflowByOrder(const QSqlDatabase& database, qint64 orderId,
                         ChargingRepositoryResult* result)
{
    bool found = false;
    if (!loadOrder(database, orderId, &result->order, &found, &result->diagnostic)) {
        return false;
    }
    if (!found) {
        result->error = RepositoryError::NotFound;
        return true;
    }
    if (result->order.reservationId <= 0 ||
        !loadReservation(database, result->order.reservationId, &result->reservation, &found,
                         &result->diagnostic) ||
        !found ||
        !loadCharger(database, result->order.chargerId, &result->charger, &found,
                     &result->diagnostic) ||
        !found) {
        if (result->diagnostic.isEmpty()) {
            result->diagnostic = QStringLiteral("Workflow references a missing row");
        }
        return false;
    }
    return workflowStateIsConsistent(*result, &result->diagnostic);
}

struct ExpiredReservation
{
    qint64 reservationId = 0;
    qint64 chargerId = 0;
};

bool expireDueReservations(const QSqlDatabase& database, const QDateTime& nowUtc,
                           QString* diagnostic)
{
    QList<ExpiredReservation> expired;
    {
        QSqlQuery select(database);
        select.prepare(
            QStringLiteral("SELECT id, charger_id FROM reservations "
                           "WHERE status = 'ACTIVE' AND expires_at <= :now ORDER BY id"));
        select.bindValue(QStringLiteral(":now"), toStorageUtc(nowUtc));
        if (!select.exec()) {
            *diagnostic = select.lastError().text();
            return false;
        }
        while (select.next()) {
            bool reservationOk = false;
            bool chargerOk = false;
            ExpiredReservation value;
            value.reservationId = select.value(0).toLongLong(&reservationOk);
            value.chargerId = select.value(1).toLongLong(&chargerOk);
            if (!reservationOk || !chargerOk || value.reservationId <= 0 || value.chargerId <= 0) {
                *diagnostic = QStringLiteral("An expired reservation row is invalid");
                return false;
            }
            expired.append(value);
        }
    }

    const QString now = toStorageUtc(nowUtc);
    for (const ExpiredReservation& value : expired) {
        QSqlQuery reservationUpdate(database);
        reservationUpdate.prepare(QStringLiteral(
            "UPDATE reservations SET status = 'EXPIRED', ended_at = :now, updated_at = :now "
            "WHERE id = :id AND status = 'ACTIVE' AND expires_at <= :now"));
        reservationUpdate.bindValue(QStringLiteral(":now"), now);
        reservationUpdate.bindValue(QStringLiteral(":id"), value.reservationId);
        if (!executeUpdate(&reservationUpdate, diagnostic)) {
            return false;
        }

        QSqlQuery orderUpdate(database);
        orderUpdate.prepare(
            QStringLiteral("UPDATE orders SET status = 'CANCELLED', updated_at = :now "
                           "WHERE reservation_id = :reservationId AND status = 'RESERVED'"));
        orderUpdate.bindValue(QStringLiteral(":now"), now);
        orderUpdate.bindValue(QStringLiteral(":reservationId"), value.reservationId);
        if (!executeUpdate(&orderUpdate, diagnostic)) {
            return false;
        }

        QSqlQuery chargerUpdate(database);
        chargerUpdate.prepare(
            QStringLiteral("UPDATE chargers SET status = 'AVAILABLE', updated_at = :now "
                           "WHERE id = :chargerId AND status = 'RESERVED'"));
        chargerUpdate.bindValue(QStringLiteral(":now"), now);
        chargerUpdate.bindValue(QStringLiteral(":chargerId"), value.chargerId);
        if (!executeUpdate(&chargerUpdate, diagnostic)) {
            return false;
        }
    }
    return true;
}

bool expireDueReservationsAtomically(const QSqlDatabase& database, const QDateTime& nowUtc,
                                     QString* diagnostic)
{
    ImmediateTransaction transaction(database);
    if (!transaction.begin(diagnostic) || !expireDueReservations(database, nowUtc, diagnostic)) {
        return false;
    }
    return transaction.commit(diagnostic);
}

bool userIsActive(const QSqlDatabase& database, qint64 userId, RepositoryError* error,
                  QString* diagnostic)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT status FROM users WHERE id = :id LIMIT 1"));
    query.bindValue(QStringLiteral(":id"), userId);
    if (!query.exec()) {
        *diagnostic = query.lastError().text();
        *error = RepositoryError::Database;
        return false;
    }
    if (!query.next()) {
        *error = RepositoryError::NotFound;
        return false;
    }
    if (query.value(0).toString() != QStringLiteral("ACTIVE")) {
        *error = RepositoryError::UserFrozen;
        return false;
    }
    return true;
}

bool finishTransaction(ImmediateTransaction* transaction, ChargingRepositoryResult* result)
{
    if (!transaction->commit(&result->diagnostic)) {
        result->error = RepositoryError::Database;
        return false;
    }
    result->ok = true;
    result->error = RepositoryError::None;
    return true;
}

} // namespace

bool repository_detail::expireReservationsInTransaction(const QSqlDatabase& database,
                                                        const QDateTime& nowUtc,
                                                        QString* diagnostic)
{
    return expireDueReservations(database, nowUtc, diagnostic);
}

ChargingRepository::ChargingRepository(const QSqlDatabase& database) : database_(database) {}

ChargingRepositoryResult ChargingRepository::reserve(qint64 userId, qint64 chargerId,
                                                     const QDateTime& reservedAtUtc,
                                                     const QDateTime& expiresAtUtc,
                                                     const QString& orderNo) const
{
    if (userId <= 0 || chargerId <= 0 || !validUtcInstant(reservedAtUtc) ||
        !validUtcInstant(expiresAtUtc) || expiresAtUtc <= reservedAtUtc || orderNo.isEmpty() ||
        orderNo.size() > 40) {
        return failure(RepositoryError::InvalidInput);
    }
    if (!database_.isValid() || !database_.isOpen()) {
        return failure(RepositoryError::Database,
                       QStringLiteral("The SQLite connection is not open"));
    }

    ChargingRepositoryResult result;
    if (!expireDueReservationsAtomically(database_, reservedAtUtc, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    ImmediateTransaction transaction(database_);
    if (!transaction.begin(&result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }

    if (!userIsActive(database_, userId, &result.error, &result.diagnostic)) {
        return result;
    }

    {
        QSqlQuery unfinished(database_);
        unfinished.prepare(
            QStringLiteral("SELECT 1 FROM orders WHERE user_id = :userId "
                           "AND status IN ('RESERVED', 'CHARGING', 'WAITING_PAYMENT') LIMIT 1"));
        unfinished.bindValue(QStringLiteral(":userId"), userId);
        if (!unfinished.exec()) {
            return failure(RepositoryError::Database, unfinished.lastError().text());
        }
        if (unfinished.next()) {
            return failure(RepositoryError::ExistingUnfinishedOrder);
        }
    }

    qint64 unitPrice = 0;
    {
        QSqlQuery chargerQuery(database_);
        chargerQuery.prepare(QStringLiteral(
            "SELECT c.id, c.station_id, c.code, c.type, c.power_watts, c.status, "
            "c.total_charge_count, c.total_charge_seconds, c.created_at, c.updated_at, "
            "s.status, s.price_cents_per_kwh "
            "FROM chargers c JOIN stations s ON s.id = c.station_id "
            "WHERE c.id = :id LIMIT 1"));
        chargerQuery.bindValue(QStringLiteral(":id"), chargerId);
        if (!chargerQuery.exec()) {
            return failure(RepositoryError::Database, chargerQuery.lastError().text());
        }
        if (!chargerQuery.next()) {
            return failure(RepositoryError::NotFound);
        }
        bool priceOk = false;
        unitPrice = chargerQuery.value(11).toLongLong(&priceOk);
        if (!repository_detail::readCharger(chargerQuery, &result.charger) || !priceOk ||
            !repository_detail::inSafeRange(unitPrice)) {
            return failure(RepositoryError::Database,
                           QStringLiteral("The stored charger or station row is invalid"));
        }
        if (result.charger.status != charging::model::ChargerStatus::Available ||
            chargerQuery.value(10).toString() != QStringLiteral("ACTIVE")) {
            return failure(RepositoryError::ChargerNotAvailable);
        }
    }

    const QString now = toStorageUtc(reservedAtUtc);
    QSqlQuery reservationInsert(database_);
    reservationInsert.prepare(QStringLiteral(
        "INSERT INTO reservations "
        "(user_id, charger_id, status, reserved_at, expires_at, ended_at, created_at, updated_at) "
        "VALUES (:userId, :chargerId, 'ACTIVE', :reservedAt, :expiresAt, NULL, :now, :now)"));
    reservationInsert.bindValue(QStringLiteral(":userId"), userId);
    reservationInsert.bindValue(QStringLiteral(":chargerId"), chargerId);
    reservationInsert.bindValue(QStringLiteral(":reservedAt"), now);
    reservationInsert.bindValue(QStringLiteral(":expiresAt"), toStorageUtc(expiresAtUtc));
    reservationInsert.bindValue(QStringLiteral(":now"), now);
    if (!reservationInsert.exec()) {
        return failure(RepositoryError::Database, reservationInsert.lastError().text());
    }
    bool reservationIdOk = false;
    const qint64 reservationId = reservationInsert.lastInsertId().toLongLong(&reservationIdOk);
    if (!reservationIdOk || reservationId <= 0) {
        return failure(RepositoryError::Database,
                       QStringLiteral("SQLite did not return a reservation ID"));
    }

    QSqlQuery orderInsert(database_);
    orderInsert.prepare(
        QStringLiteral("INSERT INTO orders "
                       "(order_no, user_id, charger_id, reservation_id, status, "
                       "unit_price_cents_per_kwh, energy_wh, duration_seconds, amount_cents, "
                       "created_at, updated_at) "
                       "VALUES (:orderNo, :userId, :chargerId, :reservationId, 'RESERVED', "
                       ":unitPrice, 0, 0, 0, :now, :now)"));
    orderInsert.bindValue(QStringLiteral(":orderNo"), orderNo);
    orderInsert.bindValue(QStringLiteral(":userId"), userId);
    orderInsert.bindValue(QStringLiteral(":chargerId"), chargerId);
    orderInsert.bindValue(QStringLiteral(":reservationId"), reservationId);
    orderInsert.bindValue(QStringLiteral(":unitPrice"), unitPrice);
    orderInsert.bindValue(QStringLiteral(":now"), now);
    if (!orderInsert.exec()) {
        return failure(RepositoryError::Database, orderInsert.lastError().text());
    }

    QSqlQuery chargerUpdate(database_);
    chargerUpdate.prepare(
        QStringLiteral("UPDATE chargers SET status = 'RESERVED', updated_at = :now "
                       "WHERE id = :chargerId AND status = 'AVAILABLE'"));
    chargerUpdate.bindValue(QStringLiteral(":now"), now);
    chargerUpdate.bindValue(QStringLiteral(":chargerId"), chargerId);
    if (!executeUpdate(&chargerUpdate, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }

    if (!loadWorkflowByReservation(database_, reservationId, &result)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    finishTransaction(&transaction, &result);
    return result;
}

ChargingRepositoryResult
ChargingRepository::cancelReservation(qint64 userId, qint64 reservationId,
                                      const QDateTime& cancelledAtUtc) const
{
    if (userId <= 0 || reservationId <= 0 || !validUtcInstant(cancelledAtUtc)) {
        return failure(RepositoryError::InvalidInput);
    }
    if (!database_.isValid() || !database_.isOpen()) {
        return failure(RepositoryError::Database,
                       QStringLiteral("The SQLite connection is not open"));
    }

    ChargingRepositoryResult result;
    if (!expireDueReservationsAtomically(database_, cancelledAtUtc, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    ImmediateTransaction transaction(database_);
    if (!transaction.begin(&result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    if (!loadWorkflowByReservation(database_, reservationId, &result)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    if (result.error == RepositoryError::NotFound) {
        return result;
    }
    if (result.reservation.userId != userId) {
        return failure(RepositoryError::Unauthorized);
    }
    if (result.reservation.status == charging::model::ReservationStatus::Cancelled &&
        result.order.status == charging::model::OrderStatus::Cancelled) {
        result.idempotent = true;
        finishTransaction(&transaction, &result);
        return result;
    }
    if (result.reservation.status != charging::model::ReservationStatus::Active ||
        result.order.status != charging::model::OrderStatus::Reserved ||
        result.charger.status != charging::model::ChargerStatus::Reserved ||
        cancelledAtUtc.toUTC() < result.reservation.reservedAtUtc) {
        return failure(RepositoryError::InvalidStateTransition);
    }

    const QString now = toStorageUtc(cancelledAtUtc);
    QSqlQuery reservationUpdate(database_);
    reservationUpdate.prepare(QStringLiteral(
        "UPDATE reservations SET status = 'CANCELLED', ended_at = :now, updated_at = :now "
        "WHERE id = :id AND user_id = :userId AND status = 'ACTIVE'"));
    reservationUpdate.bindValue(QStringLiteral(":now"), now);
    reservationUpdate.bindValue(QStringLiteral(":id"), reservationId);
    reservationUpdate.bindValue(QStringLiteral(":userId"), userId);
    if (!executeUpdate(&reservationUpdate, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }

    QSqlQuery orderUpdate(database_);
    orderUpdate.prepare(QStringLiteral(
        "UPDATE orders SET status = 'CANCELLED', updated_at = :now "
        "WHERE reservation_id = :reservationId AND user_id = :userId AND status = 'RESERVED'"));
    orderUpdate.bindValue(QStringLiteral(":now"), now);
    orderUpdate.bindValue(QStringLiteral(":reservationId"), reservationId);
    orderUpdate.bindValue(QStringLiteral(":userId"), userId);
    if (!executeUpdate(&orderUpdate, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }

    QSqlQuery chargerUpdate(database_);
    chargerUpdate.prepare(
        QStringLiteral("UPDATE chargers SET status = 'AVAILABLE', updated_at = :now "
                       "WHERE id = :chargerId AND status = 'RESERVED'"));
    chargerUpdate.bindValue(QStringLiteral(":now"), now);
    chargerUpdate.bindValue(QStringLiteral(":chargerId"), result.charger.id);
    if (!executeUpdate(&chargerUpdate, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }

    if (!loadWorkflowByReservation(database_, reservationId, &result)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    finishTransaction(&transaction, &result);
    return result;
}

ChargingRepositoryResult ChargingRepository::startCharging(qint64 userId, qint64 reservationId,
                                                           const QDateTime& startedAtUtc) const
{
    if (userId <= 0 || reservationId <= 0 || !validUtcInstant(startedAtUtc)) {
        return failure(RepositoryError::InvalidInput);
    }
    if (!database_.isValid() || !database_.isOpen()) {
        return failure(RepositoryError::Database,
                       QStringLiteral("The SQLite connection is not open"));
    }

    ChargingRepositoryResult result;
    if (!expireDueReservationsAtomically(database_, startedAtUtc, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    ImmediateTransaction transaction(database_);
    if (!transaction.begin(&result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    if (!loadWorkflowByReservation(database_, reservationId, &result)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    if (result.error == RepositoryError::NotFound) {
        return result;
    }
    if (result.reservation.userId != userId) {
        return failure(RepositoryError::Unauthorized);
    }
    if (!userIsActive(database_, userId, &result.error, &result.diagnostic)) {
        return result;
    }
    if (result.reservation.status != charging::model::ReservationStatus::Active ||
        result.order.status != charging::model::OrderStatus::Reserved ||
        result.charger.status != charging::model::ChargerStatus::Reserved ||
        startedAtUtc.toUTC() < result.reservation.reservedAtUtc ||
        result.reservation.expiresAtUtc <= startedAtUtc.toUTC()) {
        return failure(RepositoryError::InvalidStateTransition);
    }

    const QString now = toStorageUtc(startedAtUtc);
    QSqlQuery reservationUpdate(database_);
    reservationUpdate.prepare(QStringLiteral(
        "UPDATE reservations SET status = 'FULFILLED', ended_at = :now, updated_at = :now "
        "WHERE id = :id AND user_id = :userId AND status = 'ACTIVE' AND expires_at > :now"));
    reservationUpdate.bindValue(QStringLiteral(":now"), now);
    reservationUpdate.bindValue(QStringLiteral(":id"), reservationId);
    reservationUpdate.bindValue(QStringLiteral(":userId"), userId);
    if (!executeUpdate(&reservationUpdate, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }

    QSqlQuery orderUpdate(database_);
    orderUpdate.prepare(QStringLiteral(
        "UPDATE orders SET status = 'CHARGING', started_at = :now, updated_at = :now "
        "WHERE reservation_id = :reservationId AND user_id = :userId AND status = 'RESERVED'"));
    orderUpdate.bindValue(QStringLiteral(":now"), now);
    orderUpdate.bindValue(QStringLiteral(":reservationId"), reservationId);
    orderUpdate.bindValue(QStringLiteral(":userId"), userId);
    if (!executeUpdate(&orderUpdate, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }

    QSqlQuery chargerUpdate(database_);
    chargerUpdate.prepare(
        QStringLiteral("UPDATE chargers SET status = 'CHARGING', updated_at = :now "
                       "WHERE id = :chargerId AND status = 'RESERVED'"));
    chargerUpdate.bindValue(QStringLiteral(":now"), now);
    chargerUpdate.bindValue(QStringLiteral(":chargerId"), result.charger.id);
    if (!executeUpdate(&chargerUpdate, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }

    if (!loadWorkflowByReservation(database_, reservationId, &result)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    finishTransaction(&transaction, &result);
    return result;
}

ChargingRepositoryResult ChargingRepository::chargingStatus(qint64 userId, qint64 orderId,
                                                            const QDateTime& observedAtUtc) const
{
    if (userId <= 0 || orderId <= 0 || !validUtcInstant(observedAtUtc)) {
        return failure(RepositoryError::InvalidInput);
    }
    if (!database_.isValid() || !database_.isOpen()) {
        return failure(RepositoryError::Database,
                       QStringLiteral("The SQLite connection is not open"));
    }

    ChargingRepositoryResult result;
    if (!expireDueReservationsAtomically(database_, observedAtUtc, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    ImmediateTransaction transaction(database_);
    if (!transaction.begin(&result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    if (!loadWorkflowByOrder(database_, orderId, &result)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    if (result.error == RepositoryError::NotFound) {
        return result;
    }
    if (result.order.userId != userId) {
        return failure(RepositoryError::Unauthorized);
    }
    finishTransaction(&transaction, &result);
    return result;
}

ChargingRepositoryResult ChargingRepository::stopCharging(qint64 userId, qint64 orderId,
                                                          const QDateTime& expectedStartedAtUtc,
                                                          const QDateTime& stoppedAtUtc,
                                                          qint64 durationSeconds, qint64 energyWh,
                                                          qint64 amountCents) const
{
    const qint64 maximum = charging::model::kMaximumJsonSafeInteger;
    if (userId <= 0 || orderId <= 0 || !validUtcInstant(expectedStartedAtUtc) ||
        !validUtcInstant(stoppedAtUtc) || stoppedAtUtc < expectedStartedAtUtc ||
        durationSeconds < 0 || durationSeconds > maximum || energyWh < 0 || energyWh > maximum ||
        amountCents < 0 || amountCents > maximum) {
        return failure(RepositoryError::InvalidInput);
    }
    if (!database_.isValid() || !database_.isOpen()) {
        return failure(RepositoryError::Database,
                       QStringLiteral("The SQLite connection is not open"));
    }

    ChargingRepositoryResult result;
    ImmediateTransaction transaction(database_);
    if (!transaction.begin(&result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    if (!loadWorkflowByOrder(database_, orderId, &result)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    if (result.error == RepositoryError::NotFound) {
        return result;
    }
    if (result.order.userId != userId) {
        return failure(RepositoryError::Unauthorized);
    }
    if (result.order.status == charging::model::OrderStatus::WaitingPayment ||
        result.order.status == charging::model::OrderStatus::Completed) {
        result.idempotent = true;
        finishTransaction(&transaction, &result);
        return result;
    }
    if (result.order.status != charging::model::OrderStatus::Charging ||
        result.charger.status != charging::model::ChargerStatus::Charging ||
        !result.order.startedAtUtc.isValid() ||
        result.order.startedAtUtc != expectedStartedAtUtc.toUTC()) {
        return failure(RepositoryError::InvalidStateTransition);
    }
    if (result.charger.totalChargeSeconds > maximum - durationSeconds ||
        result.charger.totalChargeCount == std::numeric_limits<int>::max()) {
        return failure(RepositoryError::ArithmeticOverflow);
    }

    const QString now = toStorageUtc(stoppedAtUtc);
    QSqlQuery orderUpdate(database_);
    orderUpdate.prepare(QStringLiteral(
        "UPDATE orders SET status = 'WAITING_PAYMENT', duration_seconds = :duration, "
        "energy_wh = :energy, amount_cents = :amount, stopped_at = :now, updated_at = :now "
        "WHERE id = :orderId AND user_id = :userId AND status = 'CHARGING' "
        "AND started_at = :startedAt"));
    orderUpdate.bindValue(QStringLiteral(":duration"), durationSeconds);
    orderUpdate.bindValue(QStringLiteral(":energy"), energyWh);
    orderUpdate.bindValue(QStringLiteral(":amount"), amountCents);
    orderUpdate.bindValue(QStringLiteral(":now"), now);
    orderUpdate.bindValue(QStringLiteral(":orderId"), orderId);
    orderUpdate.bindValue(QStringLiteral(":userId"), userId);
    orderUpdate.bindValue(QStringLiteral(":startedAt"), toStorageUtc(expectedStartedAtUtc));
    if (!executeUpdate(&orderUpdate, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }

    QSqlQuery chargerUpdate(database_);
    chargerUpdate.prepare(
        QStringLiteral("UPDATE chargers SET status = 'AVAILABLE', "
                       "total_charge_count = total_charge_count + 1, "
                       "total_charge_seconds = total_charge_seconds + :duration, updated_at = :now "
                       "WHERE id = :chargerId AND status = 'CHARGING' "
                       "AND total_charge_seconds <= :maximumRemaining "
                       "AND total_charge_count < :maximumCount"));
    chargerUpdate.bindValue(QStringLiteral(":duration"), durationSeconds);
    chargerUpdate.bindValue(QStringLiteral(":now"), now);
    chargerUpdate.bindValue(QStringLiteral(":chargerId"), result.charger.id);
    chargerUpdate.bindValue(QStringLiteral(":maximumRemaining"), maximum - durationSeconds);
    chargerUpdate.bindValue(QStringLiteral(":maximumCount"), std::numeric_limits<int>::max());
    if (!executeUpdate(&chargerUpdate, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }

    if (!loadWorkflowByOrder(database_, orderId, &result)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    finishTransaction(&transaction, &result);
    return result;
}

} // namespace charging::server
