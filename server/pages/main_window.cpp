#include "main_window.h"

#include "admin_login_page.h"
#include "charger_management_page.h"
#include "server_runtime.h"
#include "dashboard_page.h"
#include "order_management_page.h"
#include "station_management_page.h"
#include "user_management_page.h"

#include <QButtonGroup>
#include <QColor>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtMath>

namespace charging::server {

namespace {

// The sidebar does not depend on an icon font or external asset.  Drawing the
// five small icons keeps their appearance stable on the Ubuntu presentation VM.
class NavigationButton final : public QPushButton
{
public:
    NavigationButton(const QString& text, int iconType, QWidget* parent)
        : QPushButton(text, parent), iconType_(iconType)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setMinimumHeight(48);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF buttonRect = rect().adjusted(0.5, 1.0, -0.5, -1.0);
        const bool selected = isChecked();
        const bool hovered = underMouse();
        if (selected || hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(selected ? QColor("#edf4ff") : QColor("#f6f8fc"));
            painter.drawRoundedRect(buttonRect, 9, 9);
        }

        const QColor color = selected ? QColor("#2878f0") : QColor("#243653");
        painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        const QRectF iconRect(27, height() / 2.0 - 9, 18, 18);
        switch (iconType_) {
        case 0: // dashboard
            painter.setBrush(color);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(QRectF(iconRect.left(), iconRect.top() + 5, 7, 13), 1.5, 1.5);
            painter.drawRoundedRect(QRectF(iconRect.left() + 10, iconRect.top(), 7, 18), 1.5, 1.5);
            break;
        case 1: // charger
            painter.drawRoundedRect(iconRect.adjusted(4, 0, -4, 0), 2, 2);
            painter.drawLine(iconRect.center().x(), iconRect.top() + 4, iconRect.center().x(), iconRect.bottom() - 4);
            painter.drawLine(iconRect.right() - 3, iconRect.top() + 5, iconRect.right() + 1, iconRect.top() + 5);
            painter.drawLine(iconRect.right() + 1, iconRect.top() + 5, iconRect.right() + 1, iconRect.top() + 11);
            break;
        case 2: // station
            painter.drawRect(iconRect.adjusted(2, 2, -2, 0));
            painter.drawLine(iconRect.left(), iconRect.bottom(), iconRect.right(), iconRect.bottom());
            painter.drawLine(iconRect.left() + 5, iconRect.top() + 7, iconRect.left() + 5, iconRect.top() + 11);
            painter.drawLine(iconRect.left() + 11, iconRect.top() + 7, iconRect.left() + 11, iconRect.top() + 11);
            break;
        case 3: // user
            painter.drawEllipse(QRectF(iconRect.left() + 5, iconRect.top(), 8, 8));
            painter.drawArc(QRectF(iconRect.left() + 2, iconRect.top() + 8, 14, 12), 25 * 16, 130 * 16);
            break;
        default: // order
            painter.drawRoundedRect(iconRect.adjusted(3, 1, -3, 0), 2, 2);
            painter.drawLine(iconRect.left() + 7, iconRect.top() - 1, iconRect.left() + 11, iconRect.top() - 1);
            painter.drawLine(iconRect.left() + 6, iconRect.top() + 7, iconRect.right() - 5, iconRect.top() + 7);
            painter.drawLine(iconRect.left() + 6, iconRect.top() + 12, iconRect.right() - 5, iconRect.top() + 12);
            break;
        }

        painter.setPen(color);
        QFont font = painter.font();
        font.setPixelSize(15);
        font.setWeight(selected ? QFont::DemiBold : QFont::Medium);
        painter.setFont(font);
        painter.drawText(QRectF(65, 0, width() - 72, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
    }

private:
    int iconType_ = 0;
};

// Qt's scroll optimization can leave stale backing-store pixels on VMware's
// virtual display driver.  Pages use this container to repaint the full
// viewport after a scroll event instead of reusing a partially scrolled frame.
class ManagementScrollArea final : public QScrollArea
{
public:
    explicit ManagementScrollArea(QWidget* parent) : QScrollArea(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true);
        setStyleSheet(QStringLiteral(
            "QScrollArea, QScrollArea > QWidget > QWidget { background: #ffffff; }"));

