import type { EChartsCoreOption } from 'echarts/core';
import { converted, formatValue, fraction } from './analytics';
import type { RechargeBehavior, ServiceBottlenecks, ServiceCell, FailureReason } from '../../contracts/types';
export type { RechargeBehavior, ServiceBottlenecks, ServiceCell, FailureReason } from '../../contracts/types';

export interface AdvancedFilters {
  datasetId: string; publishedBatchId: string; cityId?: string; stationId?: string;
  startDate: string; endDate: string;
}
export interface HeatCell { weekday: number; hour: number; energyKwh: number | null; meanPowerKw: number | null; chargingUtilization: number | null; sampleHours: number }
export interface StationPoint {
  stationId: string; stationName: string; cityId: string; cityName: string; siteType: string;
  energyKwh: number | null; chargingUtilization: number | null; meanWaitMinutes: number | null;
  overstayShare: number | null; netCashYuan: number; sessionCount: number; attemptCount: number; successRate: number | null;
}
export interface WeatherCell { temperatureBin: number; hour: number; chargingUtilization: number | null; meanPowerKw: number | null; sampleHours: number }
export interface Cohort { month: string; size: number; cells: { offset: number; users: number | null; rate: number | null }[] }
export interface Segment { siteType: string; userSegment: string; batteryCapacityBand: string; connectorType: string; sessionCount: number; energyKwh: number; meanChargeMinutes: number | null; meanOverstayMinutes: number | null }
export interface Correlation { x: string; y: string; value: number | null; n: number }
export interface AdvancedData {
  provenance: { sourceLabel: string; analysisId: string; engine: string; generatedAt: string; completeMonthsThrough?: string; notes: string[] };
  scope: { startDate: string; endDate: string; cityId: string | null; stationId: string | null; siteType: string | null };
  stationTypes: string[];
  summary: { stationCount: number; sessionCount: number; attemptCount: number; observedStationHours: number; completeStationHours: number };
  heatmap: HeatCell[]; stations: StationPoint[]; weather: WeatherCell[];
  flow: { attemptCount: number; nodes: { name: string }[]; links: { source: string; target: string; value: number }[] };
  behavior: RechargeBehavior; service: ServiceBottlenecks;
  retention: { scopeLabel: string; definition: string; observationEnd: string; cohorts: Cohort[] };
  segments: Segment[]; correlations: Correlation[]; insights: string[];
}

export const advancedColors = ['#2563eb', '#149ba2', '#7c63c4', '#ce874c', '#344862', '#dc6a7c', '#89a561'];
export const weekdays = ['周一', '周二', '周三', '周四', '周五', '周六', '周日'];
export const heatMetrics = [
  { id: 'chargingUtilization', label: '充电利用率', unit: '%', multiplier: 100 },
  { id: 'energyKwh', label: '充电量', unit: 'kWh', multiplier: 1 },
  { id: 'meanPowerKw', label: '平均功率', unit: 'kW', multiplier: 1 },
] as const;
export type HeatMetric = typeof heatMetrics[number]['id'];
export type BehaviorMetric = 'intervals' | 'energy';
export const behaviorBuckets = {
  intervals: [ { id: 'LT1D', label: '<1 天' }, { id: '1_TO_3D', label: '1–3 天' }, { id: '3_TO_7D', label: '3–7 天' }, { id: '7_TO_14D', label: '7–14 天' }, { id: 'GE14D', label: '≥14 天' } ],
  energy: [ { id: 'LT10', label: '<10 kWh' }, { id: '10_TO_20', label: '10–20 kWh' }, { id: '20_TO_40', label: '20–40 kWh' }, { id: 'GE40', label: '≥40 kWh' } ],
};
export function behaviorBucketLabel(bucket: string, metric: BehaviorMetric): string { return behaviorBuckets[metric].find(item => item.id === bucket)?.label || bucket; }
const labels: Record<string, string> = {
  office: '办公园区', workplace: '工作场所', commercial: '商业中心', shopping: '商业中心', residential: '社区', community: '社区',
  transport: '交通枢纽', highway: '高速服务区', transit: '交通枢纽', campus: '校园', university: '大学城', industrial: '产业园',
  commuter: '通勤用户', private: '私家用户', family: '家庭用户', ride_hailing: '网约车', taxi: '出租车', fleet: '营运车队', occasional: '偶尔使用',
  compact: '小型车', sedan: '轿车', suv: 'SUV', van: '厢式车', bus: '客车', ac: '交流 AC', dc: '直流 DC',
  LT50: '<50 kWh', '50_TO_69': '50–69 kWh', GE70: '≥70 kWh', UNKNOWN: '容量未知',
  temperature: '气温', temperatureC: '气温', temperature_c: '气温', energyKwh: '电量', energy_kwh: '电量',
  chargingUtilization: '利用率', charging_utilization: '利用率', utilization: '利用率',
  meanWaitMinutes: '排队等待', wait_minutes: '排队等待', queue_wait_minutes: '排队等待',
  netCashYuan: '净收款', net_cash_yuan: '净收款', meanPowerKw: '平均功率', mean_power_kw: '平均功率',
  overstayShare: '充后占位', overstay_share: '充后占位', sessionCount: '会话数', session_count: '会话数',
  energy_kwh_per_session: '单次电量', charge_minutes: '充电时长', overstay_minutes: '充后占位',
  power_kw: '功率', price_yuan_per_kwh: '电价', duration_minutes: '停留时长',
};
export function dimensionLabel(value: string): string { return labels[value] || labels[value.toLowerCase()] || value; }
export function escapeTooltip(value: unknown): string {
  return String(value ?? '').replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[char]!));
}
const axis = { axisLine: { show: false }, axisTick: { show: false }, axisLabel: { color: '#697586', fontSize: 11 }, splitLine: { show: false } };
const tooltip = { confine: true, trigger: 'item' };
const grid = { left: 22, right: 24, top: 22, bottom: 62, containLabel: true };
const number = (value: unknown, digits = 1) => formatValue(value, digits);
const percent = (value: unknown) => fraction(value, 1);
const heatScale = (min: number, max: number, colors = ['#eef3fd', '#b8cffb', '#6a9bea', '#2765d7', '#12387e']) => ({
  min, max: max > min ? max : min + 1, calculable: false, orient: 'horizontal', left: 'center', bottom: 0,
  itemWidth: 10, itemHeight: 160, precision: 1, text: [number(max), number(min)], textStyle: { color: '#697586', fontSize: 10 }, inRange: { color: colors },
});

