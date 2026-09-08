#include "admin_login_page.h"
#include "admin_request_gateway.h"
#include "main_window.h"
#include "server_runtime.h"

#include <QApplication>
#include <QComboBox>
#include <QChartView>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace charging::server;

class AdminUiPresentationTest final : public QObject
{
    Q_OBJECT

    static void savePreview(QWidget* widget, const QString& name)
    {
        const QString directory = qEnvironmentVariable("CHARGING_ADMIN_UI_SCREENSHOTS");
        if (directory.isEmpty()) return;
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(widget->grab().save(QDir(directory).filePath(name + QStringLiteral(".png"))));
    }

private slots:
    void initTestCase()
    {
        originalPalette_ = qApp->palette();
        QPalette dark = originalPalette_;
        dark.setColor(QPalette::Window, QColor("#202020"));
        dark.setColor(QPalette::WindowText, Qt::white);
        dark.setColor(QPalette::Base, Qt::black);
        dark.setColor(QPalette::AlternateBase, QColor("#333333"));
        dark.setColor(QPalette::Text, Qt::white);
        dark.setColor(QPalette::Button, QColor("#333333"));
        dark.setColor(QPalette::ButtonText, Qt::white);
        dark.setColor(QPalette::PlaceholderText, QColor("#aaaaaa"));
        qApp->setPalette(dark);
    }

    void cleanupTestCase()
    {
        qApp->setPalette(originalPalette_);
    }

    void loginRemainsLightAndFitsCompactWindow()
    {
        AdminLoginPage page;
        page.resize(1024, 720);
        page.show();
        QTest::qWait(50);
        QCOMPARE(page.size(), QSize(1024, 720));
        auto* brand = page.findChild<QFrame*>(QStringLiteral("loginBrandPanel"));
        QVERIFY(brand != nullptr);
        QVERIFY(!brand->isVisible());
        for (const QString& name : {QStringLiteral("usernameLineEdit"),
                                    QStringLiteral("passwordLineEdit")}) {
            auto* edit = page.findChild<QLineEdit*>(name);
            QVERIFY(edit != nullptr);
            QVERIFY(edit->isVisible());
            QVERIFY(page.rect().contains(QRect(edit->mapTo(&page, QPoint()), edit->size())));
            QCOMPARE(edit->palette().color(QPalette::Text), QColor("#1e3152"));
            QVERIFY(edit->palette().color(QPalette::PlaceholderText).lightness() < 200);
            // Regression: renamed inputs must still be borderless children of
            // the white input shell, rather than native dark text fields.
            QVERIFY(edit->testAttribute(Qt::WA_StyleSheetTarget));
            const QImage preview = edit->parentWidget()->grab().toImage();
            const QColor background = preview.pixelColor(preview.width() / 2, 4);
            QVERIFY(background.red() > 220 && background.green() > 220 && background.blue() > 220);
        }
        savePreview(&page, QStringLiteral("admin-login-1024"));
        page.resize(1600, 990);
        QTest::qWait(50);
        QVERIFY(brand->isVisible());
        savePreview(&page, QStringLiteral("admin-login-1600"));
    }

