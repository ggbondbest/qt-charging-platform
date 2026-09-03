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
    if (!styleFile.open(QIODevice::ReadOnly)) {
        return;
    }
    qApp->setStyleSheet(QString::fromUtf8(styleFile.readAll()));
}

} // namespace charging::client::pages::station
