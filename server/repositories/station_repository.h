#pragma once

#include "charging/common/model/models.h"

#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <optional>

namespace charging::server {

struct StationQuery
{
    QString keyword;
    std::optional<charging::model::StationStatus> status;
    int limit = 20;
    int offset = 0;
};

struct StationQueryResult
{
    bool ok = false;
    QVector<charging::model::Station> stations;
    int totalCount = 0;
    QString errorMessage;
};

class StationRepository final
{
public:
    explicit StationRepository(const QSqlDatabase& database);
    StationQueryResult list(const StationQuery& query) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
