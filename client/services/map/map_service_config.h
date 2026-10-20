#pragma once

#include <QProcessEnvironment>
#include <QString>

namespace charging::client::services::map {

// Credentials stay in memory and are never written to logs or process env.
// Only tencentMap is consumed; AI settings belong to the management service.
struct MapServiceConfig
{
    QString webServiceKey;
    QString javaScriptKey;
    QString secretKey;
    QString errorMessage;
};

// Search next to the executable and its ancestors (including .app/bin layouts),
// then the developer source tree. Neither search depends on the working dir.
QString discoverMapServiceConfig(const QString& applicationDirectory,
                                 const QString& sourceRoot = QString());

// Explicit path > CHARGING_SERVICES_CONFIG > discovered config/services.local.json.
// Explicit paths support ~/ and are relative to the caller working directory.
// Only automatic discovery is independent of the working directory.
// Nonempty modern env > legacy env > file; JS falls back to the resolved API key.
MapServiceConfig loadMapServiceConfig(
    const QString& explicitPath = QString(),
    const QProcessEnvironment& environment = QProcessEnvironment::systemEnvironment());

} // namespace charging::client::services::map
