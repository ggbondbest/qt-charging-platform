#pragma once

#include "services/reservation/reservation_service.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace charging::client::pages::station {

// 已完成的预约页（成员 2，任务 #17 迭代）：预约模块二级 Tab 之一，
// 展示全部**已结束**的历史预约记录（已完成 / 已取消 / 已过期）。
//
// 每张卡片展示预约完整信息（站点、桩编号、充电规格、预约时长、预约时间、
// 费用、最终状态）；卡片可点击，弹出详情弹窗展示该预约全部字段。
// 边界状态：加载中 / 无历史记录空页 / 接口异常（友好提示 + 重试）。
// 历史列表置于 QScrollArea，鼠标滚轮上下滚动查看更多记录。
class ReservationCompletedPage final : public QWidget
{
    Q_OBJECT

public:
    enum class PageState
    {
        Loading, // 列表拉取中
        Error,   // 接口/网络异常
        Empty,   // 暂无历史预约
        List,    // 历史卡片列表
    };

    explicit ReservationCompletedPage(QWidget* parent = nullptr);

    // 模块驱动的视图状态（数据由 setHistory 注入，不直连服务信号）。
    void showLoading();
    void showError(const QString& message);
    void setHistory(const services::reservation::ReservationList& records);

    // 测试探针。
    PageState viewState() const;
    int recordCardCount() const;
    bool detailDialogVisible() const;
    QString detailDialogText() const;

signals:
    // 错误态“重试”：模块重新拉取列表。
    void retryRequested();

private:
    void setState(PageState state);
    void clearRows();
    QWidget* createHistoryCard(
        const services::reservation::ReservationRecord& record);
    void openDetailDialog(const services::reservation::ReservationRecord& record);

    PageState viewState_ = PageState::Loading;
    services::reservation::ReservationList records_;

    QStackedWidget* stack_ = nullptr;
    QWidget* loadingPage_ = nullptr;
    QWidget* errorNotice_ = nullptr;
    QWidget* emptyNotice_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
    QWidget* listPage_ = nullptr;
    QVBoxLayout* listLayout_ = nullptr;

    QPointer<QWidget> detailDialog_;
};

} // namespace charging::client::pages::station
