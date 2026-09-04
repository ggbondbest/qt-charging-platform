#include "admin_login_page.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace charging::server {

AdminLoginPage::AdminLoginPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("adminLoginPage"));
    setStyleSheet(QStringLiteral(
        "QWidget#adminLoginPage { background: #f6f8fa; }"
        "QFrame#loginFormPanel { background: #ffffff; }"
        "QFrame#loginBrandPanel { background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
        " stop:0 #2878d4, stop:1 #6dcbc5); }"
        "QLineEdit { background: #ffffff; border: 1px solid #d0d7de; border-radius: 10px;"
        " min-height: 42px; padding: 0 12px; }"
        "QLineEdit:focus { border: 2px solid #2878d4; }"
        "QPushButton#loginButton { background: #2878d4; border: none; border-radius: 10px;"
        " color: white; font-weight: 600; min-height: 44px; padding: 0 18px; }"
        "QPushButton#loginButton:hover { background: #1f65b6; }"
        "QPushButton#loginButton:disabled { background: #9abfe7; }"));

    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* formPanel = new QFrame(this);
    formPanel->setObjectName(QStringLiteral("loginFormPanel"));
    auto* formPanelLayout = new QVBoxLayout(formPanel);
    formPanelLayout->setContentsMargins(56, 48, 56, 48);
    auto* card = new QFrame(formPanel);
    card->setMaximumWidth(420);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(14);

    auto* brandLabel = new QLabel(tr("充电平台 · 运营管理端"), card);
    brandLabel->setStyleSheet(QStringLiteral("color: #2878d4; font-weight: 600;"));

    auto* titleLabel = new QLabel(tr("管理员登录"), card);
    titleLabel->setStyleSheet(QStringLiteral("font-size: 28px; font-weight: 700; color: #1f2328;"));

    auto* subtitleLabel = new QLabel(tr("请使用管理员账号进入运营管理后台。"), card);
    subtitleLabel->setStyleSheet(QStringLiteral("color: #5e6c84;"));

    usernameLineEdit_ = new QLineEdit(card);
    usernameLineEdit_->setObjectName(QStringLiteral("usernameLineEdit"));
    usernameLineEdit_->setPlaceholderText(tr("管理员账号"));
    usernameLineEdit_->setAccessibleName(tr("管理员账号"));

    passwordLineEdit_ = new QLineEdit(card);
    passwordLineEdit_->setObjectName(QStringLiteral("passwordLineEdit"));
    passwordLineEdit_->setPlaceholderText(tr("密码"));
    passwordLineEdit_->setEchoMode(QLineEdit::Password);
    passwordLineEdit_->setAccessibleName(tr("管理员密码"));

    errorLabel_ = new QLabel(card);
    errorLabel_->setObjectName(QStringLiteral("loginErrorLabel"));
    errorLabel_->setStyleSheet(QStringLiteral("color: #c9372c;"));
    errorLabel_->setWordWrap(true);
    errorLabel_->hide();

    loginButton_ = new QPushButton(tr("登录"), card);
    loginButton_->setObjectName(QStringLiteral("loginButton"));
    loginButton_->setDefault(true);

    cardLayout->addWidget(brandLabel);
    cardLayout->addSpacing(8);
    cardLayout->addWidget(titleLabel);
    cardLayout->addWidget(subtitleLabel);
    cardLayout->addSpacing(18);
    cardLayout->addWidget(usernameLineEdit_);
    cardLayout->addWidget(passwordLineEdit_);
    cardLayout->addWidget(errorLabel_);
    cardLayout->addSpacing(6);
    cardLayout->addWidget(loginButton_);

    formPanelLayout->addStretch();
    formPanelLayout->addWidget(card, 0, Qt::AlignHCenter);
    formPanelLayout->addStretch();

    auto* brandPanel = new QFrame(this);
    brandPanel->setObjectName(QStringLiteral("loginBrandPanel"));
    brandPanel->setMinimumWidth(460);
    auto* brandPanelLayout = new QVBoxLayout(brandPanel);
    brandPanelLayout->setContentsMargins(56, 56, 56, 56);
    auto* brandTitle = new QLabel(tr("让充电运营更清晰"), brandPanel);
    brandTitle->setStyleSheet(QStringLiteral("color: white; font-size: 30px; font-weight: 700;"));
    auto* brandDescription = new QLabel(
        tr("聚合营收、设备与订单，让每一项运营决策都有可靠依据。"), brandPanel);
    brandDescription->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.84); font-size: 15px;"));
    brandDescription->setWordWrap(true);
    brandPanelLayout->addStretch();
    brandPanelLayout->addWidget(brandTitle);
    brandPanelLayout->addSpacing(12);
    brandPanelLayout->addWidget(brandDescription);
    brandPanelLayout->addStretch();

    rootLayout->addWidget(formPanel, 4);
    rootLayout->addWidget(brandPanel, 5);

    connect(loginButton_, &QPushButton::clicked, this, &AdminLoginPage::handleLoginClicked);
    connect(passwordLineEdit_, &QLineEdit::returnPressed, this,
            &AdminLoginPage::handleLoginClicked);
}

void AdminLoginPage::setBusy(bool busy)
{
    usernameLineEdit_->setEnabled(!busy);
    passwordLineEdit_->setEnabled(!busy);
    loginButton_->setEnabled(!busy);
    loginButton_->setText(busy ? tr("正在验证…") : tr("登录"));
}

void AdminLoginPage::showError(const QString& message)
{
    errorLabel_->setText(message);
    errorLabel_->show();
}

void AdminLoginPage::resetForm()
{
    passwordLineEdit_->clear();
    errorLabel_->clear();
    errorLabel_->hide();
    usernameLineEdit_->setFocus();
}

void AdminLoginPage::handleLoginClicked()
{
    errorLabel_->hide();

    const QString username = usernameLineEdit_->text().trimmed();
    const QString password = passwordLineEdit_->text();
    if (username.isEmpty() || password.isEmpty()) {
        showError(tr("请输入管理员账号和密码。"));
        return;
    }

    emit loginSubmitted(username, password);
}

} // namespace charging::server
