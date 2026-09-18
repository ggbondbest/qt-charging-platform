import { publishedRequest, shiftDate } from './analytics';
import type { Dataset } from './analytics';

export interface DashboardScope {
  datasetId: string; publishedBatchId: string; cityId: string | null;
  startDate: string; endDate: string;
}

/** Manual advisor navigation must keep its answer's batch and range. */
export async function loadDashboardScope(scope?: DashboardScope, signal?: AbortSignal) {
  const response = await publishedRequest<{ items: Dataset[] }>('/datasets', {}, { signal });
  const dataset = scope
    ? response.data.items?.find(item => item.datasetId === scope.datasetId && item.publishedBatchId === scope.publishedBatchId)
    : response.data.items?.[0];
  if (!dataset) throw new Error(scope ? '答复对应的发布批次已不可用，请返回运营参谋重新提问。' : '尚无已发布统计批次。请先完成数据清洗与发布。');
  if (!dataset.datasetId || !dataset.publishedBatchId) throw new Error('数据集缺少发布批次，请重新载入。');
  const filters = {
    datasetId: dataset.datasetId, publishedBatchId: dataset.publishedBatchId, cityId: scope?.cityId || '',
    startDate: scope?.startDate ?? (dataset.startDate > shiftDate(dataset.endDate, -7) ? dataset.startDate : shiftDate(dataset.endDate, -7)),
    endDate: scope?.endDate ?? dataset.endDate,
  };
  const validDate = (date: string) => /^\d{4}-\d{2}-\d{2}$/.test(date) && Number.isFinite(Date.parse(`${date}T00:00:00Z`)) && new Date(`${date}T00:00:00Z`).toISOString().slice(0, 10) === date;
  if (!validDate(filters.startDate) || !validDate(filters.endDate) || filters.startDate < dataset.startDate || filters.endDate > dataset.endDate || filters.startDate >= filters.endDate) {
    throw new Error('答复日期超出已发布数据范围，请返回运营参谋重新提问。');
  }
  const cityResponse = await publishedRequest<{ items: { cityId: string; cityName: string }[] }>('/cities', { datasetId: dataset.datasetId, publishedBatchId: dataset.publishedBatchId, pageSize: 100 }, { signal });
  const cities = cityResponse.data.items;
  if (filters.cityId && !cities.some(item => item.cityId === filters.cityId)) throw new Error('答复城市不在已发布数据范围内，请返回运营参谋重新提问。');
  return { dataset, cities, filters };
}
