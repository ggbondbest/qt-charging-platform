// This API deliberately does not share ChargePilot's clock, session or model state.
export type ChargerCounts = Record<'AVAILABLE' | 'RESERVED' | 'CHARGING' | 'FAULT' | 'OFFLINE', number>;
export interface QtLiveSummary {
  totalStations: number;
  activeStations: number;
  totalChargers: number;
  chargerStatusCounts: ChargerCounts;
  totalOrders: number;
  orderStatusCounts: Record<string, number>;
  chargingOrders: number;
  completedOrders: number;
  paidAmountCents: number;
  completedEnergyWh: number;
  totalUsers: number;
  activeOrders: number;
  onlineChargers: number;
  onlineRatio: number;
  todayOrders: number;
  todayCompletedOrders: number;
  todayRevenueCents: number;
  monthRevenueCents: number;
  todayCompletedEnergyWh: number;
}
export interface QtLiveStation {
  qtStationId: number;
  qtStationCode: string;
  name: string;
  city: string | null;
  district: string | null;
  status: string;
  priceCentsPerKwh: number;
  totalChargers: number;
  chargerStatusCounts: ChargerCounts;
}
export interface QtLiveOrder {
  qtOrderId: number;
  qtStationId: number;
  qtStationCode: string;
  stationName: string;
  chargerCode: string;
  status: string;
  amountCents: number;
  energyWh: number;
  createdAt: string;
  startedAt: string | null;
  stoppedAt: string | null;
  paidAt: string | null;
}
export interface QtLiveSnapshot {
  dataSource: 'qt-sqlite';
  simulated: true;
  asOf: string;
  summary: QtLiveSummary;
  stations: QtLiveStation[];
  orders: QtLiveOrder[];
  overview: {
    timeZone: 'Asia/Shanghai';
    today: string;
    trendDays: 7 | 30;
    revenueTrend: { date: string; revenueCents: number; completedOrderCount: number; completedEnergyWh: number }[];
    abnormalChargers: { total: number; items: {
      qtChargerId: number; chargerCode: string; qtStationId: number; qtStationCode: string;
      stationName: string; status: string; updatedAt: string;
      activeException: { qtExceptionId: number; code: string; severity: string; status: string;
        safeSummary: string; occurredAt: string } | null;
    }[] };
  };
  limits: { stations: number; orders: number };
  truncated: { stations: boolean; orders: boolean };
}
export class QtLiveError extends Error {
  constructor(public code: string, message: string) { super(message); }
}

function record(value: unknown): value is Record<string, any> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}
const nonnegative = (value: unknown) => typeof value === 'number' && Number.isSafeInteger(value) && value >= 0;
const countsValid = (value: unknown) => record(value)
  && ['AVAILABLE', 'RESERVED', 'CHARGING', 'FAULT', 'OFFLINE'].every(key => nonnegative(value[key]));
const dateValid = (value: unknown): value is string => typeof value === 'string'
  && /^\d{4}-\d{2}-\d{2}$/.test(value) && Number.isFinite(Date.parse(value))
  && new Date(value).toISOString().slice(0, 10) === value;

