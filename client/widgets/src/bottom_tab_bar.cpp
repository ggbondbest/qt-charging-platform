#include "charging/client/widgets/bottom_tab_bar.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVariant>

namespace charging::client {

namespace {

// 组件自带样式：与全局主题同一套 token，仅在本组件内生效。
const char* kBottomTabBarStyleSheet = R"(
QWidget#uiBottomTabBar {
    background: #FFFFFF;
    border-top: 1px solid #E5E9EF;
}
QPushButton[navTab="true"] {
    background: transparent;
    border: none;
    border-radius: 10px;
    padding: 9px 0px;
    font-size: 13px;
    font-weight: 600;
    color: #6B7280;
}
QPushButton[navTab="true"]:checked {
    background: #EAF9F2;
    color: #00A76D;
    font-weight: 700;
}
QPushButton[navTab="true"]:hover {
    color: #1F2937;
}
)";

} // namespace

BottomTabBar::BottomTabBar(const QList<Tab>& tabs, QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("uiBottomTabBar"));
    setStyleSheet(QString::fromLatin1(kBottomTabBarStyleSheet));

    group_ = new QButtonGroup(this);
    group_->setExclusive(true);

    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(12, 6, 12, 6);
    rootLayout->setSpacing(8);

    for (const auto& tab : tabs) {
        auto* button = new QPushButton(tab.text, this);
        // objectName 供测试与 QSS 定位；ids_ 与按钮一一对应。
        button->setObjectName(QStringLiteral("tab_") + tab.id);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setProperty("navTab", true);
        group_->addButton(button);
        buttons_.append(button);
        ids_.append(tab.id);
        rootLayout->addWidget(button, 1);

        const QString id = tab.id;
        connect(button, &QPushButton::clicked, this, [this, id]() {
            // 即使点击的是当前 Tab 也发信号：宿主可能有叠加页（如详情路由）
            // 需要回到该 Tab 对应的主页面。
            current_ = id;
            emit tabChanged(id);
        });
    }
}

void BottomTabBar::setCurrentTab(const QString& id)
{
    const int index = ids_.indexOf(id);
    if (index < 0) {
        return;
    }
    buttons_.at(index)->setChecked(true);
    if (current_ != id) {
        current_ = id;
        emit tabChanged(id);
    }
}

QString BottomTabBar::currentTab() const
{
    return current_;
}

} // namespace charging::client
