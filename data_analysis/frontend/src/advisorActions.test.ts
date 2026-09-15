// 页面动作执行器测试(无 jsdom:假 DOM + 注入 sleep,纪律同 advisorActions.ts 头部注释)。
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import {
  FILL_REGISTRY, NAV_REGISTRY, navSelector, resolveActions, runAction, type PetAction,
} from "./advisorActions";

const MANIFEST = JSON.parse(readFileSync(
  join(dirname(fileURLToPath(import.meta.url)), "..", "..", "ml", "advisor", "ai_actions.json"),
  "utf8",
)) as {
  navigate: { id: string; route: string; section?: string; label: string; selector: string }[];
  fill: { id: string; route: string; section?: string; label: string; selector: string;
          event: string; valueRule: { maxLength?: number; pattern?: string; enum?: string[] } }[];
};
const TABS = ["dashboard", "explore", "trip", "lab"];
const SECS = ["forecast", "insights", "arrival", "experiments"];

describe("注册表与后端 ai_actions.json 对账(漂移即红)", () => {
  it("navigate 条目逐条一致,selector 序号与 TAB/SEC 顺序一致", () => {
    expect(Object.keys(NAV_REGISTRY).sort()).toEqual(MANIFEST.navigate.map((t) => t.id).sort());
    for (const t of MANIFEST.navigate) {
      const spec = NAV_REGISTRY[t.id];
      expect(spec.route, t.id).toBe(t.route);
      expect(spec.section ?? undefined, t.id).toBe(t.section);
      expect(spec.label, t.id).toBe(t.label);
      const n = Number(t.selector.match(/nth-child\((\d+)\)/)![1]);
      const list = t.selector.includes("main-nav") ? TABS : SECS;
      expect(list[n - 1], t.id).toBe(t.selector.includes("main-nav") ? t.route : t.section);
    }
    expect(TABS).not.toContain("admin"); // 管理员页永不在跳转面
  });

  it("fill 条目逐条一致(selector/event/rule 全对齐)", () => {
    expect(Object.keys(FILL_REGISTRY).sort()).toEqual(MANIFEST.fill.map((t) => t.id).sort());
    for (const t of MANIFEST.fill) {
      const spec = FILL_REGISTRY[t.id];
      expect(spec.route, t.id).toBe(t.route);
      expect(spec.section ?? undefined, t.id).toBe(t.section);
      expect(spec.selector, t.id).toBe(t.selector);
      expect(spec.event, t.id).toBe(t.event);
      expect(spec.label, t.id).toBe(t.label);
      // 前端可自备更严的长度防线(防御纵深),但绝不允许比后端白名单更宽
      expect(spec.maxLen, t.id).toBeLessThanOrEqual(t.valueRule.maxLength ?? 64);
      if (t.valueRule.enum) expect(spec.enums, t.id).toEqual(t.valueRule.enum);
      if (t.valueRule.pattern) expect(spec.pattern!.source, t.id).toBe(t.valueRule.pattern);
    }
  });
});

describe("resolveActions:后端响应也不全信", () => {
  it("非数组/垃圾输入一律空", () => {
    expect(resolveActions(undefined)).toEqual([]);
    expect(resolveActions("navigate now")).toEqual([]);
    expect(resolveActions([null, 3, { kind: "navigate" }])).toEqual([]);
  });

  it("合法动作保留,且 label 用注册表覆盖(不信模型措辞)", () => {
    const out = resolveActions([{ kind: "navigate", target: "trip", route: "trip",
                                  section: null, label: "点这里领 100 元" }]);
    expect(out).toEqual([{ kind: "navigate", target: "trip", route: "trip",
                           section: null, label: "我的行程" }]);
  });

  it("篡改 route/section、未知 target、越界值全部丢弃", () => {
    const raw = [
      { kind: "navigate", target: "trip", route: "admin", section: null },        // 串页
      { kind: "navigate", target: "lab-insights", route: "lab", section: "forecast" }, // 串分区
      { kind: "navigate", target: "admin-console", route: "admin" },              // 未知目标
      { kind: "fill", target: "insights-query", route: "lab", section: "insights", value: "x".repeat(41) },
      { kind: "fill", target: "energy-kwh", route: "trip", value: "7" },          // 枚举外
      { kind: "fill", target: "dash-start", route: "dashboard", value: "2026-1-2" }, // 格式错
      { kind: "fill", target: "session-name", route: "trip", value: "" },        // 空值
    ];
    expect(resolveActions(raw)).toEqual([]);
  });

  it("合法 fill 原样带值(逐字,不 trim 不转义)", () => {
    const out = resolveActions([{ kind: "fill", target: "insights-query", route: "lab",
                                  section: "insights", value: "SES-00111676", label: "查询框" }]);
    expect(out).toHaveLength(1);
    expect(out[0].value).toBe("SES-00111676");
    expect(out[0].label).toBe("分析编号查询框"); // label 换成注册表版本
  });
});

