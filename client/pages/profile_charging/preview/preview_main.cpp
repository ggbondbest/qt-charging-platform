// Local-only preview shell for Member 3 pages (wallet + recharge + orders +
// charging + settlement + profile hub + profile edit). Runs against
// MockRequestTransport so the UI can be reviewed before the real interfaces
// are released. Never linked into charging_client.

#include "charging/client/profile_charging/charging_page.h"
#include "charging/client/profile_charging/charging_service.h"
#include "charging/client/profile_charging/mock_request_transport.h"
#include "charging/client/profile_charging/order_detail_page.h"
#include "charging/client/profile_charging/order_list_page.h"
#include "charging/client/profile_charging/order_service.h"
#include "charging/client/profile_charging/profile_edit_page.h"
#include "charging/client/profile_charging/profile_page.h"
#include "charging/client/profile_charging/recharge_page.h"
#include "charging/client/profile_charging/settlement_page.h"
#include "charging/client/profile_charging/wallet_page.h"
#include "charging/client/profile_charging/wallet_service.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/client/widgets/toast.h"
#include "charging/common/model/enums.h"

#include <QApplication>
#include <QFile>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>

namespace {

constexpr int kPreviewWidth = 420;
constexpr int kPreviewHeight = 860;

// Optional smoke hooks: --page=profile|profile-edit|wallet|recharge|orders|
// order-detail|charging|settlement --screenshot=/tmp/x.png
struct PreviewArgs
{
    QString page = QStringLiteral("profile");
    QString screenshotPath;
};

PreviewArgs parseArgs(const QStringList& arguments)
{
    PreviewArgs parsed;
    for (const QString& argument : arguments) {
        if (argument.startsWith(QStringLiteral("--page="))) {
            parsed.page = argument.section(QLatin1Char('='), 1);
        } else if (argument.startsWith(QStringLiteral("--screenshot="))) {
            parsed.screenshotPath = argument.section(QLatin1Char('='), 1);
        }
    }
    return parsed;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication::setApplicationName(QStringLiteral("charging-profile-preview"));
    QApplication application(argc, argv);

    QFile styleFile(QStringLiteral(":/qss/client_platform.qss"));
    if (styleFile.open(QIODevice::ReadOnly)) {
        application.setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    }

    charging::client::MockRequestTransport transport;
    charging::client::WalletService walletService(&transport);
    charging::client::OrderService orderService(&transport);
    charging::client::ChargingService chargingService(&transport);

    QWidget root;
    root.setObjectName(QStringLiteral("appRoot"));
    root.setAttribute(Qt::WA_StyledBackground, true);
    root.resize(kPreviewWidth, kPreviewHeight);
    root.setWindowTitle(QObject::tr("充电用户端 · 成员3 UI 预览（Mock 数据）"));

    auto* rootLayout = new QVBoxLayout(&root);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    QStackedWidget stack;
    rootLayout->addWidget(&stack);

    auto* walletPage = new charging::client::WalletPage(&walletService);
    auto* rechargePage = new charging::client::RechargePage(&walletService);
    auto* orderListPage = new charging::client::OrderListPage(&orderService);
    auto* orderDetailPage = new charging::client::OrderDetailPage();
    auto* chargingPage = new charging::client::ChargingPage(&chargingService);
    auto* settlementPage = new charging::client::SettlementPage(&chargingService);
    auto* profilePage = new charging::client::ProfilePage(&walletService, &orderService);
    auto* profileEditPage = new charging::client::ProfileEditPage(&walletService);
    stack.addWidget(walletPage); // index 0
    stack.addWidget(rechargePage); // index 1
    stack.addWidget(orderListPage); // index 2
    stack.addWidget(orderDetailPage); // index 3
    stack.addWidget(chargingPage); // index 4
    stack.addWidget(settlementPage); // index 5
    stack.addWidget(profilePage); // index 6 (hub)
    stack.addWidget(profileEditPage); // index 7

    // Recharge can be entered from the wallet or from settlement (insufficient
    // balance); remember which so the return lands in the right place.
    // Remember where each shared page was entered from so "back" returns to
    // the real origin (wallet page vs profile hub vs settlement) instead of
    // a hard-coded destination.
    QWidget* rechargeReturnPage = walletPage;
    QWidget* ordersReturnPage = walletPage;

    qint64 lastKnownBalanceCents = 0;
    QObject::connect(&walletService, &charging::client::WalletService::profileLoaded,
                     [&lastKnownBalanceCents](const charging::model::User& user) {
                         lastKnownBalanceCents = user.balanceCents;
                     });

    // Land on a page after navigation; refresh the data-driven ones so
    // balances and badge counts never go stale.
    const auto landOn = [&](QWidget* page) {
        stack.setCurrentWidget(page);
        if (page == walletPage) {
            walletPage->refresh();
        } else if (page == profilePage) {
            profilePage->refresh();
        }
    };

    QObject::connect(walletPage, &charging::client::WalletPage::rechargeRequested, [&]() {
        rechargeReturnPage = walletPage;
        rechargePage->setBalance(lastKnownBalanceCents);
        stack.setCurrentWidget(rechargePage);
    });
    QObject::connect(rechargePage, &charging::client::RechargePage::backRequested, [&]() {
        if (rechargeReturnPage == settlementPage) {
            stack.setCurrentWidget(settlementPage);
            return;
        }
        landOn(rechargeReturnPage);
    });
    QObject::connect(
        rechargePage, &charging::client::RechargePage::rechargeSucceeded,
        [&](qint64 balanceAfterCents) {
            if (rechargeReturnPage == settlementPage) {
                settlementPage->setBalance(balanceAfterCents);
                stack.setCurrentWidget(settlementPage);
                return;
            }
            landOn(rechargeReturnPage);
        });

    QObject::connect(walletPage, &charging::client::WalletPage::ordersRequested, [&]() {
        ordersReturnPage = walletPage;
        stack.setCurrentWidget(orderListPage);
        orderListPage->refresh();
    });
    QObject::connect(walletPage, &charging::client::WalletPage::profileRequested, [&]() {
        stack.setCurrentWidget(profilePage);
        profilePage->refresh();
    });
    QObject::connect(profilePage, &charging::client::ProfilePage::profileEditRequested, [&]() {
        stack.setCurrentWidget(profileEditPage);
        profileEditPage->refresh();
    });
    QObject::connect(profileEditPage, &charging::client::ProfileEditPage::backRequested, [&]() {
        stack.setCurrentWidget(profilePage);
        profilePage->refresh(); // picked nickname must land on the hub
    });
    QObject::connect(profilePage, &charging::client::ProfilePage::walletRequested, [&]() {
        stack.setCurrentWidget(walletPage);
        walletPage->refresh();
    });
    QObject::connect(profilePage, &charging::client::ProfilePage::rechargeRequested, [&]() {
        rechargeReturnPage = profilePage;
        rechargePage->setBalance(lastKnownBalanceCents);
        stack.setCurrentWidget(rechargePage);
    });
    const auto openFilteredOrders = [&](charging::client::OrderService::Filter filter) {
        ordersReturnPage = profilePage;
        stack.setCurrentWidget(orderListPage);
        orderListPage->showFilter(filter);
    };
    QObject::connect(profilePage, &charging::client::ProfilePage::allOrdersRequested,
                     [&]() { openFilteredOrders(charging::client::OrderService::Filter::All); });
    QObject::connect(profilePage, &charging::client::ProfilePage::chargingOrdersRequested, [&]() {
        openFilteredOrders(charging::client::OrderService::Filter::Charging);
    });
    QObject::connect(profilePage, &charging::client::ProfilePage::waitingPaymentOrdersRequested,
                     [&]() {
                         openFilteredOrders(charging::client::OrderService::Filter::WaitingPayment);
                     });
    QObject::connect(profilePage, &charging::client::ProfilePage::completedOrdersRequested, [&]() {
        openFilteredOrders(charging::client::OrderService::Filter::Completed);
    });
    QObject::connect(orderListPage, &charging::client::OrderListPage::backRequested, [&]() {
        landOn(ordersReturnPage);
    });
    QObject::connect(orderListPage, &charging::client::OrderListPage::orderOpened,
                     [&](const charging::client::OrderSummary& summary) {
                         if (summary.order.status == charging::model::OrderStatus::Charging) {
                             // A charging order jumps into the live session page.
                             charging::client::ChargingStatus initial;
                             initial.order = summary.order;
                             initial.stationName = summary.stationName;
                             initial.chargerCode = summary.chargerCode;
                             chargingPage->startFor(initial);
                             stack.setCurrentWidget(chargingPage);
                             return;
                         }
                         orderDetailPage->showOrder(summary);
                         stack.setCurrentWidget(orderDetailPage);
                     });
    QObject::connect(orderDetailPage, &charging::client::OrderDetailPage::backRequested, [&]() {
        stack.setCurrentWidget(orderListPage);
    });
    QObject::connect(orderDetailPage, &charging::client::OrderDetailPage::payRequested, [&]() {
        // The detail order is WAITING_PAYMENT; hand it to settlement.
        const charging::client::OrderSummary summary = orderDetailPage->currentOrder();
        charging::client::ChargingStatus status;
        status.order = summary.order;
        status.stationName = summary.stationName;
        status.chargerCode = summary.chargerCode;
        settlementPage->setBalance(lastKnownBalanceCents);
        settlementPage->showOrder(status);
        stack.setCurrentWidget(settlementPage);
    });

    QObject::connect(chargingPage, &charging::client::ChargingPage::backRequested, [&]() {
        chargingService.stopTracking();
        stack.setCurrentWidget(orderListPage);
    });
    QObject::connect(
        chargingPage, &charging::client::ChargingPage::settlementRequested,
        [&](const charging::client::ChargingStatus& stopped) {
            settlementPage->setBalance(lastKnownBalanceCents);
            settlementPage->showOrder(stopped);
            stack.setCurrentWidget(settlementPage);
        });

    QObject::connect(settlementPage, &charging::client::SettlementPage::backRequested, [&]() {
        stack.setCurrentWidget(orderListPage);
        orderListPage->refresh();
    });
    QObject::connect(settlementPage, &charging::client::SettlementPage::doneRequested, [&]() {
        stack.setCurrentWidget(orderListPage);
        orderListPage->refresh();
        walletService.fetchProfile(); // keep the shell's balance honest
    });
    QObject::connect(settlementPage, &charging::client::SettlementPage::rechargeRequested, [&]() {
        rechargeReturnPage = settlementPage;
        rechargePage->setBalance(lastKnownBalanceCents);
        stack.setCurrentWidget(rechargePage);
    });

    const PreviewArgs args = parseArgs(application.arguments().mid(1));
    auto openSessionSmoke = [&](bool wantCharging) {
        QWidget* targetPage = wantCharging ? static_cast<QWidget*>(chargingPage)
                                           : static_cast<QWidget*>(settlementPage);
        stack.setCurrentWidget(targetPage);
        auto pending = std::make_shared<bool>(true);
        QObject::connect(
            &orderService, &charging::client::OrderService::ordersLoaded,
            [pending, wantCharging, targetPage, chargingPage, settlementPage,
             &stack](const QVector<charging::client::OrderSummary>& orders, int, bool) {
                if (!*pending) {
                    return;
                }
                const auto want = wantCharging ? charging::model::OrderStatus::Charging
                                               : charging::model::OrderStatus::WaitingPayment;
                for (const charging::client::OrderSummary& summary : orders) {
                    if (summary.order.status != want) {
                        continue;
                    }
                    *pending = false;
                    charging::client::ChargingStatus status;
                    status.order = summary.order;
                    status.stationName = summary.stationName;
                    status.chargerCode = summary.chargerCode;
                    if (wantCharging) {
                        chargingPage->startFor(status);
                    } else {
                        settlementPage->setBalance(10000);
                        settlementPage->showOrder(status);
                    }
                    stack.setCurrentWidget(targetPage);
                    return;
                }
            });
        orderService.fetchOrders(charging::client::OrderService::Filter::All, 1);
    };

    if (args.page == QStringLiteral("recharge")) {
        rechargePage->setBalance(10000);
        stack.setCurrentWidget(rechargePage);
    } else if (args.page == QStringLiteral("profile")) {
        stack.setCurrentWidget(profilePage);
        profilePage->refresh();
    } else if (args.page == QStringLiteral("profile-edit")) {
        stack.setCurrentWidget(profileEditPage);
        profileEditPage->refresh();
    } else if (args.page == QStringLiteral("charging")) {
        openSessionSmoke(true);
    } else if (args.page == QStringLiteral("settlement")) {
        openSessionSmoke(false);
    } else if (args.page == QStringLiteral("orders") ||
               args.page == QStringLiteral("order-detail")) {
        stack.setCurrentWidget(orderListPage);
        if (args.page == QStringLiteral("order-detail")) {
            // Smoke path: open the first payable (else newest) order once the
            // list answers, so screenshots capture a populated detail page.
            auto pending = std::make_shared<bool>(true);
            QObject::connect(
                &orderService, &charging::client::OrderService::ordersLoaded,
                [pending, orderDetailPage, &stack](
                    const QVector<charging::client::OrderSummary>& orders, int, bool) {
                    if (!*pending || orders.isEmpty()) {
                        return;
                    }
                    *pending = false;
                    charging::client::OrderSummary picked = orders.first();
                    for (const charging::client::OrderSummary& summary : orders) {
                        if (summary.order.status ==
                            charging::model::OrderStatus::WaitingPayment) {
                            picked = summary;
                            break;
                        }
                    }
                    orderDetailPage->showOrder(picked);
                    stack.setCurrentWidget(orderDetailPage);
                });
        }
        orderListPage->refresh();
    }

    walletPage->refresh();
    root.show();

    if (!args.screenshotPath.isEmpty()) {
        QTimer::singleShot(1600, [&]() {
            root.grab().save(args.screenshotPath);
            QApplication::quit();
        });
    }

    return application.exec();
}
