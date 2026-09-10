// StationQueryService 接口（成员 2，找站·任务 #7；含任务 #12 详情、
// 任务 #17 模拟“已预约”覆盖、迭代 3 高级筛选）。
// 职责：站点列表关键字检索 + 站点充电桩详情异步拉取，双通道结果同构；
// 另暴露高级筛选的纯客户端投影 applyStationFilter() 与规范选项字面量
// （station_filter 命名空间——弹窗选项/模拟数据取值/过滤匹配三处共用）。
// 使用方：widgets HomeShell（找站主页/详情页；筛选弹窗 station_filter_dialog
// 直接复用选项函数）与 QML app_bridge（StationQueryBridge → StationHomePage
// 等 QML 页面）。
// 数据流向：页面 → search()/fetchDetail() → 模拟通道（内置演示数据 +
// 延迟）或真实通道 TCP 契约 GET_STATIONS / GET_CHARGERS（ClientConnection）；
// 结果经 querySucceeded / detailSucceeded 同形状回页面，排序与筛选投影
// 由页面对最近一次结果即时应用、不重发请求。
#pragma once

#include "charging/common/model/models.h"
#include "charging/common/protocol/protocol.h"

#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

namespace charging::client::network {
class ClientConnection;
}

namespace charging::client::services::station {

// 迭代 3 · 高级筛选的 8 组条件规范选项：弹窗选项、模拟数据取值、过滤匹配
// 三处共用同一字面量（字符串即匹配键，避免魔法值漂移）。
// 实现形态：每个函数返回“函数局部 static QStringList”的引用——字面量表
// 只构建一次、取用零拷贝；inline 保证全工程共享同一实例，弹窗/数据/匹配
// 三处各取所引用而不可漂移（7 组多选选项 + 1 组距离档位共 8 组）。
namespace station_filter {

inline const QStringList& operatorOptions()
{
    static const QStringList value{QStringLiteral("自营"), QStringLiteral("合作站"),
                                   QStringLiteral("互联互通"), QStringLiteral("个人桩")};
    return value;
}
inline const QStringList& operatingStatusOptions()
{
    static const QStringList value{QStringLiteral("营业中"), QStringLiteral("暂停运营")};
    return value;
}
inline const QStringList& accessTypeOptions()
{
    static const QStringList value{QStringLiteral("对外"), QStringLiteral("不对外开放")};
    return value;
}
inline const QStringList& parkingFeeOptions()
{
    static const QStringList value{QStringLiteral("免费"), QStringLiteral("限时免费"),
                                   QStringLiteral("停车减免"), QStringLiteral("收费")};
    return value;
}
inline const QStringList& featureOptions()
{
    static const QStringList value{QStringLiteral("重卡"), QStringLiteral("即插即充"),
                                   QStringLiteral("CPU即插即充"), QStringLiteral("有序充电"),
                                   QStringLiteral("V2G")};
    return value;
}
inline const QStringList& chargerTypeOptions()
{
    static const QStringList value{QStringLiteral("超充"), QStringLiteral("快充"),
                                   QStringLiteral("慢充")};
    return value;
}
inline const QStringList& voltageBandOptions()
{
    static const QStringList value{QStringLiteral("低于700V"), QStringLiteral("700V及以上")};
    return value;
}
// 距离单选档位（公里；0=不限由 criteria.maxDistanceKm 表达）。
inline constexpr int kDistanceOptionsKm[4] = {5, 10, 30, 50};

} // namespace station_filter

// 迭代 3 · 高级筛选条件（客户端 DTO，不改 common/）：各组空 = 不限制；
// 组内多选取并集（OR），组间交集（AND）。距离组单选（0=不限）。
struct StationFilterCriteria
{
    int maxDistanceKm = 0;          // 5/10/30/50 或 0=不限
    QStringList statuses;           // 营业中 / 暂停运营
    QStringList operators;          // 运营商
    QStringList accessTypes;        // 对外 / 不对外开放
    QStringList parkingFees;        // 停车费类型
    QStringList features;           // 特色功能
    QStringList chargerTypes;       // 充电桩类型
    QStringList voltageBands;       // 电压档位

