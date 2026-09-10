// 经验等级引擎实现（2026-09-09 经验等级批核心；契约与讲解口径见对应头文件）。
// 纯客户端本地成长系统：零服务端依赖，xp/礼包/任务完成态全部落
// QSettings "progress/<phone>" 分组，换机/换账号即换档。
// 使用方：QmlApp（client/qml/app_bridge.cpp）登录时按手机号 new 一个、随
// session_ 生灭，经 App.progressService 暴露；QML TasksPage/TaskSection/
// LevelPage 读属性渲染，MyPage 等级三件套同源；单测 tst_progress_service.cpp。
// 数据流向（XP 漏斗单点）：用户行为（搜索/详情/路线/月报由 app_bridge 埋点，
// 签到由 TaskSection 在真实 +10 积分入账后回执）→ reportEvent(eventId)
// → 当日幂等闸 → award(XP) 跨档结算 → save() 落盘；页面只靠三个信号刷新。
// 双账本诚实口径：本文件记的礼包"积分"是等级体系自己的账（gifts_，仅展示
// "升级记录"），绝不写入、也不镜像服务端 points_ledger 的余额账本。
#include "charging/client/profile_charging/progress_service.h"

#include <QDate>
#include <QSettings>

namespace charging::client {
namespace {

// ---- 静态表：等级档位 + 每日任务定义（表驱动，纯数据零逻辑）----
// 档位与任务全部编译期常量化：调曲线/换文案只改这张表，引擎代码不动。
// 成员用 const char* 是因为 constexpr 表装不了 QString（非平凡析构），
// 出口处统一 QString::fromUtf8 转一次，成本可忽略。

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
    {"stats",   "📊", "查看充电报告", "打开充电报告看本月账单",       20, "stats"},
};
constexpr int kTaskCount = int(sizeof(kTasks) / sizeof(kTasks[0]));

const char* const kBonusId = "bonus";
// 全勤不是第 6 个任务，而是与真实任务同机制写进 done_ 的"伪任务 id"：
// 只在内部使用，不出现在 tasks() 给 QML 的列表里；持久化同样走 task_bonus 键。

} // namespace

// 每登录会话一个实例（app_bridge 构造）。phone 取 trim 后原值做分组键，
// 空号回退 "guest"：没手机号也能玩成长系统，但绝不与真实账号串档。
ProgressService::ProgressService(const QString& phone, QObject* parent)
    : QObject(parent)
    , groupKey_(QStringLiteral("progress/")
                + (phone.trimmed().isEmpty() ? QStringLiteral("guest") : phone.trimmed()))
{
    load();
}

// "今天"的逻辑单点：生产取系统日期；setTodayForTesting 注入的固定日优先。
// 全文件所有"当日"判定（幂等闸/跨天过滤/礼包日期）都收敛到这一句 ISO
// 字符串相等比较，测试缝只需拨这一个点即可断言跨天重置。
QString ProgressService::today() const
{
    return todayOverride_.isEmpty()
        ? QDate::currentDate().toString(Qt::ISODate) : todayOverride_;
}

// ---- 等级曲线读出（xp_ 的纯函数，全部由 xpChanged 驱动刷新）----

// 从低往高扫门槛、取最后一个达标档：表小（5 档）不做二分；
// 返回 1..5（对外口径 = kTiers 下标 +1，黑金恒为 5）。
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
    // 黑金顶层 span=0：除零守卫兼"进度条恒满"语义，QML 侧无需特判顶档。
    return span <= 0 ? 1.0 : double(xpIntoLevel()) / double(span);
}
QString ProgressService::nextTierName() const
{
    const int lv = level();
    return lv >= kTierCount ? QStringLiteral("已是最高等级")
                            : QString::fromUtf8(kTiers[lv].name);
}

// QML 遍历不了 C++ 数组：整表翻译为 QVariantList 每调现算（5 项，无缓存
// 必要）。state 三态（已达标/当前/未解锁）让 LevelPage 直接按值套样式，
// 不用在 QML 里重做门槛比较。
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

// ---- 每日任务状态（done_：taskId → 完成日字符串）----

