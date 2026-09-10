#pragma once

#include "charging/common/model/models.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

// 前向声明（成员 2 的块）：登录遮罩仅按指针引用，重依赖留在 .cpp。
namespace charging::client {
class LoadingOverlay;
namespace services::station {
class AuthService;
}
// 外层命名空间收口（成员 2 的行）。
}

namespace charging::client::pages::station {

// 页面定位（成员 2，任务 #2）：表单与反馈的薄壳——手机号硬校验与在途
// 去重在 AuthService（TCP 登录契约）完成，成功后由 MainWindow 换入 HomeShell。
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

    // 成员 2 行：除构造外唯一公开动作。宿主退出登录复用同一实例（不重建
    // 页面），登录流改过的控件态全部在这里清零。
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
    // 成员 2：遮罩指针，构造尾部创建、挂在本页上盖整页，仅登录在途时可见。
    LoadingOverlay* loadingOverlay_ = nullptr;
};

} // namespace charging::client::pages::station
