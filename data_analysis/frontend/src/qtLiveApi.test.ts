import { afterEach, describe, expect, it, vi } from 'vitest';
import { fetchQtLiveSnapshot, parseQtLiveSnapshot } from './qtLiveApi';
import { qtLiveEnvelope } from './test/qtLiveFixture';

afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });
describe('Qt live snapshot contract', () => {
  it('preserves cents and Wh and only accepts the Qt business data boundary', () => {
    const value = qtLiveEnvelope();
    expect(parseQtLiveSnapshot(value).summary).toMatchObject({ paidAmountCents: 1234, completedEnergyWh: 7890 });
    value.meta.mergedWithHistory = true;
    expect(() => parseQtLiveSnapshot(value)).toThrow(/格式不完整/);
    expect(() => parseQtLiveSnapshot({ data: { ...value.data, dataSource: 'mysql' }, meta: value.meta })).toThrow();
  });
  it.each(['chargingOrders', 'paidAmountCents', 'completedEnergyWh', 'todayRevenueCents', 'monthRevenueCents', 'totalUsers', 'onlineChargers'])('does not turn missing or invalid %s into zero', key => {
    for (const bad of [undefined, null, '0', -1, NaN]) {
      const value = qtLiveEnvelope();
      (value.data.summary as unknown as Record<string, unknown>)[key] = bad;
      expect(() => parseQtLiveSnapshot(value)).toThrow();
    }
  });
  it('requires the simulated source declaration and all order status counters', () => {
    const value = qtLiveEnvelope();
    value.meta.simulated = false;
    expect(() => parseQtLiveSnapshot(value)).toThrow();
    value.meta.simulated = true;
    value.data.summary.orderStatusCounts = {};
    expect(() => parseQtLiveSnapshot(value)).toThrow();
  });
  it('requests one no-cache snapshot without bootstrap or a history session', async () => {
    const fetch = vi.fn().mockResolvedValue({ ok: true, json: async () => qtLiveEnvelope() });
    vi.stubGlobal('fetch', fetch);
    await expect(fetchQtLiveSnapshot()).resolves.toMatchObject({ dataSource: 'qt-sqlite' });
    expect(fetch).toHaveBeenCalledTimes(1);
    expect(fetch).toHaveBeenCalledWith('/api/v1/qt-live/snapshot?orderLimit=20&stationLimit=500&trendDays=7', expect.objectContaining({ cache: 'no-store' }));
    await fetchQtLiveSnapshot(undefined, 30);
    expect(fetch).toHaveBeenLastCalledWith('/api/v1/qt-live/snapshot?orderLimit=20&stationLimit=500&trendDays=30', expect.anything());
  });
  it('rejects wrong timezone, missing trend days, reordered dates, and invalid online ratios', () => {
    for (const mutate of [
      (value: any) => { value.data.overview.timeZone = 'UTC'; },
      (value: any) => { value.data.overview.revenueTrend.pop(); },
      (value: any) => { value.data.overview.revenueTrend.reverse(); },
      (value: any) => { value.data.summary.onlineRatio = 1.1; },
      (value: any) => { value.data.summary.onlineRatio = NaN; },
      (value: any) => { value.data.overview.revenueTrend[0].revenueCents = -1; },
    ]) { const value = qtLiveEnvelope(); mutate(value); expect(() => parseQtLiveSnapshot(value)).toThrow(); }
  });
  it('requires the abnormal-device summary and bounded safe event fields', () => {
    const value = qtLiveEnvelope();
    (value.data.overview as any).abnormalChargers = { total: 1, items: [{ qtChargerId: 1 }] };
    expect(() => parseQtLiveSnapshot(value)).toThrow();
  });
  it('reports adapter failure and malformed HTML as unavailability', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: false, json: async () => ({ code: 'QT_DATABASE_BUSY', message: '业务库忙，请稍后重试。', data: null }) }));
    await expect(fetchQtLiveSnapshot()).rejects.toMatchObject({ code: 'QT_DATABASE_BUSY' });
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: true, json: async () => { throw new Error('HTML'); } }));
    await expect(fetchQtLiveSnapshot()).rejects.toMatchObject({ code: 'INVALID_RESPONSE' });
  });
  it('aborts slow fetches after 6 seconds and distinguishes caller cancellation', async () => {
    vi.useFakeTimers();
    vi.stubGlobal('fetch', vi.fn((_url, { signal }: RequestInit) => new Promise((_resolve, reject) => {
      signal!.addEventListener('abort', () => reject(new DOMException('aborted', 'AbortError')));
    })));
    const slow = expect(fetchQtLiveSnapshot()).rejects.toMatchObject({ code: 'TIMEOUT' });
    await vi.advanceTimersByTimeAsync(6000);
    await slow;
    const controller = new AbortController();
    const cancelled = expect(fetchQtLiveSnapshot(controller.signal)).rejects.toMatchObject({ code: 'CANCELLED' });
    controller.abort();
    await cancelled;
    expect(vi.getTimerCount()).toBe(0);
  });
});
