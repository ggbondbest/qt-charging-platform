#include "workflow_management_page.h"
#include "admin_request_gateway.h"
#include "server_runtime.h"
#include "database_connection.h"
#include "repair_repository.h"
#include "repair_service.h"
#include <QComboBox>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QTemporaryDir>
#include <QtTest>
using namespace charging::server;

class WorkflowManagementPageTest : public QObject
{
    Q_OBJECT
private slots:
    void repairTimelineActionsAndLogout()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("workflow-ui.sqlite");
        {
            DatabaseConnection db;
            QVERIFY(db.open(path, true));
            RepairRepository repository(db.database());
            RepairService service(&repository);
            QVERIFY(service.handle("REPAIR_SUBMIT", {{"chargerId", "1"}, {"problemType", "SCREEN"},
                {"description", "屏幕无响应"}, {"operationId", "workflow-ui-report"}}, 1).value("success").toBool());
        }
        ServerRuntime runtime;
        QSignalSpy listening(&runtime, &ServerRuntime::listening);
        QVERIFY(runtime.start(path, false, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(listening.size(), 1);
        AdminRequestGateway gateway(&runtime);
        WorkflowManagementPage page(&gateway);
        page.resize(1050, 740); page.show();
        gateway.request("auth.login", {{"username", "admin"}, {"password", "123456"}}, this);
        QTRY_VERIFY(gateway.isAuthenticated());
        auto* tabs = page.findChild<QTabWidget*>("workflowTabs");
        auto* stations = page.findChild<QComboBox*>("workflowStationFilter");
        auto* table = page.findChild<QTableWidget*>("repairTable");
        auto* timeline = page.findChild<QTextEdit*>("repairTimeline");
        QVERIFY(tabs && stations && table && timeline);
        QTRY_VERIFY(stations->count() > 1);
        tabs->setCurrentIndex(1);
        QTRY_COMPARE(table->rowCount(), 1);
        QVERIFY(table->item(0, 4)->text().contains("****"));
        table->selectRow(0);
        QTRY_VERIFY(timeline->toPlainText().contains(QStringLiteral("已提交")));
        const QStringList buttons{"acceptRepairButton", "startRepairButton", "resolveRepairButton"};
        const QStringList states{QStringLiteral("已受理"), QStringLiteral("处理中"), QStringLiteral("已恢复")};
        for (int step = 0; step < buttons.size(); ++step) {
            auto* button = page.findChild<QPushButton*>(buttons.at(step));
            QTRY_VERIFY(button->isEnabled());
            QTimer::singleShot(0, &page, [&page] {
                for (auto* dialog : page.findChildren<QInputDialog*>()) {
                    dialog->setTextValue(QStringLiteral("已核实，模拟处理成功")); dialog->accept();
                }
            });
            button->click();
            QTRY_VERIFY(timeline->toPlainText().contains(states.at(step)));
        }
        QVERIFY(!page.findChild<QPushButton*>("resolveRepairButton")->isEnabled());
        const QString screenshot = qEnvironmentVariable("CHARGING_WORKFLOW_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(page.grab().save(screenshot));
        gateway.logout();
        QTRY_VERIFY(!gateway.isAuthenticated());
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(timeline->toPlainText().isEmpty());
        runtime.stop();
    }
};
QTEST_MAIN(WorkflowManagementPageTest)
#include "tst_workflow_management_page.moc"
