#pragma once

#include "charging/common/model/models.h"

#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <optional>

namespace charging::server {

struct ChargerQuery
{
    qint64 stationId = 0;
    std::optional<charging::model::ChargerStatus> status;
    std::optional<charging::model::ChargerType> type;
    int limit = 20;
    int offset = 0;
};

struct ChargerQueryResult
{
    bool ok = false;
    QVector<charging::model::Charger> chargers;
    int totalCount = 0;
    QString errorMessage;
};

class ChargerRepository final
{
public:
    explicit ChargerRepository(const QSqlDatabase& database);
    ChargerQueryResult listByStation(const ChargerQuery& query) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
