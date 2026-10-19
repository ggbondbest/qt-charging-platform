#include "intelligence_center_page.h"
#include "dashboard_service_manager.h"

#include <QDesktopServices>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QStackedWidget>
#include <QUrlQuery>
#include <QVBoxLayout>

#ifdef CHARGING_EMBED_WEB_DASHBOARD
#include <QWebEngineFullScreenRequest>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>
#endif

namespace charging::server {

QUrl IntelligenceCenterPage::dashboardUrl(const QString& input, bool embedded, QString* error)
{
    if (error) error->clear();
    QUrl url(input.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
        || !url.userInfo().isEmpty()) {
        if (error) *error = tr("运营中心服务地址不可用，请检查服务配置后重试。");
        return {};
    }
    QUrlQuery query(url);
    query.removeAllQueryItems(QStringLiteral("embedded"));
    if (embedded) query.addQueryItem(QStringLiteral("embedded"), QStringLiteral("operator"));
    url.setQuery(query);
    return url;
}

IntelligenceCenterPage::IntelligenceCenterPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("intelligenceCenterPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    configuredAddress_ = qEnvironmentVariable("CHARGING_DASHBOARD_URL", QStringLiteral("http://127.0.0.1:8000/"));
    content_ = new QStackedWidget(this);
    content_->setContentsMargins(0, 0, 0, 0);
    placeholder_ = new QWidget(content_);
    auto* placeholderLayout = new QVBoxLayout(placeholder_);
    placeholderLayout->setContentsMargins(32, 32, 32, 32);
    placeholderLayout->setSpacing(16);
    placeholderLayout->addStretch();
    status_ = new QLabel(placeholder_);
    status_->setObjectName(QStringLiteral("dashboardStatus"));
    status_->setAlignment(Qt::AlignCenter);
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    placeholderLayout->addWidget(status_);
    reload_ = new QPushButton(tr("重试"), placeholder_);
    reload_->setObjectName(QStringLiteral("dashboardReloadButton"));
    reload_->setMinimumWidth(100);
    placeholderLayout->addWidget(reload_, 0, Qt::AlignHCenter);
    placeholderLayout->addStretch();
    content_->addWidget(placeholder_);
    layout->addWidget(content_, 1);
    connect(reload_, &QPushButton::clicked, this, &IntelligenceCenterPage::loadDashboard);
    showMessage(tr("正在准备运营中心…"), false);
#ifdef CHARGING_EMBED_WEB_DASHBOARD
    loadTimeout_.setSingleShot(true);
    loadTimeout_.setInterval(20000);
    connect(&loadTimeout_, &QTimer::timeout, this, [this] {
        loading_ = false;
        if (webView_) webView_->stop();
        showMessage(tr("运营中心加载超时，请重试。"));
    });
#endif
}

void IntelligenceCenterPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (serviceManager_ && serviceManager_->state() != DashboardServiceManager::State::Ready
        && serviceManager_->state() != DashboardServiceManager::State::External) return;
    if (!attempted_) loadDashboard();
}

void IntelligenceCenterPage::hideEvent(QHideEvent* event)
{
    exitFullScreen();
    QWidget::hideEvent(event);
}

void IntelligenceCenterPage::setDashboardAddress(const QString& address)
{
    exitFullScreen();
    loading_ = false;
    loadTimeout_.stop();
#ifdef CHARGING_EMBED_WEB_DASHBOARD
    if (webView_) webView_->stop();
#endif
    configuredAddress_ = address;
    attempted_ = false;
    if (isVisible()) loadDashboard();
}

void IntelligenceCenterPage::setServiceManager(DashboardServiceManager* manager)
{
    if (serviceManager_) disconnect(serviceManager_, nullptr, this, nullptr);
    serviceManager_ = manager;
    if (!manager) return;
    connect(manager, &DashboardServiceManager::stateChanged, this, &IntelligenceCenterPage::updateServiceState);
    updateServiceState();
}

