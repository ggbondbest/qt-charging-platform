#include "dashboard_service_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

using charging::server::DashboardServiceManager;
using State = DashboardServiceManager::State;

namespace {
void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}

struct EnvironmentValue {
    QByteArray name;
    QByteArray previous;
    bool existed;
    explicit EnvironmentValue(const char* variable, const QByteArray& value)
        : name(variable), previous(qgetenv(variable)), existed(qEnvironmentVariableIsSet(variable))
    { qputenv(name.constData(), value); }
    ~EnvironmentValue() { if (existed) qputenv(name.constData(), previous); else qunsetenv(name.constData()); }
};

// A deterministic executable stub exercises the actual QProcess protocol and
// cleanup on every platform, without requiring Python, a web server or MySQL.
int runServiceStub(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    const auto value = [&](const QString& option) { return arguments.value(arguments.indexOf(option) + 1); };
    const QString database = value(QStringLiteral("--database"));
    const QString directory = QFileInfo(database).absolutePath();
    QFile scenarioFile(QDir(directory).filePath(QStringLiteral("scenario")));
    scenarioFile.open(QIODevice::ReadOnly);
    const QByteArray scenario = scenarioFile.readAll();
    writeFile(QDir(directory).filePath(QStringLiteral("started")), QByteArray::number(app.applicationPid()));
    std::thread([&app, directory] {
        std::string line;
        if (!std::getline(std::cin, line) || line == "CHARGING_WEB_STOP") {
            QMetaObject::invokeMethod(&app, [&app, directory] {
                writeFile(QDir(directory).filePath(QStringLiteral("cleaned")), "graceful");
                app.quit();
            }, Qt::QueuedConnection);
        }
    }).detach();
    if (scenario == "error") {
        const QByteArray error("private traceback MUST NOT DISPLAY\nCHARGING_WEB_ERROR={\"code\":\"DEPENDENCY\",\"message\":\"网页运行环境尚未安装完整，请配置后重试。\"}\n");
        std::fwrite(error.constData(), 1, static_cast<size_t>(error.size()), stderr);
        std::fflush(stderr);
    } else if (scenario != "hang") {
        QJsonObject ready{{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:43123/?embedded=operator")},
                          {QStringLiteral("database"), QFileInfo(database).canonicalFilePath()},
                          {QStringLiteral("mode"), QStringLiteral("qt-live-only")}};
        if (scenario == "wrong-database") ready.insert(QStringLiteral("database"), database + ".other");
        if (scenario == "remote-url") ready.insert(QStringLiteral("url"), QStringLiteral("http://example.com:43123/"));
        const QByteArray line = "CHARGING_WEB_READY=" + QJsonDocument(ready).toJson(QJsonDocument::Compact) + '\n';
        const int split = 23;
        std::fwrite(line.constData(), 1, split, stdout);
        std::fflush(stdout);
        QTimer::singleShot(30, &app, [line] {
            std::fwrite(line.constData() + 23, 1, static_cast<size_t>(line.size() - 23), stdout);
            std::fflush(stdout);
        });
        if (scenario == "crash") QTimer::singleShot(350, &app, [] { std::_Exit(3); });
    }
    return app.exec();
}
} // namespace

class DashboardServiceManagerTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    QString database_;
    DashboardServiceManager::Options options_;

