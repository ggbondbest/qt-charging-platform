#include "main_window.h"
#include "workflow_management_page.h"
#include "intelligence_center_page.h"

#include "admin_login_page.h"
#include "admin_request_gateway.h"
#include "activity_records_page.h"
#include "charger_management_page.h"
#include "server_runtime.h"
#include "order_management_page.h"
#include "management_page_widgets.h"
#include "station_management_page.h"
#include "user_management_page.h"

#include <QAction>
#include <QButtonGroup>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtMath>

namespace charging::server {

namespace {

// The sidebar does not depend on an icon font or external asset.  Drawing the
// small icons keeps their appearance stable on the Ubuntu presentation VM.
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
        const QRectF buttonRect = QRectF(rect()).adjusted(0.5, 1.0, -0.5, -1.0);
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
        case 4: // order
            painter.drawRoundedRect(iconRect.adjusted(3, 1, -3, 0), 2, 2);
            painter.drawLine(iconRect.left() + 7, iconRect.top() - 1, iconRect.left() + 11, iconRect.top() - 1);
            painter.drawLine(iconRect.left() + 6, iconRect.top() + 7, iconRect.right() - 5, iconRect.top() + 7);
            painter.drawLine(iconRect.left() + 6, iconRect.top() + 12, iconRect.right() - 5, iconRect.top() + 12);
            break;
        case 5: // recharge
            painter.drawRoundedRect(iconRect.adjusted(1, 4, -1, -4), 3, 3);
            painter.drawLine(iconRect.left() + 5, iconRect.center().y(), iconRect.right() - 5, iconRect.center().y());
            painter.drawLine(iconRect.center().x(), iconRect.top() + 7, iconRect.center().x(), iconRect.bottom() - 7);
            break;
        default: // operation log
            painter.drawRoundedRect(iconRect.adjusted(3, 1, -3, 0), 2, 2);
            painter.drawLine(iconRect.left() + 6, iconRect.top() + 6, iconRect.right() - 4, iconRect.top() + 6);
            painter.drawLine(iconRect.left() + 6, iconRect.top() + 11, iconRect.right() - 4, iconRect.top() + 11);
            painter.drawLine(iconRect.left() + 6, iconRect.top() + 16, iconRect.right() - 7, iconRect.top() + 16);
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

// Keep the whole account row one focusable button, including its avatar. This
// avoids dead click targets and gives keyboard users the same menu as a mouse.
class SidebarAccountButton final : public QPushButton
{
public:
    explicit SidebarAccountButton(QWidget* parent) : QPushButton(tr("管理员"), parent)
    {
        setObjectName(QStringLiteral("sidebarAccountButton"));
        setAccessibleName(tr("管理员会话菜单"));
        setAccessibleDescription(tr("按 Enter 或空格打开退出登录菜单"));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(168, 56);
    }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
            || event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {
            click();
            event->accept();
            return;
        }
        QPushButton::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF row = QRectF(rect()).adjusted(1, 1, -1, -1);
        painter.setPen(hasFocus() ? QPen(QColor("#2878f0"), 1.5) : Qt::NoPen);
        painter.setBrush(isDown() || underMouse() ? QColor("#edf4ff") : QColor("#f6f8fc"));
        painter.drawRoundedRect(row, 9, 9);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#2878f0"));
        const QRectF avatar(14, (height() - 34) / 2.0, 34, 34);
        painter.drawEllipse(avatar);
        QFont font = painter.font();
        font.setPixelSize(14);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(avatar, Qt::AlignCenter, QStringLiteral("A"));
        painter.setPen(QColor("#243653"));
        painter.drawText(QRectF(60, 0, width() - 88, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
        painter.setPen(QPen(QColor("#718098"), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const qreal x = width() - 20;
        const qreal y = height() / 2.0;
        painter.drawLine(QPointF(x - 4, y + 2), QPointF(x, y - 2));
        painter.drawLine(QPointF(x, y - 2), QPointF(x + 4, y + 2));
    }
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
        // Compact presentation windows still need a discoverable way to reach
        // all rows and the right-hand detail panel, including with a mouse.
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setStyleSheet(QStringLiteral(
            // A softly tinted canvas makes the white cards visibly float above
            // the page without introducing a dark or distracting backdrop.
            "QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget {"
            " background: #f5f7fb; }"));

        QPalette viewportPalette = viewport()->palette();
        viewportPalette.setColor(QPalette::Window, QColor("#f5f7fb"));
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
    applyManagementLightPalette(this);
    adminGateway_ = new AdminRequestGateway(server, this);

    setWindowTitle(tr("充电平台运营管理系统"));
    resize(1600, 990);
    setMinimumSize(1024, 720);

    rootStackedWidget_ = new QStackedWidget(this);
    loginPage_ = new AdminLoginPage(rootStackedWidget_);
    rootStackedWidget_->addWidget(loginPage_);
    rootStackedWidget_->addWidget(createManagementPage());
    setCentralWidget(rootStackedWidget_);

    connect(loginPage_, &AdminLoginPage::loginSubmitted, this, &MainWindow::handleLoginSubmitted);
    connect(adminGateway_, &AdminRequestGateway::finished, this,
            [this](const QString& id, const QJsonObject& response) {
        if (id != loginRequestId_) return;
        loginRequestId_.clear();
        loginPage_->setBusy(false);
        loginPage_->resetForm();
        if (response.value(QStringLiteral("success")).toBool()) showManagementShell();
        else loginPage_->showError(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString());
    });
    connect(adminGateway_, &AdminRequestGateway::authenticationChanged, this, [this](bool authenticated) {
        if (!authenticated) { loginPage_->setBusy(false); showLoginPage(); }
    });
    // Ten seconds keeps the current page reasonably fresh without repeatedly
    // competing with normal filtering and detail actions.
    autoRefreshTimer_.setInterval(10000);
    connect(&autoRefreshTimer_, &QTimer::timeout, this, &MainWindow::refreshActivePage);
    auto* exitFullScreen = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(exitFullScreen, &QShortcut::activated, this, &MainWindow::exitDashboardFullScreen);
    showLoginPage();
}

void MainWindow::setDashboardServiceManager(DashboardServiceManager* manager)
{
    intelligenceCenterPage_->setServiceManager(manager);
}

QWidget* MainWindow::createManagementPage()
{
    auto* managementPage = new QWidget(rootStackedWidget_);
    managementPage->setObjectName(QStringLiteral("managementPage"));
    managementPage->setStyleSheet(QStringLiteral(
        "QWidget#managementPage, QWidget#contentWidget { background: #f5f7fb; color: #1d2c46;"
        " font-size: 14px; }"
        "QLabel { color: #1d2c46; }"
        "QDialog, QMessageBox { background: #f5f7fb; color: #1d2c46; }"
        "QFrame#sidebar { background: #ffffff; border-right: 1px solid #e7edf5; }"
        "QFrame#contentCard, QFrame#summaryCard { background: #ffffff; border: 1px solid #e8eef6; border-radius: 14px; }"
        "QLineEdit, QComboBox, QAbstractSpinBox { background: white; color: #1d2c46;"
        " selection-background-color: #eaf3ff; selection-color: #1d2c46;"
        " border: 1px solid #dfe6f0; border-radius: 9px;"
        " min-height: 40px; padding: 0 12px; font-size: 14px; }"
        "QLineEdit:disabled, QComboBox:disabled, QAbstractSpinBox:disabled {"
        " background: #f2f5f9; color: #8995a7; border-color: #e4e9f1; }"
        "QAbstractSpinBox QLineEdit { border: none; min-height: 30px; padding: 0; }"
        "QTextEdit, QPlainTextEdit { background: #ffffff; color: #1d2c46;"
        " border: 1px solid #dfe6f0; border-radius: 8px; padding: 8px; }"
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
        "QPushButton#primaryButton:disabled { background: #b6ccef; color: #ffffff; }"
        "QPushButton#secondaryButton, QPushButton#tableActionButton { background: white;"
        " border: 1px solid #e1e7f0; border-radius: 8px; color: #2878f0; min-height: 38px;"
        " padding: 0 12px; font-size: 15px; }"
        "QPushButton#tableActionButton { min-height: 26px; max-height: 26px; min-width: 0;"
        " padding: 0 8px; font-size: 12px; }"
        "QPushButton#secondaryButton:hover, QPushButton#tableActionButton:hover { background: #edf4ff; }"
        "QPushButton#secondaryButton:disabled, QPushButton#tableActionButton:disabled {"
        " background: #f2f5f9; color: #8995a7; border-color: #e4e9f1; }"
        "QDialogButtonBox QPushButton { background: #ffffff; color: #2878f0; border: 1px solid #dfe6f0;"
        " border-radius: 8px; min-height: 36px; min-width: 70px; padding: 0 14px; }"
        "QTableWidget { background: white; color: #1d2c46; alternate-background-color: #f8fafd;"
        " border: none; gridline-color: #edf1f7;"
        " selection-background-color: #eaf3ff; selection-color: #1d2c46; font-size: 14px; }"
        "QTableWidget::item { border: none; padding: 0 8px; }"
        "QTableWidget::item:selected, QTableWidget::item:selected:active,"
        " QTableWidget::item:selected:!active, QTableWidget::item:selected:focus {"
        " background: #eaf3ff; color: #1d2c46; border: none; outline: 0; }"
        "QTableWidget::item:focus { border: none; outline: 0; }"
        "QHeaderView::section { background: #ffffff; border: none; border-bottom: 1px solid #edf1f7;"
        " color: #68758a; font-size: 13px; font-weight: 600; min-height: 46px; padding: 0 6px;"
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
    sidebarLayout->setContentsMargins(10, 24, 8, 14);
    sidebarLayout->setSpacing(6);

    const QList<QString> navigationTitles = {tr("运营中心"), tr("电桩管理"),
                                             tr("电站管理"), tr("用户管理"), tr("订单管理"),
                                             tr("充值记录"), tr("操作日志"), tr("排队与维修")};
    auto* navigationGroup = new QButtonGroup(managementPage);
    navigationGroup_ = navigationGroup;
    navigationGroup->setExclusive(true);
    const int navigationCount = navigationTitles.size();
    for (int index = 0; index < navigationCount; ++index) {
        auto* button = new NavigationButton(navigationTitles.at(index), index, sidebar_);
        button->setObjectName(QStringLiteral("managementNav%1").arg(index));
        button->setAccessibleName(navigationTitles.at(index));
        navigationGroup->addButton(button, index);
        sidebarLayout->addWidget(button);
    }
    sidebarLayout->addStretch();
    auto* accountDivider = new QFrame(sidebar_);
    accountDivider->setObjectName(QStringLiteral("sidebarAccountDivider"));
    accountDivider->setFixedHeight(1);
    accountDivider->setStyleSheet(QStringLiteral("background: #e7edf5; border: none;"));
    sidebarLayout->addWidget(accountDivider);
    sidebarLayout->addSpacing(6);
    auto* accountButton = new SidebarAccountButton(sidebar_);
    sidebarLayout->addWidget(accountButton);
    sessionMenu_ = new QMenu(accountButton);
    sessionMenu_->setObjectName(QStringLiteral("sidebarAccountMenu"));
    sessionMenu_->setStyleSheet(QStringLiteral(
        "QMenu { background: #ffffff; border: 1px solid #e1e7f0; border-radius: 8px; padding: 5px; }"
        "QMenu::item { color: #34435b; min-height: 34px; padding: 0 24px 0 12px; border-radius: 6px; }"
        "QMenu::item:selected { background: #edf4ff; color: #2878f0; }"));
    auto* signOutAction = sessionMenu_->addAction(tr("退出登录"));
    signOutAction->setObjectName(QStringLiteral("sidebarLogoutAction"));
    // Revoke the shared management session before returning to the login page.
    connect(signOutAction, &QAction::triggered, adminGateway_, &AdminRequestGateway::logout);
    connect(accountButton, &QPushButton::clicked, this, [this, accountButton, signOutAction] {
        if (sessionMenu_->isVisible()) {
            sessionMenu_->hide();
            return;
        }
        sessionMenu_->setMinimumWidth(accountButton->width());
        sessionMenu_->ensurePolished();
        const QPoint above = accountButton->mapToGlobal(QPoint(0, -sessionMenu_->sizeHint().height() - 8));
        accountButton->setDown(true);
        sessionMenu_->popup(above);
        sessionMenu_->setActiveAction(signOutAction);
    });
    connect(sessionMenu_, &QMenu::aboutToHide, accountButton, [accountButton] {
        accountButton->setDown(false);
    });

    auto* contentWidget = new QWidget(managementPage);
    contentWidget->setObjectName(QStringLiteral("contentWidget"));
    auto* contentLayout = new QVBoxLayout(contentWidget);
    contentLayout_ = contentLayout;
    contentLayout->setContentsMargins(24, 24, 24, 24);
    contentLayout->setSpacing(16);

    auto* topBar = new QWidget(contentWidget);
    topBar_ = topBar;
    topBar->setObjectName(QStringLiteral("managementHeader"));
    auto* topBarLayout = new QHBoxLayout(topBar);
    topBarLayout->setContentsMargins(0, 0, 0, 0);
    pageTitleLabel_ = new QLabel(tr("运营中心"), topBar);
    pageTitleLabel_->setObjectName(QStringLiteral("managementPageTitle"));
    pageTitleLabel_->setStyleSheet(QStringLiteral("color: #1d2c46;"));
    QFont titleFont = pageTitleLabel_->font();
    titleFont.setBold(true);
    titleFont.setPixelSize(24);
    pageTitleLabel_->setFont(titleFont);
    topBarLayout->addWidget(pageTitleLabel_, 1);
    contentLayout->addWidget(topBar);

    pageStackedWidget_ = new QStackedWidget(contentWidget);
    pageStackedWidget_->setObjectName(QStringLiteral("managementPages"));
    // Vue owns the unified overview and analytics experience, including its
    // own scrolling. There is no second native overview or duplicate entry.
    intelligenceCenterPage_ = new IntelligenceCenterPage(pageStackedWidget_);
    pageStackedWidget_->addWidget(intelligenceCenterPage_);
    connect(intelligenceCenterPage_, &IntelligenceCenterPage::fullScreenRequested,
            this, &MainWindow::setDashboardFullScreen);
    auto* chargerScrollArea = new ManagementScrollArea(pageStackedWidget_);
    chargerManagementPage_ = new ChargerManagementPage(chargerScrollArea);
    chargerManagementPage_->setAdminGateway(adminGateway_);
    chargerScrollArea->setContentWidget(chargerManagementPage_);
    pageStackedWidget_->addWidget(chargerScrollArea);
    auto* stationScrollArea = new ManagementScrollArea(pageStackedWidget_);
    stationManagementPage_ = new StationManagementPage(stationScrollArea);
    stationManagementPage_->setAdminGateway(adminGateway_);
    stationScrollArea->setContentWidget(stationManagementPage_);
    pageStackedWidget_->addWidget(stationScrollArea);
    auto* userScrollArea = new ManagementScrollArea(pageStackedWidget_);
    userManagementPage_ = new UserManagementPage(userScrollArea);
    userManagementPage_->setAdminGateway(adminGateway_);
    userScrollArea->setContentWidget(userManagementPage_);
    pageStackedWidget_->addWidget(userScrollArea);
    auto* orderScrollArea = new ManagementScrollArea(pageStackedWidget_);
    orderManagementPage_ = new OrderManagementPage(orderScrollArea);
    orderManagementPage_->setAdminGateway(adminGateway_);
    orderScrollArea->setContentWidget(orderManagementPage_);
    pageStackedWidget_->addWidget(orderScrollArea);
    auto* rechargeScrollArea = new ManagementScrollArea(pageStackedWidget_);
    rechargeRecordsPage_ = new ActivityRecordsPage(ActivityRecordsMode::Recharge, rechargeScrollArea);
    rechargeRecordsPage_->setAdminGateway(adminGateway_);
    rechargeScrollArea->setContentWidget(rechargeRecordsPage_);
    pageStackedWidget_->addWidget(rechargeScrollArea);
    auto* operationLogScrollArea = new ManagementScrollArea(pageStackedWidget_);
    operationLogPage_ = new ActivityRecordsPage(ActivityRecordsMode::OperationLog, operationLogScrollArea);
    operationLogPage_->setAdminGateway(adminGateway_);
    operationLogScrollArea->setContentWidget(operationLogPage_);
    pageStackedWidget_->addWidget(operationLogScrollArea);
    workflowPage_ = new WorkflowManagementPage(adminGateway_, pageStackedWidget_);
    pageStackedWidget_->addWidget(workflowPage_);
    contentLayout->addWidget(pageStackedWidget_, 1);

    const auto showPage = [this, navigationGroup, navigationTitles](int index) {
        Q_ASSERT(index >= 0 && index < navigationTitles.size());
        if (index != 0) exitDashboardFullScreen();
        pageStackedWidget_->setCurrentIndex(index);
        updateManagementChrome();
        pageTitleLabel_->setText(navigationTitles.at(index));
        navigationGroup->button(index)->setChecked(true);
        refreshActivePage();
    };
    connect(navigationGroup, &QButtonGroup::idClicked, this,
            [showPage](int index) { showPage(index); });
    connect(stationManagementPage_, &StationManagementPage::stationChargersRequested, this,
            [this, showPage](const QString& stationId) {
                chargerManagementPage_->showStationRecords(stationId);
                showPage(1);
            });
    navigationGroup->button(0)->setChecked(true);


    rootLayout->addWidget(sidebar_);
    rootLayout->addWidget(contentWidget, 1);
    updateManagementChrome();
    updateSidebarWidth();
    return managementPage;
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    updateSidebarWidth();
    updateManagementChrome();
}

void MainWindow::showManagementShell()
{
    pageStackedWidget_->setCurrentIndex(0);
    navigationGroup_->button(0)->setChecked(true);
    updateManagementChrome();
    rootStackedWidget_->setCurrentIndex(1);
    autoRefreshTimer_.start();
}

void MainWindow::showLoginPage()
{
    exitDashboardFullScreen();
    if (sessionMenu_) sessionMenu_->hide();
    autoRefreshTimer_.stop();
    rootStackedWidget_->setCurrentIndex(0);
    loginPage_->resetForm();
}

void MainWindow::refreshActivePage()
{
    if (!adminGateway_->isAuthenticated() || pageStackedWidget_ == nullptr) {
        return;
    }
    switch (pageStackedWidget_->currentIndex()) {
    case 0: break; // Vue retains its navigation and refreshes its own data.
    case 1: chargerManagementPage_->refreshData(); break;
    case 2: stationManagementPage_->refreshData(); break;
    case 3: userManagementPage_->refreshData(); break;
    case 4: orderManagementPage_->refreshData(); break;
    case 5: rechargeRecordsPage_->refreshData(); break;
    case 6: operationLogPage_->refreshData(); break;
    case 7: workflowPage_->refreshData(); break;
    default: break;
    }
}

void MainWindow::handleLoginSubmitted(const QString& username, const QString& password)
{
    loginRequestId_ = adminGateway_->request(QStringLiteral("auth.login"),
        {{QStringLiteral("username"), username}, {QStringLiteral("password"), password}},
        loginPage_, QStringLiteral("login"));
    loginPage_->setBusy(!loginRequestId_.isEmpty());
}

void MainWindow::updateSidebarWidth()
{
    if (sidebar_ == nullptr) {
        return;
    }

    // Keep the navigation readable on compact windows while allowing it to
    // proportionally breathe on large desktop displays.
    const int width = qBound(196, qRound(this->width() * 0.15), 248);
    sidebar_->setFixedWidth(width);
}

void MainWindow::updateManagementChrome()
{
    if (!topBar_ || !contentLayout_ || !pageStackedWidget_) return;
    const bool webPage = pageStackedWidget_->currentIndex() == 0;
    topBar_->setVisible(!webPage && !dashboardFullScreen_);
    sidebar_->setVisible(!dashboardFullScreen_);
    const int margin = width() < 1280 ? 16 : 24;
    contentLayout_->setContentsMargins(webPage || dashboardFullScreen_ ? QMargins() : QMargins(margin, 20, margin, 20));
    contentLayout_->setSpacing(webPage || dashboardFullScreen_ ? 0 : 16);
    if (dashboardFullScreen_ && sessionMenu_) sessionMenu_->hide();
}

void MainWindow::setDashboardFullScreen(bool enabled)
{
    if (dashboardFullScreen_ == enabled) return;
    if (enabled && (rootStackedWidget_->currentIndex() != 1 || pageStackedWidget_->currentIndex() != 0)) {
        intelligenceCenterPage_->exitFullScreen();
        return;
    }
    dashboardFullScreen_ = enabled;
    if (enabled) {
        previousWindowGeometry_ = saveGeometry();
        previousWindowState_ = windowState();
        updateManagementChrome();
        showFullScreen();
    } else {
        setWindowState(previousWindowState_);
        restoreGeometry(previousWindowGeometry_);
        updateManagementChrome();
    }
}

void MainWindow::exitDashboardFullScreen()
{
    if (!dashboardFullScreen_) return;
    intelligenceCenterPage_->exitFullScreen();
    // Also restore when a renderer has already exited or terminated.
    setDashboardFullScreen(false);
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && dashboardFullScreen_ && !isFullScreen())
        exitDashboardFullScreen();
}

} // namespace charging::server
