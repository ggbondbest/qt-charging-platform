#pragma once

#include "charging/common/model/models.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace charging::client {

// 顶部导航公共组件（成员 2，任务 #2；迭代 3 由成员 2 追加筛选/通知入口）。
//
// 布局：左侧项目 Logo + 平台名称；中间站点搜索输入框 + 高级筛选按钮 +
// 消息通知图标按钮（迭代 3，随搜索框同组显隐）；右侧按登录态动态渲染——
// 未登录展示“登录”按钮（loginRequested），已登录展示头像（profileRequested，
// 点击跳转个人中心）。登录态通过 setUser/clearUser 在页面间传递维护。
class TopNavBar final : public QWidget
{
    Q_OBJECT

public:
    explicit TopNavBar(QWidget* parent = nullptr);

    // 进入已登录态：右侧显示头像与昵称，隐藏登录按钮。
    void setUser(const charging::model::User& user);
    // 回到未登录态：右上角显示登录按钮。
    void clearUser();
    bool hasUser() const;

    QString searchText() const;
    void clearSearch();

    // 详情页等二级页面：在 Logo 左侧显示“‹ 返回”按钮（任务 #12）。
    // 默认隐藏，一级页面保持原样。
    void setBackVisible(bool visible);
    bool isBackVisible() const;

    // 搜索框只在「找站」语境有意义；订单/充电/我的等 Tab 与路由页可收起，
    // 收起后品牌区（Logo + 平台名）回到左侧平衡版式。默认显示。
    void setSearchVisible(bool visible);
    bool isSearchVisible() const;

    // 迭代 3：高级筛选入口（漏斗图标）与消息通知入口（铃铛图标），排在搜索
    // 框右侧。可见性与搜索框同组（「找站」语境）：搜索收起的页面二者同步收起。
    bool isFilterVisible() const;
    bool isNotificationsVisible() const;

signals:
    // 搜索框回车触发；keyword 为去空白后的文本。
    void searchSubmitted(const QString& keyword);
    // 未登录状态下点击右上角“登录”。
    void loginRequested();
    // 已登录状态下点击头像，请求进入个人中心。
    void profileRequested();
    // 二级页面点击左上角“返回”。
    void backRequested();
    // 点击“高级筛选”：外层打开筛选弹窗（弹窗归页面层，组件只做入口）。
    void filterRequested();
    // 点击“消息通知”图标：外层跳转通知页（未登录拦截同样在外层）。
    void notificationsRequested();

private:
    QPushButton* backButton_ = nullptr;
    QLabel* logoLabel_ = nullptr;
    QLabel* nameLabel_ = nullptr;
    QLineEdit* searchLineEdit_ = nullptr;
    QPushButton* filterButton_ = nullptr;
    QPushButton* notifyButton_ = nullptr;
    QPushButton* loginButton_ = nullptr;
    QPushButton* avatarButton_ = nullptr;
    QLabel* nicknameLabel_ = nullptr;
    bool hasUser_ = false;
    bool backVisible_ = false;
    bool searchVisible_ = true;

    // 依 hasUser_/backVisible_/searchVisible_ 统一刷新各分区可见性。
    void refreshVisibility();
};

} // namespace charging::client
