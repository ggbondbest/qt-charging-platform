import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { loadDashboardScope } from './dashboardScope';

const dataset = { datasetId: 'D1', publishedBatchId: 'B1', startDate: '2026-01-01', endDate: '2026-06-01' };
const scope = { datasetId: 'D1', publishedBatchId: 'B1', cityId: 'DL', startDate: '2026-05-01', endDate: '2026-05-08' };
let datasets: typeof dataset[];
let cityBatch: string;
const fetcher = vi.fn();
beforeEach(() => {
  datasets = [{ ...dataset, datasetId: 'newer', publishedBatchId: 'B2' }, dataset]; cityBatch = 'B1';
  fetcher.mockReset();
  fetcher.mockImplementation((url: string) => Promise.resolve(new Response(JSON.stringify({ code: 'OK', message: 'ok',
    data: url === '/api/v1/datasets' ? { items: datasets } : { items: [{ cityId: 'DL', cityName: '大连市' }] },
    meta: url === '/api/v1/datasets' ? {} : { datasetId: 'D1', publishedBatchId: cityBatch },
  }))));
  vi.stubGlobal('fetch', fetcher);
});
afterEach(() => vi.unstubAllGlobals());

describe('advisor dashboard scope handoff', () => {
  it('loads the exact answer batch, city and historical dates even when another dataset is listed first', async () => {
    const initial = await loadDashboardScope(scope);
    expect(initial.dataset).toEqual(dataset); expect(initial.filters).toEqual(scope);
    expect(fetcher.mock.calls[1][0]).toContain('datasetId=D1&publishedBatchId=B1');
  });
  it('keeps an all-city answer all-city and uses recent dates only for ordinary navigation', async () => {
    expect((await loadDashboardScope({ ...scope, cityId: null })).filters).toEqual({ ...scope, cityId: '' });
    datasets = [dataset];
    expect((await loadDashboardScope()).filters).toEqual({ ...scope, cityId: '', startDate: '2026-05-25', endDate: '2026-06-01' });
  });
  it('fails closed when the answer batch has been replaced without querying the replacement', async () => {
    datasets = [{ ...dataset, publishedBatchId: 'B2' }];
    await expect(loadDashboardScope(scope)).rejects.toThrow('发布批次已不可用');
    expect(fetcher).toHaveBeenCalledTimes(1);
  });
  it.each([
    { startDate: '2025-01-01' }, { endDate: '2027-01-01' }, { startDate: scope.endDate }, { startDate: '2026-02-30' },
  ])('rejects an invalid answer date scope %j before fetching evidence', async dates => {
    await expect(loadDashboardScope({ ...scope, ...dates })).rejects.toThrow('日期超出');
    expect(fetcher).toHaveBeenCalledTimes(1);
  });
  it('rejects a city outside the pinned dataset', async () => {
    await expect(loadDashboardScope({ ...scope, cityId: 'unknown' })).rejects.toThrow('答复城市');
  });
  it('rejects changed publication metadata during city discovery', async () => {
    cityBatch = 'B2';
    await expect(loadDashboardScope(scope)).rejects.toMatchObject({ code: 'BATCH_MISMATCH' });
  });
});