// "今日已完成"就是存储值与 today() 的字符串相等——跨天重置不需要定时器
// 或清库动作：日期一变，昨天的记录自然判 false，任务自动回到未完成。
bool ProgressService::taskDoneToday(const QString& id) const
{
    return done_.value(id) == today();
}

// 只改内存不落盘：调用方 reportEvent 在一轮变更（含全勤奖励）全部记完后
// 统一 save() 一次，一次事件只付一次写盘钱。
void ProgressService::markTaskDone(const QString& id) { done_.insert(id, today()); }

// 扫的是静态任务表而非 done_ 的 size：done_ 里可能混着 bonus 键和盘上
// 读入的未知旧任务，按表数才等于"今日完成了几个正经任务"。
int ProgressService::doneTaskCount() const
{
    int n = 0;
    for (int i = 0; i < kTaskCount; ++i)
        if (taskDoneToday(QString::fromLatin1(kTasks[i].id))) ++n;
    return n;
}

// 全勤旗标直接读 done_ 里的伪任务键：与真实任务同一存储、同一幂等闸、
// 同一持久化路径，引擎里没有第二套"全勤状态"字段。
bool ProgressService::allTasksDone() const { return taskDoneToday(QLatin1String(kBonusId)); }

// 任务页渲染数据：静态定义 + 今日 done 态拼成 QVariantList（done 每次现算，
// 跨天后即使不重开页面、属性重读也会自然变 false）。action 供 QML 决定
// "去完成"跳转（station/stats）或特判 checkin 走签到链路。
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

// 升级礼包账目（旧→新）：LevelPage「升级记录」的数据源。这里只记录
// "升到某档送过多少积分"这笔本机账，不代替、也不影响服务端积分余额。
QVariantList ProgressService::gifts() const { return gifts_; }

// XP 的唯一漏斗：全系统成长只能从这五个已知事件 id 进来（埋点在
// app_bridge 各页面动作处，签到在 TaskSection 回执处），未知 id 直接丢弃，
// 保证"经验从哪来"可枚举、可审计。
// 顺序语义（答辩点）：
//   1) 幂等闸先查 taskDoneToday——当日重复上报不叠 XP 也不落盘；
//   2) 先 markTaskDone 再 award——若 award 的 levelUp 信号被同步处理器
//      重入调本函数，该任务已标记完成，不会再走一遍加分分支；
//   3) 全勤判定放在 award 之后——最后一条任务此刻已计入 doneTaskCount()，
//      恰好凑满即连奖励一起发；!allTasksDone() 前置防奖励重复入账；
//   4) save() 收尾一次落盘：本次事件引发的任务态/XP/礼包共享同一次写。
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

// 加 XP + 跨档结算。xpChanged 先发（页面立刻读到新 xp_/level）；
// 循环取 (oldLevel, newLevel] 而不是只发最高档：一次大额事件（全勤 +30 或
// 测试灌的批量 XP）可能连跨数档，逐档补发 levelUp 并入账礼包，一档不漏。
// 礼包"积分"只进 gifts_ 本地账（见文件头双账本口径），不碰积分服务。
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

// 盘→内存：仅构造时跑一次，之后以内存态为准（读接口不碰盘）。总账 xp_/gifts_
// 整体存取，每日任务态却平铺成 task_<id> 散键——存/读形状不对称，原因见
// 循环内的 QSettings 子组坑注释。
void ProgressService::load()
{
    QSettings settings;
    settings.beginGroup(groupKey_);
    // 键不存在时 value() 给空 QVariant、toLongLong() 得 0：全新用户天然
    // 青铜 0 起步，load 无需为"第一次登录"特判。
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

// 内存→盘：只由 reportEvent 在事件落定时调一次。除覆盖写外还要剪枝：
// done_ 载入时已按今天过滤，盘上昨天的 task_* 残键若不删会随日累积、
// 下次 load 又要多扫一遍。sync() 显式刷盘，保证同键新建实例（测试里
// 跨实例断言持久化效果）立即可见，不等 QSettings 析构时机。
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
