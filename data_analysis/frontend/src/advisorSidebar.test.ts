import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createRenderer, h, nextTick } from 'vue';
import * as Vue from 'vue';
import { compileScript, compileTemplate, parse } from '@vue/compiler-sfc';
import AdvisorSidebar from './components/AdvisorSidebar.vue';
import source from './components/AdvisorSidebar.vue?raw';

const child = vi.hoisted(() => ({ mounts: 0, unmounts: 0 }));
const scope = vi.hoisted(() => ({
  datasetId: 'D1', publishedBatchId: 'B1', cityId: 'DL', startDate: '2026-05-25', endDate: '2026-06-01',
}));
vi.mock('./components/Icon.vue', () => ({ default: { render: () => h('icon-stub') } }));
vi.mock('./components/OperationsAdvisor.vue', async () => {
  const { defineComponent, h, onUnmounted, ref } = await import('vue');
  return { default: defineComponent({
    props: { compact: Boolean, active: Boolean },
    emits: ['navigate', 'busy'],
    setup(props, { emit }) {
      child.mounts++;
      onUnmounted(() => child.unmounts++);
      const draft = ref('');
      return () => h('advisor-stub', { compact: props.compact, active: props.active }, [
        h('textarea', { value: draft.value, onInput: (event: { target: { value: string } }) => { draft.value = event.target.value; } }),
        h('button', { onClick: () => emit('navigate', 'advanced', scope) }, '查看分析'),
        h('button', { onClick: () => emit('busy', true) }, '开始答复'),
        h('button', { onClick: () => emit('busy', false) }, '完成答复'),
      ]);
    },
  }) };
});

let focused: TestNode | undefined;
class TestNode {
  parent: TestNode | null = null;
  children: TestNode[] = [];
  props: Record<string, any> = {};
  style = { display: '' };
  text = '';
  constructor(public kind: string) {}
  get textContent(): string { return this.text + this.children.map(child => child.textContent).join(''); }
  focus() { focused = Vue.toRaw(this); }
}
function detach(node: TestNode) {
  if (!node.parent) return;
  const index = node.parent.children.indexOf(node);
  if (index >= 0) node.parent.children.splice(index, 1);
  node.parent = null;
}
const renderer = createRenderer<TestNode, TestNode>({
  createElement: tag => new TestNode(tag),
  createText: text => Object.assign(new TestNode('#text'), { text }),
  createComment: text => Object.assign(new TestNode('#comment'), { text }),
  setText: (node, text) => { node.text = text; },
  setElementText: (node, text) => { node.text = text; node.children = []; },
  parentNode: node => node.parent,
  nextSibling: node => node.parent?.children[node.parent.children.indexOf(node) + 1] || null,
  patchProp: (node, key, _previous, value) => { node.props[key] = value; },
  insert: (node, parent, anchor = null) => {
    detach(node); node.parent = parent;
    const index = anchor ? parent.children.indexOf(anchor) : -1;
    if (index < 0) parent.children.push(node); else parent.children.splice(index, 0, node);
  },
  remove: detach,
});
const { descriptor } = parse(source);
const bindings = compileScript(descriptor, { id: 'advisor-sidebar-test' }).bindings;
const compiled = compileTemplate({
  source: descriptor.template!.content, filename: 'AdvisorSidebar.vue', id: 'advisor-sidebar-test',
  compilerOptions: { bindingMetadata: bindings, hoistStatic: false },
});
if (compiled.errors.length) throw new Error(String(compiled.errors[0]));
const renderCode = compiled.code
  .replace(/import \{([^}]+)\} from "vue"/g, (_match, names: string) => `const {${names.replace(/ as /g, ': ')}} = Vue`)
  .replace('export function render', 'return function render');
const component = { ...AdvisorSidebar, render: new Function('Vue', renderCode)(Vue) } as unknown as Vue.Component;
const navigate = vi.fn();
let root: TestNode;
let app: ReturnType<typeof renderer.createApp> | undefined;
function nodes(node = root): TestNode[] { return [node, ...node.children.flatMap(child => nodes(child))]; }
function find(predicate: (node: TestNode) => boolean) {
  const result = nodes().find(predicate);
  if (!result) throw new Error('Expected rendered companion control');
  return result;
}
function control(label: string) { return find(node => node.props['aria-label'] === label); }
function panel() { return find(node => node.props.role === 'dialog'); }
function advisor() { return find(node => node.kind === 'advisor-stub'); }
async function settle() { await nextTick(); await nextTick(); }
async function click(node: TestNode) { node.props.onClick({}); await settle(); }
async function mount() {
  root = new TestNode('root');
  app = renderer.createApp({ setup: () => () => h(component, { onNavigate: navigate }) });
  app.provide(Vue.ssrContextKey, {});
  app.mount(root);
  await settle();
}
beforeEach(() => { child.mounts = 0; child.unmounts = 0; focused = undefined; navigate.mockReset(); });
afterEach(() => { app?.unmount(); app = undefined; });

