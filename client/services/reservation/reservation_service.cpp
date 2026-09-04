#include "services/reservation/reservation_service.h"

#include "charging/common/model/model_json.h"
#include "network/client_connection.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>

namespace charging::client::services::reservation {

namespace {

// 模拟网络延迟：驱动提交/列表的加载状态；真实通道无此延迟。
constexpr int kMockLatencyMs = 400;

using charging::model::Reservation;
using charging::model::ReservationStatus;

Reservation makeCoreReservation(qint64 id, qint64 userId, qint64 chargerId,
                                ReservationStatus status, int reservedMinutesAgo,
                                int durationMinutes)
{
    Reservation reservation;
    reservation.id = id;
    reservation.userId = userId;
    reservation.chargerId = chargerId;
    reservation.status = status;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    reservation.reservedAtUtc = now.addSecs(-reservedMinutesAgo * 60);
    reservation.expiresAtUtc = reservation.reservedAtUtc.addSecs(durationMinutes * 60);
    if (status == ReservationStatus::Fulfilled || status == ReservationStatus::Cancelled
        || status == ReservationStatus::Expired) {
        reservation.endedAtUtc = reservation.expiresAtUtc;
    }
    return reservation;
}

// 演示记录：覆盖 预约中 / 已完成 / 已取消 / 已过期 四种状态；距离为虚拟
// 占位数据（预留对接后续导航模块）。默认“预约中”剩余约 50 分钟（倒计时
// 绿色档），供预约订单页演示每秒刷新。
ReservationList defaultMockRecords()
{
    return {
        {makeCoreReservation(9001, 0, 3007, ReservationStatus::Active, 10, 60),
         QStringLiteral("南山智造充电站"), QStringLiteral("SZ-NSZ-03-07"),
         QStringLiteral("直流快充 · 120kW"), 60, 98, 1250},
        {makeCoreReservation(9002, 0, 5002, ReservationStatus::Fulfilled, 300, 90),
         QStringLiteral("后海城市广场站"), QStringLiteral("SZ-HTC-05-02"),
         QStringLiteral("直流快充 · 160kW"), 90, 158, 860},
        {makeCoreReservation(9003, 0, 1005, ReservationStatus::Cancelled, 480, 30),
         QStringLiteral("科技园充电驿站"), QStringLiteral("SZ-KEY-01-05"),
         QStringLiteral("交流慢充 · 7kW"), 30, 60, 2400},
        {makeCoreReservation(9004, 0, 2003, ReservationStatus::Expired, 1500, 120),
         QStringLiteral("深大北门超充站"), QStringLiteral("SZ-SDU-02-03"),
         QStringLiteral("直流快充 · 240kW"), 120, 276, 5100},
    };
}

// 协议尚未定义预约列表查询命令；就绪后替换为 protocol 常量即可（UI 不变）。
const QString kGetReservationsType = QStringLiteral("GET_RESERVATIONS");

} // namespace

ReservationService::ReservationService(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<ReservationList>(
        "charging::client::services::reservation::ReservationList");
    qRegisterMetaType<ReservationRecord>(
        "charging::client::services::reservation::ReservationRecord");
    mockStore_ = defaultMockRecords();
}

void ReservationService::setConnection(charging::client::network::ClientConnection* connection)
{
    if (connection_ == connection) {
        return;
    }
    connection_ = connection;
    if (connection_ != nullptr) {
        connect(connection_, &network::ClientConnection::responseReceived, this,
                &ReservationService::handleResponse);
        connect(connection_, &network::ClientConnection::requestFailed, this,
                &ReservationService::handleRequestFailure);
    }
}

void ReservationService::setLiveMode(bool enabled)
{
    liveMode_ = enabled;
}

bool ReservationService::liveMode() const
{
    return liveMode_;
}

void ReservationService::setUserId(qint64 userId)
{
    userId_ = userId;
}

void ReservationService::setSimulateFailure(bool simulate)
{
    simulateFailure_ = simulate;
}

void ReservationService::setSimulateNextSubmitConflict(bool conflict)
{
    simulateSubmitConflict_ = conflict;
}

void ReservationService::setMockRecords(const ReservationList& records)
{
    mockStore_ = records;
}

void ReservationService::fetchList()
{
    emit listStarted();

    if (liveMode_ && connection_ != nullptr) {
        QJsonObject data;
        data.insert(QStringLiteral("userId"), QString::number(userId_));
        pendingListRequestId_ = connection_->sendRequest(kGetReservationsType, data);
        return;
    }

    QTimer::singleShot(kMockLatencyMs, this, &ReservationService::finishMockList);
}

bool ReservationService::hasUnfinishedReservation() const
{
    // “未结束”= 状态为预约中且倒计时（expiresAtUtc）未归零。真实通道下
    // 本方法仅作前端入口拦截的参考态，提交时服务端仍会独立校验。
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const auto& record : mockStore_) {
        if (record.reservation.status == charging::model::ReservationStatus::Active
            && record.reservation.expiresAtUtc > now) {
            return true;
        }
    }
    return false;
}

