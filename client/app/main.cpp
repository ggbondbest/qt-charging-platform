#include "main_window.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication::setApplicationName(QStringLiteral("ChargingPlatformClient"));
    QApplication::setApplicationDisplayName(QStringLiteral("电动汽车充电桩应用管理平台"));
    QApplication::setApplicationVersion(QStringLiteral(CHARGING_PLATFORM_VERSION));
    QApplication::setOrganizationName(QStringLiteral("ChargingPlatformTeam"));

    QApplication application(argc, argv);
    charging::client::MainWindow window;
    window.show();
    return application.exec();
}
