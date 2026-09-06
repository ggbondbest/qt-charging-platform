// Local-only preview shell for the INTEGRATED HomeShell (member 2 chrome +
// member 3 pages), used to screenshot each tab/route for UI review.
// Runs entirely on the mock channels (station/reservation/profile services).
// Never linked into charging_client.

#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/pull_to_refresh_area.h"
#include "pages/station/home_shell.h"
#include "pages/station/station_home_page.h"
#include "charging/common/model/models.h"

#include <QApplication>
#include <QFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>

#include <functional>

namespace {

constexpr int kPreviewWidth = 420;
constexpr int kPreviewHeight = 860;

charging::model::User sampleUser()
{
    // 与 mock 种子账号（13800138000 / 余额 100.00）保持一致，预览截图里
    // 顶栏、钱包头、充值记录三处余额才是同一个数。
    charging::model::User user;
    user.id = 1;
    user.phone = QStringLiteral("13800138000");
    user.nickname = QStringLiteral("用户8000");
    user.balanceCents = 10000;
    return user;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication::setApplicationName(QStringLiteral("charging-shell-preview"));
    QApplication application(argc, argv);

    QString view = QStringLiteral("station");
    QString screenshotPath;
    bool pullDemo = false;
    for (const QString& argument : application.arguments()) {
        if (argument.startsWith(QStringLiteral("--view="))) {
            view = argument.section(QLatin1Char('='), 1);
        } else if (argument.startsWith(QStringLiteral("--screenshot="))) {
            screenshotPath = argument.section(QLatin1Char('='), 1);
        } else if (argument == QLatin1String("--pull")) {
            pullDemo = true;
        }
    }

    charging::client::pages::station::HomeShell shell(sampleUser());
    shell.resize(kPreviewWidth, kPreviewHeight);
    shell.setWindowTitle(QObject::tr("整合壳预览 · %1 (Mock)").arg(view));

    const auto clickTab = [&](const QString& id) {
        if (auto* button = shell.findChild<QPushButton*>(
                QStringLiteral("tab_%1").arg(id))) {
            button->click();
        }
    };

    const auto shoot = [&](int delayMs) {
        if (screenshotPath.isEmpty()) {
            return;
        }
        QTimer::singleShot(delayMs, [&]() {
            shell.grab().save(screenshotPath);
            QApplication::quit();
        });
    };

    // --pull：截图前用合成事件把当前可见的下拉区按进"松开刷新"态并保持
    // 手势（不发 release），让胶囊在图里定格——验证手势在整合壳层里真可用，
    // 而不只是单元测试的隔离环境。
    const auto armPullDemo = [&]() {
        if (!pullDemo) {
            return;
        }
        QTimer::singleShot(1100, [&shell]() {
            const auto areas = shell.findChildren<charging::client::PullToRefreshArea*>();
            for (auto* area : areas) {
                if (!area->isVisible()) {
                    continue;
                }
                QWidget* viewport = area->viewport();
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(120.0, 40.0), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(viewport, &press);
                QMouseEvent move(QEvent::MouseMove, QPointF(120.0, 110.0), Qt::NoButton,
                                 Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(viewport, &move);
                return;
            }
        });
    };

    shell.show();

    if (view == QLatin1String("order")) {
        clickTab(QStringLiteral("order"));
        armPullDemo();
        shoot(1600);
    } else if (view == QLatin1String("recharge") || view == QLatin1String("wallet")) {
        // 钱包/充值是路由页，经「我的」页的钱包卡入口进入。
        clickTab(QStringLiteral("profile"));
        const QString buttonId = view == QLatin1String("recharge")
            ? QStringLiteral("openRechargeButton") : QStringLiteral("openWalletButton");
        if (auto* entry = shell.findChild<QPushButton*>(buttonId)) {
            QTimer::singleShot(400, [entry]() { entry->click(); });
        }
        // recharge 页没有下拉区，armPullDemo 自动空转，无需分支。
        armPullDemo();
        shoot(1600);
    } else if (view == QLatin1String("profile")) {
        clickTab(QStringLiteral("profile"));
        shoot(1600);
    } else if (view == QLatin1String("charging")) {
        clickTab(QStringLiteral("charging"));
        armPullDemo();
        shoot(1600);
    } else if (view == QLatin1String("detail")) {
        // Wait for the mock station list to render, then click the first card.
        auto retries = std::make_shared<int>(12);
        auto timer = std::make_shared<QTimer>();
        timer->setInterval(300);
        QObject::connect(timer.get(), &QTimer::timeout, [&shell, retries, timer, shoot]() {
            auto* stationPage = shell.stationPage();
            if (stationPage && stationPage->stationCardCount() > 0) {
                timer->stop();
                if (auto* card = stationPage->findChild<charging::client::ClickableCard*>()) {
                    // ClickableCard 走真实鼠标释放信号：注入一对鼠标事件。
                    const QPointF pos(card->rect().center());
                    QMouseEvent press(QEvent::MouseButtonPress, pos, pos, pos, Qt::LeftButton,
                                      Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent release(QEvent::MouseButtonRelease, pos, pos, pos, Qt::LeftButton,
                                        Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(card, &press);
                    QApplication::sendEvent(card, &release);
                }
                shoot(900);
            } else if (--(*retries) <= 0) {
                timer->stop();
                shoot(200); // 空列表也出一张图，便于排查
            }
        });
        timer->start();
    } else if (view == QLatin1String("records")) {
        clickTab(QStringLiteral("profile"));
        if (auto* entry = shell.findChild<QPushButton*>(
                QStringLiteral("openReservationsButton"))) {
            QTimer::singleShot(400, [entry]() { entry->click(); });
        }
        shoot(1400);
    } else {
        shoot(1600); // station tab (default)
    }

    return application.exec();
}
