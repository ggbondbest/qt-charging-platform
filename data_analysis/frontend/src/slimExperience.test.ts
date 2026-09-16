import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createRenderer, h, nextTick } from "vue";
import * as Vue from "vue";
import { compileScript, compileTemplate, parse } from "@vue/compiler-sfc";
import App from "./App.vue";
import appSource from "./App.vue?raw";
import { request } from "./api";
import type { Candidate } from "./types";

vi.mock("./api", async importOriginal => ({
  ...await importOriginal<typeof import("./api")>(), request: vi.fn(),
}));
vi.mock("./components/AutoHideHeader.vue", () => ({
  default: { setup: (_: unknown, { slots }: any) => () => h("header", slots.default?.()) },
}));
vi.mock("./components/StationMap.vue", () => ({ default: { render: () => h("station-map") } }));
vi.mock("./components/Icon.vue", () => ({ default: { render: () => h("icon-stub") } }));
vi.mock("./components/OriginPicker.vue", () => ({ default: { render: () => h("origin-picker") } }));
vi.mock("./components/WorkspaceTabs.vue", () => ({
  default: {
    props: ["items"], emits: ["update:modelValue"],
    setup: (props: any, { emit }: any) => () => h("nav", props.items.map((item: any) =>
      h("button", { onClick: () => emit("update:modelValue", item.id) }, item.label))),
  },
}));
vi.mock("./components/Chart.vue", () => ({ default: { render: () => h("chart-stub") } }));
vi.mock("./components/AnalyticsDashboard.vue", () => ({ default: { props: ['initialSection', 'initialScope'], setup: (props: any) => () => h('dashboard-stub', { section: props.initialSection, scope: props.initialScope }) } }));
vi.mock("./components/ForecastPanel.vue", () => ({ default: { render: () => h("forecast-stub") } }));
vi.mock("./components/ManagementInsights.vue", () => ({ default: { props: ['initialTarget'], setup: (props: any) => () => h('insights-stub', { target: props.initialTarget }) } }));
vi.mock("./components/OperationsAdvisor.vue", () => ({ default: {
  emits: ['navigate'], setup: (_: unknown, { emit }: any) => () => h('advisor-stub', [
    h('button', { onClick: () => emit('navigate', 'advanced', { datasetId: 'D1', publishedBatchId: 'B1', cityId: 'DL', startDate: '2026-05-01', endDate: '2026-05-08' }) }, '查看多维运营分析'),
    h('button', { onClick: () => emit('navigate', 'anomalies') }, '查看充电异常筛查'),
  ]),
} }));
vi.mock("./components/RecommendationRoute.vue", () => ({
  default: {
    props: ["candidate", "loading", "error"],
    emits: ["close", "retry"],
    setup: (props: any, { emit }: any) => () => h("route-preview", { station: props.candidate?.stationId }, [
      h("button", { onClick: () => emit("close") }, "收起路线"),
      h("button", { onClick: () => emit("retry") }, "重试路线"),
    ]),
  },
}));

// A small in-memory host exercises the actual App handlers and watchers without a browser or Leaflet.
class TestNode {
  parent: TestNode | null = null;
  children: TestNode[] = [];
  props: Record<string, any> = {};
  text = "";
  value: any;
  selected = false;
  selectedIndex = -1;
  multiple = false;
  listeners: Record<string, Function> = {};
  constructor(public kind: string) {}
  get tagName() { return this.kind.toUpperCase(); }
  get options() { return this.children.filter(child => child.kind === "option"); }
  get textContent(): string { return this.text + this.children.map(child => child.textContent).join(""); }
  get isConnected() { return true; }
  getRootNode() { return document; }
  addEventListener(name: string, handler: Function) { this.listeners[name] = handler; }
  removeEventListener(name: string) { delete this.listeners[name]; }
  focus() {}
  scrollIntoView() {}
}
function detach(node: TestNode) {
  if (!node.parent) return;
  const index = node.parent.children.indexOf(node);
  if (index >= 0) node.parent.children.splice(index, 1);
  node.parent = null;
}
const renderer = createRenderer<TestNode, TestNode>({
  createElement: tag => new TestNode(tag),
  createText: text => Object.assign(new TestNode("#text"), { text }),
  createComment: text => Object.assign(new TestNode("#comment"), { text }),
  setText: (node, text) => { node.text = text; },
  setElementText: (node, text) => { node.text = text; node.children = []; },
  parentNode: node => node.parent,
  nextSibling: node => node.parent?.children[node.parent.children.indexOf(node) + 1] || null,
  patchProp: (node, key, _previous, value) => {
    node.props[key] = value;
    if (key === "value") node.value = value;
  },
  insert: (node, parent, anchor = null) => {
    detach(node);
    node.parent = parent;
    const index = anchor ? parent.children.indexOf(anchor) : -1;
    if (index < 0) parent.children.push(node);
    else parent.children.splice(index, 0, node);
  },
  remove: detach,
});
// Vitest loads SFCs in SSR mode. Compile the same template for this in-memory client renderer.
const { descriptor } = parse(appSource);
const bindings = compileScript(descriptor, { id: "slim-experience" }).bindings;
const compiled = compileTemplate({
  source: descriptor.template!.content, filename: "App.vue", id: "slim-experience",
  compilerOptions: { bindingMetadata: bindings },
});
if (compiled.errors.length) throw new Error(String(compiled.errors[0]));
const renderCode = compiled.code
  .replace(/import \{([^}]+)\} from "vue"/g, (_match, names: string) => `const {${names.replace(/ as /g, ": ")}} = Vue`)
  .replace("export function render", "return function render");
