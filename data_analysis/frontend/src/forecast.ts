import type { RecordData } from './analytics';
export function shanghaiInputToUtc(value: string): string {
  if (!/^\d{4}-\d{2}-\d{2}T\d{2}:00$/.test(value)) throw new Error('预测起点必须精确到整点。');
  const date = new Date(`${value}:00+08:00`);
  if (!Number.isFinite(date.getTime())) throw new Error('预测起点无效。');
  const roundTrip = new Date(date.getTime() + 8 * 3600000).toISOString().slice(0, 16);
  if (roundTrip !== value) throw new Error('预测起点不是有效日期。');
  return date.toISOString().replace('.000Z', 'Z');
}
export function utcToShanghaiInput(value: string): string {
  const date = new Date(value);
  return Number.isFinite(date.getTime()) ? new Date(date.getTime() + 8 * 3600000).toISOString().slice(0, 13) + ':00' : '';
}
export function modelForHorizon(capability: RecordData | undefined, horizon: number): RecordData | undefined {
  return capability?.models?.find((model: RecordData) => model.horizonHours == null || model.horizonHours === horizon || (Array.isArray(model.horizonHours) && model.horizonHours.includes(horizon))) ||
    (capability?.modelId && (!capability.horizonHours || capability.horizonHours === horizon || capability.horizonHours.includes?.(horizon)) ? capability : undefined);
}
export function forecastDisplayTimestamp(point: RecordData, target: string): string {
  if (target !== 'availability') return point.timestamp;
  return point.sampleTimestamp || new Date(Date.parse(point.timestamp) + 55 * 60000).toISOString();
}
