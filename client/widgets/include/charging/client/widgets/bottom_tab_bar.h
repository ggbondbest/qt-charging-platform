// 底部 Tab 组件对外接口（widgets 通道）。实现见 src/bottom_tab_bar.cpp：exclusive
// QButtonGroup 保证单选视觉、setCurrentTab 与点击两路各自如何发 tabChanged 都在那边。
// QML 通道有职责对齐的孪生件 client/qml/platform/BottomTabBar.qml。
#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QButtonGroup;
class QPushButton;

namespace charging::client {

// 底部 Tab 导航公共组件（成员 2，任务 #2）：固定在页面底部，所有用户端页面复用。
//
// Tab 项由宿主页面注入（首页为：找站 / 订单 / 充电 / 我的）；同一时刻仅一个
// Tab 处于选中态，点击后发 tabChanged(id)；程序切换用 setCurrentTab()。
class BottomTabBar final : public QWidget
{
    Q_OBJECT

public:
    // Tab 项：id 是与宿主约定的稳定路由键（station/order/charging/profile，同时用作
    // objectName 后缀供测试定位），text 仅为展示文案，可含 emoji 不参与逻辑。
    struct Tab
    {
        QString id;
        QString text;
    };

    explicit BottomTabBar(const QList<Tab>& tabs, QWidget* parent = nullptr);

    // 程序化选中；未知 id 或当前已选中时为空操作。
    void setCurrentTab(const QString& id);
    QString currentTab() const;

signals:
    // 点击选中或 setCurrentTab 切换成功时发出，宿主（HomeShell::showTab）据此切页；
    // 重复点击当前 Tab 也会发，用于从叠加路由页拉回主页面。
    void tabChanged(const QString& id);

private:
    // group_ 只管视觉互斥；buttons_ 与 ids_ 按下标一一对应，setCurrentTab 用
    // indexOf(ids_) 反查按钮。current_ 是逻辑选中态，防止无变化时重发信号。
    QButtonGroup* group_ = nullptr;
    QList<QPushButton*> buttons_;
    QStringList ids_;
    QString current_;
};

} // namespace charging::client
