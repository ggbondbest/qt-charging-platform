import type { Dataset } from './analytics';

export interface AdvisorCapabilities {
  defaultMode: 'offline'; onlineAvailable: boolean; onlineProvider: string | null;
  maxQuestionLength: number; disclosure: string;
  supportedQuestions: { id: string; label: string; question: string }[];
}
export type AdvisorTarget = 'overview' | 'advanced' | 'models' | 'anomalies';
export const advisorDestinations: Record<AdvisorTarget, string> = {
  overview: '查看运营总览', advanced: '查看多维运营分析', models: '查看负荷与空闲预测', anomalies: '查看充电异常筛查',
};
export function advisorTarget(value: unknown): value is AdvisorTarget {
  return typeof value === 'string' && Object.hasOwn(advisorDestinations, value);
}
export interface AdvisorRequest {
  question: string; datasetId: string; publishedBatchId: string;
  cityId?: string; startDate: string; endDate: string;
  mode: 'offline' | 'online'; consent: boolean;
}
export interface AdvisorResponse {
  status: 'answered' | 'unsupported' | 'no_evidence'; mode: 'offline' | 'online'; intent: string; answer: string;
  scope: { datasetId: string; publishedBatchId: string; startDate: string; endDate: string; timeZone: string; dataKind: string; cityId: string | null; stationId: string | null };
  evidence: { id: string; label: string; value: number | string | null; unit: string; source: { endpoint: string; field: string } }[];
  limitations: string[]; suggestions: { target: AdvisorTarget; label: string }[];
}
export function validateAdvisorResponse(data: AdvisorResponse, request: AdvisorRequest): AdvisorResponse {
  if (!data || !['answered', 'unsupported', 'no_evidence'].includes(data.status) || typeof data.answer !== 'string'
      || data.mode !== request.mode || !Array.isArray(data.evidence) || !Array.isArray(data.limitations) || !Array.isArray(data.suggestions)) {
    throw new Error('参谋响应不完整，请重试。');
  }
  if (!data.scope || ['datasetId', 'publishedBatchId', 'startDate', 'endDate'].some(key => data.scope[key as keyof typeof data.scope] !== request[key as keyof AdvisorRequest])
      || (data.scope.cityId || '') !== (request.cityId || '') || data.scope.stationId) {
    throw new Error('答复的数据批次或统计范围与问题不一致，请重新载入。');
  }
  if (data.status === 'answered' && !data.evidence.length) throw new Error('服务未返回支持答复的数据证据，请重试。');
  if (data.evidence.some(item => !item || typeof item.label !== 'string' || !item.source || typeof item.source.endpoint !== 'string' || typeof item.source.field !== 'string')) {
    throw new Error('答复缺少可核对的数据来源，请重试。');
  }
  return { ...data, suggestions: data.suggestions.filter(item => advisorTarget(item?.target)) };
}
export function advisorDatesValid(dataset: Dataset, start: string, end: string): boolean {
  return /^\d{4}-\d{2}-\d{2}$/.test(start) && /^\d{4}-\d{2}-\d{2}$/.test(end)
    && start >= dataset.startDate && end <= dataset.endDate && start < end;
}