/** A missing measurement is absent from the heatmap, never painted as a measured zero. */
export function demandHeatmap(rows: HeatCell[], metric: HeatMetric): EChartsCoreOption {
  const spec = heatMetrics.find(item => item.id === metric)!;
  const data = rows.filter(row => converted(row[metric]) !== null).map(row => ({ value: [row.hour, row.weekday, row[metric]! * spec.multiplier], row }));
  const max = metric === 'chargingUtilization' ? 100 : Math.max(0, ...data.map(point => point.value[2]!));
  return { tooltip: { ...tooltip, formatter: (point: any) => {
    const row: HeatCell = point.data.row;
    return `${weekdays[row.weekday]} · ${String(row.hour).padStart(2, '0')}:00<br/>${spec.label} <b>${number(row[metric]! * spec.multiplier)} ${spec.unit}</b><br/>有效站点小时 ${number(row.sampleHours, 0)}`;
  } }, grid, visualMap: heatScale(0, max),
    xAxis: { ...axis, type: 'category', data: Array.from({ length: 24 }, (_, i) => String(i).padStart(2, '0')), axisLabel: { ...axis.axisLabel, interval: 2 } },
    yAxis: { ...axis, type: 'category', inverse: true, data: weekdays },
    series: [{ type: 'heatmap', data, itemStyle: { borderWidth: 3, borderColor: '#fff', borderRadius: 3 }, emphasis: { itemStyle: { borderColor: '#171a20', borderWidth: 2 } }, label: { show: false } }],
  };
}

