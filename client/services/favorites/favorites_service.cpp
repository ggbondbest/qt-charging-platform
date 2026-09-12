// FavoritesService 实现（迭代 3 个人中心域 · 成员 2 收藏批）：收藏状态的统一读写落点。
// 纯本地实现、零网络——后端 FAVORITE_* 接口未定义（扩展口径见头文件 TODO(contract)）。
// 消费方：widgets HomeShell 与 QML app_bridge/service_bridges 共用本实现，
// 均在登录/登出时以用户 id 串调 setCurrentUser 注入登录键。
// 数据流向：页面 toggle() → 更新内存 ids_ + persist() 落 QSettings →
// favoritesChanged 信号 → 页面重读 contains()/favoriteIds() 刷新星星与列表。
#include "services/favorites/favorites_service.h"

#include <QStringList>
#include <QSettings>

namespace charging::client::services::favorites {

// ---- 生命周期与用户键 ----

FavoritesService::FavoritesService(QObject* parent) : QObject(parent)
{
    // 构造即空态：登录键要等宿主 setCurrentUser 注入，此时不读盘。
}

QString FavoritesService::storageKey(const QString& userKey)
{
    // 按用户分键：未登录（空键）不落盘，此处不会被调用。
    // userKey 实为登录用户 id 串（HomeShell/QML 两宿主同口径），天然无 '/'，
    // 不会踩 QSettings 子组语义的坑。
    return QStringLiteral("favorites/%1/stationIds").arg(userKey);
}

// 换用户 = 换整个数据视图：先切键再 load()，保证 load/persist 读写的都是
// 新用户的键位；同键早退是幂等闸——宿主登录流程可能重复注入同一用户，
// 若不去重会做无谓的读盘并多发一次 favoritesChanged 打扰页面。
void FavoritesService::setCurrentUser(const QString& userKey)
{
    const QString normalized = userKey.trimmed();
    if (normalized == userKey_) {
        return;
    }
    userKey_ = normalized;
    load();
    // 无论新旧用户有无收藏都发：页面据此清空或重绘列表。
    emit favoritesChanged();
}

QString FavoritesService::currentUser() const
{
    return userKey_;
}

// ---- 持久化读写 ----

// 读盘：QSettings → ids_。逐条 toLongLong 校验，脏值/非正值静默跳过，
// 手改配置文件的坏数据不会把列表撑成非法状态。
void FavoritesService::load()
{
    ids_.clear();
    if (userKey_.isEmpty()) {
        return; // 未登录：内存态为空，不读盘
    }
    QSettings settings;
    // 以 QStringList 落盘（QSettings 对 qint64 列表的可移植性一般）。
    const QStringList stored = settings.value(storageKey(userKey_)).toStringList();
    for (const QString& value : stored) {
        bool ok = false;
        const qint64 id = value.toLongLong(&ok);
        if (ok && id > 0) {
            ids_.append(id);
        }
    }
}

// 写盘：ids_ → QSettings，整表覆盖（收藏是"列表"语义，没有增量协议）；
// 只在变更点（toggle）后调用，不做防抖——量级小，同步写盘换取"重启必回显"。
void FavoritesService::persist()
{
    if (userKey_.isEmpty()) {
        return; // 未登录：仅内存态
    }
    QStringList stored;
    stored.reserve(ids_.size());
    for (const qint64 id : ids_) {
        stored.append(QString::number(id));
    }
    QSettings settings;
    settings.setValue(storageKey(userKey_), stored);
}

// ---- 页面消费面（读查询 + 唯一写入口）----

bool FavoritesService::contains(qint64 stationId) const
{
    return ids_.contains(stationId);
}

// 收藏切换是唯一的写入口：内存更新、落盘、发信号三步一体，页面不需要
// （也不应该）自己维护勾选态。返回值 = 操作后的收藏态，正好契合按钮的
// toggle 语义，省一次 contains() 回读。
bool FavoritesService::toggle(qint64 stationId)
{
    if (stationId <= 0) {
        return contains(stationId); // 非法 ID 防御：不改状态
    }
    if (const int index = ids_.indexOf(stationId); index >= 0) {
        ids_.removeAt(index);
        persist();
        emit favoritesChanged();
        return false;
    }
    ids_.append(stationId); // 收藏时间正序：新收藏排末尾
    persist();
    emit favoritesChanged();
    return true;
}

QVector<qint64> FavoritesService::favoriteIds() const
{
    return ids_;
}

int FavoritesService::favoriteCount() const
{
    return ids_.size();
}

// 测试缝：删盘上键 + 清内存并广播，让下一个用例拿到干净的收藏态
//（不 reset 的话 QSettings 残留会跨用例串数据）。
void FavoritesService::resetForTesting()
{
    if (!userKey_.isEmpty()) {
        QSettings settings;
        settings.remove(storageKey(userKey_));
    }
    ids_.clear();
    emit favoritesChanged();
}

} // namespace charging::client::services::favorites