const appWithClientRender = { ...App, render: new Function("Vue", renderCode)(Vue) };
const candidate: Candidate = {
  stationId: "station-dl-1", stationName: "星海充电站", cityId: "DL",
  latitude: 38.88, longitude: 121.59, capacity: 12, powerKw: 120, pricePerKwh: 1.4,
  rank: 1, score: 88, etaMinutes: 12, distanceKm: 5.8, routeSource: "ESTIMATED_DISTANCE_SPEED",
  currentFree: 4, expectedFree: 5, availableProbability: 0.9, waitMinutes: 2, waitP90Minutes: 6,
  serviceProbability: 0.95, arrivalTime: "2026-09-15T04:12:00Z", forecastTime: "2026-09-15T04:00:00Z",
  resolutionMinutes: 15, loadRatio: 0.4, rewardPoints: 100, scoreBreakdown: {}, reasons: [], totalMinutes: 14,
};
const clock = { time: "2026-09-15T04:00:00Z", speed: 60, paused: true };
let root: TestNode;
let app: ReturnType<typeof renderer.createApp>;
function nodes(node = root): TestNode[] { return [node, ...node.children.flatMap(child => nodes(child))]; }
function find(kind: string, text?: string) {
  const node = nodes().find(item => item.kind === kind && (!text || item.textContent.includes(text)));
  if (!node) throw new Error(`Missing ${kind}: ${text || ""}`);
  return node;
}
async function settle() {
  for (let i = 0; i < 12; i++) { await Promise.resolve(); await nextTick(); }
}
async function click(text: string) {
  const button = find("button", text);
  expect(button.props.disabled).not.toBe(true);
  await button.props.onClick({});
  await settle();
}
async function mount() {
  root = new TestNode("root");
  app = renderer.createApp(appWithClientRender);
  app.provide(Vue.ssrContextKey, {});
  app.mount(root);
  await settle();
}
async function showRoute() {
  await click("智能找站");
  await click("为我推荐");
  await click("查看路线");
  expect(find("route-preview").props.station).toBe(candidate.stationId);
}
const paths = () => vi.mocked(request).mock.calls.map(([path]) => path);

beforeEach(() => {
  vi.stubGlobal("window", {
    scrollTo: vi.fn(), setInterval: vi.fn(() => 1), clearInterval: vi.fn(),
    setTimeout: vi.fn(() => 2), clearTimeout: vi.fn(),
  });
  class TestDocument { hidden = false; addEventListener = vi.fn(); removeEventListener = vi.fn(); }
  vi.stubGlobal("Document", TestDocument);
  vi.stubGlobal("document", new TestDocument());
  vi.stubGlobal("localStorage", { getItem: vi.fn(() => null), setItem: vi.fn(), removeItem: vi.fn() });
  vi.mocked(request).mockReset();
  vi.mocked(request).mockImplementation(async path => {
    const data: Record<string, any> = {
      "/bootstrap": {
        cities: [
          { cityId: "DL", cityName: "大连市", latitude: 38.914, longitude: 121.6147 },
          { cityId: "BJ", cityName: "北京市", latitude: 39.9, longitude: 116.4 },
        ], stations: [candidate], clock, modelStatus: {}, provenance: {},
      },
      "/models": { status: "READY", arrival: { metadata: {}, metrics: {} } },
      "/experiments": { status: "NOT_RUN" },
      "/sessions": { token: "demo-test-session" },
      "/recommendations": {
        recommendationId: "rec-1", createdAt: clock.time, expiresAt: "2026-09-15T05:00:00Z",
        referenceTime: clock.time, origin: { latitude: 38.914, longitude: 121.6147 },
        candidates: [candidate], warnings: [],
      },
      "/route": { routeSource: "STRAIGHT_LINE_DEMO", coordinates: [[38.914, 121.6147], [38.88, 121.59]] },
      "/admin": { clock },
      "/stations": [candidate],
    };
    if (!(path in data)) throw new Error(`Unexpected API request: ${path}`);
    return { data: data[path] };
  });
});
afterEach(() => { app?.unmount(); vi.unstubAllGlobals(); });

