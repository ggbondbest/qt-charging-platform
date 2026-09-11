// 头文件：收藏服务的对外契约（实现见同目录 favorites_service.cpp）。
// 宿主（widgets HomeShell、QML app_bridge/service_bridges）各持一个实例，
// 页面（站点卡星星/收藏夹页）只经本契约读写，不直接碰 QSettings。
#pragma once

#include <QObject>
#include <QVector>

namespace charging::client::services::favorites {

// 收藏服务（成员 2，迭代 3）：站点收藏的统一状态源。
// 与同域通知服务（notification_service.h）共用库与命名空间 services::favorites。
//
// 收藏状态必须活在服务层而非卡片控件——找站页每次筛选/搜索都会整体重建
// 卡片（clearLayoutItems + deleteLater），控件上的勾选态会随之丢失；
// 页面（首页卡片星星、收藏夹页）只从本服务读 contains()、写 toggle()。
//
// 持久化口径：QSettings 本地存储（组织/应用名在 client/app/main.cpp 统一
// 设置），按登录用户分键 favorites/<userKey>/stationIds，切换账号互不可见、
// 重启回显。后端收藏接口（FAVORITE_LIST / FAVORITE_TOGGLE）尚未定义
// （protocol/common 属成员 1/3 领域）——TODO(contract)：接口就绪后本服务
// 按 ReservationService 同款双通道模式扩展，信号形状不变，页面零改动。
//
// 未登录（userKey 为空）：仅内存态，不落盘、不读盘（切号不泄漏数据）。
class FavoritesService final : public QObject
{
    Q_OBJECT

public:
    // 生命周期由宿主托管（QObject parent），无需显式释放。
    explicit FavoritesService(QObject* parent = nullptr);

    // 登录用户键（HomeShell 在登录/登出时注入；空串 = 未登录）。
    // 切换时重新加载该用户的持久化收藏并发 favoritesChanged。
    void setCurrentUser(const QString& userKey);
    QString currentUser() const;

    bool contains(qint64 stationId) const;
    // 收藏/取消收藏，返回操作后的收藏态。变更即持久化（登录态）并发信号。
    bool toggle(qint64 stationId);
    // 当前用户全部收藏站点 ID（收藏时间正序）。
    QVector<qint64> favoriteIds() const;
    int favoriteCount() const;

    // 清除当前用户在 QSettings 中的持久化数据（测试隔离用）。
    void resetForTesting();

signals:
    // 收藏集合或所属用户发生变化（页面据此刷新星星/列表）。
    void favoritesChanged();

private:
    // ---- 内部实现（页面不可见）----
    // QSettings 键位拼接：favorites/<userKey>/stationIds。
    static QString storageKey(const QString& userKey);
    // 读写成对：两者都带"空键 = 未登录"守卫（不读盘/不落盘），
    // 盘上是字符串列表、内存是 qint64 向量，换算只发生在这两个函数里。
    void load();
    void persist();

    QString userKey_;
    // 盘上收藏的内存镜像（收藏时间正序）：所有查询/切换只读它，避免
    // 每次星星刷新都走一遍 QSettings 反序列化。
    QVector<qint64> ids_;
};

} // namespace charging::client::services::favorites
