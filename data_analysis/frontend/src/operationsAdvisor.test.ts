import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createRenderer, h, nextTick } from 'vue';
import * as Vue from 'vue';
import { compileScript, compileTemplate, parse } from '@vue/compiler-sfc';
import OperationsAdvisor from './components/OperationsAdvisor.vue';
import source from './components/OperationsAdvisor.vue?raw';
import type { AdvisorCapabilities, AdvisorRequest, AdvisorResponse } from './advisor';
import { advisorHistory, validateAdvisorResponse } from './advisor';

vi.mock('./components/Icon.vue', () => ({ default: { render: () => h('icon-stub') } }));
class TestNode {
  parent: TestNode | null = null; children: TestNode[] = []; props: Record<string, any> = {}; text = ''; value: any;
  selected = false; selectedIndex = -1; multiple = false; checked = false; listeners: Record<string, Function> = {};
  scrollHeight = 0; scrollTop = 0; clientHeight = 0;
  constructor(public kind: string) {}
  get tagName() { return this.kind.toUpperCase(); }
  get options() { return this.children.filter(child => child.kind === 'option'); }
  get textContent(): string { return this.text + this.children.map(child => child.textContent).join(''); }
  getBoundingClientRect() {
    let container = this.parent;
    while (container && container.props['aria-label'] !== '参谋对话') container = container.parent;
    return { top: 100 + (this.kind === 'article' ? 220 : 0) - (container?.scrollTop || 0) };
  }
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
const fetchMock = vi.fn(); const navigate = vi.fn(); const busy = vi.fn();
let root: TestNode; let app: ReturnType<typeof renderer.createApp> | undefined;
let advisorProps: { compact: boolean; active: boolean };
let advisorInstance: { pause: () => void } | null;
let reply: (body: AdvisorRequest, options: RequestInit) => Promise<Response>;
function response(data: unknown, meta = { datasetId: 'D1', publishedBatchId: 'B1' }) { return Promise.resolve(new Response(JSON.stringify({ code: 'OK', message: 'ok', data, meta }), { status: 200 })); }
function answer(body: AdvisorRequest, extra: Partial<AdvisorResponse> = {}): AdvisorResponse {
  return { status: 'answered', mode: body.mode, intent: 'overview', answer: '当前范围有 120 次充电会话，建议核查高峰期供给。',
    scope: { datasetId: body.datasetId, publishedBatchId: body.publishedBatchId, startDate: body.startDate, endDate: body.endDate, cityId: body.cityId || null, stationId: null, timeZone: 'Asia/Shanghai', dataKind: 'SIMULATED' },
    evidence: [{ id: 'sessions', label: '充电会话', value: 120, unit: '次', source: { endpoint: '/dashboard/overview', field: 'metrics.sessionCount' } }],
    citations: body.mode === 'online' ? ['sessions'] : [], knowledge: [],
    limitations: ['模拟历史数据，不能解释因果。'], suggestions: [{ target: 'overview', label: '运营总览' }], ...extra };
}
const posts = () => fetchMock.mock.calls.filter(([, options]) => options?.method === 'POST');
function nodes(node = root): TestNode[] { return [node, ...node.children.flatMap(child => nodes(child))]; }
function node(kind: string, text?: string) {
  const candidates = nodes().filter(item => item.kind === kind);
  const found = text ? candidates.find(item => item.textContent === text) || candidates.find(item => item.textContent.includes(text)) : candidates[0];
  if (!found) throw new Error(`Missing ${kind}: ${text}`); return found;
}
function control(label: string) { const found = nodes().find(item => item.props['aria-label'] === label); if (!found) throw new Error(`Missing control: ${label}`); return found; }
async function settle() { for (let i = 0; i < 35; i++) { await Promise.resolve(); await nextTick(); } }
async function edit(element: TestNode, value: unknown) { element.props['onUpdate:modelValue'](value); await settle(); }
async function click(text: string) { const button = node('button', text); expect(button.props.disabled).not.toBe(true); button.props.onClick({}); await settle(); }
async function submit() { node('form').props.onSubmit({ preventDefault() {} }); await settle(); }
async function mount(props: { compact?: boolean; active?: boolean } = {}) {
  advisorProps = Vue.reactive({ compact: false, active: true, ...props }); advisorInstance = null;
  root = new TestNode('root'); app = renderer.createApp({ setup: () => () => h(component, {
    ...advisorProps, onNavigate: navigate, onBusy: busy, ref: (instance: any) => { advisorInstance = instance; },
  }) }); app.provide(Vue.ssrContextKey, {}); app.mount(root); await settle();
}
function deferred() {
  let resolve!: (value: Response) => void; let reject!: (reason: Error) => void;
  const promise = new Promise<Response>((res, rej) => { resolve = res; reject = rej; }); return { promise, resolve, reject };
}

beforeEach(() => {
  class TestDocument {}
  vi.stubGlobal('Document', TestDocument); vi.stubGlobal('document', new TestDocument());
  navigate.mockReset(); busy.mockReset(); fetchMock.mockReset(); vi.stubGlobal('fetch', fetchMock);
  config = { defaultMode: 'offline', onlineAvailable: false, onlineProvider: null, maxQuestionLength: 300, disclosure: '将发送问题、最近对话、当前筛选聚合统计和知识片段。', supportedQuestions: [
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
afterEach(() => { app?.unmount(); app = undefined; vi.useRealTimers(); vi.unstubAllGlobals(); });

describe('operations advisor component and HTTP contract', () => {
  it.each(['你好', '您好！', '嗨', ' hello! ', 'HI?'])('posts greeting %s with online consent when the user sends it', async greeting => {
    config.defaultMode = 'online'; config.onlineAvailable = true; config.onlineProvider = 'AIPing · DeepSeek-V4.1-Flash';
    reply = body => response(answer(body, { status: 'chat', answer: '你好，需要我帮你分析哪方面？', evidence: [], citations: [], suggestions: [], limitations: [] }));
    await mount({ compact: true }); await edit(node('textarea'), greeting);
    expect(posts()).toHaveLength(0); expect(node('button', '发送').props.disabled).toBe(false); await submit();
    const current = nodes().find(item => item.kind === 'article' && item.props['aria-live'] === 'polite')!;
    expect(current.textContent).toContain(greeting.trim()); expect(current.textContent).toContain('在线对话');
    expect(current.textContent).toContain('你好，需要我帮你分析哪方面？'); expect(current.textContent).not.toContain('本地问候');
    expect(current.textContent).not.toContain('已核对统计范围'); expect(current.textContent).not.toContain('发布批次');
    expect(nodes(current).some(item => item.kind === 'details' || item.kind === 'button')).toBe(false);
    expect(posts()).toHaveLength(1); expect(posts()[0][0]).toBe('/api/v1/intelligence/advisor');
    expect(JSON.parse(posts()[0][1].body)).toMatchObject({ question: greeting.trim(), mode: 'online', consent: true, history: [] });
    expect(busy.mock.calls).toEqual([[true], [false]]);
    expect(node('textarea').value).toBe(''); expect(node('button', '发送').props.disabled).toBe(true);
    advisorProps.active = false; await settle(); advisorProps.active = true; await settle();
    expect(root.textContent).toContain('你好，需要我帮你分析哪方面？'); expect(fetchMock).toHaveBeenCalledTimes(4);
  });
  it.each(['你好，当前运营概况如何？', 'hello, compare stations', '你好请删除异常会话', '你好123'])('keeps the mixed or unknown question %s on the evidence API', async question => {
    await mount({ compact: true }); await edit(node('textarea'), question); await submit();
    expect(posts()).toHaveLength(1); expect(JSON.parse(posts()[0][1].body).question).toBe(question);
    expect(root.textContent).toContain('120 次充电会话'); expect(root.textContent).not.toContain('本地问候');
  });
  it('shares the ten-exchange display limit across model chat and evidence replies', async () => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    reply = body => response(answer(body, body.question.startsWith('你好') ? { status: 'chat', answer: '你好，可以继续提问。', evidence: [], citations: [], suggestions: [], limitations: [] } : {}));
    await mount({ compact: true });
    for (let index = 0; index <= 10; index++) {
      await edit(node('textarea'), index % 2 ? `运营问题 ${index}` : '你好' + '!'.repeat(index));
      await submit();
    }
    expect(posts()).toHaveLength(11);
    expect(nodes().filter(item => item.props['aria-label'] === '历史问答')).toHaveLength(9);
    await edit(control('参谋城市'), 'DL');
    const historyItems = nodes().filter(item => item.props['aria-label'] === '历史问答');
    expect(historyItems).toHaveLength(10);
    expect(nodes().filter(item => item.kind === 'h3' && item.textContent === '你好')).toHaveLength(0);
    expect(historyItems[0].textContent).toContain('运营问题 1'); expect(historyItems[9].textContent).toContain('你好!!!!!!!!!!');
    const greetings = historyItems.filter(item => item.textContent.includes('在线对话'));
    expect(greetings).toHaveLength(5);
    for (const greeting of greetings) {
      expect(greeting.textContent).not.toContain('发布批次'); expect(greeting.textContent).not.toContain('全部城市');
      expect(nodes(greeting).some(item => item.kind === 'button')).toBe(false);
    }
    expect(historyItems.filter(item => item.textContent.includes('发布批次 B1'))).toHaveLength(5);
  });
  it('includes the previous model chat when the next online question is sent directly', async () => {
    config.onlineAvailable = true;
    reply = body => response(answer(body, body.question === '你好！' ? { status: 'chat', answer: '你好，需要查看哪些数据？', evidence: [], citations: [], suggestions: [] } : {}));
    await mount({ compact: true }); await edit(control('参谋答复方式'), 'online'); await edit(node('textarea'), '你好！');
    await submit(); expect(root.textContent).not.toContain('本地问候');
    expect(root.textContent).toContain('在线对话'); expect(posts()).toHaveLength(1);
    await edit(node('textarea'), '当前运营概况如何？');
    expect(node('button', '发送').props.disabled).toBe(false); await submit(); expect(posts()).toHaveLength(2);
    expect(JSON.parse(posts()[1][1].body)).toMatchObject({ question: '当前运营概况如何？', mode: 'online', consent: true, history: [
      { role: 'user', content: '你好！' }, { role: 'assistant', content: '你好，需要查看哪些数据？' },
    ] });
  });
  it('follows API chat while preserving the position of someone reading older replies', async () => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    reply = body => response(answer(body, { status: 'chat', answer: '你好，需要查看哪些数据？', evidence: [], citations: [], suggestions: [] }));
    await mount({ compact: true }); const conversation = control('参谋对话');
    conversation.scrollHeight = 1000; conversation.clientHeight = 400; conversation.scrollTop = 600; conversation.props.onScroll({});
    await edit(node('textarea'), '你好'); await submit(); expect(conversation.scrollTop).toBe(212);
    conversation.props.onScroll({});
    conversation.scrollTop = 100; conversation.props.onScroll({});
    await edit(node('textarea'), '您好'); await submit(); expect(conversation.scrollTop).toBe(100);
    expect(root.scrollTop).toBe(0); expect(posts()).toHaveLength(2);
  });
  it('keeps all greetings on the API and rejects evidence-free offline factual replies', async () => {
    await mount(); await edit(node('textarea'), '你好'); await submit(); expect(posts()).toHaveLength(1);
    app!.unmount(); app = undefined; fetchMock.mockClear();
    await mount({ compact: true }); await edit(node('textarea'), '你好'); await submit();
    reply = body => response(answer(body, { evidence: [] }));
    await edit(node('textarea'), '当前运营概况如何？'); await submit();
    expect(posts()).toHaveLength(2); expect(root.textContent).toContain('服务未返回支持答复的数据证据');
    expect(nodes().some(item => item.kind === 'article' && item.props['aria-live'] === 'polite')).toBe(false);
  });
  it('retains at most ten completed compact exchanges with their own scope and no historical actions', async () => {
    await mount({ compact: true }); await edit(control('参谋城市'), 'DL');
    for (let index = 1; index <= 11; index++) {
      await edit(node('textarea'), `历史问题 ${String(index).padStart(2, '0')}`); await submit();
    }
    expect(nodes().filter(item => item.props['aria-label'] === '历史问答')).toHaveLength(9);
    await edit(node('textarea'), '新问题草稿'); await edit(control('参谋城市'), ''); await edit(control('参谋开始日期'), '2026-05-20');
    const historyItems = nodes().filter(item => item.props['aria-label'] === '历史问答');
    expect(historyItems).toHaveLength(10);
    expect(historyItems[0].textContent).toContain('历史问题 02'); expect(historyItems[9].textContent).toContain('历史问题 11');
    for (const item of historyItems) {
      expect(item.textContent).toContain('大连市 · 2026-05-25 至 2026-06-01');
      expect(nodes(item).some(child => child.kind === 'button')).toBe(false);
      expect(nodes(item).find(child => child.kind === 'details')?.props.open).not.toBe(true);
    }
    expect(posts()).toHaveLength(11);
    expect(JSON.parse(posts()[10][1].body)).toEqual({ question: '历史问题 11', datasetId: 'D1', publishedBatchId: 'B1', cityId: 'DL', startDate: '2026-05-25', endDate: '2026-06-01', mode: 'offline', consent: false });
    expect(navigate).not.toHaveBeenCalled();
  });
  it('shows a pending question bubble and emits busy only while receiving an answer', async () => {
    const pending = deferred(); reply = () => pending.promise;
    await mount({ compact: true }); expect(busy).not.toHaveBeenCalled();
    await click('运营表现如何？'); await submit();
    const bubble = nodes().find(item => String(item.props.class).includes('advisor-pending-question'))!;
    expect(bubble.textContent).toBe('当前范围运营表现如何？'); expect(busy.mock.calls).toEqual([[true]]);
    expect(node('textarea').value).toBe(''); expect(posts()[0][1].signal.aborted).toBe(false);
    expect(nodes().some(item => String(item.props.class).includes('advisor-typing-dots'))).toBe(true);
    pending.resolve(await response(answer(JSON.parse(posts()[0][1].body)))); await settle();
    expect(busy.mock.calls).toEqual([[true], [false]]);
    expect(nodes().some(item => String(item.props.class).includes('advisor-pending-question'))).toBe(false);
    expect(root.textContent).toContain('120 次充电会话'); expect(node('button', '发送').props.disabled).toBe(true);
    await submit(); expect(posts()).toHaveLength(1);
  });
  it('sends with Enter while allowing Shift+Enter and Chinese input composition', async () => {
    await mount({ compact: true }); await click('运营表现如何？'); const textarea = node('textarea');
    const preventDefault = vi.fn();
    textarea.props.onKeydown({ key: 'Enter', shiftKey: true, preventDefault });
    textarea.props.onKeydown({ key: 'Enter', isComposing: true, preventDefault });
    textarea.props.onKeydown({ key: 'Enter', keyCode: 229, preventDefault });
    textarea.props.onCompositionstart({}); textarea.props.onKeydown({ key: 'Enter', preventDefault });
    await settle(); expect(posts()).toHaveLength(0); expect(preventDefault).not.toHaveBeenCalled();
    textarea.props.onCompositionend({}); textarea.props.onKeydown({ key: 'Enter', preventDefault }); await settle();
    expect(preventDefault).toHaveBeenCalledOnce(); expect(posts()).toHaveLength(1);
  });
  it.each(['offline', 'online'] as const)('keeps a pending %s answer while drafting the next chat question', async mode => {
    config.onlineAvailable = true;
    const pending = deferred(); reply = () => pending.promise;
    await mount({ compact: true }); await click('运营表现如何？');
    await edit(control('参谋答复方式'), mode);
    await submit(); const [url, options] = posts()[0]; const firstQuestion = JSON.parse(options.body);
    expect(url).toBe('/api/v1/intelligence/advisor');
    expect(firstQuestion).toMatchObject({ question: '当前范围运营表现如何？', mode, consent: mode === 'online' });
    await edit(node('textarea'), '哪些电站需要关注？');
    expect(options.signal.aborted).toBe(false);
    expect(root.textContent).toContain('当前范围运营表现如何？');
    expect(root.textContent).not.toContain('已停止接收');
    expect(busy.mock.calls).toEqual([[true]]);
    await submit(); expect(posts()).toHaveLength(1);
    pending.resolve(await response(answer(firstQuestion))); await settle();
    expect(root.textContent).toContain('120 次充电会话');
    expect(node('textarea').value).toBe('哪些电站需要关注？');
    expect(busy.mock.calls).toEqual([[true], [false]]);
    expect(nodes().filter(item => item.props['aria-label'] === '历史问答')).toHaveLength(0);
    expect(nodes().some(item => String(item.props.class).includes('advisor-pending-question'))).toBe(false);
    expect(node('button', '发送').props.disabled).toBe(false);
    await edit(node('textarea'), '哪些电站需要优先关注？');
    expect(node('button', '发送').props.disabled).toBe(false);
    const nextQuestion = node('textarea').value;
    reply = body => response(answer(body)); await submit();
    expect(posts()).toHaveLength(2);
    expect(JSON.parse(posts()[1][1].body)).toMatchObject({ question: nextQuestion, mode, consent: mode === 'online' });
    const historyItems = nodes().filter(item => item.props['aria-label'] === '历史问答');
    expect(historyItems).toHaveLength(1);
    expect(historyItems[0].textContent).toContain('当前范围运营表现如何？');
    expect(nodes().filter(item => item.kind === 'h3' && item.textContent === '当前范围运营表现如何？')).toHaveLength(1);
    expect(nodes().filter(item => item.kind === 'h3' && item.textContent === nextQuestion)).toHaveLength(1);
  });
  it.each(['http', 'network', 'validation'])('restores an untouched compact question after a %s failure and retries only when explicitly clicked', async failure => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    const pending = deferred(); reply = () => pending.promise;
    await mount({ compact: true }); await edit(node('textarea'), '需要重试的原问题'); await submit();
    const options = posts()[0][1];
    expect(node('textarea').value).toBe(''); expect(options.signal.aborted).toBe(false);
    if (failure === 'network') pending.reject(new Error('offline'));
    else if (failure === 'http') pending.resolve(new Response(JSON.stringify({ code: 'ADVISOR_UNAVAILABLE', message: '在线模型暂不可用，请稍后重试。' }), { status: 503 }));
    else pending.resolve(await response(answer(JSON.parse(options.body), { citations: [] })));
    await settle();
    expect(nodes().some(item => item.props.role === 'alert')).toBe(true);
    expect(nodes().some(item => item.kind === 'article')).toBe(false);
    expect(node('textarea').value).toBe('需要重试的原问题'); expect(node('button', '发送').props.disabled).toBe(false);
    expect(busy.mock.calls).toEqual([[true], [false]]);
    expect(root.textContent).toContain('需要重试的原问题'); expect(node('button', '重试发送').props.disabled).not.toBe(true);
    expect(nodes().some(item => item.kind === 'button' && item.textContent.includes('重新载入'))).toBe(false);
    expect(posts()).toHaveLength(1); expect(fetchMock).toHaveBeenCalledTimes(4);
    reply = body => response(answer(body)); await click('重试发送');
    expect(posts()).toHaveLength(2);
    expect(JSON.parse(posts()[1][1].body)).toEqual(JSON.parse(options.body)); expect(fetchMock).toHaveBeenCalledTimes(5);
    expect(JSON.parse(posts()[1][1].body)).toMatchObject({ question: '需要重试的原问题', mode: 'online', consent: true, history: [] });
    expect(node('textarea').value).toBe(''); expect(root.textContent).toContain('120 次充电会话');
    expect(nodes().some(item => item.kind === 'button' && item.textContent === '重试发送')).toBe(false);
  });
  it.each([
    { label: 'a newer draft', edits: ['新的草稿'], expected: '新的草稿' },
    { label: 'an intentionally cleared draft', edits: ['曾经输入的新问题', ''], expected: '' },
    { label: 'a whitespace-only draft', edits: ['新的问题', '   '], expected: '   ' },
    { label: 'a whitespace edit followed by clearing', edits: ['   ', ''], expected: '' },
  ])('does not restore a failed question over $label', async ({ edits, expected }) => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    const pending = deferred(); reply = () => pending.promise;
    await mount({ compact: true }); await edit(node('textarea'), '已经提交的旧问题'); await submit();
    const options = posts()[0][1]; expect(node('textarea').value).toBe('');
    for (const draft of edits) await edit(node('textarea'), draft);
    expect(options.signal.aborted).toBe(false);
    pending.reject(new Error('offline')); await settle();
    expect(nodes().some(item => item.props.role === 'alert')).toBe(true);
    expect(node('textarea').value).toBe(expected);
    expect(node('button', '发送').props.disabled).toBe(!expected.trim());
    expect(posts()).toHaveLength(1); expect(busy.mock.calls).toEqual([[true], [false]]);
    reply = body => response(answer(body)); await click('重试发送');
    expect(posts()).toHaveLength(2); expect(JSON.parse(posts()[1][1].body)).toEqual(JSON.parse(options.body));
    expect(node('textarea').value).toBe(expected); expect(node('button', '发送').props.disabled).toBe(!expected.trim());
    expect(fetchMock).toHaveBeenCalledTimes(5);
  });
  it('keeps prior answers and a new draft through failure, explicit retry, success and the next question', async () => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    const firstQuestion = '先分析当前运营概况'; const firstAnswer = '首问的已核对答复。';
    const failedQuestion = '继续分析需要关注的电站'; const retriedAnswer = '重试后完成的电站分析。'; const nextQuestion = '接下来应该核查什么？';
    reply = body => response(answer(body, { answer: firstAnswer }));
    await mount({ compact: true }); await edit(control('参谋城市'), 'DL'); await edit(control('参谋开始日期'), '2026-05-20');
    await edit(node('textarea'), firstQuestion); await submit();
    expect(posts()).toHaveLength(1); expect(fetchMock).toHaveBeenCalledTimes(4);
    const failure = deferred(); reply = () => failure.promise;
    await edit(node('textarea'), failedQuestion); await submit(); const failedBody = JSON.parse(posts()[1][1].body);
    expect(failedBody).toMatchObject({ question: failedQuestion, cityId: 'DL', startDate: '2026-05-20', mode: 'online', consent: true });
    expect(failedBody.history).toEqual([{ role: 'user', content: firstQuestion }, { role: 'assistant', content: firstAnswer }]);
    await edit(node('textarea'), nextQuestion); failure.reject(new Error('offline')); await settle();
    expect(root.textContent).toContain(firstAnswer); expect(root.textContent).toContain(failedQuestion);
    expect(node('textarea').value).toBe(nextQuestion); expect(posts()).toHaveLength(2); expect(fetchMock).toHaveBeenCalledTimes(5);
    expect(nodes().filter(item => item.props['aria-label'] === '历史问答')).toHaveLength(1);
    expect(nodes().some(item => item.kind === 'button' && item.textContent.includes('重新载入'))).toBe(false);
    const retry = deferred(); reply = () => retry.promise;
    const retryButton = node('button', '重试发送');
    retryButton.props.onClick({}); retryButton.props.onClick({}); await settle();
    expect(posts()).toHaveLength(3); expect(JSON.parse(posts()[2][1].body)).toEqual(failedBody);
    expect(node('textarea').value).toBe(nextQuestion); expect(fetchMock).toHaveBeenCalledTimes(6);
    await submit(); expect(posts()).toHaveLength(3);
    retry.resolve(await response(answer(failedBody, { answer: retriedAnswer }))); await settle();
    expect(root.textContent).toContain(firstAnswer); expect(root.textContent).toContain(retriedAnswer);
    expect(node('textarea').value).toBe(nextQuestion); expect(posts()).toHaveLength(3);
    expect(nodes().some(item => item.kind === 'button' && item.textContent === '重试发送')).toBe(false);
    reply = body => response(answer(body)); await submit();
    expect(posts()).toHaveLength(4); expect(fetchMock).toHaveBeenCalledTimes(7);
    expect(JSON.parse(posts()[3][1].body)).toMatchObject({ question: nextQuestion, cityId: 'DL', startDate: '2026-05-20', mode: 'online', consent: true });
    expect(JSON.parse(posts()[3][1].body).history).toEqual([
      { role: 'user', content: firstQuestion }, { role: 'assistant', content: firstAnswer },
      { role: 'user', content: failedQuestion }, { role: 'assistant', content: retriedAnswer },
    ]);
    expect(nodes().filter(item => item.props['aria-label'] === '历史问答')).toHaveLength(2);
    expect(busy.mock.calls).toEqual([[true], [false], [true], [false], [true], [false], [true], [false]]);
  });
  it.each([
    { label: 'a replacement draft', edits: ['失败后写下的新问题'], expected: '失败后写下的新问题' },
    { label: 'the same text after intentional editing', edits: ['临时的新问题', '需要重试的原问题'], expected: '需要重试的原问题' },
    { label: 'an intentionally cleared restored question', edits: [''], expected: '' },
    { label: 'a whitespace-only replacement', edits: ['   '], expected: '   ' },
  ])('preserves $label when retrying a previously restored question', async ({ edits, expected }) => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    reply = () => Promise.reject(new Error('offline'));
    await mount({ compact: true }); await edit(node('textarea'), '需要重试的原问题'); await submit();
    const failedBody = JSON.parse(posts()[0][1].body); expect(node('textarea').value).toBe(failedBody.question);
    for (const draft of edits) await edit(node('textarea'), draft);
    expect(posts()).toHaveLength(1);
    const retry = deferred(); reply = () => retry.promise; await click('重试发送');
    expect(node('textarea').value).toBe(expected); expect(JSON.parse(posts()[1][1].body)).toEqual(failedBody);
    retry.resolve(await response(answer(failedBody))); await settle();
    expect(node('textarea').value).toBe(expected); expect(posts()).toHaveLength(2); expect(fetchMock).toHaveBeenCalledTimes(5);
  });
  it.each([
    ['参谋城市', 'DL'], ['参谋开始日期', '2026-05-20'],
    ['参谋结束日期', '2026-05-30'], ['参谋答复方式', 'offline'],
  ])('invalidates a failed request when %s changes even if its old retry handler is invoked', async (label, value) => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    reply = () => Promise.reject(new Error('offline'));
    await mount({ compact: true }); await edit(node('textarea'), '旧范围的失败问题'); await submit();
    const retryClick = node('button', '重试发送').props.onClick;
    await edit(node('textarea'), '新范围的草稿'); await edit(control(label), value);
    expect(nodes().some(item => item.kind === 'button' && item.textContent === '重试发送')).toBe(false);
    retryClick({}); await settle();
    expect(posts()).toHaveLength(1); expect(fetchMock).toHaveBeenCalledTimes(4); expect(node('textarea').value).toBe('新范围的草稿');
    reply = body => response(answer(body)); await submit();
    expect(posts()).toHaveLength(2); expect(JSON.parse(posts()[1][1].body).question).toBe('新范围的草稿');
  });
  it.each(['stop', 'scope', 'close'])('cancels a pending retry on %s and ignores its late answer', async action => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    reply = () => Promise.reject(new Error('offline'));
    await mount({ compact: true }); await edit(node('textarea'), '需要重试的原问题'); await submit();
    const retry = deferred(); reply = () => retry.promise; await click('重试发送'); const options = posts()[1][1];
    expect(node('textarea').value).toBe(''); expect(options.signal.aborted).toBe(false);
    await edit(node('textarea'), '取消后保留的草稿');
    if (action === 'stop') await click('停止接收');
    else if (action === 'scope') await edit(control('参谋城市'), 'DL');
    else { advisorProps.active = false; await settle(); }
    expect(options.signal.aborted).toBe(true);
    retry.resolve(await response(answer(JSON.parse(options.body), { answer: '不应出现的迟到重试答复。' }))); await settle();
    expect(root.textContent).not.toContain('不应出现的迟到重试答复。'); expect(node('textarea').value).toBe('取消后保留的草稿');
    expect(nodes().filter(item => item.props['aria-label'] === '历史问答')).toHaveLength(0);
    expect(posts()).toHaveLength(2); expect(fetchMock).toHaveBeenCalledTimes(5);
    expect(busy.mock.calls).toEqual([[true], [false], [true], [false]]);
  });
  it('offers setup reload only for initialization failures and does not post while reloading', async () => {
    const original = fetchMock.getMockImplementation()!; let failDiscovery = true;
    fetchMock.mockImplementation((url: string, options: RequestInit) => failDiscovery && url === '/api/v1/datasets'
      ? Promise.reject(new Error('offline')) : original(url, options));
    await mount({ compact: true });
    expect(nodes().some(item => item.props.role === 'alert')).toBe(true);
    expect(nodes().some(item => item.kind === 'button' && item.textContent === '重试发送')).toBe(false);
    expect(posts()).toHaveLength(0); failDiscovery = false; await click('重新载入');
    expect(nodes().some(item => item.props.role === 'alert')).toBe(false); expect(posts()).toHaveLength(0);
    await edit(node('textarea'), '设置恢复后发送的问题'); expect(node('button', '发送').props.disabled).toBe(false);
  });
  it.each([
    ['参谋城市', 'DL'], ['参谋开始日期', '2026-05-20'],
    ['参谋结束日期', '2026-05-30'], ['参谋答复方式', 'online'],
  ])('still invalidates pending chat when %s changes', async (label, value) => {
    config.onlineAvailable = true;
    const pending = deferred(); reply = () => pending.promise;
    await mount({ compact: true }); await click('运营表现如何？'); await submit();
    const options = posts()[0][1];
    await edit(node('textarea'), '下一问草稿'); await edit(control(label), value);
    expect(options.signal.aborted).toBe(true);
    expect(node('textarea').value).toBe('下一问草稿');
    pending.resolve(await response(answer(JSON.parse(options.body)))); await settle();
    expect(root.textContent).not.toContain('120 次充电会话');
    expect(nodes().filter(item => item.props['aria-label'] === '历史问答')).toHaveLength(0);
    expect(busy.mock.calls).toEqual([[true], [false]]);
  });
  it('follows new messages only when the reader is near the conversation bottom', async () => {
    const pending = deferred(); reply = () => pending.promise;
    await mount({ compact: true }); const conversation = control('参谋对话');
    conversation.scrollHeight = 1000; conversation.clientHeight = 400; conversation.scrollTop = 600; conversation.props.onScroll({});
    await click('运营表现如何？'); await submit(); expect(conversation.scrollTop).toBe(1000);
    conversation.scrollTop = 200; conversation.props.onScroll({});
    pending.resolve(await response(answer(JSON.parse(posts()[0][1].body)))); await settle(); expect(conversation.scrollTop).toBe(200);
    conversation.scrollTop = 600; conversation.props.onScroll({});
    reply = body => response(answer(body)); await edit(node('textarea'), '另一个问题'); await submit();
    expect(conversation.scrollTop).toBe(212);
    conversation.props.onScroll({});
    const anotherPending = deferred(); reply = () => anotherPending.promise;
    await edit(node('textarea'), '继续提问'); await submit();
    expect(conversation.scrollTop).toBe(1000);
  });
  it('reveals the start of a completed answer and keeps limitations collapsed', async () => {
    await mount({ compact: true }); const conversation = control('参谋对话');
    conversation.scrollHeight = 1600; conversation.clientHeight = 400;
    await click('运营表现如何？'); await submit();
    expect(conversation.scrollTop).toBe(212);
    const limitations = nodes().find(item => item.kind === 'details' && String(item.props.class).includes('advisor-compact-limits'))!;
    expect(limitations.props.open).not.toBe(true); expect(limitations.textContent).toContain('模拟历史数据，不能解释因果。');
    expect(root.scrollTop).toBe(0);
  });
  it('does not initialize or submit while inactive and initializes once opened', async () => {
    await mount({ compact: true, active: false }); expect(fetchMock).not.toHaveBeenCalled();
    await edit(node('textarea'), '运营表现如何？'); await submit(); expect(fetchMock).not.toHaveBeenCalled();
    advisorProps.active = true; await settle(); expect(fetchMock).toHaveBeenCalledTimes(3);
    await submit(); expect(posts()).toHaveLength(1);
  });
  it('renders compact chat with collapsed settings and evidence while retaining scope controls', async () => {
    await mount({ compact: true });
    expect(root.textContent).toContain('运营参谋');
    expect(nodes().some(item => item.kind === 'h2')).toBe(false);
    const settings = nodes().find(item => item.kind === 'details' && String(item.props.class).includes('advisor-settings'))!;
    expect(settings.props.open).toBe(false);
    expect(control('参谋开始日期')).toBeDefined(); expect(control('参谋结束日期')).toBeDefined();
    await click('运营表现如何？'); await edit(control('参谋城市'), 'DL'); await submit();
    expect(JSON.parse(posts()[0][1].body)).toMatchObject({ cityId: 'DL', publishedBatchId: 'B1' });
    const evidence = nodes().find(item => item.kind === 'details' && String(item.props.class).includes('advisor-compact-evidence'))!;
    expect(evidence.props.open).not.toBe(true); expect(evidence.textContent).toContain('查看 1 项数据依据');
    expect(root.textContent).toContain('120 次充电会话'); expect(node('button', '发送')).toBeDefined();
  });
  it.each([true, false])('keeps a brief online disclosure inside settings and has no consent checkbox when compact=%s', async compact => {
    config.defaultMode = 'online'; config.onlineAvailable = true; config.onlineProvider = 'AIPing · DeepSeek-V4.1-Flash';
    await mount({ compact });
    const settings = nodes().find(item => item.kind === 'details' && String(item.props.class).includes('advisor-settings'))!;
    expect(settings.props.open).toBe(!compact);
    const disclosure = nodes(settings).find(item => item.kind === 'p' && item.textContent.includes('在线发送'))!;
    expect(disclosure).toBeDefined(); expect(disclosure.textContent).toContain('AIPing');
    for (const detail of ['问题', '最近对话', '聚合统计', '知识来源']) expect(disclosure.textContent).toContain(detail);
    expect(disclosure.textContent.length).toBeLessThan(100);
    expect(nodes().filter(item => item.kind === 'p' && item.textContent.includes('在线发送'))).toEqual([disclosure]);
    expect(nodes().some(item => item.kind === 'input' && item.props.type === 'checkbox')).toBe(false);
    expect(root.textContent).not.toContain('每次提问需重新勾选');
    await edit(node('textarea'), '直接发送的问题');
    expect(node('button', compact ? '发送' : '生成运营答复').props.disabled).toBe(false); expect(posts()).toHaveLength(0);
  });
  it('pauses pending chat on close, ignores late replies and can send the draft after reopening', async () => {
    config.onlineAvailable = true;
    const pending = deferred(); reply = () => pending.promise;
    await mount({ compact: true }); await click('运营表现如何？'); await edit(control('参谋答复方式'), 'online');
    await submit(); const options = posts()[0][1]; await edit(node('textarea'), '重新打开后继续的问题');
    advisorProps.active = false; await settle(); expect(options.signal.aborted).toBe(true);
    pending.resolve(await response(answer(JSON.parse(options.body)))); await settle();
    expect(root.textContent).not.toContain('120 次充电会话');
    advisorProps.active = true; await settle(); expect(node('button', '发送').props.disabled).toBe(false);
    expect(node('textarea').value).toBe('重新打开后继续的问题');
    expect(posts()).toHaveLength(1);
    reply = body => response(answer(body)); await submit();
    expect(posts()).toHaveLength(2);
    expect(JSON.parse(posts()[1][1].body)).toMatchObject({ question: '重新打开后继续的问题', mode: 'online', consent: true, history: [] });
  });
  it('preserves a completed reply across pause and reopening without additional requests', async () => {
    config.onlineAvailable = true;
    await mount({ compact: true }); await click('运营表现如何？'); await edit(control('参谋答复方式'), 'online');
    await submit(); await edit(node('textarea'), '保留的新问题');
    advisorInstance!.pause(); await settle();
    advisorProps.active = false; await settle(); advisorProps.active = true; await settle();
    expect(root.textContent).toContain('120 次充电会话'); expect(posts()).toHaveLength(1);
    expect(node('textarea').value).toBe('保留的新问题'); expect(node('button', '发送').props.disabled).toBe(false);
    expect(fetchMock).toHaveBeenCalledTimes(4);
  });
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
  it('sends configured online requests directly with consent and keeps the next send available', async () => {
    config.onlineAvailable = true; config.onlineProvider = '已配置模型';
    await mount(); await click('运营表现如何？'); await edit(control('参谋答复方式'), 'online');
    expect(node('button', '生成运营答复').props.disabled).toBe(false); expect(posts()).toHaveLength(0);
    await submit();
    expect(JSON.parse(posts()[0][1].body)).toMatchObject({ mode: 'online', consent: true });
    await edit(node('textarea'), '还需要关注什么？'); expect(node('button', '生成运营答复').props.disabled).toBe(false);
    await submit(); expect(posts()).toHaveLength(2);
    expect(JSON.parse(posts()[1][1].body)).toMatchObject({ question: '还需要关注什么？', mode: 'online', consent: true });
  });
  it('disables unconfigured online mode and invalid date submissions', async () => {
    await mount(); expect(node('option', '在线模型 · 未配置').props.disabled).toBe(true);
    expect(root.textContent).toContain('在后端配置 AIPing API Key'); expect(root.textContent).toContain('密钥仅保存在后端');
    await click('运营表现如何？'); await edit(control('参谋答复方式'), 'online');
    expect(node('button', '生成运营答复').props.disabled).toBe(true); await submit(); expect(posts()).toHaveLength(0);
    await edit(control('参谋答复方式'), 'offline');
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
  it('allows an edited online question and valid range to be sent without another control', async () => {
    config.onlineAvailable = true; await mount(); await click('运营表现如何？');
    await edit(control('参谋答复方式'), 'online');
    expect(node('button', '生成运营答复').props.disabled).toBe(false);
    await edit(node('textarea'), '哪些站点需要关注？'); await edit(control('参谋城市'), 'DL'); await edit(control('参谋开始日期'), '2026-05-20');
    expect(node('button', '生成运营答复').props.disabled).toBe(false); await submit(); expect(posts()).toHaveLength(1);
    expect(JSON.parse(posts()[0][1].body)).toMatchObject({ question: '哪些站点需要关注？', cityId: 'DL', startDate: '2026-05-20', mode: 'online', consent: true });
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
  it('accepts cited knowledge-only model answers and renders their sources as text', async () => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    const knowledge = [{ id: 'knowledge-model', title: '模型说明', text: '<img src=x onerror=alert(1)>需要结合时间范围理解预测。', source: 'docs/model-guide.md' }];
    reply = body => response(answer(body, { evidence: [], citations: ['knowledge-model'], knowledge, answer: '预测误差应结合模型说明理解。[knowledge-model]' }));
    await mount({ compact: true }); await edit(node('textarea'), '预测结果怎么理解？'); await submit();
    expect(root.textContent).toContain('模型解读 · 请结合来源复核'); expect(root.textContent).not.toContain('已核对统计范围');
    const citations = control('答复引用'); expect(citations.kind).toBe('details'); expect(citations.props.open).not.toBe(true);
    expect(citations.children.find(item => item.kind === 'summary')?.textContent).toBe('查看 1 项引用来源');
    expect(citations.textContent).toContain('knowledge-model · 模型说明 · docs/model-guide.md');
    expect(control('知识来源').kind).toBe('details'); expect(control('知识来源').props.open).not.toBe(true);
    expect(control('知识来源').textContent).toContain('<img src=x onerror=alert(1)>');
    expect(nodes().some(item => item.kind === 'img')).toBe(false);
  });
  it.each([true, false])('keeps current and archived citation sources collapsed with accurate counts when compact=%s', async compact => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    const knowledge = [{ id: 'knowledge-model', title: '模型说明', text: '需要结合时间范围理解预测。', source: 'docs/model-guide.md' }];
    reply = body => response(answer(body, { citations: ['sessions', 'knowledge-model'], knowledge, answer: '当前范围有 120 次充电会话。[sessions] 请结合模型说明理解。[knowledge-model]' }));
    await mount({ compact }); await edit(node('textarea'), '查看数据和知识来源'); await submit();
    const current = control('答复引用');
    expect(current.kind).toBe('details'); expect(current.props.open).not.toBe(true);
    expect(current.children.find(item => item.kind === 'summary')?.textContent).toBe('查看 2 项引用来源');
    expect(nodes(current).filter(item => item.kind === 'li')).toHaveLength(2);
    expect(current.textContent).toContain('sessions · 充电会话 · /dashboard/overview · metrics.sessionCount');
    expect(current.textContent).toContain('knowledge-model · 模型说明 · docs/model-guide.md');
    if (compact) {
      await edit(node('textarea'), '后续问题'); await submit();
      const archived = control('历史答复引用');
      expect(archived.kind).toBe('details'); expect(archived.props.open).not.toBe(true);
      expect(archived.children.find(item => item.kind === 'summary')?.textContent).toBe('查看 2 项引用来源');
      expect(nodes(archived).filter(item => item.kind === 'li')).toHaveLength(2);
      expect(archived.textContent).toContain('/dashboard/overview'); expect(archived.textContent).toContain('docs/model-guide.md');
      expect(control('历史知识来源').kind).toBe('details'); expect(control('历史知识来源').props.open).not.toBe(true);
      expect(control('答复引用').props.open).not.toBe(true);
    }
  });
  it.each([
    { citations: [] }, { citations: ['unknown-source'] }, { citations: ['sessions', 'unknown-source'] },
    { citations: null as any }, { citations: [17] as any },
    { knowledge: [{ id: 'missing-source', title: '说明', text: '内容' }] as any },
  ])('rejects invalid online sources before rendering factual claims: %j', async extra => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    reply = body => response(answer(body, extra));
    await mount({ compact: true }); await edit(node('textarea'), '当前运营情况？'); await submit();
    expect(nodes().some(item => item.props.role === 'alert')).toBe(true);
    expect(root.textContent).not.toContain('120 次充电会话'); expect(navigate).not.toHaveBeenCalled();
  });
  it('reports a model failure for a greeting without creating a substitute answer', async () => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    reply = () => Promise.resolve(new Response(JSON.stringify({ code: 'ADVISOR_UNAVAILABLE', message: '在线模型暂不可用，请稍后重试。' }), { status: 503 }));
    await mount({ compact: true }); await edit(node('textarea'), '你好'); await submit();
    expect(posts()).toHaveLength(1); expect(root.textContent).toContain('在线模型暂不可用');
    expect(nodes().some(item => item.kind === 'article')).toBe(false); expect(root.textContent).not.toContain('本地问候');
  });
  it('sends at most three prior exchanges and excludes a different city or date range', async () => {
    config.defaultMode = 'online'; config.onlineAvailable = true;
    reply = body => response(answer(body, { answer: `答复：${body.question}` }));
    await mount({ compact: true });
    for (let index = 1; index <= 5; index++) {
      await edit(node('textarea'), `问题${index}`); await submit();
    }
    expect(JSON.parse(posts()[4][1].body).history).toEqual([
      { role: 'user', content: '问题2' }, { role: 'assistant', content: '答复：问题2' },
      { role: 'user', content: '问题3' }, { role: 'assistant', content: '答复：问题3' },
      { role: 'user', content: '问题4' }, { role: 'assistant', content: '答复：问题4' },
    ]);
    await edit(control('参谋城市'), 'DL'); await edit(node('textarea'), '大连问题'); await submit();
    expect(JSON.parse(posts()[5][1].body).history).toEqual([]);
    await edit(control('参谋开始日期'), '2026-05-20'); await edit(node('textarea'), '新日期问题'); await submit();
    expect(JSON.parse(posts()[6][1].body).history).toEqual([]);
  });
  it('allows the model sixty seconds before the advisor-specific seventy-second timeout', async () => {
    vi.useFakeTimers();
    reply = (_body, options) => new Promise((_resolve, reject) => options.signal!.addEventListener('abort', () => reject(new Error('aborted')), { once: true }));
    await mount({ compact: true }); await edit(node('textarea'), '运营情况？'); await submit();
    const signal = posts()[0][1].signal;
    await vi.advanceTimersByTimeAsync(61000); expect(signal.aborted).toBe(false);
    await vi.advanceTimersByTimeAsync(9000); await settle(); expect(signal.aborted).toBe(true);
    expect(root.textContent).toContain('数据请求超时'); expect(nodes().some(item => item.kind === 'article')).toBe(false);
    expect(node('textarea').value).toBe('运营情况？'); expect(node('button', '发送').props.disabled).toBe(false);
  });
});

