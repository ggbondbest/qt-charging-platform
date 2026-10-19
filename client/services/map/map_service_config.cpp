#include "map_service_config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace charging::client::services::map {
namespace {
constexpr qint64 kMaximumConfigBytes = 32 * 1024;

QString firstNonempty(const QString& preferred, const QString& fallback)
{
    return preferred.trimmed().isEmpty() ? fallback.trimmed() : preferred.trimmed();
}

bool readKey(const QJsonObject& object, const QString& field, QString* key)
{
    if (!object.contains(field)) return true;
    if (!object.value(field).isString()) return false;
    const QString value = object.value(field).toString().trimmed();
    if (value.size() > 512) return false;
    for (const QChar character : value) {
        if (character.unicode() < 32 || character.unicode() == 127
            || character.isSurrogate()) return false;
    }
    *key = value;
    return true;
}

MapServiceConfig readMapFile(const QString& path)
{
    MapServiceConfig config;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        config.errorMessage = QStringLiteral("地图配置文件无法读取，请检查文件路径和权限。");
        return config;
    }
    const QByteArray contents = file.read(kMaximumConfigBytes + 1);
    if (contents.size() > kMaximumConfigBytes) {
        config.errorMessage = QStringLiteral("地图配置文件过大，请保留必要配置。");
        return config;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(contents, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        config.errorMessage = QStringLiteral("地图配置文件不是有效的 JSON 对象，请检查格式。");
        return config;
    }
    const auto root = document.object();
    if (!root.value(QStringLiteral("version")).isDouble()
        || root.value(QStringLiteral("version")).toDouble() != 1.0) {
        config.errorMessage = QStringLiteral("地图配置 version 须为 1。");
        return config;
    }
    // Deliberately ignore AI content; it belongs to the management service.
    if (!root.contains(QStringLiteral("tencentMap"))) return config;
    const auto maps = root.value(QStringLiteral("tencentMap"));
    if (maps.isObject()) {
        const QStringList allowed{QStringLiteral("webServiceKey"), QStringLiteral("jsKey"),
                                  QStringLiteral("secretKey")};
        const auto entries = maps.toObject();
        for (auto entry = entries.constBegin(); entry != entries.constEnd(); ++entry) {
            if (!allowed.contains(entry.key())) {
                config.errorMessage = QStringLiteral("地图配置 tencentMap 含不支持的字段，请检查字段名。");
                return config;
            }
        }
    }
    if (!maps.isObject()
        || !readKey(maps.toObject(), QStringLiteral("webServiceKey"), &config.webServiceKey)
        || !readKey(maps.toObject(), QStringLiteral("jsKey"), &config.javaScriptKey)
        || !readKey(maps.toObject(), QStringLiteral("secretKey"), &config.secretKey)) {
        config = {};
        config.errorMessage = QStringLiteral("地图配置 tencentMap 须为对象，密钥须为单行字符串。");
    }
    return config;
}
} // namespace

QString discoverMapServiceConfig(const QString& applicationDirectory, const QString& sourceRoot)
{
    if (!applicationDirectory.isEmpty()) {
        QDir directory(applicationDirectory);
        do {
            const QString candidate = directory.filePath(QStringLiteral("config/services.local.json"));
            if (QFileInfo(candidate).isFile()) return QFileInfo(candidate).absoluteFilePath();
        } while (directory.cdUp());
    }
    if (!sourceRoot.isEmpty()) {
        const QString candidate = QDir(sourceRoot).filePath(QStringLiteral("config/services.local.json"));
        if (QFileInfo(candidate).isFile()) return QFileInfo(candidate).absoluteFilePath();
    }
    return {};
}

MapServiceConfig loadMapServiceConfig(const QString& explicitPath,
                                     const QProcessEnvironment& environment)
{
    QString path = explicitPath.isEmpty()
        ? environment.value(QStringLiteral("CHARGING_SERVICES_CONFIG")) : explicitPath;
    const bool specified = !explicitPath.isEmpty()
        || environment.contains(QStringLiteral("CHARGING_SERVICES_CONFIG"));
    path = path.trimmed();
    MapServiceConfig config;
    if (specified && path.isEmpty()) {
        config.errorMessage = QStringLiteral("地图配置路径不能为空，请检查 CHARGING_SERVICES_CONFIG。");
    } else {
        if (!specified) {
            path = discoverMapServiceConfig(QCoreApplication::applicationDirPath(),
                                            QStringLiteral(CHARGING_MAP_SOURCE_ROOT));
        } else {
            if (path == QStringLiteral("~")) path = QDir::homePath();
            else if (path.startsWith(QStringLiteral("~/")) || path.startsWith(QStringLiteral("~\\")))
                path = QDir(QDir::homePath()).filePath(path.mid(2));
            if (QFileInfo(path).isRelative()) path = QDir::current().absoluteFilePath(path);
        }
        if (!path.isEmpty()) config = readMapFile(path);
    }

    const QString api = firstNonempty(environment.value(QStringLiteral("TENCENT_MAP_API_KEY")),
                                     environment.value(QStringLiteral("CHARGING_TENCENT_MAP_KEY")));
    const QString secret = firstNonempty(environment.value(QStringLiteral("TENCENT_MAP_SECRET_KEY")),
                                        environment.value(QStringLiteral("CHARGING_TENCENT_MAP_SECRET")));
    config.webServiceKey = firstNonempty(api, config.webServiceKey);
    config.secretKey = firstNonempty(secret, config.secretKey);
    config.javaScriptKey = firstNonempty(environment.value(QStringLiteral("TENCENT_MAP_JS_KEY")),
                                         firstNonempty(config.javaScriptKey, config.webServiceKey));
    return config;
}

} // namespace charging::client::services::map
