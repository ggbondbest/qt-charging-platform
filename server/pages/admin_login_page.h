#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace charging::server {

class AdminLoginPage final : public QWidget
{
    Q_OBJECT

public:
    explicit AdminLoginPage(QWidget* parent = nullptr);

    void setBusy(bool busy);
    void showError(const QString& message);
    void resetForm();

signals:
    void loginSubmitted(const QString& username, const QString& password);

private slots:
    void handleLoginClicked();

private:
    QLineEdit* usernameLineEdit_ = nullptr;
    QLineEdit* passwordLineEdit_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QPushButton* loginButton_ = nullptr;
};

} // namespace charging::server