        QPalette viewportPalette = viewport()->palette();
        viewportPalette.setColor(QPalette::Window, Qt::white);
        viewport()->setPalette(viewportPalette);
        viewport()->setAutoFillBackground(true);
        viewport()->setAttribute(Qt::WA_StyledBackground, true);

        connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
                &ManagementScrollArea::requestFullRepaint);
        connect(horizontalScrollBar(), &QScrollBar::valueChanged, this,
                &ManagementScrollArea::requestFullRepaint);
    }

    void setContentWidget(QWidget* content)
    {
        Q_ASSERT(content != nullptr);
        content->setAutoFillBackground(true);
        content->setAttribute(Qt::WA_StyledBackground, true);
        setWidget(content);
    }

protected:
    void scrollContentsBy(int dx, int dy) override
    {
        QScrollArea::scrollContentsBy(dx, dy);
        requestFullRepaint();
    }

private:
    void requestFullRepaint()
    {
        if (repaintPending_) {
            return;
        }
        repaintPending_ = true;
        QTimer::singleShot(0, this, [this]() {
            repaintPending_ = false;
            if (auto* content = widget(); content != nullptr) {
                content->update(content->rect());
            }
            viewport()->update(viewport()->rect());
        });
    }

    bool repaintPending_ = false;
};

} // namespace

MainWindow::MainWindow(ServerRuntime* server, QWidget* parent)
    : QMainWindow(parent), server_(server)
{
    Q_ASSERT(server != nullptr);

    setWindowTitle(tr("充电平台运营管理系统"));
    resize(1600, 990);
    setMinimumSize(1024, 720);

    rootStackedWidget_ = new QStackedWidget(this);
    loginPage_ = new AdminLoginPage(rootStackedWidget_);
    rootStackedWidget_->addWidget(loginPage_);
    rootStackedWidget_->addWidget(createManagementPage());
    setCentralWidget(rootStackedWidget_);

    connect(loginPage_, &AdminLoginPage::loginSubmitted, this, &MainWindow::handleLoginSubmitted);
    connect(server_, &ServerRuntime::clientCountChanged, this, &MainWindow::updateClientCount);
    dashboardPage_->setClientCount(server_->clientCount());
    showLoginPage();
}