void IntelligenceCenterPage::updateServiceState()
{
    if (!serviceManager_) return;
    using State = DashboardServiceManager::State;
    const State state = serviceManager_->state();
    if (state == State::Ready || state == State::External) {
        setDashboardAddress(serviceManager_->url().toString());
        return;
    }
    exitFullScreen();
    loading_ = false;
    attempted_ = false;
    loadTimeout_.stop();
#ifdef CHARGING_EMBED_WEB_DASHBOARD
    if (webView_) webView_->stop();
#endif
    showMessage(serviceManager_->message(), state == State::Failed || state == State::Disabled);
}

void IntelligenceCenterPage::showMessage(const QString& text, bool retry)
{
    exitFullScreen();
    status_->setText(text);
    reload_->setVisible(retry);
    reload_->setEnabled(retry);
    content_->setCurrentWidget(placeholder_);
}

void IntelligenceCenterPage::loadDashboard()
{
    if (serviceManager_) {
        using State = DashboardServiceManager::State;
        const State state = serviceManager_->state();
        if (state == State::Failed && serviceManager_->managesService()) {
            serviceManager_->retry();
            return;
        }
        if (state == State::Idle || state == State::Starting || state == State::Stopping || state == State::Stopped)
            return;
    }
    if (loading_) return;
    attempted_ = true;
    QString error;
    const QUrl url = dashboardUrl(configuredAddress_, true, &error);
    if (url.isEmpty()) { showMessage(error); return; }
#ifdef CHARGING_EMBED_WEB_DASHBOARD
    // Chromium and its signal connections are created once, after login and
    // navigation to this page. Service retry reuses the same surface.
    if (!webView_) {
        webView_ = new QWebEngineView(content_);
        webView_->setObjectName(QStringLiteral("dashboardWebView"));
        webView_->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
        content_->addWidget(webView_);
        connect(webView_->page(), &QWebEnginePage::fullScreenRequested, this,
                [this](QWebEngineFullScreenRequest request) {
            if (request.toggleOn() && !isVisible()) { request.reject(); return; }
            request.accept();
            setFullScreenState(request.toggleOn());
        });
        connect(webView_, &QWebEngineView::loadFinished, this, [this](bool ok) {
            if (!loading_) return;
            loading_ = false;
            loadTimeout_.stop();
            if (ok) content_->setCurrentWidget(webView_);
            else showMessage(tr("运营中心加载失败，请重试。"));
        });
        connect(webView_->page(), &QWebEnginePage::renderProcessTerminated, this,
                [this](QWebEnginePage::RenderProcessTerminationStatus, int) {
            loading_ = false;
            loadTimeout_.stop();
            showMessage(tr("运营中心网页已停止，请重试。"));
        });
    }
    loading_ = false;
    webView_->stop();
    showMessage(tr("正在加载运营中心…"), false);
    loading_ = true;
    loadTimeout_.start();
    webView_->load(url);
#else
    // The explicit browser-only build retains a single recovery action, never
    // a persistent toolbar around an embedded dashboard.
    showMessage(tr("当前构建需在浏览器中查看运营中心。"));
    reload_->setText(tr("打开运营中心"));
    if (sender() == reload_ && !QDesktopServices::openUrl(dashboardUrl(configuredAddress_, false)))
        showMessage(tr("浏览器未能打开运营中心，请重试。"));
#endif
}

void IntelligenceCenterPage::setFullScreenState(bool enabled)
{
    if (fullScreen_ == enabled) return;
    fullScreen_ = enabled;
    emit fullScreenRequested(enabled);
}

void IntelligenceCenterPage::exitFullScreen()
{
    if (!fullScreen_) return;
#ifdef CHARGING_EMBED_WEB_DASHBOARD
    if (webView_) webView_->page()->triggerAction(QWebEnginePage::ExitFullScreen);
#endif
    setFullScreenState(false);
}

} // namespace charging::server