void ReservationService::submit(const charging::model::Charger& charger,
                                const charging::model::Station& station, int durationMinutes,
                                int distanceMeters)
{
    emit submitStarted(charger.id);

    ReservationRecord record;
    record.reservation.chargerId = charger.id;
    record.reservation.userId = userId_;
    record.stationName = station.name;
    record.chargerCode = charger.code;
    record.chargerSpec = chargerSpecText(charger);
    record.durationMinutes = durationMinutes;
    record.estimatedFeeCents = station.priceCentsPerKwh * durationMinutes / 60;
    record.distanceMeters = distanceMeters;
    pendingSubmitRecord_ = record;

    if (durationMinutes <= 0) {
        // 参数非法：直接失败（真实通道同样先做本地校验，避免无效请求）。
        QTimer::singleShot(0, this,
                           [this]() { emit submitFailed(tr("预约时长无效，请重新选择")); });
        return;
    }

    if (liveMode_ && connection_ != nullptr) {
        QJsonObject data;
        data.insert(QStringLiteral("chargerId"), QString::number(charger.id));
        pendingSubmitRequestId_ = connection_->sendRequest(
            QString::fromLatin1(charging::protocol::request_type::kReserveCharger), data);
        return;
    }

    QTimer::singleShot(kMockLatencyMs, this, &ReservationService::finishMockSubmit);
}

void ReservationService::cancel(qint64 reservationId)
{
    emit cancelStarted(reservationId);
    pendingCancelId_ = reservationId;

    if (liveMode_ && connection_ != nullptr) {
        QJsonObject data;
        data.insert(QStringLiteral("reservationId"), QString::number(reservationId));
        pendingCancelRequestId_ = connection_->sendRequest(
            QString::fromLatin1(charging::protocol::request_type::kCancelReservation), data);
        return;
    }

    QTimer::singleShot(kMockLatencyMs, this, &ReservationService::finishMockCancel);
}

void ReservationService::finishMockList()
{
    if (simulateFailure_) {
        simulateFailure_ = false;
        emit listFailed(tr("预约记录加载失败，请稍后重试"));
        return;
    }

    // 最新预约在前（页面按此顺序展示）。
    ReservationList sorted = mockStore_;
    std::stable_sort(sorted.begin(), sorted.end(), [](const ReservationRecord& left,
                                                      const ReservationRecord& right) {
        return left.reservation.reservedAtUtc > right.reservation.reservedAtUtc;
    });
    emit listSucceeded(sorted);
}

QString ReservationService::chargerSpecText(const charging::model::Charger& charger)
{
    const bool fast = charger.type == charging::model::ChargerType::Fast;
    return QStringLiteral("%1 · %2kW")
        .arg(fast ? QStringLiteral("直流快充") : QStringLiteral("交流慢充"))
        .arg(charger.powerWatts / 1000);
}

void ReservationService::finishMockSubmit()
{
    const ReservationRecord requested = pendingSubmitRecord_;
    pendingSubmitRecord_ = ReservationRecord{};

    if (simulateFailure_) {
        simulateFailure_ = false;
        emit submitFailed(tr("预约服务暂时不可用，请稍后重试"));
        return;
    }

    if (simulateSubmitConflict_) {
        // 并发边界：提交瞬间桩被其他人抢占（真实通道对应服务端返回错误）。
        simulateSubmitConflict_ = false;
        emit submitFailed(tr("该充电桩刚刚被其他用户抢占，请选择其他空闲桩"));
        return;
    }

    // 业务约束（任务 #17 迭代）：同一用户同一时刻仅允许一条未结束预约。
    // 即使前端入口被绕过，模拟通道在这里同样返回业务错误。
    if (hasUnfinishedReservation()) {
        emit submitFailed(tr("您当前尚有未结束的预约，请结束当前预约后再发起新预约"));
        return;
    }

    ReservationRecord created = requested;
    created.reservation.id = 9000 + mockStore_.size() + 1;
    created.reservation.status = charging::model::ReservationStatus::Active;
    created.reservation.reservedAtUtc = QDateTime::currentDateTimeUtc();
    created.reservation.expiresAtUtc =
        created.reservation.reservedAtUtc.addSecs(created.durationMinutes * 60);
    mockStore_.append(created);
    emit submitSucceeded(created);
}

void ReservationService::finishMockCancel()
{
    const qint64 reservationId = pendingCancelId_;
    pendingCancelId_ = 0;

    if (simulateFailure_) {
        simulateFailure_ = false;
        emit cancelFailed(tr("取消预约失败，请稍后重试"));
        return;
    }

    for (auto& record : mockStore_) {
        if (record.reservation.id != reservationId) {
            continue;
        }
        if (record.reservation.status != charging::model::ReservationStatus::Active) {
            emit cancelFailed(tr("该预约已结束，无法取消"));
            return;
        }
        record.reservation.status = charging::model::ReservationStatus::Cancelled;
        record.reservation.endedAtUtc = QDateTime::currentDateTimeUtc();
        emit cancelSucceeded(reservationId);
        return;
    }
    emit cancelFailed(tr("预约记录不存在，请刷新后重试"));
}

