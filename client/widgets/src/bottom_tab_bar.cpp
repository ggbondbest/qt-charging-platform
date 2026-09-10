// 文件职责：widgets 通道底部 Tab 导航公共组件 BottomTabBar 的实现（成员 2，任务 #2）。
// 数据流向：Tab 列表（id+文案）由宿主 HomeShell 在构造时注入，组件只负责“选中样式 +
// 把点击翻译成 tabChanged(id)”；页面切换、路由栈等全部由宿主响应信号完成，本组件
// 不感知任何页面。与 QML 通道的孪生件 client/qml/platform/BottomTabBar.qml 并行存在。
#include "charging/client/widgets/bottom_tab_bar.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVariant>

namespace charging::client {

namespace {

// ---- 组件级样式表 ----
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

// ---- 构造：宿主注入 Tab 列表，逐个落成互斥的可勾选按钮 ----
BottomTabBar::BottomTabBar(const QList<Tab>& tabs, QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("uiBottomTabBar"));
    setStyleSheet(QString::fromLatin1(kBottomTabBarStyleSheet));

    // exclusive 的 QButtonGroup 从视觉上钉死“同一时刻只有一个选中”，无需手写
    // 取消其他按钮勾选的逻辑；current_ 则负责逻辑侧记住选中 id。
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

        // range-for 的 tab 是循环作用域引用，lambda 若捕获它会悬垂——先按值拷贝 id 再捕获。
        const QString id = tab.id;
        connect(button, &QPushButton::clicked, this, [this, id]() {
            // 即使点击的是当前 Tab 也发信号：宿主可能有叠加页（如详情路由）
            // 需要回到该 Tab 对应的主页面。
            current_ = id;
            emit tabChanged(id);
        });
    }
}

// ---- 程序化切 Tab ----
// 与点击路径互补：宿主切页后回调本函数同步选中态。setChecked 只改按钮的 checked
// 属性、不会触发 clicked，因此本函数改由自己发 tabChanged；且仅在 current_ 真变化
// 时发——宿主即便在 showTab 处理器里回灌同 id 也不会形成信号环。未知 id 直接忽略，
// 防路由表笔误把选中态打没。
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