    // 全空 = 投影直通（页面据此短路，省一次全表扫描）。
    bool isEmpty() const
    {
        return maxDistanceKm <= 0 && statuses.isEmpty() && operators.isEmpty()
            && accessTypes.isEmpty() && parkingFees.isEmpty() && features.isEmpty()
            && chargerTypes.isEmpty() && voltageBands.isEmpty();
    }
};

// 站点列表条目：站点基础信息 + 距离（米）。距离由服务端提供；
// 真实接口未返回时置 -1（UI 显示“--”）。
//
// 迭代 3 筛选属性（运营状态复用 station.status，不新增字段）为客户端模拟
// 扩展：GET_STATIONS 响应尚无这些字段（TODO(contract)：服务端补充
// operator/accessType/parkingFee/features/chargerTypes/voltageBands 后，
// 真实通道解析处直接赋值即可，过滤逻辑零改动）。
struct StationListItem
{
    charging::model::Station station;
    int distanceMeters = -1;
    QString operatorName;      // station_filter::operatorOptions() 之一
    QString accessType;        // 对外 / 不对外开放
    QString parkingFee;        // 停车费类型
    QStringList features;      // 特色功能（可多个）
    QStringList chargerTypes;  // 站内充电桩类型集合（超充/快充/慢充）
    // 两个 bool 可同时成立：一站内既可并存低/高压桩，电压多选按“任一
    // 满足即命中”，单值枚举表达不了这种存在性语义。
    bool hasVoltageBelow700 = true;   // 站内存在电压 <700V 的桩
    bool hasVoltageAtLeast700 = false; // 站内存在电压 ≥700V 的桩
};

using StationList = QVector<StationListItem>;

// 迭代 3：把筛选条件应用到给定结果集（稳定顺序），返回子集。与排序/电价
// 筛选同为纯客户端投影——由页面在 lastResults_ 上即时应用，不重发请求。
// 距离口径：真实通道 distanceMeters=-1（服务端未给距离）时无法证明在圈内，
// 距离档位激活时不展示；模拟通道恒有距离，不受影响。
StationList applyStationFilter(const StationList& results,
                               const StationFilterCriteria& criteria);

// 站点详情（任务 #12）：站点信息 + 距离 + 该站点下全部充电桩。
struct StationDetail
{
    charging::model::Station station;
    int distanceMeters = -1;
    QVector<charging::model::Charger> chargers;
    bool hasChargerData = false; // 服务端是否返回了桩列表（区分空与缺数据）
};

// 站点查询服务（成员 2，任务 #7）。
//
// 双通道设计，页面 UI 逻辑对二者无感知：
// - 模拟通道（当前默认）：站点查询接口（服务端 GET_STATIONS）尚未实现，
//   返回内置演示数据，带模拟网络延迟以驱动“加载中”状态；
// - 真实通道：接口就绪后 setConnection() + setLiveMode(true) 即无缝切换到
//   GET_STATIONS 请求，结果解析为同一 StationList 信号，页面代码零改动。
//
// 排序（空闲优先/距离最近）与电价筛选是纯客户端投影，由页面对最近一次
// querySucceeded 结果即时应用，不重复请求。
class StationQueryService final : public QObject
{
    Q_OBJECT

public:
    explicit StationQueryService(QObject* parent = nullptr);

    // 真实通道注入：连接对象 + 开关（默认关，保持模拟数据渲染）。
    void setConnection(charging::client::network::ClientConnection* connection);
    void setLiveMode(bool enabled);
    bool liveMode() const;

    // 演示/测试异常分支：置 true 后下一次查询直接走 queryFailed。
    void setSimulateFailure(bool simulate);

    // 在途标志：真实通道列表请求未回时 search 防重入即以此判定；
    // 页面可据此显示骨架/禁用下拉刷新。
    bool isQueryPending() const;

    // 按关键字（站名/地址，不区分大小写）异步检索；空关键字返回全部。
    void search(const QString& keyword = QString());

    // 站点详情（任务 #12）：异步拉取指定站点的充电桩列表。station 由列表页
    // 路由携带（ID 非法/≤0 直接失败）；liveMode 下请求 GET_CHARGERS。
    void fetchDetail(const charging::model::Station& station, int distanceMeters);

    // 任务 #17：模拟通道把某桩标记为“已预约”（预约成功后详情页刷新用）。
    // 仅作用于模拟数据；真实通道以服务端状态为准。
    void setMockChargerReserved(qint64 chargerId);

    // ---- 信号面：列表与详情各一组 started/succeeded/failed，
    // 模拟与真实通道共用同一形状，页面无感 ----
signals:
    void queryStarted();
    void querySucceeded(const charging::client::services::station::StationList& stations);
    void queryFailed(const QString& message);
    void detailStarted();
    void detailSucceeded(const charging::client::services::station::StationDetail& detail);
    void detailFailed(const QString& message);

private:
    // ---- 真实通道响应路由 + 模拟延迟完成入口（与 ReservationService 同构）----
    void handleResponse(const charging::protocol::ResponseEnvelope& response);
    void handleRequestFailure(const QString& requestId, const QString& errorCode,
                              const QString& message);
    void finishMockQuery();
    void finishMockDetail();

    charging::client::network::ClientConnection* connection_ = nullptr;
    bool liveMode_ = false;
    bool simulateFailure_ = false;
    // ---- 在途配对键与上下文：列表/详情各一条线互不串扰；
    // pendingKeyword_ 供真实通道翻页续发与模拟完成复用（续页须带原词），
    // pendingDetail_ 在分页期间持续累加充电桩列表 ----
    QString pendingRequestId_;
    QString pendingDetailRequestId_;
    QString pendingKeyword_;
    StationDetail pendingDetail_;
    // ---- 分页累加缓冲（收齐前不外发中间态）----
    StationList accumulatedStations_;
    int stationPage_ = 1;
    int chargerPage_ = 1;
    // 任务 #17：“已预约”桩状态覆盖表（chargerId→status）。模拟桩数据每次
    // 调用现生成、改了即丢，预约成功只能记在这张表、详情完成时应用并重算空位。
    QHash<qint64, charging::model::ChargerStatus> mockChargerOverrides_;
};

} // namespace charging::client::services::station

// 排队（跨线程）连接携带信号参数所需，与构造函数里的 qRegisterMetaType 成对。
Q_DECLARE_METATYPE(charging::client::services::station::StationList)
Q_DECLARE_METATYPE(charging::client::services::station::StationDetail)
