#include "main_window.h"

#include "charging_server.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace charging::server {

namespace {

QFrame* createSummaryCard(const QString& title, const QString& value, QLabel** valueLabel,
                          QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setFrameShape(QFrame::StyledPanel);
    card->setMinimumWidth(190);

    auto* layout = new QVBoxLayout(card);
    auto* titleLabel = new QLabel(title, card);
    auto* summaryLabel = new QLabel(value, card);
    QFont valueFont = summaryLabel->font();
    valueFont.setBold(true);
    valueFont.setPointSize(18);
    summaryLabel->setFont(valueFont);

    layout->addWidget(titleLabel);
    layout->addWidget(summaryLabel);
    if (valueLabel != nullptr) {
        *valueLabel = summaryLabel;
    }
    return card;
}

} // namespace

MainWindow::MainWindow(ChargingServer* server, QWidget* parent) : QMainWindow(parent)
{
    Q_ASSERT(server != nullptr);

    setWindowTitle(tr("充电平台 - PC 运营管理端"));
    resize(1200, 760);

    auto* centralWidget = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(centralWidget);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* sidebar = new QFrame(centralWidget);
    sidebar->setFrameShape(QFrame::StyledPanel);
    sidebar->setFixedWidth(220);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(24, 28, 24, 28);
    auto* brandLabel = new QLabel(tr("充电平台"), sidebar);
    QFont brandFont = brandLabel->font();
    brandFont.setBold(true);
    brandFont.setPointSize(16);
    brandLabel->setFont(brandFont);
    sidebarLayout->addWidget(brandLabel);
    sidebarLayout->addSpacing(24);
    sidebarLayout->addWidget(new QLabel(tr("运营概览"), sidebar));
    sidebarLayout->addWidget(new QLabel(tr("充电桩管理"), sidebar));
    sidebarLayout->addWidget(new QLabel(tr("充电站管理"), sidebar));
    sidebarLayout->addWidget(new QLabel(tr("用户与订单"), sidebar));
    sidebarLayout->addStretch();

    auto* contentWidget = new QWidget(centralWidget);
    auto* contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(32, 28, 32, 28);
    contentLayout->setSpacing(20);

    auto* titleLabel = new QLabel(tr("服务端基础框架"), contentWidget);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(20);
    titleLabel->setFont(titleFont);
    contentLayout->addWidget(titleLabel);

    auto* cardLayout = new QHBoxLayout();
    cardLayout->setSpacing(16);
    cardLayout->addWidget(createSummaryCard(tr("TCP 服务"), tr("已启动"), nullptr, contentWidget));
    cardLayout->addWidget(createSummaryCard(tr("监听端口"), QString::number(server->serverPort()),
                                            nullptr, contentWidget));
    cardLayout->addWidget(createSummaryCard(tr("当前连接"), QString::number(server->clientCount()),
                                            &clientCountValue_, contentWidget));
    cardLayout->addStretch();
    contentLayout->addLayout(cardLayout);

    auto* noticeLabel = new QLabel(
        tr("手机号登录的 Socket、Service 和 Repository 已接通。后续 Dashboard 与其他业务"
           "请按架构文档分模块接入。"),
        contentWidget);
    noticeLabel->setWordWrap(true);
    contentLayout->addWidget(noticeLabel);
    contentLayout->addStretch();

    rootLayout->addWidget(sidebar);
    rootLayout->addWidget(contentWidget, 1);
    setCentralWidget(centralWidget);
    statusBar()->showMessage(tr("TCP 服务正在等待客户端连接"));

    connect(server, &ChargingServer::clientCountChanged, this, &MainWindow::updateClientCount);
}

void MainWindow::updateClientCount(int count)
{
    clientCountValue_->setText(QString::number(count));
}

} // namespace charging::server
