#include "main_window.h"

#include <QFont>
#include <QLabel>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace charging::client {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(tr("电动汽车充电桩应用管理平台"));
    resize(960, 640);

    auto* centralWidget = new QWidget(this);
    auto* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(16);

    auto* titleLabel = new QLabel(tr("电动汽车充电桩应用管理平台"), centralWidget);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(20);
    titleLabel->setFont(titleFont);

    auto* descriptionLabel = new QLabel(tr("客户端基础框架已就绪。后续页面请按模块接入，"
                                           "不要直接耦合数据库实现。"),
                                        centralWidget);
    descriptionLabel->setWordWrap(true);

    layout->addWidget(titleLabel);
    layout->addWidget(descriptionLabel);
    layout->addStretch();

    setCentralWidget(centralWidget);
    statusBar()->showMessage(tr("准备就绪"));
}

} // namespace charging::client
