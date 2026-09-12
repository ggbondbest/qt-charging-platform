// PullToRefreshArea 状态机单元测试。
//
// 事件用 QApplication::sendEvent 直接投递到 viewport，绕开真实指针抓取，
// 状态机路径与线下鼠标完全一致（release 会被拖拽期间安装的 qApp 过滤器
// 抢先消费，这正是防止卡片把拖拽误判为点击的那条路）。
// offscreen 平台上 motion::animationsEnabled() 为 false，setGap 立即生效、
// setRefreshing(false) 无 350ms 停留——断言因此是确定性的。
//
// 私有常量在测试中镜像（与头文件保持同步）：
//   kActivatePx = 8, kThresholdPx = 56, kRestGapPx = 44。

#include "charging/client/widgets/pull_to_refresh_area.h"

#include <QApplication>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

using namespace charging::client;

namespace {

constexpr int kActivatePx = 8;
constexpr int kThresholdPx = 56;
constexpr int kRestGapPx = 44;

// 一个可滚动（内容 1500px 高于视口）的下拉区，展平后布局已生效。
PullToRefreshArea* makeArea()
{
    auto* area = new PullToRefreshArea();
    area->setWidgetResizable(true);

    auto* container = new QWidget();
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* content = new QWidget(container);
    content->setFixedHeight(1500);
    layout->addWidget(content);
    layout->addStretch();
    area->setPullContent(container);

    area->resize(360, 300);
    area->show();
    if (!QTest::qWaitForWindowExposed(area)) {
        QWARN("window not exposed");
    }
    return area;
}

void sendPress(PullToRefreshArea* area, int y)
{
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(100.0, y), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(area->viewport(), &press);
}

void sendMove(PullToRefreshArea* area, int y)
{
    QMouseEvent move(QEvent::MouseMove, QPointF(100.0, y), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(area->viewport(), &move);
}

void sendRelease(PullToRefreshArea* area, int y)
{
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(100.0, y), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(area->viewport(), &release);
}

} // namespace

class PullToRefreshTest : public QObject
{
    Q_OBJECT

private slots:
    // 垫块必须坐在容器布局第 0 项——各页"清行保垫块"的模式以此为前提。
    void spacerIsFirstLayoutItem()
    {
        auto* area = makeArea();
        auto* container = area->widget();
        auto* layout = qobject_cast<QVBoxLayout*>(container->layout());
        QVERIFY(layout != nullptr);
        QCOMPARE(layout->itemAt(0)->widget(), area->pullSpacer());
        delete area;
    }

    // 全链路：顶格按下 → 过激活阈值进 Pulling → 过触发阈值进 Armed →
    // 松手触发 refreshRequested 并进 Refreshing（垫块停在 kRestGapPx）→
    // 数据回包 setRefreshing(false) 收起。
    void pullBeyondThresholdTriggersRefresh()
    {
        auto* area = makeArea();
        QSignalSpy spy(area, &PullToRefreshArea::refreshRequested);

        sendPress(area, 100);
        sendMove(area, 100 + kActivatePx); // dy=8 恰好到激活线。
        QCOMPARE(area->state(), PullToRefreshArea::State::Pulling);
        QVERIFY(area->pullSpacer()->height() > 0);

        sendMove(area, 100 + kThresholdPx); // dy=56 恰好到触发线。
        QCOMPARE(area->state(), PullToRefreshArea::State::Armed);

        sendRelease(area, 100 + kThresholdPx);
        QCOMPARE(area->state(), PullToRefreshArea::State::Refreshing);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(area->pullSpacer()->height(), kRestGapPx);

        area->setRefreshing(false);
        QCOMPARE(area->state(), PullToRefreshArea::State::Collapsed);
        QCOMPARE(area->pullSpacer()->height(), 0);
        delete area;
    }

    // 没过触发阈值就松手：取消，不发信号，垫块回零。
    void releaseBelowThresholdCancels()
    {
        auto* area = makeArea();
        QSignalSpy spy(area, &PullToRefreshArea::refreshRequested);

        sendPress(area, 100);
        sendMove(area, 100 + 30); // 激活但不够触发。
        QCOMPARE(area->state(), PullToRefreshArea::State::Pulling);

        sendRelease(area, 100 + 30);
        QCOMPARE(area->state(), PullToRefreshArea::State::Collapsed);
        QCOMPARE(spy.count(), 0);
        QCOMPARE(area->pullSpacer()->height(), 0);
        delete area;
    }

    // 未过激活线的微动按普通点击收场，状态纹丝不动。
    void smallMoveNeverArms()
    {
        auto* area = makeArea();
        QSignalSpy spy(area, &PullToRefreshArea::refreshRequested);

        sendPress(area, 100);
        sendMove(area, 100 + kActivatePx - 1);
        QCOMPARE(area->state(), PullToRefreshArea::State::Collapsed);
        sendRelease(area, 100 + kActivatePx - 1);
        QCOMPARE(area->state(), PullToRefreshArea::State::Collapsed);
        QCOMPARE(spy.count(), 0);
        delete area;
    }

    // 列表不在顶部时下拉无效（那是正常滚动不是刷新）；回顶后恢复可用。
    void pullDisabledAwayFromTop()
    {
        auto* area = makeArea();
        QSignalSpy spy(area, &PullToRefreshArea::refreshRequested);
        QVERIFY(area->verticalScrollBar()->maximum() > 0); // 内容确凿可滚。

        area->verticalScrollBar()->setValue(area->verticalScrollBar()->maximum());
        sendPress(area, 100);
        sendMove(area, 100 + 200);
        QCOMPARE(area->state(), PullToRefreshArea::State::Collapsed);
        sendRelease(area, 100 + 200);
        QCOMPARE(spy.count(), 0);

        area->verticalScrollBar()->setValue(0); // 回顶，手势重新武装。
        sendPress(area, 100);
        sendMove(area, 100 + kThresholdPx);
        QCOMPARE(area->state(), PullToRefreshArea::State::Armed);
        sendRelease(area, 100 + kThresholdPx);
        QCOMPARE(spy.count(), 1);
        area->setRefreshing(false);
        delete area;
    }

    // 页面也可编程驱动（例如进入页面时已有请求在途）：幂等且成对收起。
    void programmaticRefreshingCycle()
    {
        auto* area = makeArea();

        area->setRefreshing(true);
        area->setRefreshing(true); // 幂等。
        QCOMPARE(area->state(), PullToRefreshArea::State::Refreshing);
        QCOMPARE(area->pullSpacer()->height(), kRestGapPx);

        area->setRefreshing(false);
        QCOMPARE(area->state(), PullToRefreshArea::State::Collapsed);
        area->setRefreshing(false); // 收起后再收一次不得翻转状态。
        QCOMPARE(area->state(), PullToRefreshArea::State::Collapsed);
        delete area;
    }

    // 刷新中按下不得重新武装（防重入：请求回包前不许再触发一次）。
    void pressWhileRefreshingDoesNotArm()
    {
        auto* area = makeArea();
        QSignalSpy spy(area, &PullToRefreshArea::refreshRequested);

        area->setRefreshing(true);
        sendPress(area, 100);
        sendMove(area, 100 + kThresholdPx + 20);
        QCOMPARE(area->state(), PullToRefreshArea::State::Refreshing);
        sendRelease(area, 100 + kThresholdPx + 20);
        QCOMPARE(spy.count(), 0);
        area->setRefreshing(false);
        delete area;
    }
};

QTEST_MAIN(PullToRefreshTest)

#include "tst_pull_to_refresh.moc"