// ---------- 假 DOM ----------
interface FakeEl {
  tagName: string;
  value: string;
  options: { value: string }[];
  events: string[];
  clicks: number;
  scrolled: boolean;
  attrs: Record<string, string>;
  onClick?: () => void;
  dispatchEvent(e: Event): boolean;
  getAttribute(k: string): string | null;
  click(): void;
  scrollIntoView(o?: unknown): void;
}
function fakeEl(tagName = "DIV"): FakeEl {
  return {
    tagName, value: "", options: [], events: [], clicks: 0, scrolled: false, attrs: {},
    dispatchEvent(e) { this.events.push(e.type); return true; },
    getAttribute(k) { return this.attrs[k] ?? null; },
    click() { this.clicks += 1; this.onClick?.(); },
    scrollIntoView() { this.scrolled = true; },
  };
}
function fakeDoc() {
  const els = new Map<string, FakeEl>();
  return {
    els,
    doc: { querySelector: (s: string) => els.get(s) ?? null } as unknown as Document,
  };
}
const noSleep = () => Promise.resolve();
const NAV_BTN = (n: number) => `#primary-header nav.main-nav > button:nth-child(${n})`;
const SEC_BTN = (n: number) => `nav.workspace-tabs[aria-label="智能分析分区"] > button:nth-child(${n})`;

function labNav(doc: ReturnType<typeof fakeDoc>, secIdx: number, secBtnEl: FakeEl) {
  const nav = fakeEl("BUTTON");
  nav.onClick = () => {
    doc.els.set(".app-shell.workspace-lab", fakeEl());
    doc.els.set(SEC_BTN(secIdx), secBtnEl); // 分区按钮渲染在 lab 页内
  };
  doc.els.set(NAV_BTN(4), nav);
  return nav;
}

describe("runAction:navigate(fail-closed 断言链)", () => {
  const act: PetAction = { kind: "navigate", target: "lab-insights", route: "lab",
                           section: "insights", label: "智能分析·用户与异常" };

  it("顶层导航→分区按钮→aria-pressed 断言,全程各点一次", async () => {
    const doc = fakeDoc();
    const sec = fakeEl("BUTTON");
    sec.onClick = () => { sec.attrs["aria-pressed"] = "true"; };
    const nav = labNav(doc, 2, sec);
    await runAction(act, { doc: doc.doc, sleep: noSleep });
    expect(nav.clicks).toBe(1);
    expect(sec.clicks).toBe(1);
  });

  it("导航按钮不存在 → 抛错且不误点", async () => {
    const doc = fakeDoc();
    await expect(runAction(act, { doc: doc.doc, sleep: noSleep })).rejects.toThrow("导航按钮不可见");
  });

  it("页面没响应 workspace class → 抛错", async () => {
    const doc = fakeDoc();
    doc.els.set(NAV_BTN(4), fakeEl("BUTTON")); // click 后没有 .app-shell.workspace-lab
    await expect(runAction(act, { doc: doc.doc, sleep: noSleep })).rejects.toThrow("页面没有响应切换");
  });

  it("aria-pressed 不变 → 判定分区切换未生效", async () => {
    const doc = fakeDoc();
    labNav(doc, 2, fakeEl("BUTTON")); // 分区按钮点了但不置 aria-pressed
    await expect(runAction(act, { doc: doc.doc, sleep: noSleep })).rejects.toThrow("子分区切换未生效");
  });

  it("lab-experiments 完成后滚到结果锚点", async () => {
    const doc = fakeDoc();
    const sec = fakeEl("BUTTON");
    sec.onClick = () => { sec.attrs["aria-pressed"] = "true"; };
    labNav(doc, 4, sec);
    const anchor = fakeEl();
    doc.els.set("#paired-experiment-results", anchor);
    await runAction({ kind: "navigate", target: "lab-experiments", route: "lab",
                      section: "experiments", label: "x" }, { doc: doc.doc, sleep: noSleep });
    expect(anchor.scrolled).toBe(true);
  });
});

