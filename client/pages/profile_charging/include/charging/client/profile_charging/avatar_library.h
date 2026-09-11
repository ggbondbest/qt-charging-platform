#pragma once

#include <QPixmap>
#include <QString>
#include <QVector>

class QLabel;

namespace charging::client {

// 内置头像库：契约 v1 §3（UPDATE_USER_INFO）冻结 avatarKey 只能取内置清单
// （bolt/plug/car/leaf/cat/panda/moon/rocket，空串恢复默认），图片上传被明确
// 排除在协议外；可持久化的"换头像"能力 = 从本库选一个，选择结果以
// User.avatarKey 随资料保存。本清单是三方的单点（客户端渲染、服务端
// UserApiService 校验、mock 镜像），漂移由对拍测试拦截。
// 渲染为确定性 QPainter 绘制（渐变圆底 + emoji 字形），不引入图片资源，
// 离线可用，尺寸任意。key 为空或未知时返回空 pixmap——调用方回退到
// "昵称首字"字母头像（全局 QSS 的 uiAvatar/uiAvatarHub 样式）。
struct AvatarSpec
{
    QString key;      // 持久化进 User.avatarKey 的标识
    QString glyph;    // emoji 字形
    QRgb startRgb;    // 圆底渐变起点（左上）
    QRgb endRgb;      // 圆底渐变终点（右下）
};

class AvatarLibrary
{
public:
    static const QVector<AvatarSpec>& all();
    static bool contains(const QString& key);
    // sizePx 为逻辑像素；返回带 DPR=2 的清晰 pixmap。空/未知 key → 空 QPixmap。
    static QPixmap render(const QString& key, int sizePx);
    // 把 key 对应的头像（或 fallbackText 字母头像）应用到 QLabel：切
    // hasAvatar 属性让 QSS 在 pixmap 态隐藏绿底圆，避免透出方角底色。
    static void applyToLabel(QLabel* label, const QString& key, const QString& fallbackText,
                             int sizePx);
};

} // namespace charging::client
