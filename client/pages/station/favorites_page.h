#pragma once

#include "services/favorites/favorites_service.h"
#include "services/station/station_query_service.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace charging::client {
class NoticePanel;
}

namespace charging::client::pages::station {

class StationFilterDialog;

// 收藏夹页（成员 2，迭代 3）：「我的 → 收藏」进入的路由页，复用全局顶部
// 导航 + 底部 Tab（壳层口径）。展示收藏站点列表（复用站点卡样式），卡片
// 星星可直接取消收藏；顶部「高级筛选」复用找站页同一 StationFilterDialog
// 组件（同一条件模型、同一投影过滤 applyStationFilter）。
//
// 数据口径：收藏 ID 来自 FavoritesService（与首页星星同实例，变更实时回显）；
// 站点资料当前从 StationQueryService 模拟通道取（本地页自建实例），ID 无对应
// 站点时跳过。TODO(contract)：后端收藏详情接口就绪后改由服务端直出收藏列表，
// 页面零改动。
//
// 四态与找站页同构：加载 / 空（“暂无收藏的充电站”）/ 异常（重试）/ 列表。
class FavoritesPage final : public QWidget
{
    Q_OBJECT

public:
    explicit FavoritesPage(QWidget* parent = nullptr);

    // 非拥有：HomeShell 注入（与首页收藏服务同实例）；未注入时页面自建兜底。
    void setFavoritesService(charging::client::services::favorites::FavoritesService* service);

    // 重建列表：收藏 ∩ 查询结果 → 高级筛选投影（组内 OR / 组间 AND）。
    void refresh();

    // 打开高级筛选弹窗（QPointer 去重，组件与找站页共用）。
    void openFilterDialog();
    void setFilterCriteria(const charging::client::services::station::StationFilterCriteria& criteria);

    // 测试探针。
    int favoriteCardCount() const;
    bool emptyStateVisible() const;

    // 查询状态机（验收缺陷 4）：入口无条件 refresh() 不得旁路——首查未落定
    // 保持加载态、上次查询失败保持异常态（重试入口可达），只有结果可用后
    // 才允许渲染空/列表态。
    enum class ViewState { Loading, Empty, Error, List };
    ViewState viewState() const;

signals:
    // 点击卡片：请求跳转站点详情（与找站页同口径，壳层统一路由）。
    void stationSelected(const charging::model::Station& station, int distanceMeters);

private slots:
    void handleQueryStarted();
    void handleQuerySucceeded(const charging::client::services::station::StationList& stations);
    void handleQueryFailed(const QString& message);
    void handleFavoritesChanged();
    void retryQuery();

private:
    QWidget* createFavoriteCard(const charging::client::services::station::StationListItem& item);

    services::station::StationQueryService* service_ = nullptr; // owned（本页独立通道）
    services::favorites::FavoritesService* favorites_ = nullptr; // not owned
    QPointer<StationFilterDialog> filterDialog_;
    services::station::StationFilterCriteria filterCriteria_;

    services::station::StationList lastResults_;
    bool queryLoaded_ = false; // 本页查询已有一次成功结果落定
    bool queryFailed_ = false; // 最近一次查询失败（异常态直至重试成功）

    QStackedWidget* stack_ = nullptr;
    QWidget* loadingPage_ = nullptr;
    NoticePanel* emptyNotice_ = nullptr;
    NoticePanel* errorNotice_ = nullptr;
    QVBoxLayout* listLayout_ = nullptr;
    QWidget* listPage_ = nullptr;
};

} // namespace charging::client::pages::station
