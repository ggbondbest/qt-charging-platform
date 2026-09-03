#include "pages/station/login_page.h"

#include "services/station/auth_service.h"

#include <QFont>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

namespace charging::client::pages::station {

LoginPage::LoginPage(services::station::AuthService* authService, QWidget* parent)
    : QWidget(parent), authService_(authService)
{
    Q_ASSERT(authService_ != nullptr);

    auto* pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(80, 56, 80, 56);
    pageLayout->setSpacing(18);

    auto* titleLabel = new QLabel(tr("手机号登录"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(20);
    titleLabel->setFont(titleFont);

    auto* descriptionLabel =
        new QLabel(tr("输入手机号登录；未注册的手机号将自动创建用户。"), this);
    descriptionLabel->setWordWrap(true);

    phoneLineEdit_ = new QLineEdit(this);
    phoneLineEdit_->setObjectName(QStringLiteral("phoneLineEdit"));
    phoneLineEdit_->setPlaceholderText(tr("例如：13800138000"));
    phoneLineEdit_->setMaxLength(11);
    phoneLineEdit_->setClearButtonEnabled(true);
    phoneLineEdit_->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("1[0-9]{0,10}")),
                                        phoneLineEdit_));

    loginButton_ = new QPushButton(tr("登录"), this);
    loginButton_->setObjectName(QStringLiteral("loginButton"));

    resultLabel_ = new QLabel(tr("请输入11位手机号"), this);
    resultLabel_->setObjectName(QStringLiteral("resultLabel"));
    resultLabel_->setWordWrap(true);

    auto* formLayout = new QFormLayout;
    formLayout->setSpacing(12);
    formLayout->addRow(tr("手机号："), phoneLineEdit_);
    formLayout->addRow(QString(), loginButton_);

    pageLayout->addStretch();
    pageLayout->addWidget(titleLabel);
    pageLayout->addWidget(descriptionLabel);
    pageLayout->addLayout(formLayout);
    pageLayout->addWidget(resultLabel_);
    pageLayout->addStretch();

    connect(loginButton_, &QPushButton::clicked, this, &LoginPage::handleLoginClicked);
    connect(phoneLineEdit_, &QLineEdit::returnPressed, this, &LoginPage::handleLoginClicked);
    connect(authService_, &services::station::AuthService::loginStarted, this,
            &LoginPage::handleLoginStarted);
    connect(authService_, &services::station::AuthService::loginSucceeded, this,
            &LoginPage::handleLoginSucceeded);
    connect(authService_, &services::station::AuthService::loginFailed, this,
            &LoginPage::handleLoginFailed);
}

void LoginPage::handleLoginClicked()
{
    authService_->login(phoneLineEdit_->text());
}

void LoginPage::handleLoginStarted()
{
    phoneLineEdit_->setEnabled(false);
    loginButton_->setEnabled(false);
    loginButton_->setText(tr("登录中…"));
    resultLabel_->setText(tr("正在连接服务端并查询用户…"));
}

void LoginPage::handleLoginSucceeded(const charging::model::User& user, bool created)
{
    phoneLineEdit_->setEnabled(true);
    loginButton_->setEnabled(true);
    loginButton_->setText(tr("登录"));
    resultLabel_->setText(
        tr("登录成功%1\n用户ID：%2\n昵称：%3\n余额：%4 元")
            .arg(created ? tr("（已自动注册）") : QString(), QString::number(user.id),
                 user.nickname, QString::number(user.balanceCents / 100.0, 'f', 2)));
}

void LoginPage::handleLoginFailed(const QString& message)
{
    phoneLineEdit_->setEnabled(true);
    loginButton_->setEnabled(true);
    loginButton_->setText(tr("登录"));
    resultLabel_->setText(tr("登录失败：%1").arg(message));
    phoneLineEdit_->setFocus();
}

} // namespace charging::client::pages::station
