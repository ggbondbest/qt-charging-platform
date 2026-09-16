// AI 桌宠的页面动作执行器(与后端 ai_actions.json 白名单对账,双端独立校验)。
// 纪律:
// 1. 只认注册表 key,模型/后端给的 route/selector/label 一律不作为执行依据——
//    执行参数永远从本文件 resolve 出来(防 label 注入/防伪造响应指 DOM)。
// 2. fill 是 prefill-only:只写 value + 派发 input/change,绝不派发 Enter、
//    绝不点击任何按钮提交(提交永远留给用户亲手点)。
// 3. fail-closed:任何一步(按钮不存在/option 未渲染/aria-pressed 不变/回读不符)
//    都抛错中止该条,不做半执行;调用方负责把失败回显给用户。
export interface PetAction {
  kind: "navigate" | "fill";
  target: string;
  route: string;
  section?: string | null;
  value?: string;
  label?: string;
}
interface NavSpec { route: string; section?: string; label: string; scrollAfter?: string; }
interface FillSpec { route: string; section?: string; selector: string; event: "input" | "change"; label: string; maxLen: number; pattern?: RegExp; enums?: readonly string[];
  // 就绪门控:这些框的值会被组件异步加载完成时**无条件覆写**(insights 的 load() 填首个
  // 编号、ForecastPanel 的 initialize() 填默认起点)。不等到覆写落定就写入,回读虽然一致,
  // 几百毫秒后用户看到的值会被页面悄悄改回去(评审实锤"瞬时真相")。readySels 命中任一
  // 选择器 = 加载已终态(成功或报错都不再覆写),才允许写。
  readySels?: string[]; }

const TAB_ORDER = ["dashboard", "explore", "lab"]; // #76 起行程页已删;admin 刻意排除:需令牌且多为写操作入口
const SEC_ORDER = ["forecast", "insights", "arrival", "experiments"];
const navBtn = (n: number) => `#primary-header nav.main-nav > button:nth-child(${n})`;
const secBtn = (n: number) => `nav.workspace-tabs[aria-label="智能分析分区"] > button:nth-child(${n})`;

// 与后端 data_analysis/ml/advisor/ai_actions.json 同构的本地注册表
// (advisorActions.test.ts 用 fs 读那份 JSON 逐条对账,漂移即测试红)。
export const NAV_REGISTRY: Record<string, NavSpec> = {
  overview: { route: "dashboard", label: "运营总览" },
  explore: { route: "explore", label: "智能找站" },
  lab: { route: "lab", label: "智能分析" },
  "lab-forecast": { route: "lab", section: "forecast", label: "智能分析·负荷与空闲预测" },
  "lab-insights": { route: "lab", section: "insights", label: "智能分析·用户与异常" },
  "lab-arrival": { route: "lab", section: "arrival", label: "智能分析·到站模型" },
  "lab-experiments": { route: "lab", section: "experiments", label: "智能分析·策略对比", scrollAfter: "#paired-experiment-results" },
};
export const FILL_REGISTRY: Record<string, FillSpec> = {
  "insights-query": { route: "lab", section: "insights", selector: "form.insights-query input", event: "input", label: "分析编号查询框", maxLen: 40, pattern: /^[^\n\r]{1,40}$/, readySels: [".insights-grid", ".intelligence-inline-error"] },
  "dash-start": { route: "dashboard", selector: 'section.analytics-filter input[aria-label="统计开始日期"]', event: "input", label: "统计开始日期", maxLen: 10, pattern: /^\d{4}-\d{2}-\d{2}$/ },
  "dash-end": { route: "dashboard", selector: 'section.analytics-filter input[aria-label="统计结束日期"]', event: "input", label: "统计结束日期", maxLen: 10, pattern: /^\d{4}-\d{2}-\d{2}$/ },
  "forecast-target": { route: "lab", section: "forecast", selector: 'form.forecast-controls select[aria-label="预测目标"]', event: "change", label: "预测目标", maxLen: 12, enums: ["load", "availability"] },
  "forecast-horizon": { route: "lab", section: "forecast", selector: 'form.forecast-controls select[aria-label="预测跨度"]', event: "change", label: "预测跨度(小时)", maxLen: 2, enums: ["1", "6", "24"] },
  "forecast-reference": { route: "lab", section: "forecast", selector: 'form.forecast-controls input[aria-label="预测起点"]', event: "input", label: "预测起点", maxLen: 19, pattern: /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(:\d{2})?$/, readySels: ['select[aria-label="预测电站"] option', ".intelligence-inline-error"] },
  "energy-kwh": { route: "explore", selector: "#energy", event: "change", label: "计划补电(kWh)", maxLen: 2, enums: ["5", "10", "20", "30", "40", "60"] },
  "max-eta": { route: "explore", selector: "#max-eta", event: "change", label: "最远行驶(分钟)", maxLen: 2, enums: ["15", "30", "60"] },
};

