// ReservationService 接口（成员 2，充电桩预约·任务 #17 二次迭代 / 迭代 3 批）。
// 职责：预约提交/取消/记录列表的唯一业务入口，并承担“倒计时归零→已过期”
// “迟到超 15 分钟→已取消”两条自动状态流转；ReservationRecord 给页面补足
// 展示上下文（common 模型无时间段/车辆字段，客户端扩展、后端字段就绪后换源）。
// 使用方：widgets HomeShell（预约确认/订单/模块页，通知桥接经
// reservationRecord() 回查站名桩号）与 QML app_bridge/service_bridges
// （ReservationBridge → 预约相关 QML 页面）。
// 数据流向：页面动作 → 本服务 → 双通道出口——模拟通道本地 mockStore_ +
// QTimer 延迟（驱动加载态）；真实通道经 ClientConnection 走 TCP 契约
// RESERVE_CHARGER / CANCEL_RESERVATION / GET_RESERVATIONS（车辆绑定等
// 协议扩展就绪前，名额闸以模拟通道为准）。结果统一以信号回页面。
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

// 列表统一载体：信号参数、模拟/真实两份存储都用它（Qt 元类型，构造里
// qRegisterMetaType 登记后方可跨线程排队传递）。
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

    // ---- 真实通道装配三件套：注入 ClientConnection（可为空=离线演示）并
    // setLiveMode(true) 后命令才走 TCP——各命令分叉统一按 `liveMode_ &&
    // connection_` 双条件判定（细节见 .cpp），页面 UI 全程无感知 ----
    void setConnection(charging::client::network::ClientConnection* connection);
    void setLiveMode(bool enabled);
    bool liveMode() const;

    // 提交/取消在真实通道需要登录用户：由宿主壳注入（未登录为 0）。
    void setUserId(qint64 userId);

    // 车辆名额来源：注入后名额 = 车辆数、每车至多一条；未注入回退为
    // 单条约束（兼容独立测试）。由 HomeShell 统一装配。
    // 现势装配差异：widgets HomeShell 注入（车辆名额语义全量生效）；QML
    // AppBridge 于 2026-09-08“预约不再强制车辆”业务变更后撤除注入——
    // finishMockSubmit 的“无车拒绝/每车唯一”两道闸随之自然失效。
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

    // 迭代 3：按 ID 只读回查记录（取消/到期信号只带 ID，通知桥接取站名/桩号
    // 展示上下文用）。不存在返回 nullptr；指针在列表变更前保持有效。
    const ReservationRecord* reservationRecord(qint64 reservationId) const;

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

    // ---- 信号面：每条命令一组 started/succeeded/failed 三态（双通道共用
    // 同一信号形状，页面对模拟/真实无感知）；reservationExpired 是迭代 3
    // 的“状态自动流转”通知（只带 ID，上下文经 reservationRecord 回查）----
signals:
    void listStarted();
    void listSucceeded(const charging::client::services::reservation::ReservationList& records);
    void listFailed(const QString& message);
    void submitStarted(qint64 chargerId);
    void submitSucceeded(const charging::client::services::reservation::ReservationRecord& record);
    void submitFailed(const QString& reason);
    // submitRejected：仅真实通道被服务端拒绝时伴随 submitFailed 发出，携带
    // ProtocolError 错误码供 QML bridge 拆给页面做精确分支（如占用冲突）。
    void submitRejected(const charging::protocol::ProtocolError& error);
    void cancelStarted(qint64 reservationId);
    void cancelSucceeded(qint64 reservationId);
    void cancelFailed(const QString& message);
    // 记录状态自动流转（倒计时归零 → 已过期；迟到 → 已取消·迟到），
    // 预约订单页据此刷新、模块切换展示。
    void reservationExpired(qint64 reservationId);

private:
    // ---- 真实通道响应路由（见 .cpp：连接广播需按在途 requestId 认领）----
    void handleResponse(const charging::protocol::ResponseEnvelope& response);
    void handleRequestFailure(const QString& requestId, const QString& errorCode,
                              const QString& message);
    // ---- 模拟通道延迟完成入口（QTimer::singleShot(kMockLatencyMs) 回调）----
    void finishMockList();
    void finishMockSubmit();
    void finishMockCancel();
    static QString chargerSpecText(const charging::model::Charger& charger);

    // ---- 装配与开关（宿主壳注入面）----
    charging::client::network::ClientConnection* connection_ = nullptr;
    settings::SettingsService* settings_ = nullptr;
    bool liveMode_ = false;
    bool simulateFailure_ = false;
    bool simulateSubmitConflict_ = false;
    qint64 userId_ = 0;
    // ---- 在途配对键：requestId 由 sendRequest 返回、成功/失败即清，
    // 广播响应据此认领（防止串到别的在途请求）；pendingCancelId_ 兼作
    // cancel 防重入闸——同一时刻至多一笔取消在途 ----
    QString pendingListRequestId_;
    QString pendingSubmitRequestId_;
    QString pendingCancelRequestId_;
    qint64 pendingCancelId_ = 0;
    ReservationRecord pendingSubmitRecord_; // live 响应补齐展示上下文用
    // ---- 数据源三态：mockStore_ 模拟通道全量状态（状态流转只写它）；
    // liveStore_ 真实通道本地镜像（以服务端响应收敛）；
    // accumulatedList_/listPage_ 分页拉全的中间缓冲（拉齐前不外发）----
    ReservationList mockStore_;
    ReservationList liveStore_;
    ReservationList accumulatedList_;
    int listPage_ = 1;
};

} // namespace charging::client::services::reservation

// 跨线程/排队连接携带信号参数所需（构造里 qRegisterMetaType 成对登记）。
Q_DECLARE_METATYPE(charging::client::services::reservation::ReservationList)
Q_DECLARE_METATYPE(charging::client::services::reservation::ReservationRecord)
