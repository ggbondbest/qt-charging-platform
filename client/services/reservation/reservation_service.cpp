// ReservationService 实现（类职责与双通道口径见同名头文件）。
// 出口一览：提交/取消/列表三条命令各自分叉——liveMode_ 且有连接走 TCP
// 契约（RESERVE_CHARGER / CANCEL_RESERVATION / GET_RESERVATIONS），否则走
// mockStore_ + 400ms 延迟的模拟完成；状态自动流转（expireReservation /
// cancelLateReservations）仅写模拟存储，真实通道以服务端响应收敛。
#include "services/reservation/reservation_service.h"

#include "charging/common/model/model_json.h"
#include "charging/common/protocol/protocol.h"
#include "network/client_connection.h"
#include "network/page_validation.h"
#include "services/settings/settings_service.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace charging::client::services::reservation {

// ---- 匿名命名空间：模拟通道常量、核心模型构造与演示数据（文件私有）----

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

// 由“N 分钟前预约 + 时长分钟”统一推导起止时刻：终态（已完成/已取消/
// 已过期）补 endedAtUtc，“预约中”留空——页面按状态语义渲染倒计时。
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

// 列表命令类型串上提为常量：fetchList 发送与 handleResponse 匹配共用同一
// 实例（request_type 是编译期 char[]，此处只转一次 QString）。
const QString kGetReservationsType =
    QString::fromLatin1(charging::protocol::request_type::kGetReservations);

} // namespace

// ---- 构造与宿主壳装配（widgets HomeShell / QML AppBridge 注入面）----

ReservationService::ReservationService(QObject* parent) : QObject(parent)
{
    // 登记元类型：信号携带 ReservationList/ReservationRecord 经排队连接
    // （跨线程或 QML bridge 转接）传参的前提；随后以演示记录播种，
    // 模拟通道未接任何服务也能渲染四态列表。
    qRegisterMetaType<ReservationList>(
        "charging::client::services::reservation::ReservationList");
    qRegisterMetaType<ReservationRecord>(
        "charging::client::services::reservation::ReservationRecord");
    mockStore_ = defaultMockRecords();
}

