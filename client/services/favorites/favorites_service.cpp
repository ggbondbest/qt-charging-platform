#include "services/favorites/favorites_service.h"

#include <QStringList>
#include <QSettings>

namespace charging::client::services::favorites {

FavoritesService::FavoritesService(QObject* parent) : QObject(parent)
{
}

QString FavoritesService::storageKey(const QString& userKey)
{
    // 按用户分键：未登录（空键）不落盘，此处不会被调用。
    return QStringLiteral("favorites/%1/stationIds").arg(userKey);
}

void FavoritesService::setCurrentUser(const QString& userKey)
{
    const QString normalized = userKey.trimmed();
    if (normalized == userKey_) {
        return;
    }
    userKey_ = normalized;
    load();
    emit favoritesChanged();
}

QString FavoritesService::currentUser() const
{
    return userKey_;
}

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

bool FavoritesService::contains(qint64 stationId) const
{
    return ids_.contains(stationId);
}

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