export function efficiencyBubble(rows: StationPoint[]): EChartsCoreOption {
  const types = [...new Set(rows.map(row => row.siteType))].sort();
  const usable = rows.filter(row => converted(row.chargingUtilization) !== null && converted(row.meanWaitMinutes) !== null && converted(row.energyKwh) !== null);
  const maxEnergy = Math.max(1, ...usable.map(row => Math.max(0, row.energyKwh!)));
  return { color: advancedColors, legend: { bottom: 0, data: types.map(dimensionLabel), itemWidth: 9, itemHeight: 9, icon: 'circle', textStyle: { fontSize: 10 } },
    grid: { ...grid, top: 34, bottom: 68 },
    tooltip: { ...tooltip, formatter: (point: any) => {
      const row: StationPoint = point.data.row;
      return `<b>${escapeTooltip(row.stationName)}</b><br/>${escapeTooltip(row.cityName)} · ${escapeTooltip(dimensionLabel(row.siteType))}<br/>充电利用率 ${percent(row.chargingUtilization)}<br/>平均排队 ${number(row.meanWaitMinutes)} 分钟<br/>电量 ${number(row.energyKwh)} kWh<br/>会话 ${number(row.sessionCount, 0)} · 尝试 ${number(row.attemptCount, 0)}<br/>点击查看该站明细`;
    } },
    xAxis: { ...axis, type: 'value', name: '充电利用率 %', min: 0, max: 100, nameLocation: 'middle', nameGap: 29, splitLine: { show: true, lineStyle: { color: '#edf0f4', type: 'dashed' } } },
    yAxis: { ...axis, type: 'value', name: '平均排队 / 分钟', min: 0, splitLine: { show: true, lineStyle: { color: '#edf0f4', type: 'dashed' } } },
    series: types.map(type => ({ name: dimensionLabel(type), type: 'scatter',
      data: usable.filter(row => row.siteType === type).map(row => ({ name: row.stationName, value: [row.chargingUtilization! * 100, row.meanWaitMinutes, row.energyKwh], row })),
      // Area, rather than radius, scales with energy, with a 9px visibility floor.
      symbolSize: (value: number[]) => Math.max(9, 46 * Math.sqrt(Math.max(0, value[2]!) / maxEnergy)),
      itemStyle: { opacity: .77, borderColor: '#fff', borderWidth: 1.5 }, emphasis: { itemStyle: { opacity: 1, borderColor: '#171a20' } },
    })),
  };
}

export function weatherHeatmap(rows: WeatherCell[]): EChartsCoreOption {
  const bins = [...new Set(rows.map(row => row.temperatureBin))].sort((a, b) => a - b);
  return { tooltip: { ...tooltip, formatter: (point: any) => {
    const row: WeatherCell = point.data.row;
    return `${number(row.temperatureBin, 0)} ≤ 气温 < ${number(row.temperatureBin + 5, 0)} °C<br/>${String(row.hour).padStart(2, '0')}:00 · 利用率 <b>${percent(row.chargingUtilization)}</b><br/>平均功率 ${number(row.meanPowerKw)} kW<br/>有效站点小时 ${number(row.sampleHours, 0)}<br/>分组描述，不代表温度的因果效应`;
  } }, grid, visualMap: heatScale(0, 100, ['#f0f4fb', '#c4d3ed', '#77acc4', '#299ca5', '#166c79']),
    xAxis: { ...axis, type: 'category', data: Array.from({ length: 24 }, (_, i) => String(i).padStart(2, '0')), axisLabel: { ...axis.axisLabel, interval: 2 } },
    yAxis: { ...axis, type: 'category', data: bins.map(bin => `${bin}–${bin + 5}°C`) },
    series: [{ type: 'heatmap', data: rows.filter(row => converted(row.chargingUtilization) !== null).map(row => ({ value: [row.hour, bins.indexOf(row.temperatureBin), row.chargingUtilization! * 100], row })), itemStyle: { borderWidth: 3, borderColor: '#fff', borderRadius: 3 }, emphasis: { itemStyle: { borderColor: '#171a20', borderWidth: 2 } } }],
  };
}

/** Compare within-group distributions, not raw volumes or manufactured customer retention. */
export function behaviorHeatmap(behavior: RechargeBehavior, metric: BehaviorMetric): EChartsCoreOption {
  const segments = behavior.segments.map(item => item.userSegment);
  const buckets = behaviorBuckets[metric];
  const rows = behavior[metric];
  return { tooltip: { ...tooltip, formatter: (point: any) => {
    const row = point.data.row;
    const segment = behavior.segments.find(item => item.userSegment === row.userSegment);
    const denominator = metric === 'intervals' ? segment?.intervalCount : segment?.sessionCount;
    return `<b>${escapeTooltip(dimensionLabel(row.userSegment))} · ${escapeTooltip(behaviorBucketLabel(row.bucket, metric))}</b><br/>群体内占比 ${percent(row.share)}<br/>${number(row.count, 0)} / ${number(denominator, 0)} ${metric === 'intervals' ? '个有效间隔' : '次会话'}${metric === 'intervals' ? `<br/>另有 ${number(segment?.firstObservedCount, 0)} 次首次观测，不计入间隔分母` : ''}<br/>区间左含右不含；这是会话分布，不是去重人数`;
  } }, grid, visualMap: heatScale(0, 100),
    xAxis: { ...axis, type: 'category', data: buckets.map(bucket => bucket.label), axisLabel: { ...axis.axisLabel, interval: 0, fontSize: 10 } },
    yAxis: { ...axis, type: 'category', inverse: true, data: segments.map(dimensionLabel) },
    series: [{ type: 'heatmap', data: rows.filter(row => converted(row.share) !== null && buckets.some(bucket => bucket.id === row.bucket)).map(row => ({ value: [buckets.findIndex(bucket => bucket.id === row.bucket), segments.indexOf(row.userSegment), row.share! * 100], row })),
      itemStyle: { borderWidth: 4, borderColor: '#fff', borderRadius: 4 }, label: { show: true, fontSize: 11, formatter: (point: any) => `${number(point.value[2], 0)}%` },
      emphasis: { itemStyle: { borderColor: '#171a20', borderWidth: 2 } } }],
  };
}

