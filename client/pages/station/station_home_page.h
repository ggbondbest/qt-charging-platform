// station_home_page.h —— “找站”主页声明（实现见同名 cpp）。
// 数据流向：列表数据走 StationQueryService（当前模拟通道，liveMode 打开后
// 改发 GET_STATIONS TCP 契约、解析回同一组信号，本页零改动）；收藏走
// FavoritesService（本机/登录态）。本页是 widgets 通道，QML 孪生页的
// 状态/信号→绑定对账口径见 docs/design/qml-station-mapping.md。
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
    // 列表区四态状态机：任何一次查询的终局必落在其一（进行中/无结果/
    // 失败可重试/有结果），保证 loading→成功→空、loading→失败→重试 等
    // 信号序列都有确定 UI，不留空白页。
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
    // 非模态弹窗去重闸：存活期间再点筛选只置顶旧窗；WA_DeleteOnClose
    // 销毁后 QPointer 自动归零（与设置页弹窗口径一致）。
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

    // 服务最近一次返回的全量结果：筛选/排序/地图标记全部在其上做纯投影，
    // 不回源重发请求；仅 search()/retrySearch() 会更新它。
    services::station::StationList lastResults_;
    QString keyword_;
    ViewState viewState_ = ViewState::Loading;
};

} // namespace charging::client::pages::station
