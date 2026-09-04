#include "services/reservation/reservation_service.h"

#include "charging/common/model/model_json.h"
#include "network/client_connection.h"
#include "services/settings/settings_service.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace charging::client::services::reservation {

namespace {

// 模拟网络延迟：驱动提交/列表的加载状态；真实通道无此延迟。
constexpr int kMockLatencyMs = 400;

// 时间段预约业务参数（任务 #17 二次迭代）：
// - 单段时长上限 45 分钟；
// - 迟到宽限：超过“开始 + 15 分钟”仍未开始 → 自动取消；
// - 推荐时段行驶时长估算（模拟）：出发准备 5 分钟 + 每 500 米 1 分钟，
//   腾讯地图距离矩阵/路线规划 API 就绪后仅需替换本估算。
constexpr int kMaxSlotMinutes = 45;
constexpr int kLateGraceSecs = 15 * 60;
constexpr int kTravelBaseMinutes = 5;
constexpr int kTravelMetersPerMinute = 500;
constexpr int kSlotAlignmentSecs = 15 * 60;

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

// 演示记录：覆盖 预约中 / 已完成 / 已取消 / 已过期 四种状态，含时段与
// 车辆上下文；距离为虚拟占位数据（预留对接后续导航模块）。默认“预约中”
// 已开始约 2 分钟（45 分钟时段、剩余 43 分钟 → 倒计时绿色档），供预约
// 订单页演示每秒刷新。
ReservationList defaultMockRecords()
{
    const auto makeRecord = [](qint64 id, qint64 chargerId, ReservationStatus status,
                               int startMinutesAgo, int durationMinutes, const QString& station,
                               const QString& code, const QString& spec, const QString& plate,
                               qint64 vehicleId, qint64 feeCents, int distanceMeters) {
        ReservationRecord record;
        record.reservation =
            makeCoreReservation(id, 0, chargerId, status, startMinutesAgo, durationMinutes);
        record.startAtUtc = record.reservation.reservedAtUtc;
        record.stationName = station;
        record.chargerCode = code;
        record.chargerSpec = spec;
        record.vehicleId = vehicleId;
        record.vehiclePlate = plate;
        record.durationMinutes = durationMinutes;
        record.estimatedFeeCents = feeCents;
        record.distanceMeters = distanceMeters;
        return record;
    };
    return {
        makeRecord(9001, 3007, ReservationStatus::Active, 2, 45,
                   QStringLiteral("南山智造充电站"), QStringLiteral("SZ-NSZ-03-07"),
                   QStringLiteral("直流快充 · 120kW"), QStringLiteral("粤B·DA1234"), 1, 73,
                   1250),
        makeRecord(9002, 5002, ReservationStatus::Fulfilled, 300, 30,
                   QStringLiteral("后海城市广场站"), QStringLiteral("SZ-HTC-05-02"),
                   QStringLiteral("直流快充 · 160kW"), QStringLiteral("粤B·DB5678"), 2, 79,
                   860),
        makeRecord(9003, 1005, ReservationStatus::Cancelled, 480, 15,
                   QStringLiteral("科技园充电驿站"), QStringLiteral("SZ-KEY-01-05"),
                   QStringLiteral("交流慢充 · 7kW"), QStringLiteral("粤B·DC9012"), 3, 15,
                   2400),
        makeRecord(9004, 2003, ReservationStatus::Expired, 1500, 45,
                   QStringLiteral("深大北门超充站"), QStringLiteral("SZ-SDU-02-03"),
                   QStringLiteral("直流快充 · 240kW"), QStringLiteral("粤B·DF1357"), 4, 207,
                   5100),
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

int ReservationService::activeReservationCount() const
{
    // “未结束”= 状态为预约中且截止时间（expiresAtUtc）未到。真实通道下
    // 本组方法仅作前端入口拦截的参考态，提交时服务端仍会独立校验。
    const QDateTime now = QDateTime::currentDateTimeUtc();
    int count = 0;
    for (const auto& record : mockStore_) {
        if (record.reservation.status == charging::model::ReservationStatus::Active
            && record.reservation.expiresAtUtc > now) {
            ++count;
        }
    }
    return count;
}

int ReservationService::activeCountForVehicle(qint64 vehicleId) const
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    int count = 0;
    for (const auto& record : mockStore_) {
        if (record.vehicleId == vehicleId
            && record.reservation.status == charging::model::ReservationStatus::Active
            && record.reservation.expiresAtUtc > now) {
            ++count;
        }
    }
    return count;
}

int ReservationService::unfinishedSlotLimit() const
{
    // 名额制（任务 #17 二次迭代）：可预约时段数 = 车辆数；未注入车辆
    // 服务时回退为单条约束（兼容独立测试与旧口径）。
    return settings_ != nullptr ? qMax(0, settings_->vehicleCount()) : 1;
}

void ReservationService::submit(const charging::model::Charger& charger,
                                const charging::model::Station& station, const QDateTime& startUtc,
                                const QDateTime& endUtc, qint64 vehicleId, const QString& vehiclePlate,
                                int distanceMeters)
{
    emit submitStarted(charger.id);

    const int durationMinutes =
        startUtc.isValid() && endUtc.isValid() ? int(startUtc.secsTo(endUtc) / 60) : 0;

    ReservationRecord record;
    record.reservation.chargerId = charger.id;
    record.reservation.userId = userId_;
    record.stationName = station.name;
    record.chargerCode = charger.code;
    record.chargerSpec = chargerSpecText(charger);
    record.startAtUtc = startUtc;
    record.vehicleId = vehicleId;
    record.vehiclePlate = vehiclePlate;
    record.durationMinutes = durationMinutes;
    record.estimatedFeeCents =
        station.priceCentsPerKwh * qMax(0, durationMinutes) / 60;
    record.distanceMeters = distanceMeters;
    pendingSubmitRecord_ = record;

    if (durationMinutes <= 0) {
        // 参数非法（时间段无效）：直接失败（真实通道同样先做本地校验）。
        QTimer::singleShot(0, this, [this]() {
            emit submitFailed(tr("预约时间段无效，请重新选择"));
        });
        return;
    }
    if (durationMinutes > kMaxSlotMinutes) {
        // 规格约束：推荐时间段最大 45 分钟（UI 已行内拦截，此处 Service 兜底）。
        QTimer::singleShot(0, this, [this]() {
            emit submitFailed(tr("预约时间段不能超过 %1 分钟").arg(kMaxSlotMinutes));
        });
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

void ReservationService::setSettingsService(settings::SettingsService* settings)
{
    settings_ = settings;
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

    // 名额制业务约束（任务 #17 二次迭代）：车辆数 = 可同时持有的有效预约
    // 上限，且每辆至多一条未结束预约。即使前端入口被绕过，模拟通道在这里
    // 同样返回业务错误（真实通道以服务端校验为准）。
    if (settings_ != nullptr) {
        if (settings_->vehicleCount() <= 0) {
            emit submitFailed(tr("请先在设置-车辆管理添加车辆，再发起预约"));
            return;
        }
        if (activeCountForVehicle(requested.vehicleId) > 0) {
            emit submitFailed(tr("该车辆已有未结束的预约，请更换车辆或先结束该车预约"));
            return;
        }
    }
    if (activeReservationCount() >= unfinishedSlotLimit()) {
        emit submitFailed(tr("可预约名额已全部占用，请结束当前预约后再发起新预约"));
        return;
    }

    ReservationRecord created = requested;
    created.reservation.id = 9000 + mockStore_.size() + 1;
    created.reservation.status = charging::model::ReservationStatus::Active;
    // 时间段预约：开始时刻即预约生效时刻，倒计时数据源仍为 expiresAtUtc。
    created.reservation.reservedAtUtc = created.startAtUtc;
    created.reservation.expiresAtUtc =
        created.startAtUtc.addSecs(created.durationMinutes * 60);
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

int ReservationService::cancelLateReservations()
{
    // 迟到自动取消（预约订单页每秒 tick 驱动）：已过“开始 + 15 分钟”
    // 且时段仍在有效期内、状态“预约中” → 流转“已取消”并打迟到标记；
    // 已过截止时间的记录交给倒计时归零的“已过期”流转处理，此处不抢状态。
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVector<qint64> cancelledIds;
    for (auto& record : mockStore_) {
        if (record.reservation.status != charging::model::ReservationStatus::Active
            || !record.startAtUtc.isValid()) {
            continue;
        }
        const bool late = record.startAtUtc.secsTo(now) > kLateGraceSecs;
        const bool stillValid = record.reservation.expiresAtUtc > now;
        if (late && stillValid) {
            record.reservation.status = charging::model::ReservationStatus::Cancelled;
            record.reservation.endedAtUtc = now;
            record.lateCancelled = true;
            cancelledIds.append(record.reservation.id);
        }
    }
    // 先改完状态再统一发信号，避免刷新回调在迭代中读取半成品列表。
    for (const qint64 id : std::as_const(cancelledIds)) {
        emit reservationExpired(id);
    }
    return cancelledIds.size();
}

RecommendedSlot ReservationService::recommendSlot(int distanceMeters, const QDateTime& nowUtc)
{
    // 模拟估算：行驶时长 = 出发准备 5 分钟 + 距离向上取整每 500 米 1 分钟；
    // 开始时刻 = 现在 + 行驶时长，向上对齐本地整点 15 分钟刻度；
    // 结束 = 开始 + 45 分钟（规格上限）。真实地图 API 就绪后仅替换估算。
    const int safeDistance = qMax(0, distanceMeters);
    const int travelMinutes = kTravelBaseMinutes
        + (safeDistance + kTravelMetersPerMinute - 1) / kTravelMetersPerMinute;

    QDateTime local = nowUtc.toLocalTime().addSecs(travelMinutes * 60);
    const QTime time = local.time();
    const int secsOfDay = time.hour() * 3600 + time.minute() * 60 + time.second();
    const int remainder = secsOfDay % kSlotAlignmentSecs;
    if (remainder != 0) {
        local = local.addSecs(kSlotAlignmentSecs - remainder);
    }

    RecommendedSlot slot;
    slot.travelMinutes = travelMinutes;
    slot.startUtc = local.toUTC();
    slot.endUtc = slot.startUtc.addSecs(kMaxSlotMinutes * 60);
    return slot;
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
        // 服务端尚未支持时间段/车辆字段（协议扩展就绪前以服务端返回的
        // 起止时刻为准，客户端仅带入车辆与站点展示上下文）。
        if (reservation.reservedAtUtc.isValid() && reservation.expiresAtUtc.isValid()) {
            record.startAtUtc = reservation.reservedAtUtc;
            record.durationMinutes =
                int(reservation.reservedAtUtc.secsTo(reservation.expiresAtUtc) / 60);
        }
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
