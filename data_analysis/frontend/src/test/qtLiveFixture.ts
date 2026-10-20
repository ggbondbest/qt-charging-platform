import type { QtLiveSnapshot } from '../qtLiveApi';

export function qtLiveEnvelope() {
  const chargerStatusCounts = { AVAILABLE: 1, RESERVED: 0, CHARGING: 1, FAULT: 0, OFFLINE: 0 };
  const data: QtLiveSnapshot = {
    dataSource: 'qt-sqlite', simulated: true, asOf: '2026-10-17T04:00:00Z',
    summary: { totalStations: 1, activeStations: 1, totalChargers: 2, chargerStatusCounts,
      totalOrders: 2, orderStatusCounts: { RESERVED: 0, CHARGING: 1, WAITING_PAYMENT: 0, COMPLETED: 1, CANCELLED: 0 }, chargingOrders: 1,
      completedOrders: 1, paidAmountCents: 1234, completedEnergyWh: 7890,
      totalUsers: 3, activeOrders: 1, onlineChargers: 2, onlineRatio: 1, todayOrders: 2,
      todayCompletedOrders: 1, todayRevenueCents: 1234, monthRevenueCents: 1234, todayCompletedEnergyWh: 7890 },
    stations: [{ qtStationId: 1, qtStationCode: 'STA-CITY-BJ-001', name: 'Qt 测试电站', city: '北京市', district: null,
      status: 'ACTIVE', priceCentsPerKwh: 120, totalChargers: 2, chargerStatusCounts }],
    orders: [{ qtOrderId: 1, qtStationId: 1, qtStationCode: 'STA-CITY-BJ-001', stationName: 'Qt 测试电站', chargerCode: 'C001',
      status: 'COMPLETED', amountCents: 1234, energyWh: 7890, createdAt: '2026-10-17T03:00:00Z',
      startedAt: null, stoppedAt: null, paidAt: '2026-10-17T04:00:00Z' }],
    overview: { timeZone: 'Asia/Shanghai', today: '2026-10-17', trendDays: 7,
      revenueTrend: Array.from({ length: 7 }, (_, index) => ({ date: `2026-10-${11 + index}`,
        revenueCents: index === 6 ? 1234 : 0, completedOrderCount: index === 6 ? 1 : 0,
        completedEnergyWh: index === 6 ? 7890 : 0 })),
      abnormalChargers: { total: 0, items: [] } },
    limits: { stations: 500, orders: 20 }, truncated: { stations: false, orders: false },
  };
  return { code: 'OK', data, meta: { dataSource: 'qt-sqlite', scope: 'CURRENT_QT_BUSINESS', mergedWithHistory: false, simulated: true } };
}