    void authenticatedShellKeepsLightInputsAndAccessibleScrollbars()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ServerRuntime runtime;
        QSignalSpy listening(&runtime, &ServerRuntime::listening);
        QVERIFY(runtime.start(directory.filePath(QStringLiteral("ui.sqlite")), true,
                              QHostAddress::LocalHost, 0));
        QTRY_COMPARE(listening.size(), 1);
        MainWindow window(&runtime);
        window.resize(1600, 990);
        window.show();
        auto* username = window.findChild<QLineEdit*>(QStringLiteral("usernameLineEdit"));
        auto* password = window.findChild<QLineEdit*>(QStringLiteral("passwordLineEdit"));
        auto* login = window.findChild<QPushButton*>(QStringLiteral("loginButton"));
        QVERIFY(username && password && login);
        username->setText(QStringLiteral("admin"));
        password->setText(QStringLiteral("123456"));
        login->click();
        QTRY_VERIFY(window.adminGateway()->isAuthenticated());
        auto* dashboard = window.findChild<QWidget*>(QStringLiteral("dashboardPage"));
        QVERIFY(dashboard != nullptr);
        QTRY_VERIFY(dashboard->isVisible());
        for (auto* area : window.findChildren<QScrollArea*>()) {
            QCOMPARE(area->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
            QCOMPARE(area->horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
        }
        for (auto* table : window.findChildren<QTableWidget*>()) {
            QCOMPARE(table->palette().color(QPalette::Base), QColor("#ffffff"));
            QCOMPARE(table->palette().color(QPalette::AlternateBase), QColor("#f8fafd"));
            QCOMPARE(table->palette().color(QPalette::Text), QColor("#1d2c46"));
        }
        for (auto* chart : dashboard->findChildren<QChartView*>()) {
            QCOMPARE(chart->backgroundBrush().color(), QColor("#ffffff"));
            const QColor margin = chart->grab().toImage().pixelColor(2, 2);
            QVERIFY(margin.red() > 220 && margin.green() > 220 && margin.blue() > 220);
        }
        for (auto* combo : window.findChildren<QComboBox*>()) {
            if (combo->isEnabled()) {
                QCOMPARE(combo->palette().color(QPalette::Text), QColor("#1d2c46"));
            }
        }
        QTest::qWait(200);
        savePreview(&window, QStringLiteral("admin-dashboard-1600"));
        const QList<QPair<QString, QString>> otherPages = {
            {QStringLiteral("电桩管理"), QStringLiteral("chargerManagementPage")},
            {QStringLiteral("用户管理"), QStringLiteral("userManagementPage")},
            {QStringLiteral("订单管理"), QStringLiteral("orderManagementPage")},
            {QStringLiteral("充值记录"), QStringLiteral("rechargeRecordsPage")},
            {QStringLiteral("操作日志"), QStringLiteral("operationLogPage")},
        };
        for (const auto& page : otherPages) {
            for (auto* button : window.findChildren<QPushButton*>()) {
                if (button->text() == page.first) {
                    button->click();
                    break;
                }
            }
            auto* widget = window.findChild<QWidget*>(page.second);
            QVERIFY(widget != nullptr);
            QTRY_VERIFY(widget->isVisible());
            QTest::qWait(150);
            if (page.second == QStringLiteral("chargerManagementPage")) {
                auto* table = widget->findChild<QTableWidget*>(QStringLiteral("chargerManagementTable"));
                QVERIFY(table != nullptr);
                QVERIFY(table->columnWidth(1) >= 140);
                QVERIFY(!table->wordWrap());
            }
            savePreview(&window, QStringLiteral("admin-") + page.second + QStringLiteral("-1600"));
        }
        for (auto* button : window.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("电站管理")) {
                button->click();
                break;
            }
        }
        auto* stations = window.findChild<QTableWidget*>(QStringLiteral("stationManagementTable"));
        QVERIFY(stations != nullptr);
        QTRY_VERIFY(stations->isVisible() && stations->rowCount() > 0);
        QTest::qWait(150);
        savePreview(&window, QStringLiteral("admin-stations-1600"));
        window.resize(1024, 720);
        QTest::qWait(100);
        QCOMPARE(window.size(), QSize(1024, 720));
        bool visibleHorizontalScrollbar = false;
        for (auto* area : window.findChildren<QScrollArea*>()) {
            if (area->isVisible() && area->horizontalScrollBar()->maximum() > 0) {
                QVERIFY(area->horizontalScrollBar()->isVisible());
                visibleHorizontalScrollbar = true;
            }
        }
        QVERIFY(visibleHorizontalScrollbar);
        savePreview(&window, QStringLiteral("admin-stations-1024"));
        for (const QString& action : {QStringLiteral("新增电站"), QStringLiteral("编辑电站")}) {
            QPushButton* actionButton = nullptr;
            for (auto* button : window.findChildren<QPushButton*>()) {
                if (button->text() == action && button->isEnabled()) {
                    actionButton = button;
                    break;
                }
            }
            QVERIFY(actionButton != nullptr);
            bool opened = false;
            bool fieldsFit = true;
            bool lightText = true;
            QTimer::singleShot(0, &window, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (dialog == nullptr) return;
                QTest::qWait(30);
                opened = true;
                fieldsFit = dialog->width() <= 1024 && dialog->height() <= 720;
                for (auto* edit : dialog->findChildren<QLineEdit*>()) {
                    if (!edit->isVisible()) continue;
                    fieldsFit = fieldsFit && dialog->rect().contains(
                        QRect(edit->mapTo(dialog, QPoint()), edit->size()));
                    const QColor text = edit->palette().color(QPalette::Text);
                    lightText = lightText && text.lightness() < 160;
                }
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                fieldsFit = fieldsFit && buttons != nullptr;
                if (buttons != nullptr) {
                    fieldsFit = fieldsFit && dialog->rect().contains(
                        QRect(buttons->mapTo(dialog, QPoint()), buttons->size()));
                }
                savePreview(dialog, action == QStringLiteral("新增电站")
                    ? QStringLiteral("admin-station-create-dialog")
                    : QStringLiteral("admin-station-edit-dialog"));
                dialog->reject();
            });
            actionButton->click();
            QVERIFY(opened);
            QVERIFY(fieldsFit);
            QVERIFY(lightText);
        }
        for (auto* button : window.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("用户管理")) {
                button->click();
                break;
            }
        }
        auto* users = window.findChild<QWidget*>(QStringLiteral("userManagementPage"));
        QVERIFY(users != nullptr);
        QTRY_VERIFY(users->isVisible());
        for (auto* area : window.findChildren<QScrollArea*>()) {
            if (!area->isVisible()) continue;
            area->horizontalScrollBar()->setValue(area->horizontalScrollBar()->maximum());
            area->verticalScrollBar()->setValue(area->verticalScrollBar()->maximum());
            QTest::qWait(50);
            bool actionReachable = false;
            for (auto* button : users->findChildren<QPushButton*>()) {
                if (button->text() == QStringLiteral("冻结用户")) {
                    actionReachable = area->viewport()->rect().contains(
                        QRect(button->mapTo(area->viewport(), QPoint()), button->size()));
                }
            }
            QVERIFY(actionReachable);
        }
        savePreview(&window, QStringLiteral("admin-user-detail-1024"));
        runtime.stop();
    }

private:
    QPalette originalPalette_;
};

QTEST_MAIN(AdminUiPresentationTest)
#include "tst_admin_ui_presentation.moc"