void ReservationService::setConnection(charging::client::network::ClientConnection* connection)
{
    // 同对象早退：重复注入不再 connect（否则一次响应会多次进 handleResponse）；
    // 换注入新连接时旧连接随宿主销毁自动断连，这里只需接上新一组广播。
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

// 通道开关：各命令分叉统一按 `liveMode_ && connection_` 双条件判定，页面无感。
// userId_ 为真实通道提交/取消的登录身份（未登录 0，由宿主壳登录态注入）。
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

// ---- 名额制业务约束读出（任务 #17 二次迭代：车辆数=有效预约上限，
// 每车至多一条；提交闸在 finishMockSubmit，这里供入口拦截/展示）----

int ReservationService::activeReservationCount() const
{
    // “未结束”= 状态为预约中且截止时间（expiresAtUtc）未到。真实通道下
    // 本组方法仅作前端入口拦截的参考态，提交时服务端仍会独立校验。
    const QDateTime now = QDateTime::currentDateTimeUtc();
    int count = 0;
    for (const auto& record : (liveMode_ ? liveStore_ : mockStore_)) {
        if (record.reservation.status == charging::model::ReservationStatus::Active
            && record.reservation.expiresAtUtc > now) {
            ++count;
        }
    }
    return count;
}

int ReservationService::activeCountForVehicle(qint64 vehicleId) const
{
    // 真实通道恒 0：服务端 v1 预约不绑定车辆（协议扩展未就绪），“每车
    // 唯一”闸在真实通道自然不适用，与 handleResponse 清零 vehicleId 同口径。
    if (liveMode_) return 0; // Server v1 does not bind reservations to vehicles.
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
    // 真实通道恒 1：服务端唯一索引现势强制“每用户至多一条有效预约”，
    // 名额协议扩展（时间段+车辆字段）就绪前不与模拟通道争口径。
    if (liveMode_) return 1;
    // 名额制（任务 #17 二次迭代）：可预约时段数 = 车辆数；未注入车辆
    // 服务时回退为单条约束（兼容独立测试与旧口径）。
    return settings_ != nullptr ? qMax(0, settings_->vehicleCount()) : 1;
}

const ReservationRecord* ReservationService::reservationRecord(qint64 reservationId) const
{
    // 迭代 3：取消/到期信号只携带 ID，通知服务桥接需要站名/桩号展示上下文，
    // 由此只读访问器回查（live/mock 各自数据源）。ID 不存在返回 nullptr。
    for (const auto& record : (liveMode_ ? liveStore_ : mockStore_)) {
        if (record.reservation.id == reservationId) {
            return &record;
        }
    }
    return nullptr;
}

// ---- 命令提交（分叉点：参数校验 → live 发请求 / mock 延迟完成）----

// 提交预约（时间段版）：先落 pendingSubmitRecord_ 再校验，保证任何失败分支
// 的 emit 都经事件循环下一拍送达——页面总能先看到 submitStarted，
// 成功/失败/拒绝三条出口时序同构（避免同步 emit 引发重入状态机）。
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
    // 粗估量级口径：单价（分/kWh）× 小时数（分钟/60），等效“平均功率 1kW”，
    // 仅演示展示；真实费用以订单结算为准。
    record.estimatedFeeCents =
        station.priceCentsPerKwh * qMax(0, durationMinutes) / 60;
    record.distanceMeters = distanceMeters;
    // 站点坐标透传：导航页据此向腾讯路线规划接口寻址目的地（无坐标则保持模拟）。
    record.hasStationLocation = station.latitude != 0.0 || station.longitude != 0.0;
    record.stationLatitude = station.latitude;
    record.stationLongitude = station.longitude;
    // 暂存副本：mock 延迟回调与 live 响应统一从这里取请求侧上下文，
    // lambda 不捕获入参（完成时页面上下文可能已变）。
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
        // 契约里 64 位 ID 一律走字符串（与响应侧 toString().toLongLong()
        // 对拍），防 JSON 数字按 double 截断精度。
        data.insert(QStringLiteral("chargerId"), QString::number(charger.id));
        pendingSubmitRequestId_ = connection_->sendRequest(
            QString::fromLatin1(charging::protocol::request_type::kReserveCharger), data);
        return;
    }

    // 模拟完成兜底：含“live 但未连接”的半开态（提交/列表回退本地，
    // 与 cancel 半开态显式失败相反——取消有票据，宁失败不假成功）。
    QTimer::singleShot(kMockLatencyMs, this, &ReservationService::finishMockSubmit);
}

// 名额闸数据源注入点：装配现状决定业务语义——widgets HomeShell 注入
// （车辆名额全量生效）；QML AppBridge 2026-09-08“预约不再强制车辆”批
// 撤除注入，finishMockSubmit 的两道车辆闸自然失效、全局闸回退“至多一条”。
void ReservationService::setSettingsService(settings::SettingsService* settings)
{
    settings_ = settings;
}

// 演示/测试钩子：置位后下一次模拟完成（列表/提交/取消）返回失败并
// 自动解除——一次性消费，只坏“下一单”，不影响之后的正常路径。
void ReservationService::setSimulateFailure(bool simulate)
{
    simulateFailure_ = simulate;
}

// 演示/测试钩子：仅拨动下一次模拟提交，复现“桩被他人抢占”的并发边界
// （真实通道无此开关，对应服务端返回 CHARGER_NOT_AVAILABLE 类错误）。
void ReservationService::setSimulateNextSubmitConflict(bool conflict)
{
    simulateSubmitConflict_ = conflict;
}

// 测试播种口：整体替换 mockStore_，不排序不校验，用例自由构造任意
// 状态组合（模拟通道的全部读接口都以该存储为数据源）。
void ReservationService::setMockRecords(const ReservationList& records)
{
    mockStore_ = records;
}