describe("slim recommendation experience", () => {
  it('keeps the advisor inside intelligent analysis and navigates only on explicit evidence-page clicks', async () => {
    await mount(); expect(nodes().some(item => item.kind === 'advisor-stub')).toBe(false);
    await click('智能分析'); await click('AI运营参谋'); expect(find('advisor-stub')).toBeTruthy();
    expect(nodes().some(item => item.kind === 'dashboard-stub')).toBe(false);
    await click('查看多维运营分析'); expect(find('dashboard-stub').props.section).toBe('space');
    expect(find('dashboard-stub').props.scope).toEqual({ datasetId: 'D1', publishedBatchId: 'B1', cityId: 'DL', startDate: '2026-05-01', endDate: '2026-05-08' });
    await click('智能分析'); await click('AI运营参谋'); await click('查看充电异常筛查');
    expect(find('insights-stub').props.target).toBe('anomalies');
    expect(root.textContent).not.toContain('桌宠');
  });
  it("opens a route directly from a recommendation and preserves the plain reward badge", async () => {
    await mount();
    await showRoute();
    expect(paths()).toContain("/recommendations");
    const routeCall = vi.mocked(request).mock.calls.find(([path]) => path === "/route");
    expect(routeCall?.[1]).toMatchObject({ method: "POST", token: "demo-test-session", body: {
      stationId: "station-dl-1", origin: { latitude: 38.914, longitude: 121.6147 },
    } });
    expect(paths().some(path => /\/me(?:$|\/)|\/select(?:$|\/)|\/trips(?:$|\/)/.test(path))).toBe(false);
    const badge = nodes().find(node => node.props.class === "reward-tag");
    expect(badge?.textContent.trim()).toMatch(/^100\s*积分$/);
    expect(badge?.props.title).toBeUndefined();
    const card = nodes().find(node => String(node.props.class).includes("candidate-card"));
    expect(card?.textContent).not.toMatch(/DEMO|不发放|仅展示|仅作展示|到账|支付|创建行程/);
    expect(root.textContent).not.toContain("我的行程");
    await click("收起路线");
    expect(nodes().some(node => node.kind === "route-preview")).toBe(false);
  });

  it.each(["energy", "max-eta", "city", "tab"])("closes the selected route when %s changes", async change => {
    await mount();
    await showRoute();
    if (change === "city") await click("北京");
    else if (change === "tab") await click("智能分析");
    else {
      const input = nodes().find(node => node.props.id === change)!;
      input.props["onUpdate:modelValue"](change === "energy" ? 30 : 15);
      await settle();
    }
    expect(nodes().some(node => node.kind === "route-preview")).toBe(false);
    if (change !== "tab") expect(nodes().some(node => String(node.props.class).includes("candidate-card"))).toBe(false);
    expect(paths().filter(path => path === "/route")).toHaveLength(1);
  });

  it("retains admin replay controls and paired experiments without resource or transaction forms", async () => {
    await mount();
    await click("模拟控制台");
    const tokenInput = nodes().find(node => node.props["aria-label"] === "管理员令牌")!;
    tokenInput.props["onUpdate:modelValue"]("test-admin-token");
    await settle();
    await click("连接控制台");
    expect(vi.mocked(request).mock.calls.find(([path]) => path === "/admin")?.[1]).toMatchObject({ admin: "test-admin-token" });
    expect(root.textContent).toContain("模拟时钟");
    expect(root.textContent).toContain("运行配对实验");
    expect(find("button", "推进时钟")).toBeTruthy();
    expect(find("button", "运行并查看结果")).toBeTruthy();
    expect(root.textContent).not.toMatch(/背景占用|站点资源|策略配置|积分账户|模拟支付|我的行程|开始充电/);
    expect(nodes().some(node => /resource|strategy|payment|session-panel|trip-list/.test(String(node.props.class || "")))).toBe(false);
    await click("智能分析");
    expect(find("forecast-stub")).toBeTruthy();
    await click("用户与异常");
    expect(find("insights-stub")).toBeTruthy();
    await click("到站模型");
    expect(root.textContent).toContain("到站空闲与等待预测");
    await click("策略对比");
    expect(root.textContent).toContain("最近站 vs. 智能推荐");
  });
});
