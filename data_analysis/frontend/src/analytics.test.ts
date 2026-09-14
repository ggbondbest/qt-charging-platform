import { afterEach, describe, expect, it, vi } from 'vitest';
import { aggregateCities, AnalyticsError, converted, fraction, publishedRequest, shiftDate, sumSnapshot } from './analytics';
import { forecastDisplayTimestamp, modelForHorizon, shanghaiInputToUtc, utcToShanghaiInput } from './forecast';
afterEach(() => { vi.unstubAllGlobals(); vi.useRealTimers(); });
describe('published analytics semantics', () => {
  it('converts cents, Wh and rates without turning missing values into zero', () => {
    expect(converted(123456, 100)).toBe(1234.56); expect(converted(71960400, 1000)).toBe(71960.4);
    expect(converted(null)).toBeNull(); expect(converted('123')).toBeNull(); expect(converted(Infinity)).toBeNull();
    expect(fraction(.687)).toBe('68.7%'); expect(fraction(null)).toBe('—'); expect(fraction(0)).toBe('0.0%');
  });
  it('keeps city totals unknown when any station energy is unknown', () => {
    const rows = aggregateCities([{ cityId: 'A', cityName: '甲', periodMetrics: { energyWh: 1000 } }, { cityId: 'A', cityName: '甲', periodMetrics: { energyWh: null } }, { cityId: 'B', cityName: '乙', periodMetrics: { energyWh: 2500 } }]);
    expect(rows[0].energyKwh).toBe(2.5); expect(rows[1].energyKwh).toBeNull(); expect(rows[1].stationCount).toBe(2);
  });
  it('does not merge occupied or unknown devices with available devices', () => {
    const states = sumSnapshot([{ availableCount: 2, chargingCount: 1, occupiedCount: 3, unknownCount: 4 }]);
    expect(states.find(item => item.name === '空闲')?.value).toBe(2); expect(states.find(item => item.name === '未知')?.value).toBe(4);
  });
  it('date arithmetic is UTC-calendar stable and Shanghai input converts explicitly', () => {
    expect(shiftDate('2026-03-01', -1)).toBe('2026-02-28');
    expect(shanghaiInputToUtc('2026-05-05T08:00')).toBe('2026-05-05T00:00:00Z');
    expect(utcToShanghaiInput('2026-05-05T00:00:00Z')).toBe('2026-05-05T08:00');
    expect(() => shanghaiInputToUtc('2026-05-05T08:30')).toThrow();
    expect(() => shanghaiInputToUtc('2026-02-30T08:00')).toThrow();
  });
  it('selects a registered model for the requested horizon, never fabricates an id', () => {
    const capability = { models: [{ modelId: 'one', horizonHours: 1 }, { modelId: 'six', horizonHours: 6 }] };
    expect(modelForHorizon(capability, 6)?.modelId).toBe('six'); expect(modelForHorizon(capability, 24)).toBeUndefined();
    expect(modelForHorizon({ models: [{ modelId: 'multi', horizonHours: [1, 6, 24] }] }, 24)?.modelId).toBe('multi');
  });
  it('labels hourly availability at hh:55, not the interval-start load timestamp', () => {
    const point = { timestamp: '2026-05-05T00:00:00Z', sampleTimestamp: '2026-05-05T00:55:00Z' };
    expect(forecastDisplayTimestamp(point, 'load')).toBe('2026-05-05T00:00:00Z');
    expect(forecastDisplayTimestamp(point, 'availability')).toBe('2026-05-05T00:55:00Z');
  });
});
describe('published API integrity', () => {
  function stub(data: unknown, meta: unknown = { datasetId: 'D', publishedBatchId: 'B' }) { const fetcher = vi.fn().mockResolvedValue(new Response(JSON.stringify({ code: 'OK', message: 'ok', data, meta }), { status: 200 })); vi.stubGlobal('fetch', fetcher); return fetcher; }
  it('pins city/date/batch on requests and rejects a mixed batch response', async () => {
    const fetcher = stub({ items: [] });
    await publishedRequest('/stations', { datasetId: 'D', publishedBatchId: 'B', cityId: 'DL', startDate: '2026-05-01', endDate: '2026-05-08' });
    const url = fetcher.mock.calls[0][0]; expect(url).toContain('cityId=DL'); expect(url).toContain('publishedBatchId=B'); expect(url).toContain('endDate=2026-05-08');
    stub({}, { datasetId: 'D', publishedBatchId: 'OTHER' });
    await expect(publishedRequest('/dashboard/overview', { datasetId: 'D', publishedBatchId: 'B' })).rejects.toMatchObject({ code: 'BATCH_MISMATCH' });
  });
  it('also pins a forecast request body', async () => {
    stub({}, { datasetId: 'D', publishedBatchId: 'OTHER' });
    await expect(publishedRequest('/intelligence/forecast', {}, { method: 'POST', body: { datasetId: 'D', publishedBatchId: 'B' } })).rejects.toBeInstanceOf(AnalyticsError);
  });
  it('rejects invalid or unavailable service without mock fallback', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('<html>wrong origin</html>')));
    await expect(publishedRequest('/datasets')).rejects.toMatchObject({ code: 'INVALID_RESPONSE' });
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify({ code: 'MODEL_NOT_READY', message: '模型未就绪', data: null }), { status: 503 })));
    await expect(publishedRequest('/intelligence/models')).rejects.toMatchObject({ code: 'MODEL_NOT_READY' });
  });
  it('supports caller cancellation for superseded filters', async () => {
    vi.stubGlobal('fetch', vi.fn((_url, options) => new Promise((_resolve, reject) => options.signal.addEventListener('abort', () => reject(new DOMException('aborted', 'AbortError'))))));
    const abort = new AbortController(); const request = publishedRequest('/stations', {}, { signal: abort.signal }); abort.abort();
    await expect(request).rejects.toMatchObject({ code: 'CANCELLED' });
  });
});