void ReservationService::expireReservation(qint64 reservationId)
{
    // 倒计时归零的状态流转：预约订单页每秒刷新检测到剩余 ≤ 0 时调用。
    // 幂等：仅对仍处于“预约中”的记录生效（真实通道以服务端流转为准，
    // 本地同步收敛展示状态，命令就绪后 UI 零改动）。
    for (auto& record : mockStore_) {
        if (record.reservation.id == reservationId
            && record.reservation.status == charging::model::ReservationStatus::Active) {
            record.reservation.status = charging::model::ReservationStatus::Expired;
            record.reservation.endedAtUtc = QDateTime::currentDateTimeUtc();
            break;
        }
    }
    emit reservationExpired(reservationId);
}

void ReservationService::handleResponse(const charging::protocol::ResponseEnvelope& response)
{
    const bool isList = response.requestId == pendingListRequestId_
        && response.type == kGetReservationsType;
    const bool isSubmit = response.requestId == pendingSubmitRequestId_
        && response.type
            == QString::fromLatin1(charging::protocol::request_type::kReserveCharger);
    const bool isCancel = response.requestId == pendingCancelRequestId_
        && response.type
            == QString::fromLatin1(charging::protocol::request_type::kCancelReservation);
    if (!isList && !isSubmit && !isCancel) {
        return;
    }

    if (!response.success) {
        const QString message = response.error.message.isEmpty()
            ? tr("预约服务暂时不可用，请稍后重试")
            : response.error.message;
        if (isList) {
            pendingListRequestId_.clear();
            emit listFailed(message);
        } else if (isSubmit) {
            pendingSubmitRequestId_.clear();
            // 服务端错误（如 CHARGER_NOT_AVAILABLE）文案面向用户可读。
            emit submitFailed(message);
        } else {
            pendingCancelRequestId_.clear();
            emit cancelFailed(message);
        }
        return;
    }

    if (isList) {
        pendingListRequestId_.clear();
        // 真实列表命令就绪后的解析路径：data["reservations"] → 记录列表。
        ReservationList records;
        const QJsonArray items = response.data.value(QStringLiteral("reservations")).toArray();
        for (const auto& value : items) {
            Reservation reservation;
            QString parseError;
            if (!charging::model::fromJson(value.toObject(), &reservation, &parseError)) {
                emit listFailed(tr("预约数据解析失败：%1").arg(parseError));
                return;
            }
            ReservationRecord record;
            record.reservation = reservation;
            records.append(record);
        }
        emit listSucceeded(records);
        return;
    }

    if (!response.data.contains(QStringLiteral("reservation"))) {
        const QString message = tr("预约响应缺少预约信息");
        if (isSubmit) {
            pendingSubmitRequestId_.clear();
            emit submitFailed(message);
        } else {
            pendingCancelRequestId_.clear();
            emit cancelFailed(message);
        }
        return;
    }

    Reservation reservation;
    QString parseError;
    if (!charging::model::fromJson(
            response.data.value(QStringLiteral("reservation")).toObject(), &reservation,
            &parseError)) {
        const QString message = tr("预约数据解析失败：%1").arg(parseError);
        if (isSubmit) {
            pendingSubmitRequestId_.clear();
            emit submitFailed(message);
        } else {
            pendingCancelRequestId_.clear();
            emit cancelFailed(message);
        }
        return;
    }

    if (isSubmit) {
        pendingSubmitRequestId_.clear();
        // 展示上下文（站名/桩编号/时长/费用）由提交请求侧带入，与列表模拟
        // 数据同构；真实列表命令就绪后改由服务端返回补齐。
        ReservationRecord record = pendingSubmitRecord_;
        pendingSubmitRecord_ = ReservationRecord{};
        record.reservation = reservation;
        emit submitSucceeded(record);
    } else {
        const qint64 reservationId = pendingCancelId_;
        pendingCancelRequestId_.clear();
        pendingCancelId_ = 0;
        Q_UNUSED(reservation)
        emit cancelSucceeded(reservationId);
    }
}

void ReservationService::handleRequestFailure(const QString& requestId, const QString& errorCode,
                                              const QString& message)
{
    Q_UNUSED(errorCode)
    if (requestId == pendingListRequestId_) {
        pendingListRequestId_.clear();
        emit listFailed(message.isEmpty() ? tr("网络异常，预约记录加载失败") : message);
    } else if (requestId == pendingSubmitRequestId_) {
        pendingSubmitRequestId_.clear();
        emit submitFailed(message.isEmpty() ? tr("网络异常，预约提交失败") : message);
    } else if (requestId == pendingCancelRequestId_) {
        pendingCancelRequestId_.clear();
        pendingCancelId_ = 0;
        emit cancelFailed(message.isEmpty() ? tr("网络异常，取消预约失败") : message);
    }
}

} // namespace charging::client::services::reservation
