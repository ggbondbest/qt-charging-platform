#pragma once

#include "charging/common/model/models.h"
#include "charging/common/protocol/protocol.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

namespace charging::client::network {
class ClientConnection;
}

namespace charging::client::services::reservation {

// 面向 UI 的预约记录：核心模型 model::Reservation + 展示上下文
// （站点名称、桩编号、预约时长、预估费用）。后端预约列表查询接口
// （GET_RESERVATIONS）就绪后由真实通道补齐这些字段，UI 层零改动。
struct ReservationRecord
{
    charging::model::Reservation reservation;
    QString stationName;
    QString chargerCode;
    int durationMinutes = 0;
    qint64 estimatedFeeCents = 0;
};

using ReservationList = QVector<ReservationRecord>;

// 充电桩预约服务（成员 2，任务 #17）。
//
// 双通道设计，与站点查询服务一致，页面 UI 对二者无感知：
// - 模拟通道（当前默认）：提交/取消/记录列表全部本地完成，带模拟延迟驱动
//   加载状态；支持“桩被抢占”“预约冲突”“参数非法”等失败分支演示；
// - 真实通道：服务端已实现 RESERVE_CHARGER / CANCEL_RESERVATION（需登录
//   会话），注入 ClientConnection + setLiveMode(true) 即无缝切换；预约列表
//   查询命令协议尚未定义，真实通道按未知命令走友好失败路径（UI 显示错误
//   提示），命令就绪后无需改页面代码。
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

    // 演示/测试分支开关：
    // - setSimulateFailure：下一次任意请求走失败路径（网络/接口异常态）；
    // - setSimulateNextSubmitConflict：下一次提交模拟“桩被他人抢占”（并发）。
    void setSimulateFailure(bool simulate);
    void setSimulateNextSubmitConflict(bool conflict);

    // 覆盖模拟记录集合（空列表用于演示“暂无预约记录”态）。
    void setMockRecords(const ReservationList& records);

    // 我的预约记录（记录页）。
    void fetchList();

    // 提交预约：站点/桩来自详情页上下文；时长分钟数用于预估费用与到期时间。
    void submit(const charging::model::Charger& charger, const charging::model::Station& station,
                int durationMinutes);

    // 取消预约（仅“预约中”记录可取消）。
    void cancel(qint64 reservationId);

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

private:
    void handleResponse(const charging::protocol::ResponseEnvelope& response);
    void handleRequestFailure(const QString& requestId, const QString& errorCode,
                              const QString& message);
    void finishMockList();
    void finishMockSubmit();
    void finishMockCancel();

    charging::client::network::ClientConnection* connection_ = nullptr;
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
};

} // namespace charging::client::services::reservation

Q_DECLARE_METATYPE(charging::client::services::reservation::ReservationList)
Q_DECLARE_METATYPE(charging::client::services::reservation::ReservationRecord)