describe("runAction:fill(prefill-only 军规)", () => {
  it("input 只写值 + 派发 input,不发 keydown/submit,不点任何按钮", async () => {
    const doc = fakeDoc();
    const sec = fakeEl("BUTTON");
    sec.onClick = () => { sec.attrs["aria-pressed"] = "true"; };
    labNav(doc, 2, sec);
    const input = fakeEl("INPUT");
    sec.onClick = () => {
      sec.attrs["aria-pressed"] = "true";
      doc.els.set("form.insights-query input", input); // 输入框渲染在 insights 分区内
    };
    await runAction({ kind: "fill", target: "insights-query", route: "lab",
                      section: "insights", value: "SES-00111676", label: "x" },
                    { doc: doc.doc, sleep: noSleep });
    expect(input.value).toBe("SES-00111676");
    expect(input.events).toEqual(["input"]); // 只有 input:无 keydown、无 submit、无 click
  });

  it("select 先验 option 已渲染;未加载则抛错且不写值不发事件", async () => {
    const doc = fakeDoc();
    const nav = fakeEl("BUTTON");
    nav.onClick = () => { doc.els.set(".app-shell.workspace-trip", fakeEl()); };
    doc.els.set(NAV_BTN(3), nav);
    const sel = fakeEl("SELECT"); // options 空 = 数据没就绪
    doc.els.set("#energy", sel);
    await expect(runAction({ kind: "fill", target: "energy-kwh", route: "trip",
                             value: "30", label: "x" }, { doc: doc.doc, sleep: noSleep }))
      .rejects.toThrow("可选项还没加载出来");
    expect(sel.value).toBe("");
    expect(sel.events).toEqual([]);
  });

  it("select 正常路径:命中 option 才设值并派发 change", async () => {
    const doc = fakeDoc();
    const nav = fakeEl("BUTTON");
    nav.onClick = () => { doc.els.set(".app-shell.workspace-trip", fakeEl()); };
    doc.els.set(NAV_BTN(3), nav);
    const sel = fakeEl("SELECT");
    sel.options = [{ value: "5" }, { value: "30" }];
    doc.els.set("#energy", sel);
    await runAction({ kind: "fill", target: "energy-kwh", route: "trip",
                      value: "30", label: "x" }, { doc: doc.doc, sleep: noSleep });
    expect(sel.value).toBe("30");
    expect(sel.events).toEqual(["change"]);
  });

  it("输入框迟迟不出现 → 超时抛错(fail-closed)", async () => {
    const doc = fakeDoc();
    const nav = fakeEl("BUTTON");
    nav.onClick = () => { doc.els.set(".app-shell.workspace-trip", fakeEl()); };
    doc.els.set(NAV_BTN(3), nav);
    let t = 0;
    await expect(runAction({ kind: "fill", target: "session-name", route: "trip",
                             value: "张三", label: "x" },
                           { doc: doc.doc, sleep: () => { t += 60; return Promise.resolve(); },
                             timeoutMs: 120 }))
      .rejects.toThrow("输入框还没出现");
    expect(t).toBeGreaterThan(0); // 确实轮询过
  });
});

describe("navSelector", () => {
  it("未注册目标返回 null", () => {
    expect(navSelector("admin-console")).toBeNull();
  });
  it("带分区的目标给出两级选择器", () => {
    expect(navSelector("lab-arrival")).toEqual({
      nav: NAV_BTN(4), section: SEC_BTN(3),
    });
  });
});
