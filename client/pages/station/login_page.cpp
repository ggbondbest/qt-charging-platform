#include "pages/station/login_page.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/loading_overlay.h"
#include "pages/station/platform_theme.h"
#include "services/station/auth_service.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QStyle>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

// 页面局部样式：主按钮与结果提示（颜色与全局主题同一套 token）。
const char* kLoginPageStyleSheet = R"(
QPushButton#loginButton {
    background: #00B578;
    color: #FFFFFF;
    border: none;
    border-radius: 24px;
    padding: 13px 0px;
    font-size: 15px;
    font-weight: 700;
}
QPushButton#loginButton:pressed {
    background: #009A66;
}
QPushButton#loginButton:disabled {
    background: #B7E5D2;
    color: #F2FBF7;
}
QLabel#resultLabel {
    color: #6B7280;
    font-size: 12px;
}
QLabel#resultLabel[tone="error"] {
    color: #D9363E;
    font-weight: 600;
}
QLabel#resultLabel[tone="success"] {
    color: #089E62;
    font-weight: 600;
}
)";

// 动态切换 resultLabel 的 tone 属性后需要重新应用 QSS。
void applyResultTone(QLabel* label, const QString& tone)
{
    label->setProperty("tone", tone);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

} // namespace

LoginPage::LoginPage(services::station::AuthService* authService, QWidget* parent)
    : QWidget(parent), authService_(authService)
{
    Q_ASSERT(authService_ != nullptr);

    // 页面可能在测试/预览中独立构造；主题安装是幂等的。
    installPlatformTheme();

    setObjectName(QStringLiteral("loginPage"));
    setStyleSheet(QString::fromLatin1(kLoginPageStyleSheet));

    auto* pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(40, 32, 40, 24);
    pageLayout->setSpacing(0);

    // 品牌区。
    auto* brandGlyph = new QLabel(QStringLiteral("⚡"), this);
    brandGlyph->setObjectName(QStringLiteral("brandGlyph"));
    brandGlyph->setAlignment(Qt::AlignCenter);
    brandGlyph->setProperty("role", QStringLiteral("successCheck"));

    auto* brandTitle = new QLabel(tr("电动汽车充电桩应用管理平台"), this);
    brandTitle->setAlignment(Qt::AlignCenter);
    brandTitle->setProperty("role", QStringLiteral("pageTitle"));

    auto* brandSubtitle = new QLabel(tr("手机号快捷登录 · 充电服务一触即达"), this);
    brandSubtitle->setAlignment(Qt::AlignCenter);
    brandSubtitle->setProperty("role", QStringLiteral("secondary"));

    pageLayout->addStretch();
    pageLayout->addWidget(brandGlyph);
    pageLayout->addSpacing(6);
    pageLayout->addWidget(brandTitle);
    pageLayout->addSpacing(2);
    pageLayout->addWidget(brandSubtitle);
    pageLayout->addSpacing(22);

    // 居中登录卡片。
    auto* cardRow = new QHBoxLayout();
    auto* loginCard = new Card(this);
    loginCard->setObjectName(QStringLiteral("loginCard"));
    loginCard->setFixedWidth(380);
    loginCard->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    cardRow->addStretch();
    cardRow->addWidget(loginCard);
    cardRow->addStretch();
    pageLayout->addLayout(cardRow);
    pageLayout->addStretch();

    auto* cardLayout = loginCard->bodyLayout();
    cardLayout->setContentsMargins(28, 26, 28, 24);
    cardLayout->setSpacing(14);

    auto* titleLabel = new QLabel(tr("手机号登录"), loginCard);
    titleLabel->setProperty("role", QStringLiteral("sectionTitle"));

    auto* descriptionLabel = new QLabel(
        tr("未注册的手机号将自动创建账号，登录后即可找站、预约与充电。"), loginCard);
    descriptionLabel->setWordWrap(true);
    descriptionLabel->setProperty("role", QStringLiteral("secondary"));
    phoneLineEdit_ = new QLineEdit(loginCard);
    phoneLineEdit_->setObjectName(QStringLiteral("phoneLineEdit"));
    phoneLineEdit_->setPlaceholderText(tr("请输入 11 位手机号"));
    phoneLineEdit_->setMaxLength(11);
    phoneLineEdit_->setClearButtonEnabled(true);
    phoneLineEdit_->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("1[0-9]{0,10}")),
                                        phoneLineEdit_));

    auto* phoneRow = new QHBoxLayout();
    phoneRow->setSpacing(8);
    auto* prefixLabel = new QLabel(QStringLiteral("+86"), loginCard);
    prefixLabel->setProperty("role", QStringLiteral("secondary"));
    phoneRow->addWidget(prefixLabel);
    phoneRow->addWidget(phoneLineEdit_, 1);

    loginButton_ = new QPushButton(tr("登 录"), loginCard);
    loginButton_->setObjectName(QStringLiteral("loginButton"));
    loginButton_->setCursor(Qt::PointingHandCursor);

    resultLabel_ = new QLabel(tr("请输入11位手机号"), loginCard);
    resultLabel_->setObjectName(QStringLiteral("resultLabel"));
    resultLabel_->setWordWrap(true);
    resultLabel_->setAlignment(Qt::AlignCenter);

    auto* agreementLabel =
        new QLabel(tr("登录即表示同意用户服务协议与隐私政策"), loginCard);
    agreementLabel->setAlignment(Qt::AlignCenter);
    agreementLabel->setProperty("role", QStringLiteral("caption"));

    cardLayout->addWidget(titleLabel);
    cardLayout->addLayout(phoneRow);
    // 注册说明紧跟在手机号输入框下方。
    cardLayout->addWidget(descriptionLabel);
    cardLayout->addWidget(loginButton_);
    cardLayout->addWidget(resultLabel_);
    cardLayout->addWidget(agreementLabel);

    // 登录中遮罩：盖住整页，禁用期间给出进度反馈。
    loadingOverlay_ = new LoadingOverlay(this);

    connect(loginButton_, &QPushButton::clicked, this, &LoginPage::handleLoginClicked);
    connect(phoneLineEdit_, &QLineEdit::returnPressed, this, &LoginPage::handleLoginClicked);
    connect(authService_, &services::station::AuthService::loginStarted, this,
            &LoginPage::handleLoginStarted);
    connect(authService_, &services::station::AuthService::loginSucceeded, this,
            &LoginPage::handleLoginSucceeded);
    connect(authService_, &services::station::AuthService::loginFailed, this,
            &LoginPage::handleLoginFailed);
}

