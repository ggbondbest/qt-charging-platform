#pragma once

#include "charging/common/model/models.h"
#include "charging/common/protocol/protocol.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

namespace charging::client::network {
class ClientConnection;
}

namespace charging::client::services::station {

// 站点列表条目：站点基础信息 + 距离（米）。距离由服务端提供；
// 真实接口未返回时置 -1（UI 显示“--”）。
struct StationListItem
{
    charging::model::Station station;
    int distanceMeters = -1;
};

using StationList = QVector<StationListItem>;

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

    bool isQueryPending() const;

    // 按关键字（站名/地址，不区分大小写）异步检索；空关键字返回全部。
    void search(const QString& keyword = QString());

    // 站点详情（任务 #12）：异步拉取指定站点的充电桩列表。station 由列表页
    // 路由携带（ID 非法/≤0 直接失败）；liveMode 下请求 GET_CHARGERS。
    void fetchDetail(const charging::model::Station& station, int distanceMeters);

signals:
    void queryStarted();
    void querySucceeded(const charging::client::services::station::StationList& stations);
    void queryFailed(const QString& message);
    void detailStarted();
    void detailSucceeded(const charging::client::services::station::StationDetail& detail);
    void detailFailed(const QString& message);

private:
    void handleResponse(const charging::protocol::ResponseEnvelope& response);
    void handleRequestFailure(const QString& requestId, const QString& errorCode,
                              const QString& message);
    void finishMockQuery();
    void finishMockDetail();

    charging::client::network::ClientConnection* connection_ = nullptr;
    bool liveMode_ = false;
    bool simulateFailure_ = false;
    QString pendingRequestId_;
    QString pendingDetailRequestId_;
    QString pendingKeyword_;
    StationDetail pendingDetail_;
};

} // namespace charging::client::services::station

Q_DECLARE_METATYPE(charging::client::services::station::StationList)
Q_DECLARE_METATYPE(charging::client::services::station::StationDetail)
