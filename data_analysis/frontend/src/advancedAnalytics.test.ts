import { afterEach, describe, expect, it, vi } from 'vitest';
import dashboardSource from './components/AnalyticsDashboard.vue?raw';
import componentSource from './components/AdvancedAnalytics.vue?raw';
import { behaviorHeatmap, correlationHeatmap, demandHeatmap, dimensionLabel, efficiencyBubble, escapeTooltip, failurePareto, segmentHierarchy, segmentSunburst, serviceFlow, serviceSuccessHeatmap, weatherHeatmap } from './advancedAnalytics';
import type { HeatCell, RechargeBehavior, Segment, ServiceCell, StationPoint } from './advancedAnalytics';
import { publishedRequest } from './analytics';

const heat = (values: Partial<HeatCell> = {}): HeatCell => ({ weekday: 0, hour: 8, energyKwh: 100, meanPowerKw: 50, chargingUtilization: .5, sampleHours: 20, ...values });
const station = (values: Partial<StationPoint> = {}): StationPoint => ({ stationId: 'S1', stationName: '一号站', cityId: 'DL', cityName: '大连市', siteType: 'commercial', energyKwh: 1000, chargingUtilization: .5, meanWaitMinutes: 4, overstayShare: .1, netCashYuan: 1300, sessionCount: 80, attemptCount: 100, successRate: .8, ...values });
const segment = (values: Partial<Segment> = {}): Segment => ({ siteType: 'commercial', userSegment: 'private', batteryCapacityBand: '50_TO_69', connectorType: 'DC', sessionCount: 10, energyKwh: 300, meanChargeMinutes: 40, meanOverstayMinutes: 8, ...values });
const behavior = (): RechargeBehavior => ({ definition: '跨站前序充电回看', sessionCount: 10, intervalCount: 8, firstObservedCount: 2, segments: [{ userSegment: 'FAMILY', sessionCount: 10, intervalCount: 8, firstObservedCount: 2, meanIntervalDays: 3, meanEnergyKwh: 20 }], intervals: [{ userSegment: 'FAMILY', bucket: 'LT1D', count: 0, share: 0 }, { userSegment: 'FAMILY', bucket: '1_TO_3D', count: 8, share: 1 }, { userSegment: 'FAMILY', bucket: 'GE14D', count: 0, share: null }], energy: [{ userSegment: 'FAMILY', bucket: '20_TO_40', count: 10, share: 1 }] });
const serviceCell = (values: Partial<ServiceCell> = {}): ServiceCell => ({ siteType: 'RESIDENTIAL', hour: 18, stationCount: 1, attemptCount: 20, successfulAttempts: 10, successRate: .5, meanWaitMinutes: 12, queueWaitCount: 6, overstayShare: .2, sessionCount: 10, chargingUtilization: .7, completeStationHours: 7, interfaces: [{ connectorType: 'AC', chargerCount: 3, ratedPowerKw: 21 }], failures: [], ...values });
afterEach(() => vi.unstubAllGlobals());