export function navSelector(target: string): { nav: string; section?: string } | null {
  const spec = NAV_REGISTRY[target];
  if (!spec) return null;
  const nav = navBtn(TAB_ORDER.indexOf(spec.route) + 1);
  const section = spec.section ? secBtn(SEC_ORDER.indexOf(spec.section) + 1) : undefined;
  return { nav, section };
}

// 后端响应也不全信:逐条按本地注册表二次过滤(route/section/value 任一不符即丢)
export function resolveActions(raw: unknown): PetAction[] {
  if (!Array.isArray(raw)) return [];
  const out: PetAction[] = [];
  for (const item of raw) {
    if (!item || typeof item !== "object") continue;
    const a = item as Record<string, unknown>;
    const sec = a.section == null || a.section === "" ? null : String(a.section);
    if (a.kind === "navigate") {
      const spec = NAV_REGISTRY[String(a.target)];
      if (spec && a.route === spec.route && sec === (spec.section ?? null)) {
        out.push({ kind: "navigate", target: String(a.target), route: spec.route,
                   section: spec.section ?? null, label: spec.label });
      }
    } else if (a.kind === "fill") {
      const spec = FILL_REGISTRY[String(a.target)];
      const v = typeof a.value === "string" ? a.value : "";
      if (spec && a.route === spec.route && sec === (spec.section ?? null)
          && v.length >= 1 && v.length <= spec.maxLen
          && (!spec.pattern || spec.pattern.test(v))
          && (!spec.enums || spec.enums.includes(v))) {
        out.push({ kind: "fill", target: String(a.target), route: spec.route,
                   section: spec.section ?? null, value: v, label: spec.label });
      }
    }
  }
  return out;
}

// ---------- DOM 执行器(doc/sleep 注入式,单测用假 DOM) ----------
export interface ExecCtx {
  doc: Document;
  sleep?: (ms: number) => Promise<void>;
  timeoutMs?: number;
}
const sleepDefault = (ms: number) => new Promise<void>((r) => window.setTimeout(r, ms));

async function waitForEl(doc: Document, selector: string, sleep: (ms: number) => Promise<void>, timeoutMs: number): Promise<Element | null> {
  let waited = 0;
  for (;;) {
    const el = doc.querySelector(selector);
    if (el) return el;
    if (waited >= timeoutMs) return null;
    await sleep(60);
    waited += 60;
  }
}
// 任一选择器命中即算就绪(异步覆写见 FillSpec.readySels 注释:成功/失败两种终态都要放行,
// 失败态不会覆写值,填入是安全的)
async function waitAny(doc: Document, selectors: string[], sleep: (ms: number) => Promise<void>, timeoutMs: number): Promise<boolean> {
  let waited = 0;
  for (;;) {
    if (selectors.some((s) => doc.querySelector(s))) return true;
    if (waited >= timeoutMs) return false;
    await sleep(60);
    waited += 60;
  }
}
const doubleRaf = (sleep: (ms: number) => Promise<void>) => sleep(20).then(() => sleep(20));