void ReservationService::fetchList()
{
    // 防重入（仅真实通道）：列表在途时忽略重复触发，避免叠发请求互踩
    // 配对键；模拟通道无在途概念，重复刷新直接排新延迟。
    if (liveMode_ && !pendingListRequestId_.isEmpty()) return;
    emit listStarted();

    if (liveMode_ && connection_ != nullptr) {
        // 每次查询重置分页累加：上一次的残页不得混入新结果。
        listPage_ = 1;
        accumulatedList_.clear();
        QJsonObject data{{"page", listPage_}, {"pageSize", 100}};
        // User identity belongs to the authenticated TCP Session.
        pendingListRequestId_ = connection_->sendRequest(kGetReservationsType, data);
        return;
    }

    QTimer::singleShot(kMockLatencyMs, this, &ReservationService::finishMockList);
}

void ReservationService::cancel(qint64 reservationId)
{
    // Keep one owner for the request/response pair. A repeated click or a
    // second view must not replace the ID whose acknowledgement is in flight.
    // 防重入闸：pendingCancelId_ 是非零票据（注释同义），同一时刻至多一笔
    // 取消在途；票据在失败/成功路径都会清零（见 handleResponse/此函数）。
    if (pendingCancelId_ > 0) return;
    if (reservationId <= 0) {
        // 票据未领用即早退：无效 ID 不占用闸，页面可立刻重试。
        emit cancelFailed(tr("预约编号无效，请刷新后重试"));
        return;
    }
    pendingCancelId_ = reservationId;
    emit cancelStarted(reservationId);

    if (liveMode_ && connection_ == nullptr) {
        // 半开态（live 但连接已断）显式失败，不静默回落模拟；先清零票据
        // 再 emit，取消闸不会因这条失败路径永久卡死。
        pendingCancelId_ = 0;
        emit cancelFailed(tr("预约服务未连接，请重新登录后重试"));
        return;
    }

    if (liveMode_ && connection_ != nullptr) {
        QJsonObject data;
        data.insert(QStringLiteral("reservationId"), QString::number(reservationId));
        pendingCancelRequestId_ = connection_->sendRequest(
            QString::fromLatin1(charging::protocol::request_type::kCancelReservation), data);
        return;
    }

    QTimer::singleShot(kMockLatencyMs, this, &ReservationService::finishMockCancel);
}

// ---- 模拟通道延迟完成（QTimer::singleShot(kMockLatencyMs) 回调落点）----

void ReservationService::finishMockList()
{
    // 护栏：延迟窗口内宿主壳切到真实通道，本次完成作废（结果由
    // handleResponse 负责），不向页面吐跨通道脏数据。
    if (liveMode_) return;
    if (simulateFailure_) {
        // 演示失败分支一次性消费：发出即解除，只影响“下一次”请求。
        simulateFailure_ = false;
        emit listFailed(tr("预约记录加载失败，请稍后重试"));
        return;
    }

    // 最新预约在前（页面按此顺序展示）。
    // 拷贝后排序：不改 mockStore_ 本体（新增/取消按 append 序，与展示解耦）；
    // stable_sort 保证同时刻记录次序可复现（测试钉序不受扰动）。
    ReservationList sorted = mockStore_;
    std::stable_sort(sorted.begin(), sorted.end(), [](const ReservationRecord& left,
                                                      const ReservationRecord& right) {
        return left.reservation.reservedAtUtc > right.reservation.reservedAtUtc;
    });
    emit listSucceeded(sorted);
}

// 桩规格文案（“直流快充 · 120kW”）：mock 演示记录与真实提交共用同一生成
// 口径，列表/订单页展示不再分叉。
QString ReservationService::chargerSpecText(const charging::model::Charger& charger)
{
    const bool fast = charger.type == charging::model::ChargerType::Fast;
    return QStringLiteral("%1 · %2kW")
        .arg(fast ? QStringLiteral("直流快充") : QStringLiteral("交流慢充"))
        .arg(charger.powerWatts / 1000);
}

