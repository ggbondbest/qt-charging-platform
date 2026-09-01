#include "charging_server.h"
#include "main_window.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QHostAddress>
#include <QStringList>

int main(int argc, char* argv[])
{
    QApplication::setApplicationName(QStringLiteral("charging-server"));
    QApplication::setApplicationDisplayName(QStringLiteral("充电平台 PC 运营管理端"));
    QApplication::setApplicationVersion(QStringLiteral(CHARGING_PLATFORM_VERSION));
    QApplication::setOrganizationName(QStringLiteral("ChargingPlatformTeam"));

    QApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "Electric vehicle charging platform server"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption addressOption(
        QStringList{QStringLiteral("a"), QStringLiteral("address")},
        QCoreApplication::translate("main", "Listen on the specified IP address."),
        QCoreApplication::translate("main", "address"), QStringLiteral("127.0.0.1"));
    const QCommandLineOption portOption(
        QStringList{QStringLiteral("p"), QStringLiteral("port")},
        QCoreApplication::translate("main", "Listen on the specified TCP port."),
        QCoreApplication::translate("main", "port"), QStringLiteral("9527"));
    parser.addOption(addressOption);
    parser.addOption(portOption);
    parser.process(application);

    QHostAddress address;
    if (!address.setAddress(parser.value(addressOption))) {
        qCritical().noquote() << QCoreApplication::translate("main", "Invalid listen address:")
                              << parser.value(addressOption);
        return 2;
    }

    bool portIsValid = false;
    const quint16 port = parser.value(portOption).toUShort(&portIsValid);
    if (!portIsValid || port == 0) {
        qCritical().noquote() << QCoreApplication::translate("main", "Invalid listen port:")
                              << parser.value(portOption);
        return 2;
    }

    charging::server::ChargingServer server;
    if (!server.listen(address, port)) {
        qCritical().noquote() << QCoreApplication::translate("main", "Unable to start server:")
                              << server.errorString();
        return 1;
    }

    qInfo().noquote() << QCoreApplication::translate("main", "Server listening on")
                      << address.toString() << ':' << server.serverPort();

    charging::server::MainWindow window(&server);
    window.show();
    return application.exec();
}
