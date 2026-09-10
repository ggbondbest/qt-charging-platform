// 经验等级引擎契约面（2026-09-09 经验等级批；实现逐段讲解见
// src/progress_service.cpp 文件头）。对外只有两张脸：
//   读——Q_PROPERTY 绑定面（QML 属性直读）+ 普通 getter（C++/测试共用）；
//   写——唯一 Q_INVOKABLE 漏斗 reportEvent()，别无公开变更入口。
// 成长路径因此天然单点：任何 XP 变化都能回溯到某个任务事件。
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>

namespace charging::client {

// 经验等级 + 每日任务引擎（2026-09-09 需求批）。纯客户端本地成长系统：
// QSettings 持久化（与 SettingsService 同通道，按登录手机号分组，互不串档）。
// 与积分体系（PointService/points_ledger，服务端单一事实源）严格分账：
// 本服务永不写积分账本、也不镜像服务端总分——签到任务只上报本地事件
// （真实 +10 积分仍走 pointsService.checkIn），升级礼包的"积分"记录在
// 等级体系自己的礼包账目里（LevelPage「升级记录」展示），不入余额/积分页。
// 等级曲线（累计 XP）：青铜0 / 白银60 / 黄金150 / 铂金350 / 黑金700。
// 每日任务（当天幂等，跨天自动重置）：签到30 / 搜索20 / 详情20 / 路线20 /
// 月报20，全勤奖励30 —— 全勤一天 140 XP，约两天白银、四天铂金、五天黑金。
class ProgressService final : public QObject
{
    Q_OBJECT
    // ---- QML 绑定面 ----：全部只读（无 WRITE），三条变更流按数据域分工——
    // xpChanged 驱动等级/档位/进度条整组读出，tasksChanged 管任务列表，
    // giftsChanged 管礼包账；QML 只订阅信号，不自己推导依赖。
    Q_PROPERTY(qint64 xp READ xp NOTIFY xpChanged)
    Q_PROPERTY(int level READ level NOTIFY xpChanged)
    Q_PROPERTY(QString tierName READ tierName NOTIFY xpChanged)
    Q_PROPERTY(QString tierGlyph READ tierGlyph NOTIFY xpChanged)
    Q_PROPERTY(qint64 xpIntoLevel READ xpIntoLevel NOTIFY xpChanged)
    Q_PROPERTY(qint64 xpSpan READ xpSpan NOTIFY xpChanged)
    Q_PROPERTY(qint64 xpToNext READ xpToNext NOTIFY xpChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY xpChanged)
    Q_PROPERTY(QString nextTierName READ nextTierName NOTIFY xpChanged)
    Q_PROPERTY(QVariantList tasks READ tasks NOTIFY tasksChanged)
    Q_PROPERTY(int doneTaskCount READ doneTaskCount NOTIFY tasksChanged)
    Q_PROPERTY(bool allTasksDone READ allTasksDone NOTIFY tasksChanged)
    Q_PROPERTY(QVariantList gifts READ gifts NOTIFY giftsChanged)
    // 等级阶梯（LevelPage 依赖 xpChanged 重算——QML 方法调用不带依赖追踪）。
    Q_PROPERTY(QVariantList tiers READ tierTable NOTIFY xpChanged)

public:
    // 按登录手机号建组构造（app_bridge 随会话 new）；组键回退 guest 口径见实现。
    explicit ProgressService(const QString& phone, QObject* parent = nullptr);

    // ---- 等级读出组：xp_ 的纯函数，只随 xpChanged 动 ----
    qint64 xp() const { return xp_; }
    int level() const;                       // 1..5（kTiers 下标+1）
    QString tierName() const;
    QString tierGlyph() const;               // 🥉🥈🥇💎👑
    qint64 xpIntoLevel() const;              // 本级内已累计 XP（黑金时=溢出部分）
    qint64 xpSpan() const;                   // 本级跨度（黑金=0）
    qint64 xpToNext() const;                 // 距升级还需（黑金=0）
    qreal progress() const;                  // 0..1，黑金恒 1
    QString nextTierName() const;            // 黑金=「已是最高等级」

    // ---- 每日任务/礼包/阶梯读出：随 tasksChanged / giftsChanged 动 ----
    QVariantList tasks() const;              // [{id,glyph,title,desc,xp,done,action}]
    int doneTaskCount() const;               // 今日已完成数（不含全勤奖励）
    bool allTasksDone() const;               // 今日全勤旗标
    QVariantList gifts() const;              // 升级记录 [{level,tier,glyph,points,date}] 旧→新
    QVariantList tierTable() const;          // 等级阶梯（tiers 属性，LevelPage 渲染）

    // XP 事件唯一入口：id ∈ 任务 id 集合；当日重复上报幂等丢弃，跨天重置。
    Q_INVOKABLE void reportEvent(const QString& eventId);

    // 测试缝：固定"今天"（yyyy-MM-dd），用于跨天重置断言。生产勿调。
    void setTodayForTesting(const QString& isoDate) { todayOverride_ = isoDate; }

signals:
    void xpChanged();
    void tasksChanged();
    void giftsChanged();
    // 一次 reportEvent 跨过多档时逐档发射（黑金前共 4 次）。
    void levelUp(int level, const QString& tierName, qint64 giftPoints);

private:
    // 逻辑"今天"单点：生产=系统日期、测试=setTodayForTesting 覆盖，
    // 当日判定与跨天重置全部由它派生（实现见 today() 处注释）。
    QString today() const;
    bool taskDoneToday(const QString& id) const;
    void markTaskDone(const QString& id);
    void award(qint64 amount);               // 加 XP + 跨档结算（礼包入 gifts_）
    // 盘↔内存成对私有：load 只在构造跑一次（此后内存态为准），
    // save 只在 reportEvent 落定时整体覆盖 + 剪昨日残键。
    void load();
    void save() const;

    QString groupKey_;                       // progress/<phone>
    qint64 xp_ = 0;
    QVariantList gifts_;
    QHash<QString, QString> done_;           // taskId → 完成日（含全勤键 "bonus"）
    QString todayOverride_;
};

} // namespace charging::client
