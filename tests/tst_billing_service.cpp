#include "billing_service.h"
#include "charging_state_machine.h"

#include "charging/common/model/enums.h"
#include "charging/common/model/models.h"

#include <QtTest>

#include <array>
#include <limits>

class BillingServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void zeroDurationProducesZeroCharge();
    void calculatesExactCharge();
    void roundsAmountAtHalfCentBoundary();
    void rejectsInvalidAndOverflowingInput();
    void stateMachineAllowsOnlyDocumentedTransitions();
};

void BillingServiceTest::zeroDurationProducesZeroCharge()
{
    const charging::server::BillingResult result =
        charging::server::BillingService().calculate(7000, 0, 120);

    QVERIFY(result.success);
    QCOMPARE(result.durationSeconds, 0);
    QCOMPARE(result.energyWh, 0);
    QCOMPARE(result.amountCents, 0);
    QVERIFY(result.error.isEmpty());
}

void BillingServiceTest::calculatesExactCharge()
{
    // 7.2 kW for 500 seconds = 1 kWh, billed at 123 cents/kWh.
    const charging::server::BillingResult result =
        charging::server::BillingService().calculate(7200, 500, 123);

    QVERIFY(result.success);
    QCOMPARE(result.durationSeconds, 500);
    QCOMPARE(result.energyWh, 1000);
    QCOMPARE(result.amountCents, 123);
}

void BillingServiceTest::roundsAmountAtHalfCentBoundary()
{
    // One watt-hour keeps the amount numerator equal to the configured price.
    const charging::server::BillingResult belowHalf =
        charging::server::BillingService().calculate(3600, 1, 499);
    const charging::server::BillingResult exactlyHalf =
        charging::server::BillingService().calculate(3600, 1, 500);

    QVERIFY(belowHalf.success);
    QVERIFY(exactlyHalf.success);
    QCOMPARE(belowHalf.energyWh, 1);
    QCOMPARE(belowHalf.amountCents, 0);
    QCOMPARE(exactlyHalf.energyWh, 1);
    QCOMPARE(exactlyHalf.amountCents, 1);
}

void BillingServiceTest::rejectsInvalidAndOverflowingInput()
{
    const charging::server::BillingService service;

    QVERIFY(!service.calculate(0, 1, 120).success);
    QVERIFY(!service.calculate(7000, -1, 120).success);
    QVERIFY(!service.calculate(7000, 1, -1).success);

    const qint64 maximumJsonInteger = charging::model::kMaximumJsonSafeInteger;
    const charging::server::BillingResult wattSecondOverflow =
        service.calculate(std::numeric_limits<int>::max(), maximumJsonInteger, 1);
    QVERIFY(!wattSecondOverflow.success);
    QVERIFY(!wattSecondOverflow.error.isEmpty());

    const charging::server::BillingResult amountOverflow =
        service.calculate(1024, 3600, maximumJsonInteger);
    QVERIFY(!amountOverflow.success);
    QVERIFY(!amountOverflow.error.isEmpty());
}

void BillingServiceTest::stateMachineAllowsOnlyDocumentedTransitions()
{
    using charging::model::ChargerStatus;
    using charging::model::OrderStatus;
    using charging::model::ReservationStatus;
    using charging::server::ChargingStateMachine;

    const std::array<ReservationStatus, 4> reservationStatuses = {
        ReservationStatus::Active, ReservationStatus::Fulfilled, ReservationStatus::Cancelled,
        ReservationStatus::Expired};
    const bool reservationTransitions[4][4] = {
        {false, true, true, true},
        {false, false, false, false},
        {false, false, false, false},
        {false, false, false, false},
    };
    for (std::size_t fromIndex = 0; fromIndex < reservationStatuses.size(); ++fromIndex) {
        for (std::size_t toIndex = 0; toIndex < reservationStatuses.size(); ++toIndex) {
            const ReservationStatus from = reservationStatuses[fromIndex];
            const ReservationStatus to = reservationStatuses[toIndex];
            const QString transition =
                QStringLiteral("reservation %1 -> %2")
                    .arg(charging::model::toString(from), charging::model::toString(to));
            QVERIFY2(ChargingStateMachine::canTransition(from, to) ==
                         reservationTransitions[fromIndex][toIndex],
                     qPrintable(transition));
        }
    }

    const std::array<OrderStatus, 5> orderStatuses = {
        OrderStatus::Reserved, OrderStatus::Charging, OrderStatus::WaitingPayment,
        OrderStatus::Completed, OrderStatus::Cancelled};
    const bool orderTransitions[5][5] = {
        {false, true, false, false, true},   {false, false, true, false, false},
        {false, false, false, true, false},  {false, false, false, false, false},
        {false, false, false, false, false},
    };
    for (std::size_t fromIndex = 0; fromIndex < orderStatuses.size(); ++fromIndex) {
        for (std::size_t toIndex = 0; toIndex < orderStatuses.size(); ++toIndex) {
            const OrderStatus from = orderStatuses[fromIndex];
            const OrderStatus to = orderStatuses[toIndex];
            const QString transition =
                QStringLiteral("order %1 -> %2")
                    .arg(charging::model::toString(from), charging::model::toString(to));
            QVERIFY2(ChargingStateMachine::canTransition(from, to) ==
                         orderTransitions[fromIndex][toIndex],
                     qPrintable(transition));
        }
    }

    const std::array<ChargerStatus, 5> chargerStatuses = {
        ChargerStatus::Available, ChargerStatus::Reserved, ChargerStatus::Charging,
        ChargerStatus::Fault, ChargerStatus::Offline};
    const bool chargerTransitions[5][5] = {
        {false, true, false, true, true},   {true, false, true, false, false},
        {true, false, false, false, false}, {true, false, false, false, false},
        {true, false, false, false, false},
    };
    for (std::size_t fromIndex = 0; fromIndex < chargerStatuses.size(); ++fromIndex) {
        for (std::size_t toIndex = 0; toIndex < chargerStatuses.size(); ++toIndex) {
            const ChargerStatus from = chargerStatuses[fromIndex];
            const ChargerStatus to = chargerStatuses[toIndex];
            const QString transition =
                QStringLiteral("charger %1 -> %2")
                    .arg(charging::model::toString(from), charging::model::toString(to));
            QVERIFY2(ChargingStateMachine::canTransition(from, to) ==
                         chargerTransitions[fromIndex][toIndex],
                     qPrintable(transition));
        }
    }
}

QTEST_GUILESS_MAIN(BillingServiceTest)

#include "tst_billing_service.moc"