void ReservationService::finishMockSubmit()
{
    // 取出即清 pendingSubmitRecord_：延迟回调只服务本次提交，作废旧暂存，
    // 连续两次提交不会互相污染上下文。
    const ReservationRecord requested = pendingSubmitRecord_;
    pendingSubmitRecord_ = ReservationRecord{};

    if (simulateFailure_) {
        // 一次性演示开关（与 finishMockList 同口径）。
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
    // 两道车辆闸整体包在非空判定里：setSettingsService 注入与否直接决定
    // 它们是否生效——QML 通道现势未注入（预约不强制车辆），此块整体跳过，
    // 只剩下方全局名额闸（回退口径“至多一条有效预约”）。
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
        // 全局名额闸无条件生效：未注入 settings_ 时上限回退 1，即旧口径
        // “全局至多一条有效预约”（与 QML 撤注入后的现势语义吻合）。
        emit submitFailed(tr("可预约名额已全部占用，请结束当前预约后再发起新预约"));
        return;
    }

    ReservationRecord created = requested;
    // 模拟 ID 固定 9000 段、接续演示记录（9001–9004）：与服务端分配域
    // 天然区分，页面与测试可一眼判断记录来源。
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
    // 先领用票据再清零：无论后续走失败/已结束/不存在哪条出口，取消闸
    // 都已重新打开，不会因一次坏结果永久卡死。
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
        // 仅“预约中”可取消：任何终态统一拒绝，不覆写已定格记录。
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
    // 写状态仅落 mockStore_：真实通道记录由服务端流转、列表回查收敛。
    for (auto& record : mockStore_) {
        if (record.reservation.id == reservationId
            && record.reservation.status == charging::model::ReservationStatus::Active) {
            record.reservation.status = charging::model::ReservationStatus::Expired;
            record.reservation.endedAtUtc = QDateTime::currentDateTimeUtc();
            break;
        }
    }
    // 命中与否都发刷新信号：每秒 tick 可能重复触达，页面以回读方式收敛，
    // 信号幂等、调用方无需判重。
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
    // 模拟估算：行驶时长 = 出发准备 5 分钟 + 距离向上取整每 500 米 1 分钟。
    // 真实地图 API 就绪后由确认页改用 recommendSlotFromTravelMinutes
    // （腾讯距离矩阵返回的真实 duration 换算分钟），本估算保持原口径兜底。
    const int safeDistance = qMax(0, distanceMeters);
    const int travelMinutes = kTravelBaseMinutes
        + (safeDistance + kTravelMetersPerMinute - 1) / kTravelMetersPerMinute;
    return recommendSlotFromTravelMinutes(travelMinutes, nowUtc);
}

RecommendedSlot ReservationService::recommendSlotFromTravelMinutes(int travelMinutes,
                                                                   const QDateTime& nowUtc)
{
    // 开始时刻 = 现在 + 行驶时长，向上对齐本地整点 15 分钟刻度；
    // 结束 = 开始 + 45 分钟（规格上限）。
    const int safeMinutes = qMax(0, travelMinutes);

    // 对齐换算走本地时区（用户感知的是 xx:15/xx:30 这类整齐刻度），
    // 算完再转回 UTC 存储与传输；secsOfDay 取当日秒数做整除上取对齐。
    QDateTime local = nowUtc.toLocalTime().addSecs(safeMinutes * 60);
    const QTime time = local.time();
    const int secsOfDay = time.hour() * 3600 + time.minute() * 60 + time.second();
    const int remainder = secsOfDay % kSlotAlignmentSecs;
    if (remainder != 0) {
        local = local.addSecs(kSlotAlignmentSecs - remainder);
    }

    RecommendedSlot slot;
    slot.travelMinutes = safeMinutes;
    slot.startUtc = local.toUTC();
    slot.endUtc = slot.startUtc.addSecs(kMaxSlotMinutes * 60);
    return slot;
}

// ---- 真实通道响应/故障路由（ClientConnection 广播给全部服务，本函数
// 必须认领自己的请求；requestId 全局唯一，type 再校验一道防串）----