describe('advisor context and response boundary', () => {
  const request: AdvisorRequest = { question: '当前情况？', datasetId: 'D1', publishedBatchId: 'B1', startDate: '2026-05-25', endDate: '2026-06-01', mode: 'online', consent: true };
  it('defaults omitted legacy offline citation and knowledge fields to empty lists', () => {
    const offline = { ...request, mode: 'offline' as const, consent: false };
    const data = answer(offline); delete data.citations; delete data.knowledge;
    expect(validateAdvisorResponse(data, offline)).toMatchObject({ citations: [], knowledge: [] });
    expect(() => validateAdvisorResponse({ ...data, mode: 'online' }, request)).toThrow('有效引用');
  });
  it.each(['datasetId', 'publishedBatchId', 'startDate', 'endDate', 'cityId'] as const)('does not reuse history when %s differs', field => {
    const previous = answer(request); previous.scope[field] = 'another-scope';
    expect(advisorHistory([{ question: '旧问题', response: previous }], request)).toEqual([]);
  });
  it('bounds conversation size to six messages, eight hundred characters each and four thousand total', () => {
    const exchanges = Array.from({ length: 8 }, (_, index) => ({ question: `${index}`.repeat(300), response: answer(request, { answer: '答'.repeat(3000) }) }));
    const context = advisorHistory(exchanges, request);
    expect(context).toHaveLength(6); expect(context[0].content).toBe('5'.repeat(300));
    expect(context.map(item => item.role)).toEqual(['user', 'assistant', 'user', 'assistant', 'user', 'assistant']);
    expect(context.every(item => item.content.length <= 800)).toBe(true);
    expect(context.reduce((total, item) => total + item.content.length, 0)).toBeLessThanOrEqual(4000);
  });
  it.each([798, 799])('keeps whole emoji when truncating a reply after %i preceding characters', prefixLength => {
    const question = '你好😊';
    const context = advisorHistory([{ question, response: answer(request, { answer: '甲'.repeat(prefixLength) + '😊尾' }) }], request);
    expect(context[0].content).toBe(question);
    expect(context[1].content).toBe('甲'.repeat(prefixLength) + (prefixLength === 798 ? '😊' : ''));
    expect(context[1].content.length).toBeLessThanOrEqual(800);
    expect(() => encodeURIComponent(context[1].content)).not.toThrow();
  });
});
