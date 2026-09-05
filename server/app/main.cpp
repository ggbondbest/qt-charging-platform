#include "main_window.h"
#include "server_runtime.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QHostAddress>
#include <QStandardPaths>
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
    const QString applicationDataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString defaultDatabasePath =
        QDir(applicationDataPath).filePath(QStringLiteral("charging-platform.sqlite3"));
    const QCommandLineOption databaseOption(
        QStringList{QStringLiteral("d"), QStringLiteral("database")},
        QCoreApplication::translate("main", "Use the specified SQLite database file."),
        QCoreApplication::translate("main", "path"), defaultDatabasePath);
    const QCommandLineOption demoSeedOption(
        QStringLiteral("demo-seed"),
        QCoreApplication::translate(
            "main", "Load the idempotent demo data. Use only with a demo database."));
    parser.addOption(addressOption);
    parser.addOption(portOption);
    parser.addOption(databaseOption);
    parser.addOption(demoSeedOption);
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

    charging::server::ServerRuntime server;
    charging::server::MainWindow window(&server);
    // Closing the last window requests shutdown but keeps the GUI event loop
    // alive until the worker has closed sockets, repositories and SQLite.
    application.setQuitOnLastWindowClosed(false);
    int exitCode = 0;
    QObject::connect(&application, &QApplication::lastWindowClosed, &server,
                     &charging::server::ServerRuntime::stop);
    QObject::connect(&server, &charging::server::ServerRuntime::startupFailed, &application,
                     [&](const QString& message) {
                         exitCode = 1;
                         qCritical().noquote() << message;
                     });
    QObject::connect(&server, &charging::server::ServerRuntime::stopped, &application,
                     [&]() { application.exit(exitCode); });
    QObject::connect(&server, &charging::server::ServerRuntime::listening, &application,
                     [address](quint16 actualPort) {
                         qInfo().noquote() << "Server listening on" << address.toString()
                                           << ':' << actualPort;
                     });
    server.start(parser.value(databaseOption), parser.isSet(demoSeedOption), address, port);
    window.show();
    return application.exec();
}