private slots:
    void init()
    {
        QVERIFY(directory_.isValid());
        QDir root(directory_.path());
        root.mkpath("scripts");
        writeFile(root.filePath("scripts/managed_web_service.py"), "stub handled by test executable");
        database_ = root.filePath("business.sqlite3");
        writeFile(database_, "existing database fixture");
        writeFile(root.filePath("scenario"), "ready");
        QFile::remove(root.filePath("cleaned"));
        QFile::remove(root.filePath("started"));
        options_ = {};
        options_.repositoryRoot = root.path();
        options_.pythonExecutable = QCoreApplication::applicationFilePath();
        options_.startupTimeoutMs = 2000;
        options_.shutdownTimeoutMs = 500;
    }
    void fragmentedReadyAndGracefulCleanup()
    {
        DashboardServiceManager manager(options_);
        QSignalSpy ready(&manager, &DashboardServiceManager::ready);
        QSignalSpy stopped(&manager, &DashboardServiceManager::stopped);
        manager.start(database_);
        QCOMPARE(manager.state(), State::Starting);
        QTRY_COMPARE(manager.state(), State::Ready);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(manager.url().port(), 43123);
        QCOMPARE(manager.mode(), QString("qt-live-only"));
        QVERIFY(manager.processId() > 0);
        manager.stop();
        QTRY_COMPARE(manager.state(), State::Stopped);
        QCOMPARE(stopped.size(), 1);
        QCOMPARE(manager.processId(), qint64(0));
        QVERIFY(QFileInfo::exists(QDir(directory_.path()).filePath("cleaned")));
    }
    void safeFailureAndRetry()
    {
        writeFile(QDir(directory_.path()).filePath("scenario"), "error");
        DashboardServiceManager manager(options_);
        manager.start(database_);
        QTRY_COMPARE(manager.state(), State::Failed);
        QVERIFY(manager.message().contains("尚未安装完整"));
        QVERIFY(!manager.message().contains("traceback"));
        QVERIFY(manager.url().isEmpty());
        // Retry while old process may still be shutting down: only one owner.
        writeFile(QDir(directory_.path()).filePath("scenario"), "ready");
        manager.retry();
        QTRY_COMPARE(manager.state(), State::Ready);
        manager.stop();
        QTRY_VERIFY(manager.isStopped());
    }
    void rejectsMismatchedReady_data()
    {
        QTest::addColumn<QByteArray>("scenario");
        QTest::newRow("wrong business database") << QByteArray("wrong-database");
        QTest::newRow("non-loopback URL") << QByteArray("remote-url");
    }
    void rejectsMismatchedReady()
    {
        QFETCH(QByteArray, scenario);
        writeFile(QDir(directory_.path()).filePath("scenario"), scenario);
        DashboardServiceManager manager(options_);
        manager.start(database_);
        QTRY_COMPARE(manager.state(), State::Failed);
        QVERIFY(manager.message().contains("不匹配"));
        QVERIFY(manager.url().isEmpty());
        manager.stop();
        QTRY_VERIFY(manager.isStopped());
    }
    void startupTimeoutCleansOwnedProcess()
    {
        writeFile(QDir(directory_.path()).filePath("scenario"), "hang");
        options_.startupTimeoutMs = 500;
        DashboardServiceManager manager(options_);
        manager.start(database_);
        QTRY_COMPARE(manager.state(), State::Failed);
        QVERIFY(manager.message().contains("超时"));
        QTRY_COMPARE(manager.processId(), qint64(0));
        QVERIFY(QFileInfo::exists(QDir(directory_.path()).filePath("cleaned")));
    }
    void unexpectedExitAllowsRetry()
    {
        writeFile(QDir(directory_.path()).filePath("scenario"), "crash");
        DashboardServiceManager manager(options_);
        manager.start(database_);
        QTRY_COMPARE(manager.state(), State::Ready);
        QTRY_COMPARE(manager.state(), State::Failed);
        QVERIFY(manager.message().contains("已停止"));
        writeFile(QDir(directory_.path()).filePath("scenario"), "ready");
        manager.retry();
        QTRY_COMPARE(manager.state(), State::Ready);
        manager.stop();
        QTRY_VERIFY(manager.isStopped());
    }
    void failedToStartThenImmediateStopAlwaysFinishes()
    {
        options_.pythonExecutable = QDir(directory_.path()).filePath("missing-python");
        DashboardServiceManager manager(options_);
        QSignalSpy stopped(&manager, &DashboardServiceManager::stopped);
        manager.start(database_);
        manager.stop();
        QTRY_VERIFY(manager.isStopped());
        QCOMPARE(stopped.size(), 1);
    }
    void externalServiceIsNeverOwnedOrStopped()
    {
        QProcess external;
        external.start(options_.pythonExecutable, {QDir(directory_.path()).filePath("scripts/managed_web_service.py"), "--database", database_});
        QVERIFY(external.waitForStarted());
        options_.externalUrl = "http://127.0.0.1:43123/?city=Beijing";
        options_.autoStart = false; // external URL still works for launch_demo
        DashboardServiceManager manager(options_);
        manager.start("missing-database");
        QCOMPARE(manager.state(), State::External);
        QCOMPARE(manager.url().query(), QString("city=Beijing"));
        QCOMPARE(manager.processId(), qint64(0));
        manager.stop();
        QVERIFY(manager.isStopped());
        QCOMPARE(external.state(), QProcess::Running);
        external.write("CHARGING_WEB_STOP\n");
        external.closeWriteChannel();
        QVERIFY(external.waitForFinished(2000));
    }
    void disabledDoesNotStartProcess()
    {
        options_.autoStart = false;
        DashboardServiceManager manager(options_);
        manager.start(database_);
        QCOMPARE(manager.state(), State::Disabled);
        QCOMPARE(manager.processId(), qint64(0));
        QVERIFY(!QFileInfo::exists(QDir(directory_.path()).filePath("started")));
        manager.stop();
        QVERIFY(manager.isStopped());
    }
    void configurationPreservesVenvSymlinkAndEnvironmentPriority()
    {
        QString interpreter = QCoreApplication::applicationFilePath();
#ifdef Q_OS_UNIX
        const QString symlink = QDir(directory_.path()).filePath("venv-python");
        QFile::remove(symlink);
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), symlink));
        interpreter = symlink;
#endif
        const QString config = QDir(directory_.path()).filePath("service.json");
        writeFile(config, QJsonDocument(QJsonObject{{"pythonExecutable", interpreter}}).toJson());
        EnvironmentValue external("CHARGING_DASHBOARD_URL", "");
        EnvironmentValue configuration("CHARGING_DASHBOARD_CONFIG", config.toUtf8());
        EnvironmentValue python("CHARGING_WEB_PYTHON", "");
        const auto discovered = DashboardServiceManager::discoverOptions();
        QVERIFY(discovered.configurationError.isEmpty());
        QCOMPARE(discovered.pythonExecutable, QFileInfo(interpreter).absoluteFilePath());
        qputenv("CHARGING_WEB_PYTHON", QCoreApplication::applicationFilePath().toUtf8());
        QCOMPARE(DashboardServiceManager::discoverOptions().pythonExecutable, QCoreApplication::applicationFilePath());
    }
};

int main(int argc, char** argv)
{
    if (argc > 1 && QString::fromLocal8Bit(argv[1]).endsWith("managed_web_service.py"))
        return runServiceStub(argc, argv);
    QCoreApplication application(argc, argv);
    DashboardServiceManagerTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_dashboard_service_manager.moc"