describe('multidimensional chart semantics', () => {
  it('labels real uppercase Spark dimensions and battery bands without presenting raw enums', () => {
    expect(['OFFICE', 'SHOPPING', 'RESIDENTIAL', 'TRANSIT', 'CAMPUS'].map(dimensionLabel)).toEqual(['办公园区', '商业中心', '社区', '交通枢纽', '校园']);
    expect(['COMMUTER', 'RIDE_HAILING', 'FAMILY', 'FLEET'].map(dimensionLabel)).toEqual(['通勤用户', '网约车', '家庭用户', '营运车队']);
    expect(['LT50', '50_TO_69', 'GE70', 'UNKNOWN', 'AC', 'DC'].map(dimensionLabel)).toEqual(['<50 kWh', '50–69 kWh', '≥70 kWh', '容量未知', '交流 AC', '直流 DC']);
    const hierarchy = segmentHierarchy([segment({ siteType: 'SHOPPING', userSegment: 'FAMILY', batteryCapacityBand: 'GE70' })]);
    expect(hierarchy[0]!.name).toBe('商业中心');
    expect(hierarchy[0]!.children![0]!.name).toBe('家庭用户');
    expect(hierarchy[0]!.children![0]!.children![0]!.name).toBe('≥70 kWh');
  });
  it('encodes Monday=0 and local hour without silently replacing missing values by zero', () => {
    const option: any = demandHeatmap([heat({ chargingUtilization: null }), heat({ hour: 9, chargingUtilization: 0 }), heat({ weekday: 6, hour: 22, chargingUtilization: .75 })], 'chargingUtilization');
    expect(option.yAxis.data[0]).toBe('周一'); expect(option.yAxis.data[6]).toBe('周日');
    expect(option.series[0].data.map((item: any) => item.value)).toEqual([[9, 0, 0], [22, 6, 75]]);
    expect(option.visualMap.max).toBe(100);
  });
  it('keeps Wh/kWh/power fields distinct and reports actual station-hour sample size', () => {
    const option: any = demandHeatmap([heat()], 'energyKwh');
    expect(option.series[0].data[0].value[2]).toBe(100);
    const tooltip = option.tooltip.formatter({ data: option.series[0].data[0] });
    expect(tooltip).toContain('100.0 kWh'); expect(tooltip).toContain('有效站点小时 20');
    const power: any = demandHeatmap([heat()], 'meanPowerKw');
    expect(power.series[0].data[0].value[2]).toBe(50);
  });
  it('preserves scatter dimensions and excludes incomplete pairs rather than inventing waiting times', () => {
    const source = [station(), station({ stationId: 'missing', meanWaitMinutes: null }), station({ stationId: 'zero', energyKwh: 0, meanWaitMinutes: 0, chargingUtilization: 0 }), station({ stationId: 'energyMissing', energyKwh: null })];
    const option: any = efficiencyBubble(source);
    expect(option.series[0].data.map((item: any) => item.value)).toEqual([[50, 4, 1000], [0, 0, 0]]);
    expect(option.series[0].data[0].row.cityName).toBe('大连市');
    expect(option.series[0].data[0].row.siteType).toBe('commercial');
    expect(option.series[0].symbolSize([0, 0, 250])).toBe(23);
    expect(option.series[0].symbolSize([0, 0, 0])).toBe(9);
    expect(source[1]!.meanWaitMinutes).toBeNull();
  });
  it('does not render backend-supplied station names as tooltip HTML', () => {
    const option: any = efficiencyBubble([station({ stationName: '<img src=x onerror=alert(1)>', cityName: 'A&B' })]);
    const tooltip = option.tooltip.formatter({ data: option.series[0].data[0] });
    expect(tooltip).toContain('&lt;img'); expect(tooltip).not.toContain('<img'); expect(tooltip).toContain('A&amp;B');
    expect(escapeTooltip('"x\'')).toBe('&quot;x&#39;');
  });
  it('sorts temperature bins numerically, keeps sparse samples visible and states the noncausal interpretation', () => {
    const option: any = weatherHeatmap([
      { temperatureBin: 20, hour: 15, chargingUtilization: .8, meanPowerKw: 12, sampleHours: 1 },
      { temperatureBin: -5, hour: 9, chargingUtilization: null, meanPowerKw: null, sampleHours: 0 },
      { temperatureBin: 5, hour: 7, chargingUtilization: 0, meanPowerKw: 0, sampleHours: 4 },
    ]);
    expect(option.yAxis.data).toEqual(['-5–0°C', '5–10°C', '20–25°C']);
    expect(option.series[0].data.map((item: any) => item.value)).toEqual([[15, 2, 80], [7, 1, 0]]);
    const tooltip = option.tooltip.formatter({ data: option.series[0].data[0] });
    expect(tooltip).toContain('有效站点小时 1'); expect(tooltip).toContain('不代表温度的因果效应');
  });
  it('uses within-segment interval denominators, excludes missing and distinguishes measured zero', () => {
    const option: any = behaviorHeatmap(behavior(), 'intervals');
    expect(option.xAxis.data).toEqual(['<1 天', '1–3 天', '3–7 天', '7–14 天', '≥14 天']);
    expect(option.series[0].data.map((item: any) => item.value)).toEqual([[0, 0, 0], [1, 0, 100]]);
    const tooltip = option.tooltip.formatter({ data: option.series[0].data[1] });
    expect(tooltip).toContain('8 / 8 个有效间隔'); expect(tooltip).toContain('2 次首次观测'); expect(tooltip).toContain('不是去重人数');
  });
  it('changes distribution buckets and denominator when comparing session energy', () => {
    const option: any = behaviorHeatmap(behavior(), 'energy');
    expect(option.xAxis.data).toEqual(['<10 kWh', '10–20 kWh', '20–40 kWh', '≥40 kWh']);
    expect(option.series[0].data[0].value).toEqual([2, 0, 100]);
    expect(option.tooltip.formatter({ data: option.series[0].data[0] })).toContain('10 / 10 次会话');
  });
  it('shows small samples and the actual success denominator; missing hours stay blank', () => {
    const option: any = serviceSuccessHeatmap([serviceCell(), serviceCell({ hour: 19, attemptCount: 0, successfulAttempts: 0, successRate: null }), serviceCell({ hour: 20, successRate: 0, successfulAttempts: 0 })], { siteType: 'RESIDENTIAL', hour: 18 });
    expect(option.yAxis.data).toEqual(['社区']);
    expect(option.series[0].data.map((item: any) => item.value)).toEqual([[18, 0, 50], [20, 0, 0]]);
    expect(option.series[0].data[0].itemStyle.borderColor).toBe('#171a20');
    const tooltip = option.tooltip.formatter({ data: option.series[0].data[0] });
    expect(tooltip).toContain('10 / 20 次'); expect(tooltip).toContain('小样本');
  });
  it('ranks specific failure reasons without conflating the failure and all-attempt denominators', () => {
    const option: any = failurePareto([{ reason: 'TIMEOUT', label: '叫号未确认', count: 20, shareOfFailures: .25, shareOfAttempts: .2 }, { reason: 'NO_FREE', label: '无可用桩', count: 60, shareOfFailures: .75, shareOfAttempts: .6 }]);
    expect(option.xAxis.data).toEqual(['无可用桩', '叫号未确认']);
    expect(option.series[0].data.map((item: any) => item.value)).toEqual([60, 20]);
    expect(option.series[1].data).toEqual([75, 100]);
    const tooltip = option.tooltip.formatter({ dataIndex: 0 });
    expect(tooltip).toContain('占未成功尝试 75.0%'); expect(tooltip).toContain('占全部尝试 60.0%');
  });
  it('keeps flow amounts in counts with one source cohort and escapes node text', () => {
    const option: any = serviceFlow({ attemptCount: 100, nodes: [{ name: '尝试' }, { name: '充电' }], links: [{ source: '尝试', target: '充电', value: 80 }, { source: '尝试', target: '空组', value: 0 }] });
    expect(option.series[0].links).toEqual([{ source: '尝试', target: '充电', value: 80 }]);
    const tooltip = option.tooltip.formatter({ data: { source: '<b>x</b>', target: '充电', value: 80 } });
    expect(tooltip).toContain('80 次'); expect(tooltip).toContain('当前范围尝试总数 100'); expect(tooltip).toContain('&lt;b&gt;');
  });
  it('builds a four-level additive hierarchy without counting parent rows twice', () => {
    const roots = segmentHierarchy([segment(), segment({ connectorType: 'AC', sessionCount: 20 }), segment({ siteType: 'residential', sessionCount: 5 }), segment({ sessionCount: 0 })]);
    expect(roots.map(node => node.value)).toEqual([30, 5]);
    expect(roots[0]!.children![0]!.value).toBe(30);
    expect(roots[0]!.children![0]!.children![0]!.name).toBe('50–69 kWh');
    expect(roots[0]!.children![0]!.children![0]!.children!.map(node => node.value)).toEqual([10, 20]);
    const option: any = segmentSunburst([segment()]);
    expect(option.series[0].nodeClick).toBe('rootToNode');
    expect(option.series[0].data[0].value).toBe(10);
  });
  it('keeps negative and zero correlations distinct from undefined correlation', () => {
    const rows = [{ x: 'temperature', y: 'energyKwh', value: -.7, n: 200 }, { x: 'energyKwh', y: 'temperature', value: 0, n: 199 }, { x: 'energyKwh', y: 'energyKwh', value: null, n: 1 }];
    const option: any = correlationHeatmap(rows);
    expect(option.series[0].data.map((item: any) => item.value[2])).toEqual([-.7, 0]);
    expect(option.visualMap.min).toBe(-1); expect(option.visualMap.max).toBe(1);
    expect(option.tooltip.formatter({ data: option.series[0].data[0] })).toContain('成对完整样本 200');
  });
  it('supports an empty filtered scope without fabricating observations', () => {
    for (const option of [demandHeatmap([], 'energyKwh'), weatherHeatmap([]), behaviorHeatmap({ ...behavior(), intervals: [], segments: [] }, 'intervals'), serviceSuccessHeatmap([]), failurePareto([]), correlationHeatmap([])] as any[]) expect(option.series[0].data).toEqual([]);
    expect(segmentHierarchy([])).toEqual([]);
    expect((efficiencyBubble([]) as any).series).toEqual([]);
  });
});

