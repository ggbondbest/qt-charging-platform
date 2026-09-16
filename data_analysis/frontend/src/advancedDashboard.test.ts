import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createRenderer, h, nextTick, ref } from 'vue';
import * as Vue from 'vue';
import { compileScript, compileTemplate, parse } from '@vue/compiler-sfc';
import AdvancedAnalytics from './components/AdvancedAnalytics.vue';
import source from './components/AdvancedAnalytics.vue?raw';
import { AnalyticsError, publishedRequest } from './analytics';
import type { AdvancedData, AdvancedFilters } from './advancedAnalytics';

vi.mock('./analytics', async original => ({ ...await original<typeof import('./analytics')>(), publishedRequest: vi.fn() }));
vi.mock('./components/Chart.vue', () => ({ default: { props: ['option', 'label'], emits: ['select'], setup: (props: any, { emit }: any) => () => h('chart-stub', { 'aria-label': props.label, option: props.option, onSelect: (event: any) => emit('select', event) }) } }));
vi.mock('./components/Icon.vue', () => ({ default: { render: () => h('icon-stub') } }));

class TestNode {
  parent: TestNode | null = null; children: TestNode[] = []; props: Record<string, any> = {}; text = ''; value: any;
  selected = false; selectedIndex = -1; multiple = false; listeners: Record<string, Function> = {};
  constructor(public kind: string) {}
  get tagName() { return this.kind.toUpperCase(); }
  get options() { return this.children.filter(child => child.kind === 'option'); }
  get textContent(): string { return this.text + this.children.map(child => child.textContent).join(''); }
  getRootNode() { return document; }
  addEventListener(name: string, handler: Function) { this.listeners[name] = handler; }
  removeEventListener(name: string) { delete this.listeners[name]; }
}
function detach(node: TestNode) { if (!node.parent) return; const index = node.parent.children.indexOf(node); if (index >= 0) node.parent.children.splice(index, 1); node.parent = null; }
const renderer = createRenderer<TestNode, TestNode>({
  createElement: tag => new TestNode(tag), createText: text => Object.assign(new TestNode('#text'), { text }), createComment: text => Object.assign(new TestNode('#comment'), { text }),
  setText: (node, text) => { node.text = text; }, setElementText: (node, text) => { node.text = text; node.children = []; }, parentNode: node => node.parent,
  nextSibling: node => node.parent?.children[node.parent.children.indexOf(node) + 1] || null,
  patchProp: (node, key, _previous, value) => { node.props[key] = value; if (key === 'value') node.value = value; },
  insert: (node, parent, anchor = null) => { detach(node); node.parent = parent; const index = anchor ? parent.children.indexOf(anchor) : -1; if (index < 0) parent.children.push(node); else parent.children.splice(index, 0, node); }, remove: detach,
});
const { descriptor } = parse(source);
const bindings = compileScript(descriptor, { id: 'advanced-dashboard-test' }).bindings;
const compiled = compileTemplate({ source: descriptor.template!.content, filename: 'AdvancedAnalytics.vue', id: 'advanced-dashboard-test', compilerOptions: { bindingMetadata: bindings, hoistStatic: false } });
if (compiled.errors.length) throw new Error(String(compiled.errors[0]));
const renderCode = compiled.code.replace(/import \{([^}]+)\} from "vue"/g, (_match, names: string) => `const {${names.replace(/ as /g, ': ')}} = Vue`).replace('export function render', 'return function render');
const component = { ...AdvancedAnalytics, render: new Function('Vue', renderCode)(Vue) } as unknown as Vue.Component;