export function serviceSuccessHeatmap(rows: ServiceCell[], selected?: { siteType: string; hour: number }): EChartsCoreOption {
  const types = [...new Set(rows.map(row => row.siteType))].sort();
  return { tooltip: { ...tooltip, formatter: (point: any) => {
    const row: ServiceCell = point.data.row;
    return `<b>${escapeTooltip(dimensionLabel(row.siteType))} · ${String(row.hour).padStart(2, '0')}:00</b><br/>成功开始 ${number(row.successfulAttempts, 0)} / ${number(row.attemptCount, 0)} 次<br/>尝试成功率 ${percent(row.successRate)}${row.attemptCount < 30 ? '<br/>小样本：少于 30 次，请谨慎比较' : ''}<br/>点击查看占位、排队与接口关联`;
  } }, grid, visualMap: heatScale(0, 100, ['#f1d2bc', '#ece6dd', '#dde7f5', '#82a9e9', '#245bd0']),
    xAxis: { ...axis, type: 'category', data: Array.from({ length: 24 }, (_, i) => String(i).padStart(2, '0')), axisLabel: { ...axis.axisLabel, interval: 2 } },
    yAxis: { ...axis, type: 'category', inverse: true, data: types.map(dimensionLabel) },
    series: [{ type: 'heatmap', data: rows.filter(row => converted(row.successRate) !== null).map(row => ({ value: [row.hour, types.indexOf(row.siteType), row.successRate! * 100], row, itemStyle: selected?.siteType === row.siteType && selected.hour === row.hour ? { borderColor: '#171a20', borderWidth: 2 } : undefined })),
      itemStyle: { borderWidth: 3, borderColor: '#fff', borderRadius: 3 }, emphasis: { itemStyle: { borderColor: '#171a20', borderWidth: 2 } } }],
  };
}

/** Rank exact causes by volume; the second axis is cumulative share of failures, not all attempts. */
export function failurePareto(rows: FailureReason[]): EChartsCoreOption {
  const ordered = [...rows].filter(row => row.count > 0).sort((a, b) => b.count - a.count || a.reason.localeCompare(b.reason));
  const total = ordered.reduce((sum, row) => sum + row.count, 0);
  let cumulative = 0;
  const data = ordered.map(row => { cumulative += row.count; return { value: row.count, row, cumulative: total ? cumulative / total * 100 : null }; });
  return { color: ['#2563eb', '#c98750'], grid: { left: 18, right: 30, top: 34, bottom: 85, containLabel: true },
    tooltip: { ...tooltip, formatter: (point: any) => { const item = data[point.dataIndex]; return item ? `<b>${escapeTooltip(item.row.label)}</b><br/>${number(item.row.count, 0)} 次<br/>占未成功尝试 ${percent(item.row.shareOfFailures)}<br/>占全部尝试 ${percent(item.row.shareOfAttempts)}<br/>至此累计覆盖 ${number(item.cumulative)}% 的未成功尝试` : ''; } },
    legend: { bottom: 0, data: ['未成功尝试', '累计占比'], textStyle: { fontSize: 10 } },
    xAxis: { ...axis, type: 'category', data: ordered.map(row => row.label), axisLabel: { ...axis.axisLabel, interval: 0, rotate: 25, width: 75, overflow: 'truncate', fontSize: 9 } },
    yAxis: [{ ...axis, type: 'value', name: '次', min: 0, splitLine: { show: true, lineStyle: { color: '#edf0f4' } } }, { ...axis, type: 'value', min: 0, max: 100, name: '%', splitLine: { show: false } }],
    series: [{ type: 'bar', name: '未成功尝试', barMaxWidth: 32, data, itemStyle: { borderRadius: [4, 4, 0, 0] } }, { type: 'line', name: '累计占比', yAxisIndex: 1, data: data.map(item => item.cumulative), symbolSize: 6, lineStyle: { width: 2 }, connectNulls: false }],
  };
}