QWidget* MainWindow::createManagementPage()
{
    auto* managementPage = new QWidget(rootStackedWidget_);
    managementPage->setObjectName(QStringLiteral("managementPage"));
    managementPage->setStyleSheet(QStringLiteral(
        "QWidget#managementPage, QWidget#contentWidget { background: #ffffff; color: #1d2c46;"
        " font-size: 14px; }"
        "QFrame#sidebar { background: #ffffff; border-right: 1px solid #e7edf5; }"
        "QFrame#contentCard, QFrame#summaryCard { background: #ffffff; border: 1px solid #edf2f7; border-radius: 14px; }"
        "QLineEdit, QComboBox { background: white; border: 1px solid #dfe6f0; border-radius: 9px;"
        " min-height: 40px; padding: 0 12px; font-size: 14px; }"
        "QComboBox { padding: 0 34px 0 12px; }"
        "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 30px;"
        " border: none; background: transparent; }"
        "QComboBox::drop-down:hover { background: #f4f7fb; border-radius: 7px; }"
        "QComboBox::drop-down:pressed { background: #eaf3ff; }"
        "QComboBox::down-arrow { width: 0; height: 0; margin-right: 11px;"
        " border-left: 4px solid transparent; border-right: 4px solid transparent;"
        " border-top: 5px solid #718098; }"
        "QLineEdit:focus, QComboBox:focus { border: 2px solid #2878d4; }"
        "QComboBox QAbstractItemView { background: #ffffff; color: #1d2c46;"
        " border: 1px solid #dfe6f0; outline: 0; font-size: 14px; }"
        "QComboBox QAbstractItemView::item { min-height: 38px; padding: 0 12px; }"
        "QComboBox QAbstractItemView::item:hover { background: #f7f9fc; color: #1d2c46; }"
        "QComboBox QAbstractItemView::item:selected { background: #eaf3ff; color: #2878d4;"
        " font-weight: 600; }"
        "QPushButton#primaryButton { background: #2878f0; border: none; border-radius: 8px;"
        " color: white; min-height: 42px; padding: 0 16px; font-size: 15px;"
        " font-weight: 600; }"
        "QPushButton#primaryButton:hover { background: #1769e8; }"
        "QPushButton#secondaryButton, QPushButton#tableActionButton { background: white;"
        " border: 1px solid #e1e7f0; border-radius: 8px; color: #2878f0; min-height: 38px;"
        " padding: 0 12px; font-size: 15px; }"
        "QPushButton#tableActionButton { min-height: 26px; max-height: 26px; min-width: 0;"
        " padding: 0 8px; font-size: 12px; }"
        "QTableWidget { background: white; border: none; gridline-color: #edf1f7;"
        " selection-background-color: #eaf3ff; selection-color: #1d2c46; font-size: 14px; }"
        "QTableWidget::item { border: none; padding: 0 8px; }"
        "QTableWidget::item:selected, QTableWidget::item:selected:active,"
        " QTableWidget::item:selected:!active, QTableWidget::item:selected:focus {"
        " background: #eaf3ff; color: #1d2c46; border: none; outline: 0; }"
        "QTableWidget::item:focus { border: none; outline: 0; }"
        "QHeaderView::section { background: #ffffff; border: none; border-bottom: 1px solid #edf1f7;"
        " color: #68758a; font-size: 13px; font-weight: 600; min-height: 46px; padding: 0 10px;"
        " text-align: center; }"
        "QLabel#pageIntroductionLabel, QLabel#sectionHintLabel, QLabel#summaryHintLabel {"
        " color: #656d76; font-size: 13px; }"
        "QLabel#summaryValueLabel { color: #17233b; font-size: 26px; font-weight: 700; }"
        "QLabel#sectionTitleLabel { color: #1d2c46; font-size: 18px; font-weight: 700; }"
        "QLabel#deviceStatusLabel { color: #41506a; padding: 6px 0; font-size: 14px; }"
        "QLabel#emptyStateLabel { color: #656d76; min-height: 80px; font-size: 14px; }"));

    auto* rootLayout = new QHBoxLayout(managementPage);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    sidebar_ = new QFrame(managementPage);
    sidebar_->setObjectName(QStringLiteral("sidebar"));
    auto* sidebarLayout = new QVBoxLayout(sidebar_);
    sidebarLayout->setContentsMargins(10, 30, 8, 22);

    const QList<QString> navigationTitles = {tr("运营概览"), tr("电桩管理"),
                                             tr("电站管理"), tr("用户管理"), tr("订单管理")};
    auto* navigationGroup = new QButtonGroup(managementPage);
    navigationGroup->setExclusive(true);
    const int navigationCount = navigationTitles.size();
    for (int index = 0; index < navigationCount; ++index) {
        auto* button = new NavigationButton(navigationTitles.at(index), index, sidebar_);
        button->setAccessibleName(navigationTitles.at(index));
        navigationGroup->addButton(button, index);
        sidebarLayout->addWidget(button);
    }
    sidebarLayout->addStretch();

    auto* contentWidget = new QWidget(managementPage);
    contentWidget->setObjectName(QStringLiteral("contentWidget"));
    auto* contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(31, 24, 31, 30);
    contentLayout->setSpacing(18);

    auto* topBar = new QWidget(contentWidget);
    auto* topBarLayout = new QHBoxLayout(topBar);
    topBarLayout->setContentsMargins(0, 0, 0, 0);
    pageTitleLabel_ = new QLabel(tr("运营概览"), topBar);
    QFont titleFont = pageTitleLabel_->font();
    titleFont.setBold(true);
    titleFont.setPixelSize(24);
    pageTitleLabel_->setFont(titleFont);
    auto* titleLayout = new QVBoxLayout();
    titleLayout->setSpacing(2);
    pageSubtitleLabel_ = new QLabel(tr("全局数据实时监控，掌握运营核心指标"), topBar);
    pageSubtitleLabel_->setStyleSheet(QStringLiteral("color: #77849a; font-size: 13px;"));
    titleLayout->addWidget(pageTitleLabel_);
    titleLayout->addWidget(pageSubtitleLabel_);
    auto* userBadge = new QLabel(tr("A"), topBar);
    userBadge->setAlignment(Qt::AlignCenter);
    userBadge->setFixedSize(34, 34);
    userBadge->setStyleSheet(QStringLiteral(
        "background: #2878d4; color: white; border-radius: 17px; font-size: 14px; font-weight: 600;"));
    auto* userLabel = new QLabel(tr("管理员⌄"), topBar);
    userLabel->setStyleSheet(QStringLiteral("color: #34435b; font-size: 14px; font-weight: 600;"));
    topBarLayout->addLayout(titleLayout);
    topBarLayout->addStretch();
    topBarLayout->addWidget(userBadge);
    topBarLayout->addWidget(userLabel);
    contentLayout->addWidget(topBar);

    pageStackedWidget_ = new QStackedWidget(contentWidget);
    auto* dashboardScrollArea = new ManagementScrollArea(pageStackedWidget_);
    dashboardPage_ = new DashboardPage(dashboardScrollArea);
    dashboardScrollArea->setContentWidget(dashboardPage_);
    pageStackedWidget_->addWidget(dashboardScrollArea);
    auto* chargerScrollArea = new ManagementScrollArea(pageStackedWidget_);
    chargerScrollArea->setContentWidget(new ChargerManagementPage(chargerScrollArea));
    pageStackedWidget_->addWidget(chargerScrollArea);
    auto* stationScrollArea = new ManagementScrollArea(pageStackedWidget_);
    stationScrollArea->setContentWidget(new StationManagementPage(stationScrollArea));
    pageStackedWidget_->addWidget(stationScrollArea);
    auto* userScrollArea = new ManagementScrollArea(pageStackedWidget_);
    userScrollArea->setContentWidget(new UserManagementPage(userScrollArea));
    pageStackedWidget_->addWidget(userScrollArea);
    auto* orderScrollArea = new ManagementScrollArea(pageStackedWidget_);
    orderScrollArea->setContentWidget(new OrderManagementPage(orderScrollArea));
    pageStackedWidget_->addWidget(orderScrollArea);
    contentLayout->addWidget(pageStackedWidget_, 1);

    const QList<QString> navigationDescriptions = {
        tr("全局数据实时监控，掌握运营核心指标"),
        tr("查询设备状态，完成受控的运营操作"),
        tr("查看站点聚合状态并维护基础信息"),
        tr("查询账户状态，处理受控的冻结与解冻"),
        tr("筛选订单，查看关键计量与状态信息"),
    };
    connect(navigationGroup, &QButtonGroup::idClicked, this,
            [this, navigationTitles, navigationDescriptions](int index) {
                pageStackedWidget_->setCurrentIndex(index);
                pageTitleLabel_->setText(navigationTitles.at(index));
                pageSubtitleLabel_->setText(navigationDescriptions.at(index));
            });
    navigationGroup->button(0)->setChecked(true);


    rootLayout->addWidget(sidebar_);
    rootLayout->addWidget(contentWidget, 1);
    updateSidebarWidth();
    return managementPage;
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    updateSidebarWidth();
}