void ReservationService::handleResponse(const charging::protocol::ResponseEnvelope& response)
{
    // 三组双匹配：requestId 命中在途键且命令类型一致才认领；任何一条
    // 不匹配即与己无关，直接忽略（同一连接上多服务共用信号）。
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
            // 双发分工：submitFailed 供通用 toast 文案；submitRejected 带
            // 错误码/详情，QML bridge 拆转发给页面做精确分支。
            emit submitFailed(message);
            emit submitRejected(response.error);
        } else {
            pendingCancelRequestId_.clear();
            pendingCancelId_ = 0;
            emit cancelFailed(message);
        }
        return;
    }

    if (isList) {
        pendingListRequestId_.clear();
        // 真实列表命令就绪后的解析路径：data["reservations"] → 记录列表。
        bool more = false;
        // readPage 校验分页回显（页码/条数口径），不符则整体失败，
        // 绝不向页面吐半截列表。
        if (!network::readPage(response.data, "reservations", listPage_, 100, &more)) {
            emit listFailed(tr("预约分页响应无效")); return;
        }
        // 以前序累加页为底再并本页：合拢前不对外发中间态。
        ReservationList records = accumulatedList_;
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
            const QJsonObject item = value.toObject();
            record.stationName = item.value("stationName").toString();
            record.chargerCode = item.value("chargerCode").toString();
            record.orderId = item.value("orderId").toString().toLongLong();
            record.startAtUtc = reservation.reservedAtUtc;
            record.durationMinutes = int(reservation.reservedAtUtc.secsTo(reservation.expiresAtUtc) / 60);
            records.append(record);
        }
        accumulatedList_ = records;
        // more 由 readPage 依据服务端 total 推算；有下页即换新在途键续发，
        // 期间列表请求防重入闸（fetchList）继续生效。
        if (more) {
            pendingListRequestId_ = connection_->sendRequest(kGetReservationsType,
                {{"page", ++listPage_}, {"pageSize", 100}});
            return;
        }
        // 全部页收齐才落镜像并发成功信号。
        liveStore_ = records;
        emit listSucceeded(records);
        return;
    }

    // 提交/取消共用载荷校验（列表已在上面 return）：缺 reservation 对象或
    // 解析失败都清在途键并各发 failed；取消路径同时释放 pendingCancelId_
    // 票据——失败不解锁会把取消闸永久卡死。
    if (!response.data.contains(QStringLiteral("reservation"))) {
        const QString message = tr("预约响应缺少预约信息");
        if (isSubmit) {
            pendingSubmitRequestId_.clear();
            emit submitFailed(message);
        } else {
            pendingCancelRequestId_.clear();
            pendingCancelId_ = 0;
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
            pendingCancelId_ = 0;
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
        record.orderId = response.data.value("order").toObject().value("id").toString().toLongLong();
        // 服务端不回车辆/费用字段：带入的模拟口径刻意清零，避免“假装有车”
        // 误导名额展示（与 live 通道 activeCountForVehicle 恒 0 同口径）。
        record.vehicleId = 0;
        record.vehiclePlate.clear();
        record.estimatedFeeCents = 0;
        // 服务端尚未支持时间段/车辆字段（协议扩展就绪前以服务端返回的
        // 起止时刻为准，客户端仅带入车辆与站点展示上下文）。
        if (reservation.reservedAtUtc.isValid() && reservation.expiresAtUtc.isValid()) {
            record.startAtUtc = reservation.reservedAtUtc;
            record.durationMinutes =
                int(reservation.reservedAtUtc.secsTo(reservation.expiresAtUtc) / 60);
        }
        // 头插 = “最新预约在前”，与模拟通道 finishMockList 排序口径一致。
        liveStore_.prepend(record);
        emit submitSucceeded(record);
    } else {
        // 取消成功：用服务端回包的完整 reservation 整体替换本地记录，
        // 状态/endedAtUtc 以服务端流转结果为准（本地不改写只镜像）。
        const qint64 reservationId = pendingCancelId_;
        pendingCancelRequestId_.clear();
        pendingCancelId_ = 0;
        for (auto& record : liveStore_)
            if (record.reservation.id == reservationId) record.reservation = reservation;
        emit cancelSucceeded(reservationId);
    }
}

// 传输层失败广播（超时/断线，收不到响应包）：按三个在途 requestId 归位
// 对应 failed；message 空则兜底网络文案，保证页面加载态总能退出。
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
