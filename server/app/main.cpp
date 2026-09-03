#include "charging_server.h"
#include "database_connection.h"
#include "main_window.h"
#include "request_dispatcher.h"
#include "user_repository.h"
#include "user_service.h"

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

    charging::server::DatabaseConnection databaseConnection;
    QString databaseError;
    const bool loadDemoSeed = parser.isSet(demoSeedOption);
    if (!databaseConnection.open(parser.value(databaseOption), loadDemoSeed, &databaseError)) {
        qCritical().noquote()
            << QCoreApplication::translate("main", "Unable to initialize database:")
            << databaseError;
        return 1;
    }

    charging::server::UserRepository userRepository(databaseConnection.database());
    charging::server::UserService userService(&userRepository);
    charging::server::RequestDispatcher requestDispatcher(&userService);

    charging::server::ChargingServer server;
    server.setRequestDispatcher(&requestDispatcher);
    if (!server.listen(address, port)) {
        qCritical().noquote() << QCoreApplication::translate("main", "Unable to start server:")
                              << server.errorString();
        return 1;
    }

    qInfo().noquote() << QCoreApplication::translate("main", "Server listening on")
                      << address.toString() << ':' << server.serverPort();
    qInfo().noquote() << QCoreApplication::translate("main", "SQLite database:")
                      << databaseConnection.databasePath();

    charging::server::MainWindow window(&server);
    window.show();
    return application.exec();
}
