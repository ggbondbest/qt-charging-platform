#include "charging/client/widgets/top_nav_bar.h"

#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>

namespace charging::client {

namespace {

// 顶栏手绘矢量图标（迭代 3 UI 反馈：emoji 筛选码位实际渲染为“雪人”、铃铛
// 偏小且字体不一致）。QPainterPath 单色描画，色值取全局文本 token #1F2937；
// 2× 像素 + devicePixelRatio 保证高分屏锐利，随 iconSize 缩放。
constexpr const char* kGlyphColor = "#1F2937";

QPixmap makeGlyphCanvas()
{
    QPixmap pm(48, 48); // 24×24 逻辑像素 @2×
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    return pm;
}

// 漏斗（高级筛选）：上宽下窄 + 短导管，实心填充。
QIcon makeFunnelIcon()
{
    QPixmap pm = makeGlyphCanvas();
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath funnel;
    funnel.moveTo(3.8, 4.6);
    funnel.lineTo(20.2, 4.6);
    funnel.lineTo(13.9, 12.4);
    funnel.lineTo(13.9, 19.4);
    funnel.quadTo(13.9, 20.4, 12.9, 19.8);
    funnel.lineTo(10.1, 16.6);
    funnel.lineTo(10.1, 12.4);
    funnel.closeSubpath();
    painter.fillPath(funnel, QColor(QLatin1String(kGlyphColor)));
    return QIcon(pm);
}

// 铃铛（消息通知）：描线钟体 + 实心钟舌/顶钮。
QIcon makeBellIcon()
{
    QPixmap pm = makeGlyphCanvas();
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color(QString::fromLatin1(kGlyphColor));
    QPen pen(color);
    pen.setWidthF(2.0);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    QPainterPath body;
    body.moveTo(6.1, 15.6);
    body.cubicTo(7.6, 14.4, 7.4, 13.2, 7.4, 10.6);
    body.cubicTo(7.4, 5.9, 9.4, 3.9, 12.0, 3.9);
    body.cubicTo(14.6, 3.9, 16.6, 5.9, 16.6, 10.6);
    body.cubicTo(16.6, 13.2, 16.4, 14.4, 17.9, 15.6);
    painter.drawPath(body);
    painter.drawLine(QPointF(4.9, 15.6), QPointF(19.1, 15.6));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(12.0, 18.9), 1.9, 1.9); // 钟舌
    painter.drawEllipse(QPointF(12.0, 2.3), 1.0, 1.0);  // 顶钮
    return QIcon(pm);
}

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
QPushButton[navIconButton="true"] {
    background: #F4F6F8;
    color: #1F2937;
    border: 1px solid #D5DCE4;
    border-radius: 19px;
    min-width: 38px;
    min-height: 38px;
    padding: 0px;
}
QPushButton[navIconButton="true"]:hover {
    border: 1px solid #00B578;
}
QPushButton[navIconButton="true"]:pressed {
    background: #E5E9EF;
}
QPushButton#navBackButton {
    background: #F4F6F8;
    color: #1F2937;
    border: 1px solid #D5DCE4;
    border-radius: 15px;
    padding: 5px 12px;
    font-size: 13px;
    font-weight: 600;
}
QPushButton#navBackButton:pressed {
    background: #E5E9EF;
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

    // 左：可选“返回”（二级页面显示）+ Logo + 平台名称。
    backButton_ = new QPushButton(QStringLiteral("‹ 返回"), this);
    backButton_->setObjectName(QStringLiteral("navBackButton"));
    backButton_->setCursor(Qt::PointingHandCursor);
    backButton_->setAccessibleName(tr("返回"));
    backButton_->hide();

    logoLabel_ = new QLabel(QStringLiteral("⚡"), this);
    logoLabel_->setObjectName(QStringLiteral("navLogo"));
    logoLabel_->setAlignment(Qt::AlignCenter);

    nameLabel_ = new QLabel(tr("电动汽车充电桩平台"), this);
    nameLabel_->setObjectName(QStringLiteral("navPlatformName"));

    // 中：站点搜索输入框（弹性伸缩，占用中部空间；窄窗口下优先保证可读）。
    searchLineEdit_ = new QLineEdit(this);
    searchLineEdit_->setObjectName(QStringLiteral("navSearchLineEdit"));
    searchLineEdit_->setPlaceholderText(tr("搜索站点 / 地址"));
    searchLineEdit_->setClearButtonEnabled(true);
    searchLineEdit_->setMinimumWidth(140);
    searchLineEdit_->setMaximumWidth(320);

    // 迭代 3（规格排布：搜索框 → 高级筛选 → 消息图标 → 用户头像）：
    // 入口按钮只做图标与信号，弹窗/页面跳转逻辑归外层（HomeShell/页面）。
    filterButton_ = new QPushButton(this); // 手绘漏斗图标（迭代 3：emoji 码位渲染失真）
    filterButton_->setObjectName(QStringLiteral("navFilterButton"));
    filterButton_->setProperty("navIconButton", true);
    filterButton_->setIcon(makeFunnelIcon());
    filterButton_->setIconSize(QSize(24, 24));
    filterButton_->setCursor(Qt::PointingHandCursor);
    filterButton_->setAccessibleName(tr("高级筛选"));
    filterButton_->setToolTip(tr("高级筛选"));
    filterButton_->hide();

    notifyButton_ = new QPushButton(this);
    notifyButton_->setObjectName(QStringLiteral("navNotifyButton"));
    notifyButton_->setProperty("navIconButton", true);
    notifyButton_->setIcon(makeBellIcon());
    notifyButton_->setIconSize(QSize(24, 24));
    notifyButton_->setCursor(Qt::PointingHandCursor);
    notifyButton_->setAccessibleName(tr("消息通知"));
    notifyButton_->setToolTip(tr("消息通知"));
    notifyButton_->hide();

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

    rootLayout->addWidget(backButton_);
    rootLayout->addWidget(logoLabel_);
    rootLayout->addWidget(nameLabel_);
    rootLayout->addWidget(searchLineEdit_, 2);
    rootLayout->addWidget(filterButton_);
    rootLayout->addWidget(notifyButton_);
    rootLayout->addStretch(1);
    rootLayout->addWidget(nicknameLabel_);
    rootLayout->addWidget(avatarButton_);
    rootLayout->addWidget(loginButton_);

    connect(loginButton_, &QPushButton::clicked, this, [this]() { emit loginRequested(); });
    connect(backButton_, &QPushButton::clicked, this, [this]() { emit backRequested(); });
    connect(avatarButton_, &QPushButton::clicked, this,
            [this]() { emit profileRequested(); });
    connect(filterButton_, &QPushButton::clicked, this,
            [this]() { emit filterRequested(); });
    connect(notifyButton_, &QPushButton::clicked, this,
            [this]() { emit notificationsRequested(); });
    connect(searchLineEdit_, &QLineEdit::returnPressed, this, [this]() {
        emit searchSubmitted(searchLineEdit_->text().trimmed());
    });

    // 迭代 3：筛选/通知按钮以 hide() 起步，初始显隐统一走 refreshVisibility
    // （其余控件显隐不受影响，等价于原默认态）。
    refreshVisibility();
}