describe('advanced dashboard integration boundaries', () => {
  it('pins advanced scope to city, station, type, dates and immutable published batch', async () => {
    const fetcher = vi.fn().mockResolvedValue(new Response(JSON.stringify({ code: 'OK', data: { heatmap: [] }, meta: { datasetId: 'D', publishedBatchId: 'B' } })));
    vi.stubGlobal('fetch', fetcher);
    await publishedRequest('/dashboard/advanced', { datasetId: 'D', publishedBatchId: 'B', cityId: 'DL', stationId: 'S1', siteType: 'commercial', startDate: '2026-01-01', endDate: '2026-06-01' });
    const url = new URL(fetcher.mock.calls[0]![0], 'http://localhost');
    expect(url.pathname).toBe('/api/v1/dashboard/advanced');
    expect(Object.fromEntries(url.searchParams)).toEqual({ datasetId: 'D', publishedBatchId: 'B', cityId: 'DL', stationId: 'S1', siteType: 'commercial', startDate: '2026-01-01', endDate: '2026-06-01' });
  });
  it('rejects a mismatched advanced batch, without falling back to example chart data', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify({ code: 'OK', data: {}, meta: { datasetId: 'D', publishedBatchId: 'OTHER' } }))));
    await expect(publishedRequest('/dashboard/advanced', { datasetId: 'D', publishedBatchId: 'B' })).rejects.toMatchObject({ code: 'BATCH_MISMATCH' });
  });
  it('uses committed dashboard filters and keeps advanced failures separate from overview queries', () => {
    const dashboard = dashboardSource;
    const component = componentSource;
    expect(dashboard).toContain(':filters="snapshot.filters"');
    expect(dashboard).not.toContain("publishedRequest<RecordData>('/dashboard/advanced'");
    expect(component).toContain('current !== sequence');
    expect(component).toContain('controller?.abort()');
    expect(component).toContain('基础运营总览不受影响');
    expect(component).toContain('首次观测不等于新注册用户');
    expect(component).toContain('不按本页开始日期截断');
    expect(component).toContain('scope="col"');
    expect(component).toContain('aria-label="查看电站效能明细"');
  });
});
