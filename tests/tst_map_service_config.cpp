#include "services/map/map_service_config.h"
#include "services/map/map_geo_service.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace charging::client::services::map;

namespace {
struct RestoreWorkingDirectory {
    const QString previous = QDir::currentPath();
    ~RestoreWorkingDirectory() { QDir::setCurrent(previous); }
};
bool writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}
}

class MapServiceConfigTest final : public QObject
{
    Q_OBJECT
private slots:
    void consumesOnlyMapSectionAndUsesDistinctKeys();
    void environmentPrecedenceAndJavaScriptFallback();
    void javaScriptOnlyKeyDoesNotBecomeWebServiceCredential();
    void explicitPathPrecedesEnvironmentPath();
    void invalidMapConfiguration_data();
    void invalidMapConfiguration();
    void explicitPathsSupportRelativeHomeAndRejectWhitespace();
    void directProcessFindsDefaultConfiguration_data();
    void directProcessFindsDefaultConfiguration();
    void discoversCreatorBundleAndPackagedLayoutsWithoutWorkingDirectory();
};

void MapServiceConfigTest::consumesOnlyMapSectionAndUsesDistinctKeys()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("services.local.json");
    QVERIFY(writeFile(path, R"({"version":1,"ai":{"apiKey":false,"enabled":[]},
        "tencentMap":{"webServiceKey":" file-api ","jsKey":"file-js","secretKey":"file-sk"}})"));
    const auto config = loadMapServiceConfig(path, {});
    QCOMPARE(config.webServiceKey, QString("file-api"));
    QCOMPARE(config.javaScriptKey, QString("file-js"));
    QCOMPARE(config.secretKey, QString("file-sk"));
    QVERIFY(config.errorMessage.isEmpty());
}

void MapServiceConfigTest::environmentPrecedenceAndJavaScriptFallback()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("services.local.json");
    QVERIFY(writeFile(path, R"({"version":1,"tencentMap":{"webServiceKey":"file-api","secretKey":"file-sk"}})"));
    QProcessEnvironment env;
    env.insert("CHARGING_SERVICES_CONFIG", path);
    env.insert("CHARGING_TENCENT_MAP_KEY", "legacy-api");
    env.insert("CHARGING_TENCENT_MAP_SECRET", "legacy-sk");
    auto config = loadMapServiceConfig({}, env);
    QCOMPARE(config.webServiceKey, QString("legacy-api"));
    QCOMPARE(config.javaScriptKey, QString("legacy-api"));
    QCOMPARE(config.secretKey, QString("legacy-sk"));
    env.insert("TENCENT_MAP_API_KEY", "modern-api");
    env.insert("TENCENT_MAP_SECRET_KEY", "modern-sk");
    env.insert("TENCENT_MAP_JS_KEY", "modern-js");
    config = loadMapServiceConfig({}, env);
    QCOMPARE(config.webServiceKey, QString("modern-api"));
    QCOMPARE(config.javaScriptKey, QString("modern-js"));
    QCOMPARE(config.secretKey, QString("modern-sk"));
    QCOMPARE(env.value("CHARGING_TENCENT_MAP_KEY"), QString("legacy-api"));
}

void MapServiceConfigTest::javaScriptOnlyKeyDoesNotBecomeWebServiceCredential()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("services.local.json");
    QVERIFY(writeFile(path, R"({"version":1,"tencentMap":{"jsKey":"javascript-only"}})"));
    const auto config = loadMapServiceConfig(path, {});
    QVERIFY(config.webServiceKey.isEmpty());
    QCOMPARE(config.javaScriptKey, QString("javascript-only"));
}

void MapServiceConfigTest::explicitPathPrecedesEnvironmentPath()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("chosen.json");
    QVERIFY(writeFile(path, R"({"version":1,"tencentMap":{"webServiceKey":"chosen"}})"));
    QProcessEnvironment env;
    env.insert("CHARGING_SERVICES_CONFIG", directory.filePath("missing.json"));
    QCOMPARE(loadMapServiceConfig(path, env).webServiceKey, QString("chosen"));
    const auto missing = loadMapServiceConfig({}, env);
    QVERIFY(missing.webServiceKey.isEmpty());
    QVERIFY(!missing.errorMessage.isEmpty());
}

