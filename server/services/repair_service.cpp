#include "repair_service.h"
#include "repair_repository.h"

#include <QMap>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>

namespace charging::server {
namespace {
void require(bool condition) { if (!condition) throw RepairFailure("INVALID_ARGUMENT"); }
void fields(const QJsonObject& p, const QStringList& allowed)
{
    for (auto it = p.begin(); it != p.end(); ++it) require(allowed.contains(it.key()));
}
void id(const QJsonObject& p, const QString& key)
{
    bool ok = false;
    const auto value = p.value(key).toString().toLongLong(&ok);
    require(p.value(key).isString() && ok && value > 0 && QString::number(value) == p.value(key).toString());
}
void text(const QJsonObject& p, const QString& key, int maximum)
{
    const auto value = p.value(key).toString();
    require(p.value(key).isString() && !value.trimmed().isEmpty() && value.size() <= maximum &&
            !value.contains(QChar::Null));
}
void operation(const QJsonObject& p)
{
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_.:-]{1,64}$"));
    require(p.value("operationId").isString() && valid.match(p.value("operationId").toString()).hasMatch());
}
void page(const QJsonObject& p)
{
    for (const auto& key : {QStringLiteral("page"), QStringLiteral("pageSize")}) {
        if (!p.contains(key)) continue;
        const auto v = p.value(key);
        require(v.isDouble() && std::isfinite(v.toDouble()) && std::floor(v.toDouble()) == v.toDouble() &&
                v.toDouble() >= 1 && v.toDouble() <= (key == "page" ? 1000000 : 100));
    }
    if (p.contains("status")) {
        require(p.value("status").isString());
        require(QStringList{"SUBMITTED", "ACCEPTED", "PROCESSING", "RESOLVED"}.contains(p.value("status").toString()));
    }
}
void validate(const QString& action, const QJsonObject& p, bool admin)
{
    if (action == "REPAIR_SUBMIT") {
        fields(p, {"chargerId", "problemType", "description", "operationId"});
        id(p, "chargerId"); text(p, "description", 200); operation(p);
        require(p.value("problemType").isString() &&
                QStringList{"CONNECTION", "SCREEN", "CONNECTOR", "CHARGING", "OTHER"}.contains(p.value("problemType").toString()));
    } else if (action == "REPAIR_GET" || action == "repair_reports.get") {
        fields(p, {"id"}); id(p, "id");
    } else if (action == "REPAIR_GET_MINE" || action == "repair_reports.list") {
        fields(p, admin ? QStringList{"page", "pageSize", "status", "stationId", "chargerId", "keyword"}
                        : QStringList{"page", "pageSize", "status"});
        page(p);
        for (const auto& key : {QStringLiteral("stationId"), QStringLiteral("chargerId")}) if (p.contains(key)) id(p, key);
        if (p.contains("keyword")) text(p, "keyword", 64);
    } else {
        fields(p, {"id", "operationId", "expectedUpdatedAt", "note"});
        id(p, "id"); operation(p); text(p, "note", 200);
        const auto timestamp = p.value("expectedUpdatedAt").toString();
        const auto parsed = QDateTime::fromString(timestamp, Qt::ISODateWithMs);
        require(p.value("expectedUpdatedAt").isString() && timestamp.size() == 24 && parsed.isValid() &&
                parsed.toUTC().toString(Qt::ISODateWithMs) == timestamp);
    }
}
QJsonObject failure(const QString& code)
{
    const QMap<QString, QString> messages{
        {"INVALID_ARGUMENT", QStringLiteral("参数无效，请检查电桩、问题类型和说明")},
        {"UNAUTHORIZED", QStringLiteral("登录已失效，请重新登录")},
        {"USER_FROZEN", QStringLiteral("用户已被冻结")},
        {"NOT_FOUND", QStringLiteral("报障记录或电桩不存在或不可访问")},
        {"CONFLICT", QStringLiteral("记录已更新或操作编号已被使用，请刷新后重试")},
        {"ALREADY_EXISTS", QStringLiteral("您已有该电桩的未完成报障，请查看处理进度")},
        {"RESOURCE_BUSY", QStringLiteral("电桩正在预约、叫号或充电，请先等待使用结束后核实停用")},
        {"INVALID_STATE_TRANSITION", QStringLiteral("当前报障状态不支持该操作，请刷新后按流程处理")},
        {"DATABASE_ERROR", QStringLiteral("保存失败，数据未更改，请稍后重试")},
        {"UNKNOWN_REQUEST_TYPE", QStringLiteral("未知报障请求")},
        {"INTERNAL_ERROR", QStringLiteral("报障服务暂不可用")}
    };
    const QString safeCode = messages.contains(code) ? code : QStringLiteral("INTERNAL_ERROR");
    return {{"success", false}, {"error", QJsonObject{{"code", safeCode}, {"message", messages.value(safeCode)}}}};
}
} // namespace

RepairService::RepairService(RepairRepository* repository, UtcClock clock)
    : repository_(repository), clock_(clock) {}
bool RepairService::handles(const QString& type)
{ return QStringList{"REPAIR_SUBMIT", "REPAIR_GET_MINE", "REPAIR_GET"}.contains(type); }
bool RepairService::handlesAdmin(const QString& action)
{ return QStringList{"repair_reports.list", "repair_reports.get", "repair_reports.accept",
                     "repair_reports.start", "repair_reports.resolve"}.contains(action); }
QJsonObject RepairService::handle(const QString& type, const QJsonObject& p, qint64 userId) const
{
    if (userId <= 0) return failure("UNAUTHORIZED");
    if (!handles(type)) return failure("UNKNOWN_REQUEST_TYPE");
    if (!repository_) return failure("INTERNAL_ERROR");
    try {
        validate(type, p, false);
        return {{"success", true}, {"data", repository_->handle(type, p, userId,
                clock_ ? clock_().toUTC() : QDateTime::currentDateTimeUtc())}};
    } catch (const RepairFailure& e) { return failure(QString::fromLatin1(e.what())); }
    catch (...) { return failure("INTERNAL_ERROR"); }
}
QJsonObject RepairService::adminHandle(const QString& action, const QJsonObject& p, qint64 adminId) const
{
    if (adminId <= 0) return failure("UNAUTHORIZED");
    if (!handlesAdmin(action)) return failure("UNKNOWN_REQUEST_TYPE");
    if (!repository_) return failure("INTERNAL_ERROR");
    try {
        validate(action, p, true);
        return {{"success", true}, {"data", repository_->adminHandle(action, p, adminId,
                clock_ ? clock_().toUTC() : QDateTime::currentDateTimeUtc())}};
    } catch (const RepairFailure& e) { return failure(QString::fromLatin1(e.what())); }
    catch (...) { return failure("INTERNAL_ERROR"); }
}
} // namespace charging::server