void LoginPage::resetState()
{
    phoneLineEdit_->clear();
    phoneLineEdit_->setEnabled(true);
    loginButton_->setEnabled(true);
    loginButton_->setText(tr("登 录"));
    resultLabel_->setText(tr("请输入11位手机号"));
    applyResultTone(resultLabel_, QString());
    loadingOverlay_->hideFor();
    phoneLineEdit_->setFocus();
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
    applyResultTone(resultLabel_, QString());
    loadingOverlay_->showFor();
}

void LoginPage::handleLoginSucceeded(const charging::model::User& user, bool created)
{
    phoneLineEdit_->setEnabled(true);
    loginButton_->setEnabled(true);
    loginButton_->setText(tr("登 录"));
    resultLabel_->setText(
        tr("登录成功%1\n用户ID：%2\n昵称：%3\n余额：%4 元")
            .arg(created ? tr("（已自动注册）") : QString(), QString::number(user.id),
                 user.nickname, QString::number(user.balanceCents / 100.0, 'f', 2)));
    applyResultTone(resultLabel_, QStringLiteral("success"));
    loadingOverlay_->hideFor();
}

void LoginPage::handleLoginFailed(const QString& message)
{
    phoneLineEdit_->setEnabled(true);
    loginButton_->setEnabled(true);
    loginButton_->setText(tr("登 录"));
    resultLabel_->setText(tr("登录失败：%1").arg(message));
    applyResultTone(resultLabel_, QStringLiteral("error"));
    loadingOverlay_->hideFor();
    phoneLineEdit_->setFocus();
}

} // namespace charging::client::pages::station
