#pragma once

#include <QScrollArea>

class QLabel;
class QVBoxLayout;

namespace charging::client {

// 移动端直觉的"下拉刷新"：列表已在顶部时按住向下拖，顶部让出一段带回弹阻力的
// 空间并出现状态胶囊（下拉刷新 → 松开刷新 → 正在刷新），松手过阈值即发
// refreshRequested，页面数据回来后 setRefreshing(false) 收起。
//
// 实现边界（诚实记录）：Qt Widgets 没有 QScroller overscroll（那是 QML Flickable
// 的能力），这里用"顶部垫块 + 手势拦截"复刻手感：
//  * 只在按下点不落在按钮（或其后代）上、且垂直滚动条已在顶部时武装拖拽；
//  * 越过激活阈值后临时挂应用级事件过滤器吃掉本次 release——卡片不会再误触发
//    clicked，松手只意味着刷新；
//  * 拖拽位移经 1-exp 阻力曲线压缩，越拉越沉，接近"橡皮筋"观感；
//  * 收合动画走 motion token；offscreen/MOTION_REDUCED 下无动画、即时到位，
//    但状态机本身在测试平台仍然完整可驱动（合成事件喂 viewport 即可）。
class PullToRefreshArea final : public QScrollArea
{
    Q_OBJECT

public:
    enum class State
    {
        Collapsed, // 静止
        Pulling,   // 拖拽中，未达阈值
        Armed,     // 拖过阈值，松手即刷新
        Refreshing // 已发出请求，等待 setRefreshing(false)
    };
    Q_ENUM(State)

    explicit PullToRefreshArea(QWidget* parent = nullptr);

    // 页面在 fetch 结束（成功或失败）后调用；Refreshing → Collapsed。
    void setRefreshing(bool refreshing);
    State state() const { return state_; }

    // 等价 QScrollArea::setWidget，另在容器 QVBoxLayout 顶部插入隐藏的垫块。
    // 故意不叫 setWidget：QScrollArea::setWidget 非虚函数，隐藏会在基类指针
    // 调用点悄悄失效——换个名字把这条路焊死（要求容器布局必须是 QVBoxLayout）。
    void setPullContent(QWidget* widget);

    // 垫块本身。页面在"清空重建列表"时必须把它当非行内容保护起来
    // （先 takeAt 摘出、重建完再插回 0 位），否则下拉刷新会随一次刷没。
    QWidget* pullSpacer() const { return spacer_; }

signals:
    void refreshRequested();

protected:
    bool viewportEvent(QEvent* event) override;
    // 仅拖拽生效期间挂在 qApp 上：吃掉本次 release，防止卡片把拖拽误判成点击。
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setState(State state);
    void setGap(int px, bool animate = false);
    void updatePill();
    void finishGesture(bool triggerRefresh);
    void beginPull();
    void endPullInterceptor();

    State state_ = State::Collapsed;
    bool pending_ = false;    // 已按下、位移未达激活阈值
    bool pulling_ = false;    // 拖拽生效中
    bool appFiltered_ = false; // qApp release 拦截器已安装
    int pressY_ = 0;

    QWidget* spacer_ = nullptr;
    QLabel* pill_ = nullptr;

    static constexpr int kActivatePx = 8;   // 认定为拖拽的最小位移
    static constexpr int kThresholdPx = 56; // 触发刷新的下拉距离（未加阻力）
    static constexpr int kRestGapPx = 44;   // Refreshing 时驻留高度
    static constexpr double kRubber = 90.0; // 阻力曲线尺度：拉 90px 约占 63%
};

} // namespace charging::client
