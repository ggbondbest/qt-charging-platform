import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createRenderer, h, nextTick } from 'vue';
import * as Vue from 'vue';
import { compileScript, compileTemplate, parse } from '@vue/compiler-sfc';
import OperationsAdvisor from './components/OperationsAdvisor.vue';
import source from './components/OperationsAdvisor.vue?raw';
import type { AdvisorCapabilities, AdvisorRequest, AdvisorResponse } from './advisor';

vi.mock('./components/Icon.vue', () => ({ default: { render: () => h('icon-stub') } }));
class TestNode {
  parent: TestNode | null = null; children: TestNode[] = []; props: Record<string, any> = {}; text = ''; value: any;
  selected = false; selectedIndex = -1; multiple = false; checked = false; listeners: Record<string, Function> = {};
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
const bindings = compileScript(descriptor, { id: 'operations-advisor-test' }).bindings;
const compiled = compileTemplate({ source: descriptor.template!.content, filename: 'OperationsAdvisor.vue', id: 'operations-advisor-test', compilerOptions: { bindingMetadata: bindings, hoistStatic: false } });
if (compiled.errors.length) throw new Error(String(compiled.errors[0]));
const renderCode = compiled.code.replace(/import \{([^}]+)\} from "vue"/g, (_match, names: string) => `const {${names.replace(/ as /g, ': ')}} = Vue`).replace('export function render', 'return function render');
const component = { ...OperationsAdvisor, render: new Function('Vue', renderCode)(Vue) } as unknown as Vue.Component;
const dataset = { datasetId: 'D1', publishedBatchId: 'B1', startDate: '2026-01-01', endDate: '2026-06-01' };
let config: AdvisorCapabilities;
const fetchMock = vi.fn(); const navigate = vi.fn();
let root: TestNode; let app: ReturnType<typeof renderer.createApp> | undefined;
let reply: (body: AdvisorRequest, options: RequestInit) => Promise<Response>;
function response(data: unknown, meta = { datasetId: 'D1', publishedBatchId: 'B1' }) { return Promise.resolve(new Response(JSON.stringify({ code: 'OK', message: 'ok', data, meta }), { status: 200 })); }
function answer(body: AdvisorRequest, extra: Partial<AdvisorResponse> = {}): AdvisorResponse {
  return { status: 'answered', mode: body.mode, intent: 'overview', answer: '当前范围有 120 次充电会话，建议核查高峰期供给。',
    scope: { datasetId: body.datasetId, publishedBatchId: body.publishedBatchId, startDate: body.startDate, endDate: body.endDate, cityId: body.cityId || null, stationId: null, timeZone: 'Asia/Shanghai', dataKind: 'SIMULATED' },
    evidence: [{ id: 'sessions', label: '充电会话', value: 120, unit: '次', source: { endpoint: '/dashboard/overview', field: 'metrics.sessionCount' } }],
    limitations: ['模拟历史数据，不能解释因果。'], suggestions: [{ target: 'overview', label: '运营总览' }], ...extra };
}
const posts = () => fetchMock.mock.calls.filter(([, options]) => options?.method === 'POST');
function nodes(node = root): TestNode[] { return [node, ...node.children.flatMap(child => nodes(child))]; }
function node(kind: string, text?: string) { const found = nodes().find(item => item.kind === kind && (!text || item.textContent.includes(text))); if (!found) throw new Error(`Missing ${kind}: ${text}`); return found; }
function control(label: string) { const found = nodes().find(item => item.props['aria-label'] === label); if (!found) throw new Error(`Missing control: ${label}`); return found; }
async function settle() { for (let i = 0; i < 35; i++) { await Promise.resolve(); await nextTick(); } }
async function edit(element: TestNode, value: unknown) { element.props['onUpdate:modelValue'](value); await settle(); }
async function click(text: string) { const button = node('button', text); expect(button.props.disabled).not.toBe(true); button.props.onClick({}); await settle(); }
async function submit() { node('form').props.onSubmit({ preventDefault() {} }); await settle(); }
async function mount() { root = new TestNode('root'); app = renderer.createApp({ setup: () => () => h(component, { onNavigate: navigate }) }); app.provide(Vue.ssrContextKey, {}); app.mount(root); await settle(); }
function deferred() { let resolve!: (value: Response) => void; const promise = new Promise<Response>(res => { resolve = res; }); return { promise, resolve }; }