async function gotoTarget(route: string, navSelectorArg: string, sectionSelector: string | undefined,
  ctx: ExecCtx, sleep: (ms: number) => Promise<void>): Promise<void> {
  const btn = ctx.doc.querySelector(navSelectorArg);
  if (!btn) throw new Error("导航按钮不可见");
  (btn as HTMLElement).click();
  await doubleRaf(sleep);
  if (!ctx.doc.querySelector(`.app-shell.workspace-${route}`)) throw new Error("页面没有响应切换");
  if (sectionSelector) {
    const sec = await waitForEl(ctx.doc, sectionSelector, sleep, 1500);
    if (!sec) throw new Error("子分区按钮未出现");
    (sec as HTMLElement).click();
    await doubleRaf(sleep);
    if (sec.getAttribute("aria-pressed") !== "true") throw new Error("子分区切换未生效");
  }
}

export async function runAction(a: PetAction, ctx: ExecCtx): Promise<void> {
  const sleep = ctx.sleep ?? sleepDefault;
  const to = ctx.timeoutMs ?? 2000;
  if (a.kind === "navigate") {
    const spec = NAV_REGISTRY[a.target];
    const sel = navSelector(a.target);
    if (!spec || !sel) throw new Error("未注册的跳转目标");
    await gotoTarget(spec.route, sel.nav, sel.section, ctx, sleep);
    if (spec.scrollAfter) {
      const anchor = ctx.doc.querySelector(spec.scrollAfter);
      if (anchor && "scrollIntoView" in anchor) (anchor as HTMLElement).scrollIntoView({ block: "start" });
    }
    return;
  }
  const spec = FILL_REGISTRY[a.target];
  if (!spec) throw new Error("未注册的填入目标");
  if (!TAB_ORDER.includes(spec.route)) throw new Error("该页面不支持自动跳转");
  const nav = navBtn(TAB_ORDER.indexOf(spec.route) + 1);
  const secSel = spec.section ? secBtn(SEC_ORDER.indexOf(spec.section) + 1) : undefined;
  await gotoTarget(spec.route, nav, secSel, ctx, sleep);
  if (spec.readySels?.length && !(await waitAny(ctx.doc, spec.readySels, sleep, Math.max(to, 4000))))
    throw new Error("页面数据还没就绪(加载中或暂无可展示样本),过几秒再点填入");
  const el = await waitForEl(ctx.doc, spec.selector, sleep, to);
  if (!el) throw new Error("输入框还没出现(页面数据可能未就绪)");
  // 可见性断言(评审实锤):v-show 隐藏的元素照样能设值回读——大屏模式下
  // dashboard 筛选区 display:none,填了用户看不见却播报成功。写之前先拒,保持"不做半执行"。
  // getClientRects 用 typeof 守卫:node 假 DOM 没有该方法,测试环境自动跳过。
  if (typeof (el as HTMLElement).getClientRects === "function" && (el as HTMLElement).getClientRects().length === 0)
    throw new Error("输入框当前被页面隐藏(比如大屏模式收起了筛选区),未写入");
  const value = a.value ?? "";
  // 鸭子类型认 select(tagName),不用 instanceof HTMLSelectElement——后者在 node 测试环境不存在
  if (el.tagName === "SELECT") {
    const options = Array.from((el as unknown as HTMLSelectElement).options || []);
    if (!options.some((o) => o.value === value)) throw new Error("可选项还没加载出来");
    (el as HTMLSelectElement).value = value;
    el.dispatchEvent(new Event("change", { bubbles: true })); // v-model.number 经 option._value 得数字
  } else {
    (el as HTMLInputElement).value = value;
    el.dispatchEvent(new Event(spec.event, { bubbles: true })); // 只发 input/change,绝不发 Enter
  }
  if ((el as HTMLInputElement).value !== value) throw new Error("填入后回读不一致");
}

// 给用户看的动作摘要(纯插值渲染,不走 v-html)
export function actionSummary(a: PetAction): string {
  return a.kind === "navigate" ? `打开「${a.label ?? ""}」` : `在「${a.label ?? ""}」填入「${a.value ?? ""}」(不提交)`;
}