function fixture(insight = '当前范围的统计发现'): AdvancedData {
  return {
    provenance: { sourceLabel: '行为校准模拟数据', analysisId: 'AN-1', engine: 'PySpark', generatedAt: '2026-09-15T00:00:00Z', notes: ['可追溯到同一发布批次'] },
    scope: { startDate: '2026-01-01', endDate: '2026-06-01', cityId: null, stationId: null, siteType: null },
    stationTypes: ['commercial'], summary: { stationCount: 1, sessionCount: 100, attemptCount: 120, observedStationHours: 720, completeStationHours: 700 },
    heatmap: [{ weekday: 0, hour: 8, energyKwh: 100, meanPowerKw: 50, chargingUtilization: .5, sampleHours: 20 }],
    stations: [{ stationId: 'S1', stationName: '测试商业站', cityId: 'DL', cityName: '大连市', siteType: 'commercial', energyKwh: 100, chargingUtilization: .5, meanWaitMinutes: 4, overstayShare: .1, netCashYuan: 130, sessionCount: 100, attemptCount: 120, successRate: .8 }],
    weather: [], flow: { attemptCount: 120, nodes: [{ name: '尝试' }, { name: '完成' }], links: [{ source: '尝试', target: '完成', value: 100 }] },
    retention: { scopeLabel: '当前城市', definition: '完整日历月，历史回看', observationEnd: '2026-06-01', cohorts: [{ month: '2026-01', size: 100, cells: [{ offset: 0, users: 100, rate: 1 }, { offset: 1, users: null, rate: null }] }] },
    behavior: { definition: '补能间隔跨站回看，首次观测不是零间隔', sessionCount: 100, intervalCount: 90, firstObservedCount: 10, intervals: [{ userSegment: 'FAMILY', bucket: '1_TO_3D', count: 90, share: 1 }], energy: [{ userSegment: 'FAMILY', bucket: '20_TO_40', count: 100, share: 1 }], segments: [{ userSegment: 'FAMILY', sessionCount: 100, intervalCount: 90, firstObservedCount: 10, meanIntervalDays: 2, meanEnergyKwh: 30 }] },
    service: { definition: '不同事件集合，仅描述关联，不代表因果', attemptCount: 120, successfulAttempts: 100, failedAttempts: 20, failures: [{ reason: 'NO_FREE', label: '无可用电桩', count: 20, shareOfFailures: 1, shareOfAttempts: 1 / 6 }], accessPaths: [{ path: 'DIRECT', label: '直接尝试', attemptCount: 120, successfulAttempts: 100, successRate: 100 / 120 }], cells: [
      { siteType: 'RESIDENTIAL', hour: 18, stationCount: 1, attemptCount: 100, successfulAttempts: 90, successRate: .9, meanWaitMinutes: 4, queueWaitCount: 30, overstayShare: .2, sessionCount: 90, chargingUtilization: .6, completeStationHours: 30, interfaces: [{ connectorType: 'AC', chargerCount: 2, ratedPowerKw: 14 }], failures: [{ reason: 'NO_FREE', label: '无可用电桩', count: 10, shareOfFailures: 1, shareOfAttempts: .1 }] },
      { siteType: 'CAMPUS', hour: 9, stationCount: 1, attemptCount: 20, successfulAttempts: 10, successRate: .5, meanWaitMinutes: null, queueWaitCount: 0, overstayShare: null, sessionCount: 10, chargingUtilization: .3, completeStationHours: 10, interfaces: [{ connectorType: 'DC', chargerCount: 1, ratedPowerKw: 120 }], failures: [{ reason: 'NO_FREE', label: '无可用电桩', count: 10, shareOfFailures: 1, shareOfAttempts: .5 }] },
    ] },
    segments: [], correlations: [], insights: [insight],
  };
}
const envelope = (data: AdvancedData) => ({ code: 'OK', message: 'ok', data, meta: { datasetId: 'D', publishedBatchId: 'B' } });
let root: TestNode; let app: ReturnType<typeof renderer.createApp>;
const filters = ref<AdvancedFilters>({ datasetId: 'D', publishedBatchId: 'B', startDate: '2026-01-01', endDate: '2026-06-01' });
const topic = ref<'space' | 'service'>('space');
const presentation = ref(false);
function nodes(node = root): TestNode[] { return [node, ...node.children.flatMap(child => nodes(child))]; }
async function settle() { for (let i = 0; i < 12; i++) { await Promise.resolve(); await nextTick(); } }
async function mount() {
  root = new TestNode('root'); app = renderer.createApp({ setup: () => () => h(component, { filters: filters.value, topic: topic.value, presentation: presentation.value, stations: [{ stationId: 'S1', stationName: '测试商业站' }] }) });
  app.provide(Vue.ssrContextKey, {}); app.mount(root); await settle();
}
beforeEach(() => {
  class TestDocument {}
  vi.stubGlobal('Document', TestDocument); vi.stubGlobal('document', new TestDocument());
  vi.mocked(publishedRequest).mockReset(); vi.mocked(publishedRequest).mockResolvedValue(envelope(fixture()));
  filters.value = { datasetId: 'D', publishedBatchId: 'B', startDate: '2026-01-01', endDate: '2026-06-01' }; topic.value = 'space'; presentation.value = false;
});
afterEach(() => { app?.unmount(); vi.unstubAllGlobals(); });