void MainWindow::showManagementShell()
{
    rootStackedWidget_->setCurrentIndex(1);
}

void MainWindow::showLoginPage()
{
    rootStackedWidget_->setCurrentIndex(0);
    loginPage_->resetForm();
}

void MainWindow::handleLoginSubmitted(const QString& username, const QString& password)
{
    loginPage_->setBusy(true);
    // 阶段 0/1 只做 Mock UI；真实管理员认证必须由后续 Service 层提供。
    QTimer::singleShot(180, this, [this, username, password]() {
        loginPage_->setBusy(false);
        if (username == QStringLiteral("admin") && password == QStringLiteral("123456")) {
            showManagementShell();
            return;
        }

        loginPage_->showError(tr("账号或密码不正确。Mock 演示账号为 admin / 123456。"));
    });
}

void MainWindow::updateClientCount(int count)
{
    if (dashboardPage_ != nullptr) {
        dashboardPage_->setClientCount(count);
    }
}

void MainWindow::updateSidebarWidth()
{
    if (sidebar_ == nullptr) {
        return;
    }

    // Keep the navigation readable on compact windows while allowing it to
    // proportionally breathe on large desktop displays.
    const int width = qBound(220, qRound(this->width() * 0.16), 360);
    sidebar_->setFixedWidth(width);
}

} // namespace charging::server
