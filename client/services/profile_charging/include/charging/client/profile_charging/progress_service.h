#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>

namespace charging::client {

// 经验等级 + 每日任务引擎（2026-09-09 需求批）。纯客户端本地成长系统：
// QSettings 持久化（与 SettingsService 同通道，按登录手机号分组，互不串档）。
// 与积分体系（PointService/points_ledger，服务端单一事实源）边界：本服务自身
// 永不写积分账本、也不镜像服务端总分——签到任务只上报本地事件（真实 +10 积分
// 仍走 pointsService.checkIn）。升级礼包口径 2026-09-09 拍板变更：原"记等级
// 自己的账目"改为真入账——bridge 收 levelUp 后发 CREDIT_LEVEL_REWARD 落
// points_ledger（金额由服务端 levelRewardPoints() 单点推导）；本服务导出形状
// 不变，gifts_ 仍是 LevelPage「升级记录」的展示镜像与登录对账重放的数据源。
// 等级曲线（累计 XP）：青铜0 / 白银60 / 黄金150 / 铂金350 / 黑金700。
// 每日任务（当天幂等，跨天自动重置）：签到30 / 搜索20 / 详情20 / 路线20 /
// 充电报告20，全勤奖励30 —— 全勤一天 140 XP，约两天白银、四天铂金、五天黑金。
class ProgressService final : public QObject
{
    Q_OBJECT
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
    explicit ProgressService(const QString& phone, QObject* parent = nullptr);

    qint64 xp() const { return xp_; }
    int level() const;                       // 1..5（kTiers 下标+1）
    QString tierName() const;
    QString tierGlyph() const;               // 🥉🥈🥇💎👑
    qint64 xpIntoLevel() const;              // 本级内已累计 XP（黑金时=溢出部分）
    qint64 xpSpan() const;                   // 本级跨度（黑金=0）
    qint64 xpToNext() const;                 // 距升级还需（黑金=0）
    qreal progress() const;                  // 0..1，黑金恒 1
    QString nextTierName() const;            // 黑金=「已是最高等级」

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
    QString today() const;
    bool taskDoneToday(const QString& id) const;
    void markTaskDone(const QString& id);
    void award(qint64 amount);               // 加 XP + 跨档结算（礼包入 gifts_）
    void load();
    void save() const;

    QString groupKey_;                       // progress/<phone>
    qint64 xp_ = 0;
    QVariantList gifts_;
    QHash<QString, QString> done_;           // taskId → 完成日（含全勤键 "bonus"）
    QString todayOverride_;
};

} // namespace charging::client