describe('advanced workspace interactions', () => {
  it('prioritizes charts in presentation without losing findings, source labels or refetching data', async () => {
    await mount();
    const before = nodes();
    expect(before.findIndex(node => String(node.props.class).includes('advanced-findings'))).toBeLessThan(before.findIndex(node => node.kind === 'chart-stub'));
    presentation.value = true; await settle();
    const after = nodes();
    expect(after.some(node => String(node.props.class).includes('advanced-presentation'))).toBe(true);
    expect(after.findIndex(node => String(node.props.class).includes('advanced-findings--after-charts'))).toBeGreaterThan(after.map(node => node.kind).lastIndexOf('chart-stub'));
    expect(root.textContent).toContain('行为校准模拟数据'); expect(root.textContent).toContain('当前范围的统计发现');
    expect(root.textContent).toContain('退出大屏可调整筛选');
    expect(vi.mocked(publishedRequest)).toHaveBeenCalledTimes(1);
  });
  it('renders server-backed findings, samples and chart alternatives, retaining data when switching topic', async () => {
    await mount();
    expect(root.textContent).toContain('当前范围的统计发现'); expect(root.textContent).toContain('行为校准模拟数据');
    expect(nodes().some(node => node.kind === 'chart-stub')).toBe(true);
    expect(nodes().some(node => node.props['aria-label'] === '查看电站效能明细')).toBe(true);
    topic.value = 'service'; await settle();
    expect(root.textContent).toContain('谁回站频繁，谁一次充得多？'); expect(root.textContent).toContain('首次观测不等于新注册用户');
    expect(root.textContent).toContain('车辆可能在站外补能，不代表全部充电间隔');
    expect(root.textContent).not.toContain('同一批用户，之后还来吗？'); expect(root.textContent).not.toContain('用户同期群');
    expect(root.textContent).toContain('无可用电桩'); expect(root.textContent).toContain('站点静态装机清单');
    expect(vi.mocked(publishedRequest)).toHaveBeenCalledTimes(1);
  });
  it('switches behavior distribution without refetching or reusing the interval denominator', async () => {
    topic.value = 'service'; await mount();
    const control = nodes().find(node => node.props['aria-label'] === '补能分布指标')!;
    control.props['onUpdate:modelValue']('energy'); await settle();
    const chart = nodes().find(node => String(node.props['aria-label']).startsWith('不同用户群体的平台复充'))!;
    expect(chart.props.option.xAxis.data).toContain('20–40 kWh');
    expect(root.textContent).toContain('100 次充电会话');
    expect(vi.mocked(publishedRequest)).toHaveBeenCalledTimes(1);
  });
  it('links a heatmap click to the exact resource context with nulls and a small-sample warning', async () => {
    topic.value = 'service'; await mount();
    const contextBefore = nodes().find(node => node.props['aria-label'] === '选中场景的资源关联')!;
    expect(contextBefore.textContent).toContain('社区 · 18:00–19:00');
    const chart = nodes().find(node => String(node.props['aria-label']).startsWith('站型和小时尝试'))!;
    chart.props.onSelect({ data: { row: fixture().service.cells[1] } }); await settle();
    const context = nodes().find(node => node.props['aria-label'] === '选中场景的资源关联')!;
    expect(context.textContent).toContain('校园 · 09:00–10:00');
    expect(context.textContent).toContain('小样本'); expect(context.textContent).toContain('—分钟');
    expect(context.textContent).toContain('直流 DC'); expect(context.textContent).toContain('120.0 kW');
    expect(vi.mocked(publishedRequest)).toHaveBeenCalledTimes(1);
  });
  it('provides the same context through an accessible selector and clears selection after a scope change', async () => {
    topic.value = 'service'; await mount();
    const control = nodes().find(node => node.props['aria-label'] === '选择服务瓶颈场景')!;
    control.props['onUpdate:modelValue']('CAMPUS:9'); await settle();
    expect(nodes().find(node => node.props['aria-label'] === '选中场景的资源关联')!.textContent).toContain('校园 · 09:00–10:00');
    const result = fixture(); result.service.cells = [];
    vi.mocked(publishedRequest).mockResolvedValue(envelope(result));
    filters.value = { ...filters.value, cityId: 'BJ' }; await settle();
    expect(nodes().find(node => node.props['aria-label'] === '选中场景的资源关联')!.textContent).not.toContain('校园 · 09:00–10:00');
    expect(root.textContent).toContain('选择一个场景查看对应样本');
  });
  it('discloses the omitted reason count and attempts when a context shows only its four largest causes', async () => {
    topic.value = 'service';
    const result = fixture();
    result.service.cells[0]!.failures = [1, 10, 30, 2, 20, 44].map((count, index) => ({ reason: `R${index}`, label: `原因${index}`, count, shareOfFailures: count / 107, shareOfAttempts: count / 170 }));
    result.service.cells[0]!.attemptCount = 170;
    result.service.cells[0]!.successfulAttempts = 63;
    result.service.cells[0]!.successRate = 63 / 170;
    vi.mocked(publishedRequest).mockResolvedValue(envelope(result));
    await mount();
    const context = nodes().find(node => node.props['aria-label'] === '选中场景的资源关联')!;
    expect(context.textContent).toContain('主要未成功原因（前 4 项）');
    expect(context.textContent).toContain('另有 2 类原因，共 3 次未成功尝试');
    const list = nodes(context).find(node => String(node.props.class).includes('advanced-reason-list'))!;
    expect(list.children.filter(node => node.kind === 'li')).toHaveLength(4);
    expect(list.textContent).toContain('原因5'); expect(list.textContent).not.toContain('原因0');
    const select = nodes().find(node => node.props['aria-label'] === '选择服务瓶颈场景')!;
    select.props['onUpdate:modelValue']('CAMPUS:9'); await settle();
    expect(nodes().some(node => node.props['aria-label'] === '其余未成功原因')).toBe(false);
  });
  it('applies local station and site-type selection to the published request', async () => {
    await mount();
    const select = nodes().find(node => node.props['aria-label'] === '分析站点类型')!;
    select.props['onUpdate:modelValue']('commercial'); await settle();
    expect(vi.mocked(publishedRequest).mock.lastCall?.[1]).toMatchObject({ siteType: 'commercial', datasetId: 'D', publishedBatchId: 'B' });
    const stationSelect = nodes().find(node => node.props['aria-label'] === '分析电站范围')!;
    stationSelect.props['onUpdate:modelValue']('S1'); await settle();
    expect(vi.mocked(publishedRequest).mock.lastCall?.[1]).toMatchObject({ stationId: 'S1', siteType: 'commercial' });
  });
  it('ignores late stale responses even if the transport does not honor cancellation', async () => {
    const resolves: ((response: any) => void)[] = [];
    vi.mocked(publishedRequest).mockImplementation(() => new Promise(resolve => resolves.push(resolve)));
    await mount();
    const initialSignal = vi.mocked(publishedRequest).mock.calls[0]![2]!.signal;
    filters.value = { ...filters.value, cityId: 'BJ' }; await settle();
    expect(initialSignal?.aborted).toBe(true);
    resolves[1]!(envelope(fixture('新城市的结果'))); await settle();
    resolves[0]!(envelope(fixture('旧城市的过时结果'))); await settle();
    expect(root.textContent).toContain('新城市的结果'); expect(root.textContent).not.toContain('旧城市的过时结果');
  });
  it('keeps an unavailable publication explicit and can retry without fabricating charts', async () => {
    vi.mocked(publishedRequest).mockRejectedValueOnce(new AnalyticsError('ANALYSIS_NOT_READY', '该批次尚未发布多维统计'));
    await mount();
    expect(root.textContent).toContain('该批次尚未发布多维统计'); expect(root.textContent).toContain('基础运营总览不受影响');
    expect(nodes().some(node => node.kind === 'chart-stub')).toBe(false);
    const retry = nodes().find(node => node.kind === 'button' && node.textContent.includes('重试分析'))!;
    await retry.props.onClick(); await settle();
    expect(root.textContent).toContain('当前范围的统计发现');
  });
  it('aborts the pending query when the dashboard is left', async () => {
    vi.mocked(publishedRequest).mockImplementation(() => new Promise(() => {}));
    await mount();
    const signal = vi.mocked(publishedRequest).mock.lastCall?.[2]?.signal;
    app.unmount(); expect(signal?.aborted).toBe(true);
  });
});