void TopNavBar::setUser(const charging::model::User& user)
{
    hasUser_ = true;
    nicknameLabel_->setText(user.nickname);
    nicknameLabel_->show();
    avatarButton_->show();
    loginButton_->hide();
    refreshVisibility();
}

void TopNavBar::clearUser()
{
    hasUser_ = false;
    nicknameLabel_->clear();
    nicknameLabel_->hide();
    avatarButton_->hide();
    loginButton_->show();
    refreshVisibility();
}

bool TopNavBar::hasUser() const
{
    return hasUser_;
}

void TopNavBar::setBackVisible(bool visible)
{
    backVisible_ = visible;
    refreshVisibility();
}

bool TopNavBar::isBackVisible() const
{
    return backButton_->isVisible();
}

void TopNavBar::setSearchVisible(bool visible)
{
    searchVisible_ = visible;
    refreshVisibility();
}

bool TopNavBar::isSearchVisible() const
{
    return searchLineEdit_->isVisible();
}

void TopNavBar::refreshVisibility()
{
    backButton_->setVisible(backVisible_);
    searchLineEdit_->setVisible(searchVisible_);
    // 迭代 3：筛选/通知入口与搜索框同组显隐（“找站”语境三件套）。
    filterButton_->setVisible(searchVisible_);
    notifyButton_->setVisible(searchVisible_);
    // 品牌区：二级页面隐藏；已登录时只在搜索框可见的页面让位——
    // 搜索收起的页面（订单/充电/我的）恢复品牌区平衡左侧版式。
    const bool brandVisible = !backVisible_ && (!hasUser_ || !searchVisible_);
    logoLabel_->setVisible(brandVisible);
    nameLabel_->setVisible(!hasUser_ || !searchVisible_);
}

bool TopNavBar::isFilterVisible() const
{
    return filterButton_->isVisible();
}

bool TopNavBar::isNotificationsVisible() const
{
    return notifyButton_->isVisible();
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
