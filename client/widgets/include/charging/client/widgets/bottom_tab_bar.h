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
    void tabChanged(const QString& id);

private:
    QButtonGroup* group_ = nullptr;
    QList<QPushButton*> buttons_;
    QStringList ids_;
    QString current_;
};

} // namespace charging::client