beforeEach(() => {
  class TestDocument {}
  vi.stubGlobal('Document', TestDocument); vi.stubGlobal('document', new TestDocument());
  navigate.mockReset(); fetchMock.mockReset(); vi.stubGlobal('fetch', fetchMock);
  config = { defaultMode: 'offline', onlineAvailable: false, onlineProvider: null, maxQuestionLength: 300, disclosure: '不发送问题原文、用户或会话明细。', supportedQuestions: [
    { id: 'overview', label: '运营表现如何？', question: '当前范围运营表现如何？' },
    { id: 'stations', label: '哪些电站需要关注？', question: '哪些电站需要关注？' },
    { id: 'models', label: '负荷模型表现怎样？', question: '负荷模型表现怎样？' },
    { id: 'anomalies', label: '异常应该如何复核？', question: '异常应该如何复核？' },
  ] };
  reply = body => response(answer(body));
  fetchMock.mockImplementation((url: string, options: RequestInit) => {
    if (options.method === 'POST') return reply(JSON.parse(options.body as string), options);
    if (url === '/api/v1/datasets') return response({ items: [dataset] });
    if (url === '/api/v1/intelligence/advisor') return response(config);
    if (url.startsWith('/api/v1/cities?')) return response({ items: [{ cityId: 'DL', cityName: '大连市' }] });
    throw new Error(`Unexpected request: ${url}`);
  });
});
afterEach(() => { app?.unmount(); app = undefined; vi.unstubAllGlobals(); });