// Reject a wrong data source or incomplete payload instead of displaying fabricated zeroes.
export function parseQtLiveSnapshot(value: unknown): QtLiveSnapshot {
  const fail = () => { throw new QtLiveError('INVALID_RESPONSE', 'Qt 业务快照格式不完整，请检查前后端版本。'); };
  if (!record(value) || !record(value.data) || !record(value.meta)) return fail();
  const data = value.data;
  if (data.dataSource !== 'qt-sqlite' || value.meta.dataSource !== 'qt-sqlite'
    || value.meta.scope !== 'CURRENT_QT_BUSINESS' || value.meta.mergedWithHistory !== false
    || data.simulated !== true || value.meta.simulated !== true
    || typeof data.asOf !== 'string' || !data.asOf.endsWith('Z') || !Number.isFinite(Date.parse(data.asOf))) return fail();
  const summary = data.summary;
  if (!record(summary) || !['totalStations', 'activeStations', 'totalChargers', 'totalOrders', 'chargingOrders',
    'completedOrders', 'paidAmountCents', 'completedEnergyWh', 'totalUsers', 'activeOrders', 'onlineChargers',
    'todayOrders', 'todayCompletedOrders', 'todayRevenueCents', 'monthRevenueCents', 'todayCompletedEnergyWh'].every(key => nonnegative(summary[key]))
    || typeof summary.onlineRatio !== 'number' || !Number.isFinite(summary.onlineRatio)
    || summary.onlineRatio < 0 || summary.onlineRatio > 1 || summary.onlineChargers > summary.totalChargers
    || !countsValid(summary.chargerStatusCounts) || !record(summary.orderStatusCounts)
    || !['RESERVED', 'CHARGING', 'WAITING_PAYMENT', 'COMPLETED', 'CANCELLED'].every(key => nonnegative(summary.orderStatusCounts[key]))) return fail();
  if (!Array.isArray(data.stations) || !data.stations.every(station => record(station)
    && nonnegative(station.qtStationId) && typeof station.qtStationCode === 'string' && typeof station.name === 'string'
    && typeof station.status === 'string' && nonnegative(station.priceCentsPerKwh)
    && nonnegative(station.totalChargers) && countsValid(station.chargerStatusCounts))) return fail();
  if (!Array.isArray(data.orders) || !data.orders.every(order => record(order)
    && nonnegative(order.qtOrderId) && nonnegative(order.qtStationId) && typeof order.qtStationCode === 'string'
    && typeof order.stationName === 'string' && typeof order.chargerCode === 'string' && typeof order.status === 'string'
    && typeof order.createdAt === 'string' && nonnegative(order.amountCents) && nonnegative(order.energyWh))) return fail();
  const overview = data.overview;
  if (!record(overview) || overview.timeZone !== 'Asia/Shanghai' || !dateValid(overview.today)
    || ![7, 30].includes(overview.trendDays) || !Array.isArray(overview.revenueTrend)
    || overview.revenueTrend.length !== overview.trendDays
    || !overview.revenueTrend.every((row: unknown, index: number) => record(row) && dateValid(row.date)
      && Date.parse(row.date) === Date.parse(overview.today) - (overview.trendDays - index - 1) * 86400000
      && ['revenueCents', 'completedOrderCount', 'completedEnergyWh'].every(key => nonnegative(row[key])))) return fail();
  const abnormal = overview.abnormalChargers;
  if (!record(abnormal) || !nonnegative(abnormal.total) || !Array.isArray(abnormal.items)
    || abnormal.items.length > 5 || abnormal.items.length > abnormal.total
    || !abnormal.items.every(item => record(item) && nonnegative(item.qtChargerId) && nonnegative(item.qtStationId)
      && ['chargerCode', 'qtStationCode', 'stationName', 'status', 'updatedAt'].every(key => typeof item[key] === 'string')
      && (item.activeException === null || (record(item.activeException) && nonnegative(item.activeException.qtExceptionId)
        && ['code', 'severity', 'status', 'safeSummary', 'occurredAt'].every(key => typeof item.activeException[key] === 'string'))))) return fail();
  if (!record(data.limits) || !nonnegative(data.limits.stations) || !nonnegative(data.limits.orders)
    || !record(data.truncated) || typeof data.truncated.stations !== 'boolean' || typeof data.truncated.orders !== 'boolean') return fail();
  return data as QtLiveSnapshot;
}

export async function fetchQtLiveSnapshot(signal?: AbortSignal, trendDays: 7 | 30 = 7): Promise<QtLiveSnapshot> {
  const controller = new AbortController();
  let timedOut = false;
  const cancel = () => controller.abort();
  if (signal?.aborted) throw new QtLiveError('CANCELLED', '刷新已暂停。');
  signal?.addEventListener('abort', cancel, { once: true });
  const timer = globalThis.setTimeout(() => { timedOut = true; controller.abort(); }, 6000);
  try {
    const response = await fetch(`/api/v1/qt-live/snapshot?orderLimit=20&stationLimit=500&trendDays=${trendDays}`, {
      signal: controller.signal, cache: 'no-store', headers: { Accept: 'application/json' },
    });
    let value: unknown;
    try { value = await response.json(); }
    catch { throw new QtLiveError('INVALID_RESPONSE', '服务未返回有效的 Qt 业务快照。'); }
    if (!response.ok || (record(value) && value.code !== 'OK')) {
      throw new QtLiveError(record(value) ? String(value.code || 'UNAVAILABLE') : 'UNAVAILABLE',
        record(value) && typeof value.message === 'string' ? value.message : 'Qt 业务服务暂不可用。');
    }
    return parseQtLiveSnapshot(value);
  } catch (error) {
    if (controller.signal.aborted) throw new QtLiveError(timedOut ? 'TIMEOUT' : 'CANCELLED', timedOut ? '读取 Qt 业务快照超时。' : '刷新已暂停。');
    if (error instanceof QtLiveError) throw error;
    throw new QtLiveError('NETWORK_ERROR', '无法连接 Qt 业务服务，请检查服务是否启动。');
  } finally {
    globalThis.clearTimeout(timer);
    signal?.removeEventListener('abort', cancel);
  }
}
