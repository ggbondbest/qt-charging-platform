#pragma once

#include "charging/common/model/models.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace charging::client {
class LoadingOverlay;
namespace services::station {
class AuthService;
}
}

namespace charging::client::pages::station {

// 手机号登录页（成员 2，任务 #2）。
//
// 布局：品牌区 + 居中登录卡片（手机号输入、主按钮、结果提示、协议说明）。
// 登录中显示遮罩并禁用表单；成功经 AuthService::loginSucceeded 由 MainWindow
// 切换到 HomeShell；失败展示原因并可重试。
class LoginPage final : public QWidget
{
    Q_OBJECT

public:
    explicit LoginPage(services::station::AuthService* authService, QWidget* parent = nullptr);

    // 从首页“退出登录”返回时，将页面重置为初始可输入状态。
    void resetState();

private slots:
    void handleLoginClicked();
    void handleLoginStarted();
    void handleLoginSucceeded(const charging::model::User& user, bool created);
    void handleLoginFailed(const QString& message);

private:
    services::station::AuthService* authService_ = nullptr;
    QLineEdit* phoneLineEdit_ = nullptr;
    QPushButton* loginButton_ = nullptr;
    QLabel* resultLabel_ = nullptr;
    LoadingOverlay* loadingOverlay_ = nullptr;
};

} // namespace charging::client::pages::station