void MapServiceConfigTest::invalidMapConfiguration_data()
{
    QTest::addColumn<QByteArray>("contents");
    QTest::newRow("syntax") << QByteArray("not-json-private-value");
    QTest::newRow("root-array") << QByteArray("[]");
    QTest::newRow("version-missing") << QByteArray(R"({"tencentMap":{}})");
    QTest::newRow("version-string") << QByteArray(R"({"version":"1","tencentMap":{}})");
    QTest::newRow("version-other") << QByteArray(R"({"version":2,"tencentMap":{}})");
    QTest::newRow("map-unknown") << QByteArray(R"({"version":1,"tencentMap":{"jskey":"private"}})");
    QTest::newRow("endpoint-redirect") << QByteArray(R"({"version":1,"tencentMap":{"baseUrl":"https://example.test"}})");
    QTest::newRow("map-type") << QByteArray(R"({"version":1,"tencentMap":false})");
    QTest::newRow("key-type") << QByteArray(R"({"version":1,"tencentMap":{"webServiceKey":42}})");
    QTest::newRow("control") << QByteArray(R"({"version":1,"tencentMap":{"webServiceKey":"private\nvalue"}})");
    QTest::newRow("long-key") << QJsonDocument(QJsonObject{{"version", 1}, {"tencentMap",
        QJsonObject{{"webServiceKey", QString(513, 'x')}}}}).toJson();
    QTest::newRow("large-file") << QByteArray(32769, 'x');
}

void MapServiceConfigTest::invalidMapConfiguration()
{
    QFETCH(QByteArray, contents);
    QTemporaryDir directory;
    const QString path = directory.filePath("services.local.json");
    QVERIFY(writeFile(path, contents));
    auto config = loadMapServiceConfig(path, {});
    QVERIFY(config.webServiceKey.isEmpty());
    QVERIFY(config.javaScriptKey.isEmpty());
    QVERIFY(!config.errorMessage.isEmpty());
    QVERIFY(!config.errorMessage.contains("private"));
    QProcessEnvironment env;
    env.insert("TENCENT_MAP_API_KEY", "environment-still-works");
    config = loadMapServiceConfig(path, env);
    QCOMPARE(config.webServiceKey, QString("environment-still-works"));
}

void MapServiceConfigTest::explicitPathsSupportRelativeHomeAndRejectWhitespace()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("services.local.json");
    QVERIFY(writeFile(path, R"({"version":1,"tencentMap":{"webServiceKey":"relative-api"}})"));
    RestoreWorkingDirectory restore;
    QVERIFY(QDir::setCurrent(directory.path()));
    QProcessEnvironment env;
    env.insert("CHARGING_SERVICES_CONFIG", "  services.local.json  ");
    QCOMPARE(loadMapServiceConfig({}, env).webServiceKey, QString("relative-api"));
    env.insert("CHARGING_SERVICES_CONFIG", "   ");
    QVERIFY(!loadMapServiceConfig({}, env).errorMessage.isEmpty());
    QTemporaryDir home(QDir::homePath() + "/.charging-map-test-XXXXXX");
    QVERIFY(home.isValid());
    QVERIFY(writeFile(home.filePath("services.local.json"), R"({"version":1,"tencentMap":{"jsKey":"home-js"}})"));
    const QString tilde = "~/" + QFileInfo(home.path()).fileName() + "/services.local.json";
    QCOMPARE(loadMapServiceConfig(tilde, {}).javaScriptKey, QString("home-js"));
}

void MapServiceConfigTest::directProcessFindsDefaultConfiguration_data()
{
    QTest::addColumn<QString>("executableFolder");
    QTest::newRow("creator") << QString("project/build/Qt_6_12_0_for_macOS_Debug/client");
    QTest::newRow("mac-bundle") << QString("project/Charging.app/Contents/MacOS");
    QTest::newRow("packaged-bin") << QString("project/bin");
}

void MapServiceConfigTest::directProcessFindsDefaultConfiguration()
{
    QFETCH(QString, executableFolder);
    QTemporaryDir directory;
    const QString appDirectory = directory.filePath(executableFolder);
    QVERIFY(QDir().mkpath(appDirectory));
    const QString binary = QDir(appDirectory).filePath(QFileInfo(QCoreApplication::applicationFilePath()).fileName());
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), binary));
    QVERIFY(writeFile(directory.filePath("project/config/services.local.json"),
        R"({"version":1,"ai":[],"tencentMap":{"webServiceKey":"packaged-api","jsKey":"packaged-js","secretKey":"packaged-sk"}})"));
    const QString unrelated = directory.filePath("unrelated-working-directory");
    QVERIFY(QDir().mkpath(unrelated));
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const QString& name : {QString("CHARGING_SERVICES_CONFIG"), QString("TENCENT_MAP_API_KEY"),
          QString("TENCENT_MAP_JS_KEY"), QString("TENCENT_MAP_SECRET_KEY"),
          QString("CHARGING_TENCENT_MAP_KEY"), QString("CHARGING_TENCENT_MAP_SECRET")}) environment.remove(name);
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(unrelated);
    process.start(binary, {"--probe-default-map-config"});
    QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
    QVERIFY(process.readAllStandardOutput().isEmpty());
    QVERIFY(process.readAllStandardError().isEmpty());
}

void MapServiceConfigTest::discoversCreatorBundleAndPackagedLayoutsWithoutWorkingDirectory()
{
    QTemporaryDir directory;
    const QString source = directory.filePath("source");
    const QString sourceConfig = source + "/config/services.local.json";
    QVERIFY(writeFile(sourceConfig, "{}"));
    const QString creator = source + "/build/Qt_6_12_0_for_macOS_Debug/client";
    QVERIFY(QDir().mkpath(creator));
    QCOMPARE(discoverMapServiceConfig(creator), sourceConfig);
    const QString bundle = directory.filePath("installed/Charging.app/Contents/MacOS");
    QVERIFY(QDir().mkpath(bundle));
    QCOMPARE(discoverMapServiceConfig(bundle, source), sourceConfig);
    const QString packaged = directory.filePath("installed/config/services.local.json");
    QVERIFY(writeFile(packaged, "{}"));
    QCOMPARE(discoverMapServiceConfig(bundle, source), packaged);
    const QString bin = directory.filePath("installed/bin");
    QVERIFY(QDir().mkpath(bin));
    QCOMPARE(discoverMapServiceConfig(bin, source), packaged);
    const QString adjacent = bin + "/config/services.local.json";
    QVERIFY(writeFile(adjacent, "{}"));
    QCOMPARE(discoverMapServiceConfig(bin, source), adjacent);
    QVERIFY(discoverMapServiceConfig(directory.filePath("unrelated"), QString()).isEmpty());
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--probe-default-map-config"))) {
        const auto config = loadMapServiceConfig();
        MapGeoService service;
        return service.hasUsableKey() && config.webServiceKey == QStringLiteral("packaged-api")
            && config.javaScriptKey == QStringLiteral("packaged-js")
            && config.secretKey == QStringLiteral("packaged-sk") && config.errorMessage.isEmpty() ? 0 : 3;
    }
    MapServiceConfigTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_map_service_config.moc"
