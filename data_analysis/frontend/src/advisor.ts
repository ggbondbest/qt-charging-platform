import type { Dataset } from './analytics';

export interface AdvisorCapabilities {
  defaultMode: 'offline' | 'online'; onlineAvailable: boolean; onlineProvider: string | null;
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
  history?: { role: 'user' | 'assistant'; content: string }[];
}
export interface AdvisorKnowledge { id: string; title: string; text: string; source: string }
export interface AdvisorResponse {
  status: 'answered' | 'chat' | 'unsupported' | 'no_evidence'; mode: 'offline' | 'online'; intent: string; answer: string;
  scope: { datasetId: string; publishedBatchId: string; startDate: string; endDate: string; timeZone: string; dataKind: string; cityId: string | null; stationId: string | null };
  evidence: { id: string; label: string; value: number | string | null; unit: string; source: { endpoint: string; field: string } }[];
  citations?: string[]; knowledge?: AdvisorKnowledge[];
  limitations: string[]; suggestions: { target: AdvisorTarget; label: string }[];
}
export type ValidatedAdvisorResponse = AdvisorResponse & { citations: string[]; knowledge: AdvisorKnowledge[] };
export function validateAdvisorResponse(data: AdvisorResponse, request: AdvisorRequest): ValidatedAdvisorResponse {
  if (!data || !['answered', 'chat', 'unsupported', 'no_evidence'].includes(data.status) || typeof data.answer !== 'string' || !data.answer.trim()
      || data.mode !== request.mode || !Array.isArray(data.evidence) || !Array.isArray(data.limitations) || !Array.isArray(data.suggestions)) {
    throw new Error('参谋响应不完整，请重试。');
  }
  if (!data.scope || ['datasetId', 'publishedBatchId', 'startDate', 'endDate'].some(key => data.scope[key as keyof typeof data.scope] !== request[key as keyof AdvisorRequest])
      || (data.scope.cityId || '') !== (request.cityId || '') || data.scope.stationId) {
    throw new Error('答复的数据批次或统计范围与问题不一致，请重新载入。');
  }
  if (data.status === 'answered' && data.mode === 'offline' && !data.evidence.length) throw new Error('服务未返回支持答复的数据证据，请重试。');
  if (data.evidence.some(item => !item || typeof item.id !== 'string' || !item.id.trim() || typeof item.label !== 'string' || !item.source || typeof item.source.endpoint !== 'string' || typeof item.source.field !== 'string')) {
    throw new Error('答复缺少可核对的数据来源，请重试。');
  }
  const citations = data.citations === undefined ? [] : data.citations;
  const knowledge = data.knowledge === undefined ? [] : data.knowledge;
  if (!Array.isArray(knowledge) || knowledge.some(item => !item || ['id', 'title', 'text', 'source'].some(key => typeof item[key as keyof AdvisorKnowledge] !== 'string' || !item[key as keyof AdvisorKnowledge].trim()))) {
    throw new Error('答复的知识来源不完整，请重试。');
  }
  const sourceIds = new Set([...data.evidence, ...knowledge].map(item => item.id));
  if (!Array.isArray(citations) || citations.some(id => typeof id !== 'string' || !sourceIds.has(id))
      || (data.mode === 'online' && data.status === 'answered' && !citations.length)) {
    throw new Error('模型答复缺少有效引用，请重试或查看原始统计。');
  }
  return { ...data, citations, knowledge, suggestions: data.suggestions.filter(item => advisorTarget(item?.target)) };
}
export function advisorHistory(exchanges: { question: string; response: AdvisorResponse }[], request: AdvisorRequest): NonNullable<AdvisorRequest['history']> {
  const matching = exchanges.filter(({ response }) => {
    const scope = response.scope;
    return ['datasetId', 'publishedBatchId', 'startDate', 'endDate'].every(key => scope[key as keyof typeof scope] === request[key as keyof AdvisorRequest])
      && (scope.cityId || '') === (request.cityId || '') && !scope.stationId;
  }).slice(-3);
  let remaining = 4000;
  return matching.flatMap(exchange => [
    { role: 'user' as const, content: exchange.question }, { role: 'assistant' as const, content: exchange.response.answer },
  ]).map(message => {
    let content = message.content.slice(0, Math.min(800, remaining));
    // A UTF-16 limit may split an emoji; never send a trailing high surrogate.
    if (/[\uD800-\uDBFF]$/.test(content)) content = content.slice(0, -1);
    remaining -= content.length;
    return { ...message, content };
  }).filter(message => message.content.trim());
}
export function advisorDatesValid(dataset: Dataset, start: string, end: string): boolean {
  return /^\d{4}-\d{2}-\d{2}$/.test(start) && /^\d{4}-\d{2}-\d{2}$/.test(end)
    && start >= dataset.startDate && end <= dataset.endDate && start < end;
}
