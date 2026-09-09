#include "app_bridge.h"
#include "service_bridges.h"
#include "map_bridge.h"
#include "charging/client/profile_charging/charging_service.h"
#include "charging/client/profile_charging/coupon_service.h"
#include "charging/client/profile_charging/mock_request_transport.h"
#include "charging/client/profile_charging/network_request_transport.h"
#include "charging/client/profile_charging/order_service.h"
#include "charging/client/profile_charging/stats_service.h"
#include "charging/client/profile_charging/point_service.h"
#include "charging/client/profile_charging/progress_service.h"
#include "charging/client/profile_charging/rating_service.h"
#include "charging/client/profile_charging/wallet_service.h"
#include "charging/common/model/model_json.h"
#include "network/client_connection.h"
#include "network/page_validation.h"
#include "services/favorites/favorites_service.h"
#include "services/favorites/notification_service.h"
#include "services/map/map_geo_service.h"
#include "services/reservation/reservation_service.h"
#include "services/settings/settings_service.h"
#include "services/station/auth_service.h"
#include "services/station/station_query_service.h"
#include <QBuffer>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <memory>

namespace charging::qml {
namespace {
charging::model::User demoUser()
{
    charging::model::User user;
    user.id = 1; user.phone = QStringLiteral("13800138000");
    user.nickname = QStringLiteral("用户8000"); user.balanceCents = 10000;
    user.status = charging::model::UserStatus::Active;
    return user;
}
}

QmlApp::QmlApp(QObject* parent)
    : QmlApp(qEnvironmentVariable("CHARGING_SERVER_HOST", "127.0.0.1"),
             quint16(qEnvironmentVariableIntValue("CHARGING_SERVER_PORT") > 0
                     ? qEnvironmentVariableIntValue("CHARGING_SERVER_PORT") : 9527),
             qEnvironmentVariable("CHARGING_CHANNEL") == QStringLiteral("mock"), parent)
{}

QmlApp::QmlApp(const QString& host, quint16 port, bool mockMode, QObject* parent)
    : QObject(parent), mockMode_(mockMode)
{
    mapBridge_ = new MapBridge(this);
    connection_ = new charging::client::network::ClientConnection(host, port, this);
    auth_ = new charging::client::services::station::AuthService(connection_, this);
    connect(auth_, &charging::client::services::station::AuthService::loginStarted,
            this, &QmlApp::loginStarted);
    connect(auth_, &charging::client::services::station::AuthService::loginFailed,
            this, &QmlApp::loginFailed);
    connect(auth_, &charging::client::services::station::AuthService::loginSucceeded, this,
            [this](const charging::model::User& user, bool created) {
        if (loggingOut_) return;
        createSession(user);
        loggedIn_ = true;
        emit userChanged();
        emit loginSucceeded(user_, created);
        emit loginStateChanged();
        recoverUnfinishedOrder();
    });
    connect(connection_, &charging::client::network::ClientConnection::connectionStateChanged,
            this, [this](bool connected) {
        if (!connected && loggedIn_ && !mockMode_ && !loggingOut_) {
            logout();
            emit toastRequested(tr("连接已断开，请重新登录；订单和余额以服务器为准"), "warning");
        }
    });
    createSession(mockMode_ ? demoUser() : charging::model::User{});
}

QmlApp::~QmlApp()
{
    loggingOut_ = true;
    ++generation_;
    disconnect(connection_, nullptr, this, nullptr);
    connection_->disconnectFromServer();
    delete session_;
    session_ = nullptr;
}

void QmlApp::createSession(const charging::model::User& user)
{
    ++generation_;
    if (session_) {
        chargingService_->stopTracking();
        session_->deleteLater();
    }
    session_ = new QObject(this);
    const quint64 generation = generation_;
    user_ = marshalling::userToMap(user);
    if (mockMode_) {
        auto* mock = new charging::client::MockRequestTransport;
        mock->setParent(session_); mock->setUser(user); transport_ = mock;
    } else {
        transport_ = new charging::client::NetworkRequestTransport(connection_, user.id, session_);
    }
    walletService_ = new charging::client::WalletService(transport_, session_);
    orderService_ = new charging::client::OrderService(transport_, session_);
    chargingService_ = new charging::client::ChargingService(transport_, session_);
    statsService_ = new charging::client::StatsService(transport_, session_);
    couponService_ = new charging::client::CouponService(transport_, session_);
    pointService_ = new charging::client::PointService(transport_, session_);
    ratingService_ = new charging::client::RatingService(transport_, session_);
    reservationService_ = new charging::client::services::reservation::ReservationService(session_);
    settingsService_ = new charging::client::services::settings::SettingsService(session_);
    mapGeoService_ = new charging::client::services::map::MapGeoService(session_);
    // 经验等级引擎（2026-09-09）：按登录手机号分组持久化，随 session_ 生灭；
    // 升级庆祝走 toastRequested（与其余提示同一出口）。
    progressService_ = new charging::client::ProgressService(user.phone, session_);
    connect(progressService_, &charging::client::ProgressService::levelUp, this,
            [this](int, const QString& tier, qint64 giftPoints) {
        emit toastRequested(tr("恭喜升级到 %1！礼包 +%2 积分已记入等级账目")
                            .arg(tier).arg(giftPoints), "success");
    });
    reservationService_->setUserId(user.id);
    // 2026-09-08 业务变更：预约不再强制车辆（QML 侧删闸同步）。撤 settings 注入
    // = finishMockSubmit 的 0车拒绝/每车唯一两道自然失效，名额闸回退"至多 1 条
    // 有效预约"（unfinishedSlotLimit 未注入回退 1），恰合"有充电中即不可再约"。
    // widgets HomeShell 自带注入路径，其车辆语义测试零受影响。
    // （原 reservationService_->setSettingsService(settingsService_);——上游 develop
    //   该行随 favorites/notification 父对象改 session_ 一并合入，注入行按业务指令撤除。）
    favoritesService_ = new charging::client::services::favorites::FavoritesService(session_);
    notificationService_ =
        new charging::client::services::favorites::NotificationService(session_);
    notificationService_->setSettingsService(settingsService_);
    // 通知服务端通道（2026-09-08 横闯，PR 置顶报备项）：与券同源 transport；
    // 充电结束/支付成功通知由服务端（mock 镜像）落库供此拉取。
    notificationService_->setTransport(transport_);
    favoritesService_->setCurrentUser(user.id > 0 ? QString::number(user.id) : QString());
    stationQueryService_ = new charging::client::services::station::StationQueryService(session_);
    if (!mockMode_) {
        stationQueryService_->setConnection(connection_);
        stationQueryService_->setLiveMode(true);
        reservationService_->setConnection(connection_);
        reservationService_->setLiveMode(true);
    }
    walletBridge_ = new WalletBridge(walletService_, session_);
    orderBridge_ = new OrderBridge(orderService_, session_);
    chargingBridge_ = new ChargingBridge(chargingService_, session_);
    stationQueryBridge_ = new StationQueryBridge(stationQueryService_, session_);
    reservationBridge_ = new ReservationBridge(reservationService_, session_);
    settingsBridge_ = new SettingsBridge(settingsService_, session_);
    favoritesBridge_ = new FavoritesBridge(favoritesService_, session_);
    notificationBridge_ = new NotificationBridge(notificationService_, session_);
    // 2026-09-08 月报/优惠券/签到积分/电桩评价：同 transport 的读侧服务，
    // 同名转发桥即契约名 —— CouponPage 等盲调点注册后自动退演示态。
    statsBridge_ = new StatsBridge(statsService_, session_);
    couponBridge_ = new CouponBridge(couponService_, session_);
    pointBridge_ = new PointBridge(pointService_, session_);
    ratingBridge_ = new RatingBridge(ratingService_, session_);
    if (user.id > 0) {
        // Logged-in session boot: notifications pull once per session; the
        // coupon cache is only read synchronously by CouponPage, so warm it here.
        notificationService_->refresh();
        couponService_->fetchCoupons();
    }
    connect(walletBridge_, &WalletBridge::profileLoaded, this,
            [this, generation](const QVariantMap& profile) {
        if (generation != generation_ || profile.value("id") != user_.value("id")) return;
        user_ = profile; emit userChanged();
    });
    const auto balanceChanged = [this, generation](qint64, qint64 balance) {
        if (generation != generation_) return;
        user_.insert("balanceCents", balance); emit userChanged();
    };
    connect(walletBridge_, &WalletBridge::rechargeCompleted, this, balanceChanged);
    connect(chargingBridge_, &ChargingBridge::paymentCompleted, this, balanceChanged);
    // A successful start always opens the authoritative charging status page.
    connect(chargingBridge_, &ChargingBridge::startCompleted, this,
            [this, generation](const QVariantMap& status) {
        if (generation == generation_ && loggedIn_)
            emit navigateRequested(QStringLiteral("charging"), status);
    });
    emit servicesChanged();
}

QObject* QmlApp::walletService() const { return walletBridge_; }
QObject* QmlApp::orderService() const { return orderBridge_; }
QObject* QmlApp::chargingService() const { return chargingBridge_; }
QObject* QmlApp::reservationService() const { return reservationBridge_; }
QObject* QmlApp::settingsService() const { return settingsBridge_; }
QObject* QmlApp::mapGeoService() const { return mapGeoService_; }
QObject* QmlApp::mapBridge() const { return mapBridge_; }
QObject* QmlApp::favoritesService() const { return favoritesBridge_; }
QObject* QmlApp::notificationService() const { return notificationBridge_; }
QObject* QmlApp::stationQueryService() const { return stationQueryBridge_; }
QObject* QmlApp::statsService() const { return statsBridge_; }
QObject* QmlApp::couponService() const { return couponBridge_; }
QObject* QmlApp::pointsService() const { return pointBridge_; }
QObject* QmlApp::ratingsService() const { return ratingBridge_; }
QObject* QmlApp::progressService() const { return progressService_; }
QObject* QmlApp::authService() const { return const_cast<QmlApp*>(this); }
QVariantMap QmlApp::currentUser() const { return loggedIn_ ? user_ : QVariantMap{}; }

bool QmlApp::login(const QString& phone)
{
    static const QRegularExpression pattern(QStringLiteral("^1[0-9]{10}$"));
    if (!pattern.match(phone.trimmed()).hasMatch()) {
        emit loginFailed(tr("手机号必须为11位数字且以1开头")); return false;
    }
    if (loggedIn_ || auth_->isLoginPending()) return false;
    if (!mockMode_) { auth_->login(phone.trimmed()); return true; }
    createSession(demoUser());
    loggedIn_ = true;
    emit userChanged(); emit loginSucceeded(user_, false); emit loginStateChanged();
    return true;
}

void QmlApp::logout()
{
    if (loggingOut_) return;
    loggingOut_ = true;
    ++generation_;
    loggedIn_ = false;
    setCheckingOrders(false);
    user_.clear();
    // First destroy the visible authenticated page tree, then replace contexts.
    emit userChanged(); emit loginStateChanged();
    connection_->disconnectFromServer();
    createSession(mockMode_ ? demoUser() : charging::model::User{});
    loggingOut_ = false;
}

void QmlApp::navigate(const QString& route, const QVariant& arg)
{
    if (!loggedIn_ && route != QStringLiteral("login")) {
        emit navigateRequested(QStringLiteral("login"), {}); return;
    }
    // 审查 P2#3：券/通知页只读桥缓存，而 boot 拉取每会话仅一次——充值发券、
    // 停止/支付落通知后同会话进页会看到旧缓存。Shell 导航唯一漏斗即本函数
    //（底栏 onTabChanged、铃铛、Profile 行均经 App.navigate），进页强制补拉。
    // 两侧服务均单飞（在途重复请求静默丢弃），boot 拉取未落定时不产生第二条。
    if (route == QStringLiteral("coupon") && couponService_) {
        couponService_->fetchCoupons();
    } else if (route == QStringLiteral("notifications") && notificationService_) {
        notificationService_->refresh();
    }
    if (route == QStringLiteral("reservation_confirm")) {
        checkBeforeReservation(arg.toMap()); return;
    }
    // 每日任务 XP 事件（2026-09-09）：navigate 是全页面导航唯一漏斗（底栏/
    // 行卡/顶栏搜索都经此），四个浏览型任务在漏斗处上报，服务当日幂等，
    // 重复进出只记一次；搜索任务以 station 深链带非空关键词参数为判据。
    if (progressService_) {
        if (route == QStringLiteral("station_detail"))
            progressService_->reportEvent(QStringLiteral("detail"));
        else if (route == QStringLiteral("navigation"))
            progressService_->reportEvent(QStringLiteral("route"));
        else if (route == QStringLiteral("stats"))
            progressService_->reportEvent(QStringLiteral("stats"));
        else if (route == QStringLiteral("station") && arg.canConvert<QString>()
                 && !arg.toString().isEmpty())
            progressService_->reportEvent(QStringLiteral("search"));
    }
    emit navigateRequested(route, arg);
}

void QmlApp::setCheckingOrders(bool checking)
{
    if (checkingOrders_ == checking) return;
    checkingOrders_ = checking; emit checkingOrdersChanged();
}
void QmlApp::checkBeforeReservation(const QVariantMap& draft)
{ checkUnfinished(draft, true); }
void QmlApp::recoverUnfinishedOrder()
{ if (!mockMode_) checkUnfinished({}, false); }

void QmlApp::clearUnfinishedOrdersForTesting()
{
    // IRequestTransport 不是 QObject 血统：RTTI dynamic_cast + mock 守卫双保险。
    if (!mockMode_) return;
    if (auto* mock = dynamic_cast<charging::client::MockRequestTransport*>(transport_))
        mock->cancelActiveOrders();
}

void QmlApp::checkUnfinished(const QVariantMap& draft, bool reserveAfter)
{
    if (!loggedIn_) { emit navigateRequested("login", {}); return; }
    if (checkingOrders_) return;
    setCheckingOrders(true);
    const quint64 generation = generation_;
    struct Check {
        int remaining = 3; bool failed = false;
        QVariantMap charging; QVariantMap waiting; QVariantMap reserved;
    };
    const auto state = std::make_shared<Check>();
    for (const QString& status : {QStringLiteral("CHARGING"), QStringLiteral("WAITING_PAYMENT"),
                                 QStringLiteral("RESERVED")}) {
        transport_->sendFor(session_, charging::protocol::request_type::kGetOrders,
            {{"status", status}, {"page", 1}, {"pageSize", 1}},
            [this, generation, state, status, draft, reserveAfter](bool ok,
                const QJsonObject& data, const charging::protocol::ProtocolError&) {
            if (generation != generation_) return;
            bool more = false;
            if (!ok || !charging::client::network::readPage(data, "orders", 1, 1, &more)) {
                state->failed = true;
            } else if (!data.value("orders").toArray().isEmpty()) {
                const QJsonObject item = data.value("orders").toArray().first().toObject();
                charging::model::Order order;
                QString error;
                if (!charging::model::fromJson(item, &order, &error)) state->failed = true;
                else {
                    const QVariantMap mapped = marshalling::orderToMap(order,
                        item.value("stationName").toString(), item.value("chargerCode").toString());
                    if (status == "CHARGING") state->charging = mapped;
                    else if (status == "WAITING_PAYMENT") state->waiting = mapped;
                    else state->reserved = mapped;
                }
            }
            if (--state->remaining != 0) return;
            setCheckingOrders(false);
            if (state->failed) {
                emit toastRequested(tr("未完成订单检查失败，请检查网络后重试"), "danger");
                return; // Fail closed: never permit a reservation after a failed check.
            }
            if (!state->charging.isEmpty()) {
                emit toastRequested(tr("您有正在充电的订单，请先结束充电并结算"), "warning");
                emit navigateRequested("charging", state->charging);
            } else if (!state->waiting.isEmpty()) {
                emit toastRequested(tr("您有待支付订单，请先结算"), "warning");
                emit navigateRequested("settlement", state->waiting);
            } else if (!state->reserved.isEmpty()) {
                emit toastRequested(tr("您已有有效预约，请开始充电或取消后再预约"), "warning");
                emit navigateRequested("charging", state->reserved);
            } else if (reserveAfter) emit navigateRequested("reservation_confirm", draft);
        });
    }
}

QString QmlApp::chooseAvatar()
{
    const QString path = QFileDialog::getOpenFileName(nullptr, tr("选择本地头像"), {},
                                                     tr("图片 (*.png *.jpg *.jpeg *.bmp)"));
    return path.isEmpty() ? QString() : prepareAvatar(path);
}

QString QmlApp::displayTime(const QString& isoUtc) const
{
    const QDateTime time = QDateTime::fromString(isoUtc, Qt::ISODateWithMs);
    return time.isValid() ? time.toOffsetFromUtc(8 * 3600).toString("yyyy-MM-dd HH:mm:ss") : isoUtc;
}

QString QmlApp::prepareAvatar(const QString& localFile)
{
    const QUrl url(localFile);
    const QString path = url.isLocalFile() ? url.toLocalFile() : localFile;
    const QFileInfo info(path);
    auto reject = [this]() {
        emit toastRequested(tr("请选择有效本地图片（不超过10MB、4096×4096）"), "danger");
        return QString();
    };
    if (!info.isFile() || info.size() <= 0 || info.size() > 10 * 1024 * 1024) return reject();
    QImageReader reader(path);
    const QSize size = reader.size();
    if (!reader.canRead() || !size.isValid() || size.width() > 4096 || size.height() > 4096)
        return reject();
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) return reject();
    image = image.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG") || png.size() > 128 * 1024)
        return reject();
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}
}
