#include "charging/client/profile_charging/progress_service.h"

#include <QDate>
#include <QSettings>

namespace charging::client {
namespace {

constexpr int kBonusXp = 30;      // 全勤奖励

// 累计 XP 门槛即等级：青铜0 / 白银60 / 黄金150 / 铂金350 / 黑金700。
struct TierDef {
    const char* name; const char* glyph; qint64 threshold; qint64 giftPoints;
    const char* perk;
};
constexpr TierDef kTiers[] = {
    {"青铜会员", "🥉",   0,   0, "成长之路的起点 · 解锁每日任务与签到经验"},
    {"白银会员", "🥈",  60, 100, "白银专属徽章 · 升级礼包 +100 积分"},
    {"黄金会员", "🥇", 150, 150, "黄金徽章 · 充电账单月结提醒 · 礼包 +150 积分"},
    {"铂金会员", "💎", 350, 200, "铂金徽章 · 预约优先通道 · 生日券包 · 礼包 +200 积分"},
    {"黑金会员", "👑", 700, 300, "黑金至尊徽章 · 全权益解锁 · 年度尊享礼遇"},
};
constexpr int kTierCount = int(sizeof(kTiers) / sizeof(kTiers[0]));

// 每日任务定义：action=去完成跳转的路由；checkin 由任务页直接调
// pointsService.checkIn()（真实 +10 积分），成功后 reportEvent("checkin") 记 XP。
struct TaskDef {
    const char* id; const char* glyph; const char* title; const char* desc;
    int xp; const char* action;
};
constexpr TaskDef kTasks[] = {
    {"checkin", "📅", "每日签到",     "签到领积分，同时获得经验",     30, "checkin"},
    {"search",  "🔍", "搜索充电站",   "用顶栏搜索地址或找站关键词",   20, "station"},
    {"detail",  "🏢", "浏览电站详情", "打开任意电站详情页浏览",       20, "station"},
    {"route",   "🧭", "规划导航路线", "为电站规划一条驾车/步行路线",  20, "station"},
    {"stats",   "📊", "查看充电月报", "打开月报看本月充电账单",       20, "stats"},
};
constexpr int kTaskCount = int(sizeof(kTasks) / sizeof(kTasks[0]));

const char* const kBonusId = "bonus";

} // namespace

ProgressService::ProgressService(const QString& phone, QObject* parent)
    : QObject(parent)
    , groupKey_(QStringLiteral("progress/")
                + (phone.trimmed().isEmpty() ? QStringLiteral("guest") : phone.trimmed()))
{
    load();
}

QString ProgressService::today() const
{
    return todayOverride_.isEmpty()
        ? QDate::currentDate().toString(Qt::ISODate) : todayOverride_;
}

int ProgressService::level() const
{
    int lv = 1;
    for (int i = 0; i < kTierCount; ++i)
        if (xp_ >= kTiers[i].threshold) lv = i + 1;
    return lv;
}
QString ProgressService::tierName() const { return QString::fromUtf8(kTiers[level() - 1].name); }
QString ProgressService::tierGlyph() const { return QString::fromUtf8(kTiers[level() - 1].glyph); }
qint64 ProgressService::xpIntoLevel() const { return xp_ - kTiers[level() - 1].threshold; }
qint64 ProgressService::xpSpan() const
{
    const int lv = level();
    return lv >= kTierCount ? 0 : kTiers[lv].threshold - kTiers[lv - 1].threshold;
}
qint64 ProgressService::xpToNext() const
{
    const int lv = level();
    return lv >= kTierCount ? 0 : kTiers[lv].threshold - xp_;
}
qreal ProgressService::progress() const
{
    const qint64 span = xpSpan();
    return span <= 0 ? 1.0 : double(xpIntoLevel()) / double(span);
}
QString ProgressService::nextTierName() const
{
    const int lv = level();
    return lv >= kTierCount ? QStringLiteral("已是最高等级")
                            : QString::fromUtf8(kTiers[lv].name);
}

QVariantList ProgressService::tierTable() const
{
    const int lv = level();
    QVariantList out;
    for (int i = 0; i < kTierCount; ++i) {
        out.append(QVariantMap{
            {"level", i + 1},
            {"name", QString::fromUtf8(kTiers[i].name)},
            {"glyph", QString::fromUtf8(kTiers[i].glyph)},
            {"threshold", kTiers[i].threshold},
            {"giftPoints", kTiers[i].giftPoints},
            {"perk", QString::fromUtf8(kTiers[i].perk)},
            {"state", i + 1 < lv ? QStringLiteral("reached")
                    : i + 1 == lv ? QStringLiteral("current")
                                  : QStringLiteral("locked")},
        });
    }
    return out;
}

bool ProgressService::taskDoneToday(const QString& id) const
{
    return done_.value(id) == today();
}

void ProgressService::markTaskDone(const QString& id) { done_.insert(id, today()); }

int ProgressService::doneTaskCount() const
{
    int n = 0;
    for (int i = 0; i < kTaskCount; ++i)
        if (taskDoneToday(QString::fromLatin1(kTasks[i].id))) ++n;
    return n;
}

bool ProgressService::allTasksDone() const { return taskDoneToday(QLatin1String(kBonusId)); }

QVariantList ProgressService::tasks() const
{
    QVariantList out;
    for (int i = 0; i < kTaskCount; ++i) {
        const TaskDef& t = kTasks[i];
        out.append(QVariantMap{
            {"id", QString::fromLatin1(t.id)},
            {"glyph", QString::fromUtf8(t.glyph)},
            {"title", QString::fromUtf8(t.title)},
            {"desc", QString::fromUtf8(t.desc)},
            {"xp", t.xp},
            {"done", taskDoneToday(QString::fromLatin1(t.id))},
            {"action", QString::fromLatin1(t.action)},
        });
    }
    return out;
}

QVariantList ProgressService::gifts() const { return gifts_; }

void ProgressService::reportEvent(const QString& eventId)
{
    int found = -1;
    for (int i = 0; i < kTaskCount; ++i)
        if (eventId == QLatin1String(kTasks[i].id)) { found = i; break; }
    if (found < 0 || taskDoneToday(eventId)) return;   // 未知事件/当日重复：幂等丢弃
    markTaskDone(eventId);
    emit tasksChanged();
    award(kTasks[found].xp);
    // 全勤检查放在 award 之后：最后一条任务完成即触发，额外 +kBonusXp。
    if (!allTasksDone() && doneTaskCount() == kTaskCount) {
        markTaskDone(QLatin1String(kBonusId));
        emit tasksChanged();
        award(kBonusXp);
    }
    save();
}

void ProgressService::award(qint64 amount)
{
    const int oldLevel = level();
    xp_ += amount;
    const int newLevel = level();
    emit xpChanged();
    for (int lv = oldLevel + 1; lv <= newLevel; ++lv) {
        gifts_.append(QVariantMap{
            {"level", lv},
            {"tier", QString::fromUtf8(kTiers[lv - 1].name)},
            {"glyph", QString::fromUtf8(kTiers[lv - 1].glyph)},
            {"points", kTiers[lv - 1].giftPoints},
            {"date", today()},
        });
        emit giftsChanged();
        emit levelUp(lv, QString::fromUtf8(kTiers[lv - 1].name), kTiers[lv - 1].giftPoints);
    }
}

void ProgressService::load()
{
    QSettings settings;
    settings.beginGroup(groupKey_);
    xp_ = settings.value(QStringLiteral("xp")).toLongLong();
    gifts_ = settings.value(QStringLiteral("gifts")).toList();
    const QStringList ids = settings.childKeys();
    for (const QString& key : ids) {
        // 注意：键不能含 '/'——QSettings 会把 "task/x" 读成子组，childKeys
        // 即不可见（子组语义）。改用下划线分隔，任务 id 均为小写无下划线。
        if (!key.startsWith(QLatin1String("task_"))) continue;
        done_.insert(key.mid(QLatin1String("task_").size()),
                     settings.value(key).toString());
    }
    // 过期的每日态不复活（跨天自然重置）：读取时按今天过滤。
    const QString todayValue = today();
    for (auto it = done_.begin(); it != done_.end();) {
        if (it.value() != todayValue) it = done_.erase(it);
        else ++it;
    }
    settings.endGroup();
}

void ProgressService::save() const
{
    QSettings settings;
    settings.beginGroup(groupKey_);
    settings.setValue(QStringLiteral("xp"), xp_);
    settings.setValue(QStringLiteral("gifts"), gifts_);
    for (auto it = done_.constBegin(); it != done_.constEnd(); ++it)
        settings.setValue(QStringLiteral("task_") + it.key(), it.value());
    // 清掉上一天残留的 task_* 键（done_ 载入时已按今天过滤，这里落盘同步）。
    const QStringList keys = settings.childKeys();
    for (const QString& key : keys)
        if (key.startsWith(QLatin1String("task_")) && !done_.contains(key.mid(QLatin1String("task_").size())))
            settings.remove(key);
    settings.sync();
    settings.endGroup();
}

} // namespace charging::client
