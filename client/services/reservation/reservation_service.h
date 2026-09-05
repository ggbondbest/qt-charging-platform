#pragma once

#include "charging/common/model/models.h"
#include "charging/common/protocol/protocol.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QDateTime>
#include <QVector>

namespace charging::client::network {
class ClientConnection;
}

namespace charging::client::services::settings {
class SettingsService;
}

namespace charging::client::services::reservation {

// 面向 UI 的预约记录：核心模型 model::Reservation + 展示上下文
// （站点名称、桩编号、充电规格、时间段、车辆、预估费用、导航距离占位）。
// common 层模型（队友 CODEOWNERS）暂无开始时间/车辆字段，时间段与车辆
// 上下文在客户端扩展；后端预约列表查询接口就绪后由真实通道补齐，UI 零改动。
struct ReservationRecord
{
    charging::model::Reservation reservation;
    qint64 orderId = 0; // Real associated order; zero only when absent.
    QString stationName;
    QString chargerCode;
    QString chargerSpec; // 如“直流快充 · 120kW”（预约订单/历史详情展示）
    QDateTime startAtUtc; // 预约时段开始（结束仍用 reservation.expiresAtUtc）
    qint64 vehicleId = 0; // 预约关联车辆（名额按车辆唯一）
    QString vehiclePlate; // 展示用车辆牌照
    bool lateCancelled = false; // 迟到超 15 分钟被自动取消（“已取消·迟到”）
    int durationMinutes = 0;
    qint64 estimatedFeeCents = 0;
    int distanceMeters = -1; // 虚拟数据：预留对接后续导航模块
    // 站点坐标（腾讯路线规划接口寻址用；0/0 或 false = 导航页保持模拟路线）。
    bool hasStationLocation = false;
    double stationLatitude = 0.0;
    double stationLongitude = 0.0;
};

using ReservationList = QVector<ReservationRecord>;

// 系统推荐时段（时间段预约）：行驶时长为模拟估算（5 分钟出发准备 +
// 每 500 米 1 分钟），腾讯地图路线/距离矩阵 API 就绪后仅需替换估算实现，
// 页面代码不变。
struct RecommendedSlot
{
    QDateTime startUtc;
    QDateTime endUtc;
    int travelMinutes = 0;
};

// 充电桩预约服务（成员 2，任务 #17 二次迭代）。
//
// 双通道设计，与站点查询服务一致，页面 UI 对二者无感知：
// - 模拟通道（当前默认）：提交/取消/记录列表全部本地完成，带模拟延迟驱动
//   加载状态；**名额制约束**——有效预约上限 = 用户车辆数（SettingsService
//   注入），且每辆至多一条未结束预约（替换上一轮“全局仅一条”）；单段不
//   超过 45 分钟；迟到超 15 分钟自动取消（cancelLateReservations）；支持
//   “桩被抢占”“参数非法”等失败分支演示；倒计时归零可流转为“已过期”；
// - 真实通道：服务端已实现 RESERVE_CHARGER / CANCEL_RESERVATION（需登录
//   会话），注入 ClientConnection + setLiveMode(true) 即无缝切换；服务端
//   唯一索引暂强制“每用户一条有效预约”，名额制以模拟通道为准，协议扩展
//   （时间段 + 车辆字段）就绪后再提服务端变更；预约列表查询命令协议尚未
//   定义，真实通道按未知命令走友好失败路径，命令就绪后无需改页面代码。
class ReservationService final : public QObject
{
    Q_OBJECT

public:
    explicit ReservationService(QObject* parent = nullptr);

    void setConnection(charging::client::network::ClientConnection* connection);
    void setLiveMode(bool enabled);
    bool liveMode() const;

    // 提交/取消在真实通道需要登录用户：由宿主壳注入（未登录为 0）。
    void setUserId(qint64 userId);

    // 车辆名额来源：注入后名额 = 车辆数、每车至多一条；未注入回退为
    // 单条约束（兼容独立测试）。由 HomeShell 统一装配。
    void setSettingsService(settings::SettingsService* settings);

    // 演示/测试分支开关：
    // - setSimulateFailure：下一次任意请求走失败路径（网络/接口异常态）；
    // - setSimulateNextSubmitConflict：下一次提交模拟“桩被他人抢占”（并发）。
    void setSimulateFailure(bool simulate);
    void setSimulateNextSubmitConflict(bool conflict);

    // 覆盖模拟记录集合（空列表用于演示“暂无预约记录”态）。
    void setMockRecords(const ReservationList& records);

