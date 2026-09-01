#pragma once

#include "charging/common/model/models.h"

#include <QJsonObject>
#include <QString>

namespace charging::model {

QJsonObject toJson(const User& value);
QJsonObject toJson(const Admin& value);
QJsonObject toJson(const Station& value);
QJsonObject toJson(const Charger& value);
QJsonObject toJson(const Reservation& value);
QJsonObject toJson(const Order& value);
QJsonObject toJson(const RechargeRecord& value);
QJsonObject toJson(const OperationLog& value);

bool fromJson(const QJsonObject& object, User* outValue, QString* errorMessage = nullptr);
bool fromJson(const QJsonObject& object, Admin* outValue, QString* errorMessage = nullptr);
bool fromJson(const QJsonObject& object, Station* outValue, QString* errorMessage = nullptr);
bool fromJson(const QJsonObject& object, Charger* outValue, QString* errorMessage = nullptr);
bool fromJson(const QJsonObject& object, Reservation* outValue, QString* errorMessage = nullptr);
bool fromJson(const QJsonObject& object, Order* outValue, QString* errorMessage = nullptr);
bool fromJson(const QJsonObject& object, RechargeRecord* outValue, QString* errorMessage = nullptr);
bool fromJson(const QJsonObject& object, OperationLog* outValue, QString* errorMessage = nullptr);

} // namespace charging::model
