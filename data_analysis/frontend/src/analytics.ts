/** Published-batch analytics. No synthetic fallback and no partial-batch rendering. */
export type RecordData = Record<string, any>;
export interface Dataset {
  datasetId: string; publishedBatchId: string; pipelineRunId: string;
  startDate: string; endDate: string; generatedAt: string; source: string;
}
export interface PublishedEnvelope<T> {
  code: string; message: string; data: T;
  meta: { datasetId?: string; publishedBatchId?: string; requestId?: string };
}
export class AnalyticsError extends Error {
  constructor(public code: string, message: string, public status = 0) { super(message); }
}
export async function publishedRequest<T>(path: string, params: RecordData = {}, options: { signal?: AbortSignal; method?: string; body?: unknown; timeoutMs?: number } = {}): Promise<PublishedEnvelope<T>> {
  const query = new URLSearchParams(Object.entries(params).filter(([, value]) => value !== '' && value != null).map(([key, value]) => [key, String(value)]));
  const controller = new AbortController();
  const abort = () => controller.abort();
  options.signal?.addEventListener('abort', abort, { once: true });
  if (options.signal?.aborted) abort();
  let expired = false;
  const timer = setTimeout(() => { expired = true; controller.abort(); }, options.timeoutMs ?? 60000);
  try {
    const response = await fetch(`/api/v1${path}${query.size ? `?${query}` : ''}`, {
      method: options.method || 'GET', signal: controller.signal,
      headers: { 'Content-Type': 'application/json' },
      ...(options.body === undefined ? {} : { body: JSON.stringify(options.body) }),
    });
    let envelope: PublishedEnvelope<T>;
    try { envelope = await response.json(); } catch { throw new AnalyticsError('INVALID_RESPONSE', '服务未返回有效 JSON，请检查 API 地址。', response.status); }
    if (!envelope || typeof envelope !== 'object' || !response.ok || envelope.code !== 'OK') {
      throw new AnalyticsError(envelope?.code || 'REQUEST_FAILED', envelope?.message || '数据服务暂不可用。', response.status);
    }
    if (envelope.data == null || !envelope.meta) throw new AnalyticsError('INVALID_RESPONSE', '数据响应不完整。', response.status);
    const pin = options.method === 'POST' ? options.body as RecordData : params;
    for (const key of ['datasetId', 'publishedBatchId'] as const) {
      if (pin?.[key] && envelope.meta[key] !== pin[key]) throw new AnalyticsError('BATCH_MISMATCH', '发布批次已变化，请重新载入数据集。', 409);
    }
    return envelope;
  } catch (error) {
    if (error instanceof AnalyticsError) throw error;
    if (controller.signal.aborted) throw new AnalyticsError(expired ? 'TIMEOUT' : 'CANCELLED', expired ? '数据请求超时，请重试。' : '请求已取消。');
    throw new AnalyticsError('NETWORK_ERROR', '无法连接数据服务，请确认后端已启动。');
  } finally { clearTimeout(timer); options.signal?.removeEventListener('abort', abort); }
}
export function converted(value: unknown, divisor = 1): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value / divisor : null;
}
export function formatValue(value: unknown, digits = 0): string {
  const numeric = converted(value);
  return numeric === null ? '—' : numeric.toLocaleString('zh-CN', { minimumFractionDigits: digits, maximumFractionDigits: digits });
}
export function fraction(value: unknown, digits = 1): string {
  const numeric = converted(value);
  return numeric === null ? '—' : `${formatValue(numeric * 100, digits)}%`;
}
export function shiftDate(date: string, days: number): string {
  const value = new Date(`${date}T00:00:00Z`); value.setUTCDate(value.getUTCDate() + days);
  return value.toISOString().slice(0, 10);
}
export function aggregateCities(stations: RecordData[]) {
  const groups = new Map<string, { name: string; cityId: string; energyKwh: number | null; stationCount: number }>();
  for (const station of stations) {
    const row = groups.get(station.cityId) || { name: station.cityName, cityId: station.cityId, energyKwh: 0, stationCount: 0 };
    const energy = converted(station.periodMetrics?.energyWh, 1000);
    row.energyKwh = row.energyKwh === null || energy === null ? null : row.energyKwh + energy;
    row.stationCount += 1; groups.set(station.cityId, row);
  }
  return [...groups.values()].sort((a, b) => (b.energyKwh ?? -1) - (a.energyKwh ?? -1));
}
/** A single-city scope compares its stations, using the same period as the KPIs. */
export function energyContribution(stations: RecordData[]) {
  const cities = aggregateCities(stations);
  const stationLevel = cities.length === 1;
  const rows = stationLevel
    ? stations.map(station => ({ id: station.stationId, name: station.stationName,
        energyKwh: converted(station.periodMetrics?.energyWh, 1000) }))
      .sort((a, b) => (b.energyKwh ?? -1) - (a.energyKwh ?? -1))
    : cities.map(city => ({ id: city.cityId, name: city.name, energyKwh: city.energyKwh }));
  return { stationLevel, title: stationLevel ? '电站电量贡献' : '城市电量贡献',
    unit: stationLevel ? '电站' : '城市', rows };
}
export function sumSnapshot(stations: RecordData[]) {
  return [ ['空闲', 'availableCount'], ['充电', 'chargingCount'], ['预约', 'reservedCount'], ['占位', 'occupiedCount'], ['维护', 'maintenanceCount'], ['离线', 'offlineCount'], ['未知', 'unknownCount'] ].map(([name, key]) => ({ name, value: stations.reduce((sum, station) => sum + (converted(station[key]) ?? 0), 0) }));
}
