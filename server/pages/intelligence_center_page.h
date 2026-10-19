#pragma once

#include <QTimer>
#include <QPointer>
#include <QUrl>
#include <QWidget>

class QLabel;
class QPushButton;
class QShowEvent;
class QHideEvent;
class QStackedWidget;
class QWebEngineView;

namespace charging::server {
class DashboardServiceManager;

// A borderless web surface. Native controls appear only while the service or
// page is unavailable; all operational navigation belongs to Vue.
class IntelligenceCenterPage final : public QWidget
{
    Q_OBJECT
public:
    explicit IntelligenceCenterPage(QWidget* parent = nullptr);
    static QUrl dashboardUrl(const QString& input, bool embedded, QString* error = nullptr);
    void setServiceManager(DashboardServiceManager* manager);
    void setDashboardAddress(const QString& address);
    bool isWebFullScreen() const { return fullScreen_; }

public slots:
    void exitFullScreen();

signals:
    void fullScreenRequested(bool enabled);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void loadDashboard();
    void showMessage(const QString& text, bool retry = true);
    void updateServiceState();
    void setFullScreenState(bool enabled);

    QString configuredAddress_;
    QLabel* status_ = nullptr;
    QPushButton* reload_ = nullptr;
    QPointer<DashboardServiceManager> serviceManager_;
    QStackedWidget* content_ = nullptr;
    QWidget* placeholder_ = nullptr;
    QWebEngineView* webView_ = nullptr;
    QTimer loadTimeout_;
    bool attempted_ = false;
    bool loading_ = false;
    bool fullScreen_ = false;
};

} // namespace charging::server
