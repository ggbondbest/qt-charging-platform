#pragma once

#include "charging/common/model/models.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace charging::client {

// 顶部导航公共组件（成员 2，任务 #2）：所有用户端页面复用。
//
// 布局：左侧项目 Logo + 平台名称；中间站点搜索输入框；右侧按登录态动态渲染——
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

signals:
    // 搜索框回车触发；keyword 为去空白后的文本。
    void searchSubmitted(const QString& keyword);
    // 未登录状态下点击右上角“登录”。
    void loginRequested();
    // 已登录状态下点击头像，请求进入个人中心。
    void profileRequested();
    // 二级页面点击左上角“返回”。
    void backRequested();

private:
    QPushButton* backButton_ = nullptr;
    QLabel* logoLabel_ = nullptr;
    QLabel* nameLabel_ = nullptr;
    QLineEdit* searchLineEdit_ = nullptr;
    QPushButton* loginButton_ = nullptr;
    QPushButton* avatarButton_ = nullptr;
    QLabel* nicknameLabel_ = nullptr;
    bool hasUser_ = false;
};

} // namespace charging::client
