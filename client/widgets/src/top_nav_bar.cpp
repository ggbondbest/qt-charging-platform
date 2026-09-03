#include "charging/client/widgets/top_nav_bar.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

namespace charging::client {

namespace {

// 组件自带样式：颜色沿用成员 3 全局主题的同一套 token（电动绿 #00B578 等），
// 但以对象名限定在本组件内部生效，不修改全局 QSS 文件。
const char* kTopNavBarStyleSheet = R"(
QWidget#uiTopNavBar {
    background: #FFFFFF;
    border-bottom: 1px solid #E5E9EF;
}
QLabel#navLogo {
    background: #00B578;
    color: #FFFFFF;
    border-radius: 14px;
    min-width: 24px;
    min-height: 24px;
    font-size: 15px;
    font-weight: 700;
}
QLabel#navPlatformName {
    color: #1F2937;
    font-size: 15px;
    font-weight: 700;
}
QLineEdit#navSearchLineEdit {
    min-height: 34px;
}
QPushButton#navLoginButton {
    background: #00B578;
    color: #FFFFFF;
    border: none;
    border-radius: 16px;
    padding: 7px 18px;
    font-size: 13px;
    font-weight: 600;
}
QPushButton#navLoginButton:pressed {
    background: #009A66;
}
QPushButton#navAvatarButton {
    background: #EAF9F2;
    border: 1px solid #00B578;
    border-radius: 17px;
    min-width: 34px;
    min-height: 34px;
    font-size: 16px;
}
QPushButton#navAvatarButton:pressed {
    background: #D5F2E5;
}
QLabel#navNickname {
    color: #1F2937;
    font-size: 13px;
    font-weight: 600;
}
)";

} // namespace

TopNavBar::TopNavBar(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("uiTopNavBar"));
    setStyleSheet(QString::fromLatin1(kTopNavBarStyleSheet));

    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(16, 8, 16, 8);
    rootLayout->setSpacing(10);

    // 左：Logo + 平台名称。
    logoLabel_ = new QLabel(QStringLiteral("⚡"), this);
    logoLabel_->setObjectName(QStringLiteral("navLogo"));
    logoLabel_->setAlignment(Qt::AlignCenter);

    nameLabel_ = new QLabel(tr("电动汽车充电桩平台"), this);
    nameLabel_->setObjectName(QStringLiteral("navPlatformName"));

    // 中：站点搜索输入框（弹性伸缩，占用中部空间）。
    searchLineEdit_ = new QLineEdit(this);
    searchLineEdit_->setObjectName(QStringLiteral("navSearchLineEdit"));
    searchLineEdit_->setPlaceholderText(tr("搜索站点 / 地址"));
    searchLineEdit_->setClearButtonEnabled(true);
    searchLineEdit_->setMaximumWidth(320);

    // 右：登录态动态区。未登录 → 登录按钮；已登录 → 头像 + 昵称。
    nicknameLabel_ = new QLabel(this);
    nicknameLabel_->setObjectName(QStringLiteral("navNickname"));
    nicknameLabel_->hide();

    avatarButton_ = new QPushButton(QStringLiteral("👤"), this);
    avatarButton_->setObjectName(QStringLiteral("navAvatarButton"));
    avatarButton_->setCursor(Qt::PointingHandCursor);
    avatarButton_->setAccessibleName(tr("进入个人中心"));
    avatarButton_->hide();

    loginButton_ = new QPushButton(tr("登录"), this);
    loginButton_->setObjectName(QStringLiteral("navLoginButton"));
    loginButton_->setCursor(Qt::PointingHandCursor);

    rootLayout->addWidget(logoLabel_);
    rootLayout->addWidget(nameLabel_);
    rootLayout->addStretch(1);
    rootLayout->addWidget(searchLineEdit_, 2);
    rootLayout->addStretch(1);
    rootLayout->addWidget(nicknameLabel_);
    rootLayout->addWidget(avatarButton_);
    rootLayout->addWidget(loginButton_);

    connect(loginButton_, &QPushButton::clicked, this, [this]() { emit loginRequested(); });
    connect(avatarButton_, &QPushButton::clicked, this,
            [this]() { emit profileRequested(); });
    connect(searchLineEdit_, &QLineEdit::returnPressed, this, [this]() {
        emit searchSubmitted(searchLineEdit_->text().trimmed());
    });
}

void TopNavBar::setUser(const charging::model::User& user)
{
    hasUser_ = true;
    nicknameLabel_->setText(user.nickname);
    nicknameLabel_->show();
    avatarButton_->show();
    loginButton_->hide();
}

void TopNavBar::clearUser()
{
    hasUser_ = false;
    nicknameLabel_->clear();
    nicknameLabel_->hide();
    avatarButton_->hide();
    loginButton_->show();
}

bool TopNavBar::hasUser() const
{
    return hasUser_;
}

QString TopNavBar::searchText() const
{
    return searchLineEdit_->text().trimmed();
}

void TopNavBar::clearSearch()
{
    searchLineEdit_->clear();
}

} // namespace charging::client