export function serviceFlow(flow: AdvancedData['flow']): EChartsCoreOption {
  return { color: advancedColors, tooltip: { ...tooltip, formatter: (point: any) => {
    const value = point.data.value ?? point.value;
    const description = point.data.source !== undefined ? `${point.data.source} → ${point.data.target}` : point.name;
    return `${escapeTooltip(description)}<br/><b>${number(value, 0)} 次</b><br/>当前范围尝试总数 ${number(flow.attemptCount, 0)}`;
  } }, series: [{ type: 'sankey', left: 10, right: 106, top: 16, bottom: 20, nodeWidth: 11, nodeGap: 14,
    data: flow.nodes, links: flow.links.filter(link => link.value > 0), nodeAlign: 'justify', draggable: false,
    lineStyle: { color: 'gradient', opacity: .2, curveness: .55 }, itemStyle: { borderWidth: 0, borderRadius: 3 },
    label: { color: '#344054', fontSize: 10, width: 100, overflow: 'truncate' }, emphasis: { focus: 'adjacency', lineStyle: { opacity: .48 } },
  }] };
}

export interface SegmentNode { name: string; value: number; children?: SegmentNode[] }
/** Every session belongs to one leaf; parents sum leaves instead of counting parent rows twice. */
export function segmentHierarchy(rows: Segment[]): SegmentNode[] {
  const roots: SegmentNode[] = [];
  for (const row of rows) {
    if (!(row.sessionCount > 0)) continue;
    let siblings = roots;
    const path = [row.siteType, row.userSegment, row.batteryCapacityBand, row.connectorType];
    for (let index = 0; index < path.length; index++) {
      const name = dimensionLabel(path[index]!);
      let node = siblings.find(item => item.name === name);
      if (!node) { node = { name, value: 0, ...(index < path.length - 1 ? { children: [] } : {}) }; siblings.push(node); }
      node.value += row.sessionCount;
      siblings = node.children || [];
    }
  }
  return roots;
}
export function segmentSunburst(rows: Segment[]): EChartsCoreOption {
  return { color: advancedColors, tooltip: { ...tooltip, formatter: (point: any) => {
    const path = (point.treePathInfo || []).slice(1).map((part: { name: string }) => escapeTooltip(part.name)).join(' → ');
    return `${path || escapeTooltip(point.name)}<br/><b>${number(point.value, 0)} 次会话</b>`;
  } }, series: [{ type: 'sunburst', data: segmentHierarchy(rows), radius: ['12%', '94%'], center: ['50%', '50%'], sort: null,
    nodeClick: 'rootToNode', emphasis: { focus: 'ancestor' }, itemStyle: { borderWidth: 2, borderColor: '#fff' },
    label: { rotate: 'radial', minAngle: 13, fontSize: 10 },
    levels: [{}, { r0: '12%', r: '38%', label: { rotate: 'tangential' }, itemStyle: { opacity: 1 } },
      { r0: '38%', r: '59%', itemStyle: { opacity: .86 } }, { r0: '59%', r: '77%', itemStyle: { opacity: .7 } },
      { r0: '77%', r: '94%', label: { show: false }, itemStyle: { opacity: .52 } }],
  }] };
}

export function correlationHeatmap(rows: Correlation[]): EChartsCoreOption {
  const fields = [...new Set(rows.flatMap(row => [row.x, row.y]))];
  return { tooltip: { ...tooltip, formatter: (point: any) => {
    const row: Correlation = point.data.row;
    return `${escapeTooltip(dimensionLabel(row.x))} × ${escapeTooltip(dimensionLabel(row.y))}<br/>Pearson r <b>${number(row.value, 3)}</b><br/>成对完整样本 ${number(row.n, 0)}<br/>相关不代表因果`;
  } }, grid: { ...grid, bottom: 100 }, visualMap: { ...heatScale(-1, 1, ['#bd8155', '#e8d1bc', '#f7f8fb', '#aac5f1', '#2563eb']), precision: 2 },
    xAxis: { ...axis, type: 'category', data: fields.map(dimensionLabel), axisLabel: { ...axis.axisLabel, rotate: 28, interval: 0, width: 80, overflow: 'truncate' } },
    yAxis: { ...axis, type: 'category', inverse: true, data: fields.map(dimensionLabel) },
    series: [{ type: 'heatmap', data: rows.filter(row => converted(row.value) !== null).map(row => ({ value: [fields.indexOf(row.x), fields.indexOf(row.y), row.value], row })),
      itemStyle: { borderWidth: 3, borderColor: '#fff', borderRadius: 3 }, label: { show: true, fontSize: 10, formatter: (point: any) => number(point.value[2], 2) } }],
  };
}