describe('global advisor companion shell', () => {
  it('starts as an accessible whale-girl launcher without mounting the advisor', async () => {
    await mount();
    const launcher = control('打开运营参谋对话');
    expect(launcher.kind).toBe('button');
    expect(launcher.props.type).toBe('button');
    expect(launcher.props['aria-expanded']).toBe(false);
    expect(launcher.props['aria-controls']).toBeUndefined();
    const image = nodes(launcher).find(node => node.kind === 'img')!;
    expect(image.props.src).toContain('whale-girl.png');
    expect(image.props.alt).toBe('蓝发小助手');
    expect(child.mounts).toBe(0);
    expect(nodes().some(node => node.props.role === 'dialog')).toBe(false);
    expect(navigate).not.toHaveBeenCalled();
  });

  it('opens a labelled non-modal conversation and activates a compact advisor', async () => {
    await mount(); await click(control('打开运营参谋对话'));
    const conversation = panel();
    const launcher = control('收起运营参谋对话');
    expect(child.mounts).toBe(1);
    expect(advisor().props).toMatchObject({ compact: true, active: true });
    expect(launcher.props['aria-expanded']).toBe(true);
    expect(launcher.props['aria-controls']).toBe(conversation.props.id);
    expect(conversation.props['aria-hidden']).toBe(false);
    expect(conversation.props.inert).toBe(false);
    expect(conversation.props['aria-modal']).not.toBe(true);
    expect(conversation.props['aria-modal']).not.toBe('true');
    expect(find(node => node.props.id === conversation.props['aria-labelledby']).textContent).toBe('AI 运营参谋');
    expect(focused).toBe(conversation);
    expect(navigate).not.toHaveBeenCalled();
  });

  it('hides and deactivates the same child while preserving its draft across reopening', async () => {
    await mount(); await click(control('打开运营参谋对话'));
    const mountedAdvisor = advisor();
    const textarea = find(node => node.kind === 'textarea');
    textarea.props.onInput({ target: { value: '还没发送的问题' } }); await settle();
    await click(control('收起运营参谋'));
    expect(child.mounts).toBe(1); expect(child.unmounts).toBe(0);
    expect(advisor()).toBe(mountedAdvisor);
    expect(advisor().props.active).toBe(false);
    expect(panel().style.display).toBe('none');
    expect(panel().props['aria-hidden']).toBe(true);
    expect(panel().props.inert).toBe(true);
    expect(focused).toBe(control('打开运营参谋对话'));
    await click(control('打开运营参谋对话'));
    expect(child.mounts).toBe(1); expect(child.unmounts).toBe(0);
    expect(advisor()).toBe(mountedAdvisor);
    expect(advisor().props.active).toBe(true);
    expect(panel().style.display).not.toBe('none');
    expect(textarea.props.value).toBe('还没发送的问题');
    await click(control('收起运营参谋对话'));
    expect(advisor().props.active).toBe(false);
    expect(panel().style.display).toBe('none');
  });

  it('closes on Escape and returns focus to the launcher', async () => {
    await mount(); await click(control('打开运营参谋对话'));
    const preventDefault = vi.fn(); const stopPropagation = vi.fn();
    panel().props.onKeydown({ key: 'Escape', preventDefault, stopPropagation }); await settle();
    expect(preventDefault).toHaveBeenCalledOnce();
    expect(stopPropagation).toHaveBeenCalledOnce();
    expect(panel().style.display).toBe('none');
    expect(advisor().props.active).toBe(false);
    expect(focused).toBe(control('打开运营参谋对话'));
  });

  it('reflects the advisor busy state in the pet animation and status label', async () => {
    await mount(); await click(control('打开运营参谋对话'));
    const launcher = control('收起运营参谋对话');
    expect(launcher.props.class).not.toContain('is-thinking');
    await click(find(node => node.kind === 'button' && node.textContent === '开始答复'));
    expect(launcher.props.class).toContain('is-thinking');
    expect(launcher.textContent).toContain('正在看数据…');
    await click(find(node => node.kind === 'button' && node.textContent === '完成答复'));
    expect(launcher.props.class).not.toContain('is-thinking');
    expect(launcher.textContent).toContain('收起对话');
  });

  it('forwards manual navigation with its exact scope before closing the conversation', async () => {
    await mount(); await click(control('打开运营参谋对话'));
    navigate.mockImplementation(() => {
      expect(advisor().props.active).toBe(true);
      expect(panel().style.display).not.toBe('none');
    });
    await click(find(node => node.kind === 'button' && node.textContent === '查看分析'));
    expect(navigate).toHaveBeenCalledExactlyOnceWith('advanced', scope);
    expect(advisor().props.active).toBe(false);
    expect(panel().style.display).toBe('none');
    expect(focused).toBe(control('打开运营参谋对话'));
  });
});
