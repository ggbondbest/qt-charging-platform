#pragma once

#include "services/favorites/favorites_service.h"
#include "services/station/station_query_service.h"

#include <QPointer>
#include <QWidget>

class QButtonGroup;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace charging::client {
class NoticePanel;
}

namespace charging::client::pages::station {

class StationMapPanel;
class StationFilterDialog;

// “找站”页（成员 2，任务 #7）：找站业务完整落地。
//
// 结构自上而下：地图容器（StationMapPanel，缺 Key/加载失败自动降级且列表仍可
// 浏览）→ 筛选操作栏（空闲优先 / 距离最近排序 + 电价筛选，变更即时刷新）→
// 站点卡片列表（可滚动；卡片含名称、电价、空闲桩数、距离，点击进入详情路由）。
// 地址搜索由顶部导航公共组件的搜索框触发（HomeShell 接线）。
//
// 数据经 StationQueryService 获取：当前为模拟数据通道（带加载延迟），真实
// GET_STATIONS 接口就绪后切换 liveMode 即可，本页 UI 逻辑零改动。
// 列表区域具备加载 / 空数据 / 异常（可重试）/ 正常四种状态。
class StationHomePage final : public QWidget
{
    Q_OBJECT

public:
    enum class ViewState
    {
        Loading,
        Empty,
        Error,
        List
    };

    explicit StationHomePage(QWidget* parent = nullptr);

    services::station::StationQueryService* service() const;
    StationMapPanel* mapPanel() const;

    // 顶部搜索框触发：展示加载状态并检索。
    void search(const QString& keyword);

    // 状态探针（测试与调试用）。
    ViewState viewState() const;
    int stationCardCount() const;
    QString currentKeyword() const;

    // 当前列表中的站点 ID（按显示顺序）与对应卡片控件。
    QVector<qint64> visibleStationIds() const;
    QWidget* stationCardAt(int index) const;

    // —— 迭代 3 · 收藏 + 高级筛选 ——
    // 收藏服务注入（HomeShell 统一装配，与收藏夹页共用同一实例）；
    // 未注入时页面懒建自用实例兜底（独立测试/降级：内存态、按访客隔离）。
    void setFavoritesService(services::favorites::FavoritesService* service);
    services::favorites::FavoritesService* favoritesService();

    // 打开高级筛选弹窗（QPointer 去重）；“确定”经 setFilterCriteria 生效。
    void openFilterDialog();
    void setFilterCriteria(const services::station::StationFilterCriteria& criteria);
    services::station::StationFilterCriteria filterCriteria() const;

signals:
    // 点击站点卡片：请求跳转站点详情（任务 #12 页面，仅路由）。
    void stationSelected(const charging::model::Station& station, int distanceMeters);

private slots:
    void handleQueryStarted();
    void handleQuerySucceeded(const charging::client::services::station::StationList& stations);
    void handleQueryFailed(const QString& message);
    void refreshFilteredCards();
    void retrySearch();
    void clearKeywordAndSearch();
    // 收藏变化 → 只重画星星（不重建卡片列表，点击收藏不打断浏览位置）。
    void refreshStarButtons();
    // 空态操作按钮：筛选生效时“重置筛选”，否则“清空搜索”。
    void handleEmptyAction();

private:
    QWidget* createStationCard(const services::station::StationListItem& item);
    void setViewState(ViewState state);
    // 星星视觉态回显（☆/★ + starred 属性驱动局部 QSS）。
    void applyStarState(QPushButton* starButton, qint64 stationId) const;

    services::station::StationQueryService* service_ = nullptr;
    StationMapPanel* mapPanel_ = nullptr;
    services::favorites::FavoritesService* favoritesService_ = nullptr;
    QPointer<StationFilterDialog> filterDialog_;
    services::station::StationFilterCriteria filterCriteria_;

    // 筛选栏。
    QButtonGroup* sortGroup_ = nullptr;
    QPushButton* sortRecommendedButton_ = nullptr;
    QPushButton* sortAvailableButton_ = nullptr;
    QPushButton* sortDistanceButton_ = nullptr;
    QComboBox* priceFilterComboBox_ = nullptr;

    // 列表区四态。
    QStackedWidget* listStack_ = nullptr;
    QWidget* loadingPage_ = nullptr;
    NoticePanel* emptyNotice_ = nullptr;
    NoticePanel* errorNotice_ = nullptr;
    QWidget* listPage_ = nullptr;
    QVBoxLayout* listLayout_ = nullptr;

    services::station::StationList lastResults_;
    QString keyword_;
    ViewState viewState_ = ViewState::Loading;
};

} // namespace charging::client::pages::station