describe('operations advisor component and HTTP contract', () => {
  it('loads four editable examples and posts an offline question pinned to the published date range', async () => {
    await mount(); expect(posts()).toHaveLength(0);
    expect(nodes().filter(item => item.kind === 'button' && item.textContent.endsWith('？'))).toHaveLength(4);
    await click('运营表现如何？'); expect(posts()).toHaveLength(0);
    await edit(node('textarea'), '大连当前运营表现如何？'); await edit(control('参谋城市'), 'DL'); await submit();
    expect(posts()).toHaveLength(1);
    expect(posts()[0][0]).toBe('/api/v1/intelligence/advisor');
    expect(JSON.parse(posts()[0][1].body)).toEqual({ question: '大连当前运营表现如何？', datasetId: 'D1', publishedBatchId: 'B1', startDate: '2026-05-25', endDate: '2026-06-01', cityId: 'DL', mode: 'offline', consent: false });
    expect(root.textContent).toContain('120 次充电会话'); expect(root.textContent).toContain('大连市'); expect(root.textContent).toContain('不含结束日');
    expect(root.textContent).toContain('发布批次 B1'); expect(root.textContent).toContain('/dashboard/overview'); expect(root.textContent).toContain('metrics.sessionCount');
    expect(root.textContent).toContain('模拟历史数据，不能解释因果。'); expect(navigate).not.toHaveBeenCalled();
    await click('查看运营总览'); expect(navigate).toHaveBeenCalledExactlyOnceWith('overview', {
      datasetId: 'D1', publishedBatchId: 'B1', cityId: 'DL', startDate: '2026-05-25', endDate: '2026-06-01',
    });
  });
  it('makes online mode unavailable until configured and requires fresh explicit consent per request', async () => {
    config.onlineAvailable = true; config.onlineProvider = '已配置模型';
    await mount(); await click('运营表现如何？'); await edit(control('参谋答复方式'), 'online');
    expect(node('button', '生成运营答复').props.disabled).toBe(true); expect(root.textContent).toContain('不发送问题原文');
    await submit(); expect(posts()).toHaveLength(0);
    await edit(control('允许本次发送问题意图和聚合统计'), true); await submit();
    expect(JSON.parse(posts()[0][1].body)).toMatchObject({ mode: 'online', consent: true });
    expect(node('button', '生成运营答复').props.disabled).toBe(true);
    await submit(); expect(posts()).toHaveLength(1);
  });
  it('disables unconfigured online mode and invalid date submissions', async () => {
    await mount(); expect(node('option', '在线模型 · 未配置').props.disabled).toBe(true);
    await click('运营表现如何？'); await edit(control('参谋开始日期'), '2027-01-01');
    expect(node('button', '生成运营答复').props.disabled).toBe(true); await submit(); expect(posts()).toHaveLength(0);
    expect(root.textContent).toContain('开始日期须早于结束日期');
  });
  it('will not ask without a published batch even when dataset discovery returns a row', async () => {
    const original = fetchMock.getMockImplementation()!;
    fetchMock.mockImplementation((url: string, options: RequestInit) => url === '/api/v1/datasets'
      ? response({ items: [{ ...dataset, publishedBatchId: null }] }) : original(url, options));
    await mount(); await edit(node('textarea'), '运营表现如何？'); await submit();
    expect(root.textContent).toContain('数据集缺少发布批次'); expect(posts()).toHaveLength(0);
  });
  it('revokes online consent when a question or range changes', async () => {
    config.onlineAvailable = true; await mount(); await click('运营表现如何？');
    await edit(control('参谋答复方式'), 'online'); await edit(control('允许本次发送问题意图和聚合统计'), true);
    expect(node('button', '生成运营答复').props.disabled).toBe(false);
    await edit(node('textarea'), '哪些站点需要关注？');
    expect(node('button', '生成运营答复').props.disabled).toBe(true); await submit(); expect(posts()).toHaveLength(0);
  });
  it('clears stale conclusions and aborts a pending answer when scope changes', async () => {
    await mount(); await click('运营表现如何？'); await submit(); expect(root.textContent).toContain('120 次充电会话');
    await edit(control('参谋城市'), 'DL'); expect(root.textContent).not.toContain('120 次充电会话');
    const pending = deferred(); reply = () => pending.promise; await submit();
    const [,_options] = posts()[1]; const body = JSON.parse(_options.body);
    await edit(control('参谋开始日期'), '2026-05-20'); expect(_options.signal.aborted).toBe(true);
    pending.resolve(await response(answer(body))); await settle(); expect(root.textContent).not.toContain('120 次充电会话');
  });
  it('stops receiving without claiming server-side cancellation and ignores late replies', async () => {
    const pending = deferred(); reply = () => pending.promise;
    await mount(); await click('运营表现如何？'); await submit(); const options = posts()[0][1];
    await click('停止接收'); expect(options.signal.aborted).toBe(true);
    expect(root.textContent).toContain('已停止接收答复'); expect(root.textContent).toContain('服务器可能仍在处理');
    pending.resolve(await response(answer(JSON.parse(options.body)))); await settle(); expect(root.textContent).not.toContain('120 次充电会话');
  });
  it('aborts active HTTP on unmount and does not navigate after a late answer', async () => {
    const pending = deferred(); reply = () => pending.promise;
    await mount(); await click('运营表现如何？'); await submit(); const options = posts()[0][1];
    app!.unmount(); app = undefined; expect(options.signal.aborted).toBe(true);
    pending.resolve(await response(answer(JSON.parse(options.body)))); await settle(); expect(navigate).not.toHaveBeenCalled();
  });
  it('renders unavailable evidence honestly without invented metrics or actions', async () => {
    reply = body => response(answer(body, { status: 'no_evidence', answer: '当前范围没有可用证据。', evidence: [], suggestions: [] }));
    await mount(); await click('运营表现如何？'); await submit();
    expect(root.textContent).toContain('当前范围证据不足'); expect(root.textContent).toContain('当前范围没有可用证据。');
    expect(root.textContent).not.toContain('120'); expect(navigate).not.toHaveBeenCalled();
  });
  it('shows only six primary evidence cards and keeps the remainder inside closed disclosure', async () => {
    reply = body => {
      const data = answer(body);
      data.evidence = Array.from({ length: 13 }, (_, index) => ({ ...data.evidence[0], id: `metric-${index}`, label: `证据指标 ${index + 1}`, value: index + 1 }));
      return response(data);
    };
    await mount(); await click('运营表现如何？'); await submit();
    const primary = control('答复依据');
    expect(primary.children.filter(item => item.kind === 'div')).toHaveLength(6);
    expect(primary.textContent).toContain('证据指标 6'); expect(primary.textContent).not.toContain('证据指标 7');
    const disclosure = nodes().find(item => item.kind === 'details' && String(item.props.class).includes('advisor-extra-evidence'))!;
    expect(disclosure.props.open).not.toBe(true);
    expect(disclosure.children.find(item => item.kind === 'summary')?.textContent).toBe('查看全部 13 项证据');
    expect(control('补充答复依据').children.filter(item => item.kind === 'div')).toHaveLength(7);
    expect(disclosure.textContent).toContain('证据指标 13');
    expect(nodes().some(item => item.kind === 'details' && item.textContent.includes('数据来源与发布批次'))).toBe(true);
  });
  it.each(['batch', 'scope', 'evidence', 'network'])('rejects %s failures with no confident answer', async failure => {
    reply = body => {
      if (failure === 'network') return Promise.reject(new Error('offline'));
      const data = answer(body);
      if (failure === 'scope') data.scope.cityId = 'another-city';
      if (failure === 'evidence') data.evidence = [];
      return response(data, { datasetId: 'D1', publishedBatchId: failure === 'batch' ? 'B2' : 'B1' });
    };
    await mount(); await click('运营表现如何？'); await submit();
    expect(nodes().some(item => item.props.role === 'alert')).toBe(true);
    expect(root.textContent).not.toContain('120 次充电会话'); expect(navigate).not.toHaveBeenCalled();
  });
  it('never renders unregistered navigation or HTML returned by the service', async () => {
    reply = body => response(answer(body, { answer: '<img src=x onerror=alert(1)>', suggestions: [{ target: 'admin' as any, label: '执行管理操作' }] }));
    await mount(); await click('运营表现如何？'); await submit();
    expect(root.textContent).toContain('<img src=x onerror=alert(1)>'); expect(nodes().some(item => item.kind === 'img')).toBe(false);
    expect(root.textContent).not.toContain('执行管理操作'); expect(navigate).not.toHaveBeenCalled();
  });
});
