#pragma once

#include "charging/common/model/models.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace charging::client::services::station {
class AuthService;
}

namespace charging::client::pages::station {

class LoginPage final : public QWidget
{
    Q_OBJECT

public:
    explicit LoginPage(services::station::AuthService* authService, QWidget* parent = nullptr);

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
};

} // namespace charging::client::pages::station