    // 我的预约记录（预约模块：进行中/已完成两个页面共用同一列表通道）。
    void fetchList();

    // 名额制业务约束（任务 #17 二次迭代，模拟通道口径；真实通道以服务端
    // 校验为准）：
    // - activeReservationCount()：状态“预约中”且未到截止时间的记录数；
    // - activeCountForVehicle(vehicleId)：该车辆的未结束预约数（每车至多 1）；
    // - unfinishedSlotLimit()：可同时持有的有效预约上限 = 车辆数。
    int activeReservationCount() const;
    int activeCountForVehicle(qint64 vehicleId) const;
    int unfinishedSlotLimit() const;

    // 提交预约（时间段版）：站点/桩/起始时刻/车辆来自预约确认页上下文；
    // 时长 = end - start（上限 45 分钟，Service 兜底校验）；
    // distanceMeters 为展示用虚拟导航距离（对接导航模块）。
    void submit(const charging::model::Charger& charger, const charging::model::Station& station,
                const QDateTime& startUtc, const QDateTime& endUtc, qint64 vehicleId,
                const QString& vehiclePlate, int distanceMeters = -1);

    // 取消预约（仅“预约中”记录可取消）。
    void cancel(qint64 reservationId);

    // 倒计时归零 → 预约状态自动流转为“已过期”（预约订单页每秒刷新驱动；
    // 真实通道该流转由服务端负责，本地同步收敛展示状态）。
    void expireReservation(qint64 reservationId);

    // 迟到自动取消（预约订单页每秒 tick 驱动）：已过“开始 + 15 分钟”
    // 且时段仍在有效期内的“预约中”记录 → “已取消”（lateCancelled 标记），
    // 逐条发 reservationExpired 信号驱动模块刷新。返回本次取消条数。
    int cancelLateReservations();

    // 系统推荐时段：start = 现在 + 模拟行驶时长后向上对齐 15 分钟刻度，
    // end = start + 45 分钟（时间段预约的”✨ 使用系统推荐时段”）。
    static RecommendedSlot recommendSlot(int distanceMeters,
                                         const QDateTime& nowUtc = QDateTime::currentDateTimeUtc());

    // 同口径推荐时段，但行驶时长由调用方提供（腾讯距离矩阵真实时长
    // durationSeconds/60 向上取整 + 出发准备分钟由调用方合并）——真实
    // 地图数据就绪后确认页改走本入口，估算版保持兜底。
    static RecommendedSlot recommendSlotFromTravelMinutes(
        int travelMinutes, const QDateTime& nowUtc = QDateTime::currentDateTimeUtc());

signals:
    void listStarted();
    void listSucceeded(const charging::client::services::reservation::ReservationList& records);
    void listFailed(const QString& message);
    void submitStarted(qint64 chargerId);
    void submitSucceeded(const charging::client::services::reservation::ReservationRecord& record);
    void submitFailed(const QString& reason);
    void cancelStarted(qint64 reservationId);
    void cancelSucceeded(qint64 reservationId);
    void cancelFailed(const QString& message);
    // 记录状态自动流转（倒计时归零 → 已过期；迟到 → 已取消·迟到），
    // 预约订单页据此刷新、模块切换展示。
    void reservationExpired(qint64 reservationId);

private:
    void handleResponse(const charging::protocol::ResponseEnvelope& response);
    void handleRequestFailure(const QString& requestId, const QString& errorCode,
                              const QString& message);
    void finishMockList();
    void finishMockSubmit();
    void finishMockCancel();
    static QString chargerSpecText(const charging::model::Charger& charger);

    charging::client::network::ClientConnection* connection_ = nullptr;
    settings::SettingsService* settings_ = nullptr;
    bool liveMode_ = false;
    bool simulateFailure_ = false;
    bool simulateSubmitConflict_ = false;
    qint64 userId_ = 0;
    QString pendingListRequestId_;
    QString pendingSubmitRequestId_;
    QString pendingCancelRequestId_;
    qint64 pendingCancelId_ = 0;
    ReservationRecord pendingSubmitRecord_; // live 响应补齐展示上下文用
    ReservationList mockStore_;
    ReservationList liveStore_;
    ReservationList accumulatedList_;
    int listPage_ = 1;
};

} // namespace charging::client::services::reservation

Q_DECLARE_METATYPE(charging::client::services::reservation::ReservationList)
Q_DECLARE_METATYPE(charging::client::services::reservation::ReservationRecord)
