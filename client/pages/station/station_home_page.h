#pragma once

#include "services/station/station_query_service.h"

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

private:
    QWidget* createStationCard(const services::station::StationListItem& item);
    void setViewState(ViewState state);

    services::station::StationQueryService* service_ = nullptr;
    StationMapPanel* mapPanel_ = nullptr;

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
