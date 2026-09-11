#include "admin_mock_data.h"

#include <QObject>

namespace charging::server::admin_mock {

QVector<ChargerRecord> createChargerRecords()
{
    return {
        {QObject::tr("CP10010086"), QObject::tr("未来科技城充电站"), QObject::tr("直流桩"), QObject::tr("120kW"), QObject::tr("故障"), 0, 2312,
         QObject::tr("356h 22m"), QObject::tr("2025-06-01 09:58:00"), QObject::tr("充电枪通讯异常"), QObject::tr("2025-06-01 09:58:00")},
        {QObject::tr("CP10010123"), QObject::tr("滨江智慧园充电站"), QObject::tr("直流桩"), QObject::tr("60kW"), QObject::tr("故障"), 0, 1028,
         QObject::tr("180h 14m"), QObject::tr("2025-06-01 09:41:00"), QObject::tr("充电中断"), QObject::tr("2025-06-01 09:41:00")},
        {QObject::tr("CP10010205"), QObject::tr("城西银泰充电站"), QObject::tr("交流桩"), QObject::tr("7kW"), QObject::tr("故障"), 0, 648,
         QObject::tr("96h 05m"), QObject::tr("2025-06-01 09:22:00"), QObject::tr("过温保护"), QObject::tr("2025-06-01 09:22:00")},
        {QObject::tr("CP10010218"), QObject::tr("奥体中心充电站"), QObject::tr("直流桩"), QObject::tr("120kW"), QObject::tr("故障"), 0, 532,
         QObject::tr("88h 47m"), QObject::tr("2025-06-01 09:10:00"), QObject::tr("模块故障"), QObject::tr("2025-06-01 09:10:00")},
        {QObject::tr("CP10010267"), QObject::tr("萧山机场充电站"), QObject::tr("直流桩"), QObject::tr("180kW"), QObject::tr("离线"), 0, 212,
         QObject::tr("32h 06m"), QObject::tr("2025-06-01 08:55:00"), QObject::tr("设备离线"), QObject::tr("2025-06-01 08:55:00")},
        {QObject::tr("CP10010345"), QObject::tr("城西银泰充电站"), QObject::tr("交流桩"), QObject::tr("7kW"), QObject::tr("可用"), 6, 421,
         QObject::tr("62h 13m"), QObject::tr("2025-06-01 10:28:01"), {}, {}},
        {QObject::tr("CP10010378"), QObject::tr("未来科技城充电站"), QObject::tr("直流桩"), QObject::tr("120kW"), QObject::tr("充电中"), 18, 1857,
         QObject::tr("284h 36m"), QObject::tr("2025-06-01 10:27:55"), {}, {}},
        {QObject::tr("CP10010402"), QObject::tr("奥体中心充电站"), QObject::tr("交流桩"), QObject::tr("7kW"), QObject::tr("已预约"), 5, 309,
         QObject::tr("45h 21m"), QObject::tr("2025-06-01 10:27:41"), {}, {}},
        {QObject::tr("CP10010495"), QObject::tr("滨江智慧园充电站"), QObject::tr("直流桩"), QObject::tr("60kW"), QObject::tr("可用"), 9, 712,
         QObject::tr("103h 50m"), QObject::tr("2025-06-01 10:27:28"), {}, {}},
        {QObject::tr("CP10010533"), QObject::tr("富阳智造港充电站"), QObject::tr("交流桩"), QObject::tr("7kW"), QObject::tr("可用"), 4, 176,
         QObject::tr("24h 18m"), QObject::tr("2025-06-01 10:26:55"), {}, {}},
        {QObject::tr("CP10010614"), QObject::tr("未来科技城充电站"), QObject::tr("直流桩"), QObject::tr("120kW"), QObject::tr("可用"), 11, 855,
         QObject::tr("123h 04m"), QObject::tr("2025-06-01 10:26:42"), {}, {}},
        {QObject::tr("CP10010628"), QObject::tr("萧山机场充电站"), QObject::tr("直流桩"), QObject::tr("180kW"), QObject::tr("可用"), 14, 1124,
         QObject::tr("169h 32m"), QObject::tr("2025-06-01 10:26:08"), {}, {}},
    };
}

QVector<OrderRecord> createOrderRecords()
{
    using charging::model::OrderStatus;
    return {
        {QObject::tr("CP202506010001"), QObject::tr("张先生"), QObject::tr("138****5678"), QObject::tr("未来科技城充电站"), QObject::tr("CP10010086"), QObject::tr("直流桩"), OrderStatus::WaitingPayment, QObject::tr("2025-06-01 10:28:45"), QObject::tr("36分22秒"), 24160, 3462, 692, 0, QObject::tr("微信支付"), QObject::tr("待支付")},
        {QObject::tr("CP202506010002"), QObject::tr("李女士"), QObject::tr("159****8899"), QObject::tr("滨江智慧园充电站"), QObject::tr("CP10010123"), QObject::tr("直流桩"), OrderStatus::Completed, QObject::tr("2025-06-01 09:56:13"), QObject::tr("1时48分"), 38240, 5478, 1096, 0, QObject::tr("支付宝"), QObject::tr("已支付")},
        {QObject::tr("CP202506010003"), QObject::tr("王先生"), QObject::tr("137****1122"), QObject::tr("城西银泰充电站"), QObject::tr("CP10010205"), QObject::tr("交流桩"), OrderStatus::Completed, QObject::tr("2025-06-01 09:31:17"), QObject::tr("56分05秒"), 23580, 3186, 637, 0, QObject::tr("微信支付"), QObject::tr("已支付")},
        {QObject::tr("CP202506010004"), QObject::tr("陈女士"), QObject::tr("186****3344"), QObject::tr("奥体中心充电站"), QObject::tr("CP10010218"), QObject::tr("直流桩"), OrderStatus::WaitingPayment, QObject::tr("2025-06-01 08:47:25"), QObject::tr("28分47秒"), 16720, 2257, 451, 0, QObject::tr("—"), QObject::tr("待支付")},
        {QObject::tr("CP202506010005"), QObject::tr("刘先生"), QObject::tr("152****7788"), QObject::tr("萧山机场充电站"), QObject::tr("CP10010267"), QObject::tr("直流桩"), OrderStatus::Completed, QObject::tr("2025-06-01 07:55:41"), QObject::tr("32分06秒"), 18340, 2474, 495, 0, QObject::tr("支付宝"), QObject::tr("已支付")},
        {QObject::tr("CP202506010006"), QObject::tr("赵先生"), QObject::tr("139****9900"), QObject::tr("城西银泰充电站"), QObject::tr("CP10010345"), QObject::tr("交流桩"), OrderStatus::Completed, QObject::tr("2025-06-01 07:12:30"), QObject::tr("1时38分"), 32610, 4402, 880, 0, QObject::tr("微信支付"), QObject::tr("已支付")},
        {QObject::tr("CP202506010007"), QObject::tr("吴女士"), QObject::tr("158****2211"), QObject::tr("未来科技城充电站"), QObject::tr("CP10010378"), QObject::tr("直流桩"), OrderStatus::Cancelled, QObject::tr("2025-06-01 06:41:18"), QObject::tr("24分36秒"), 12480, 1685, 337, 2022, QObject::tr("支付宝"), QObject::tr("已取消")},
        {QObject::tr("CP202506010008"), QObject::tr("孙先生"), QObject::tr("187****4455"), QObject::tr("奥体中心充电站"), QObject::tr("CP10010402"), QObject::tr("交流桩"), OrderStatus::Completed, QObject::tr("2025-06-01 06:15:56"), QObject::tr("45分12秒"), 20130, 2718, 544, 0, QObject::tr("微信支付"), QObject::tr("已支付")},
        {QObject::tr("CP202506010009"), QObject::tr("周女士"), QObject::tr("150****6677"), QObject::tr("滨江智慧园充电站"), QObject::tr("CP10010495"), QObject::tr("直流桩"), OrderStatus::Completed, QObject::tr("2025-06-01 05:30:22"), QObject::tr("1时03分"), 28330, 3825, 765, 0, QObject::tr("支付宝"), QObject::tr("已支付")},
        {QObject::tr("CP202506010010"), QObject::tr("黄先生"), QObject::tr("188****5566"), QObject::tr("富阳智造港充电站"), QObject::tr("CP10010533"), QObject::tr("交流桩"), OrderStatus::WaitingPayment, QObject::tr("2025-06-01 05:05:11"), QObject::tr("24分18秒"), 10570, 1428, 285, 0, QObject::tr("—"), QObject::tr("待支付")},
        {QObject::tr("CP202505310011"), QObject::tr("杨女士"), QObject::tr("136****3456"), QObject::tr("未来科技城充电站"), QObject::tr("CP10010086"), QObject::tr("直流桩"), OrderStatus::Completed, QObject::tr("2025-05-31 23:42:11"), QObject::tr("54分36秒"), 26800, 3816, 763, 0, QObject::tr("微信支付"), QObject::tr("已支付")},
        {QObject::tr("CP202505310012"), QObject::tr("何先生"), QObject::tr("131****8024"), QObject::tr("滨江智慧园充电站"), QObject::tr("CP10010123"), QObject::tr("直流桩"), OrderStatus::Completed, QObject::tr("2025-05-31 22:17:40"), QObject::tr("48分10秒"), 21440, 3002, 600, 0, QObject::tr("支付宝"), QObject::tr("已支付")},
    };
}

} // namespace charging::server::admin_mock
