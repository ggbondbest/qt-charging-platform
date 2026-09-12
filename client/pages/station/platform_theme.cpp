// platform_theme.cpp —— installPlatformTheme 的实现（主题装载底座，详见头注释）。
#include "pages/station/platform_theme.h"

#include <QApplication>
#include <QFile>
#include <QString>

// AUTORCC 生成的资源初始化函数位于全局命名空间，Q_INIT_RESOURCE 必须在全局
// 作用域调用，否则符号解析会带上当前命名空间导致链接失败。
static void ensureClientPlatformResourceRegistered()
{
    Q_INIT_RESOURCE(client_platform);
}

namespace charging::client::pages::station {

void installPlatformTheme()
{
    ensureClientPlatformResourceRegistered();

    QFile styleFile(QStringLiteral(":/qss/client_platform.qss"));
    // 资源缺失即静默放弃：不装样式页面照常运行，各页局部 QSS 仍生效；
    // 重复调用只是把同一份样式表再设一遍（setStyleSheet 全量替换），无副作用。
    if (!styleFile.open(QIODevice::ReadOnly)) {
        return;
    }
    qApp->setStyleSheet(QString::fromUtf8(styleFile.readAll()));
}

} // namespace charging::client::pages::station
