<script setup lang="ts">
// AI 运营参谋桌宠:右下角的"鲸鱼娘",点击展开对话面板。
// 立绘化用开源项目 MeteorNOX/DeepSeek-Balance-Whale-Widget(MIT © MeteorNOX)的
// DeepSeek 鲸鱼娘形象(assets/whale-girl.png,署名见同目录 whale-girl.LICENSE.txt);
// 交互(按压 Q 弹、拖拽贴边吸附、靠左吸附时镜像翻转、气泡描边 #203170)也参考该项目规格。
// 状态机(idle/thinking/success/offline)由 CSS transform/filter 驱动,零新增 npm 依赖;
// Markdown 回答由本文件自带的极简渲染器先转义后组标签(白名单),出处只显示"人话标签",
// 内部文件名/模型技术名已在服务端 scrub 掉。npm run build 的 vue-tsc 全仓检查必须绿。
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import whaleGirl from "../assets/whale-girl.png";
import { resolveActions, runAction, type PetAction } from "../advisorActions";

const BASE = "http://127.0.0.1:8765";
const PET_SIZE = 88; // 桌宠占位边长(方形立绘)

interface Msg {
  role: "user" | "assistant";
  text: string; // 原始全文
  shown: string; // 打字机已展示前缀
  html: string; // shown 的渲染结果
  sources: string[]; // 人话标签(服务端已翻译)
  actions?: PetAction[]; // 页面动作(已过前端本地注册表二次过滤)
  naviDone?: boolean; // navigate 自动执行过(打字机完成后)
  fillState?: "done" | "void"; // fill 确认芯片的终态(未标记=待确认)
  meta?: string;
  error?: boolean;
  retry?: string;
}
interface Health {
  status?: string;
  mode?: string;
  alertsReady?: boolean;
  indexReady?: boolean;
}
type PetState = "idle" | "thinking" | "success" | "offline";

const open = ref(false);
const petTop = ref<number | null>(null);
const petLeft = ref<number | null>(null);
const question = ref("");
const sending = ref(false);
const online = ref(false);
const health = ref<Health | null>(null);
const backendChoice = ref("auto");
const messages = ref<Msg[]>([]);
const petState = ref<PetState>("idle");
const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
const winW = ref(window.innerWidth);
const winH = ref(window.innerHeight);
const flipped = ref(false); // 吸附到左边缘时立绘镜像(鲸鱼娘朝向随位置变)

const chips = [
  "现行异常检测模型在 TEST 段误报了几条?",
  "TEST 段告警清单给我,挑一条下钻讲讲为什么",
  "对比各版本异常检测模型的盲评成绩",
  "某站光伏板今天的发电量",
];

// ---------- 迷你 Markdown 渲染(先转义后组标签,杜绝 XSS) ----------

function esc(s: string): string {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}
function inlineMd(s: string): string {
  let e = esc(s);
  e = e.replace(/`([^`]+)`/g, "<code>$1</code>");
  e = e.replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>");
  e = e.replace(/(^|[^*])\*([^*\n]+)\*/g, "$1<i>$2</i>");
  // 链接一律转成纯文本:内部路径不可点,也不给钓鱼链接开口子
  e = e.replace(/\[([^\]]+)\]\([^)]*\)/g, '<span class="md-ref">$1</span>');
  return e;
}
function blockMd(text: string): string {
  const lines = String(text).split(/\r?\n/);
  const out: string[] = [];
  let para: string[] = [];
  let list: { t: string; items: string[] } | null = null;
  let table: string[][] | null = null;
  const flushPara = () => { if (para.length) { out.push(`<p>${inlineMd(para.join(" "))}</p>`); para = []; } };
  const flushList = () => { if (list) { out.push(`<${list.t}>${list.items.map((x) => `<li>${inlineMd(x)}</li>`).join("")}</${list.t}>`); list = null; } };
  const flushTable = () => {
    if (table) {
      const [head, ...body] = table;
      out.push(
        `<table><thead><tr>${head.map((c) => `<th>${inlineMd(c)}</th>`).join("")}</tr></thead>` +
        `<tbody>${body.map((r) => `<tr>${r.map((c) => `<td>${inlineMd(c)}</td>`).join("")}</tr>`).join("")}</tbody></table>`,
      );
      table = null;
    }
  };
  for (const raw of lines) {
    const t = raw.trim();
    if (t.startsWith("|") && t.endsWith("|") && t.length > 2) {
      const cells = t.slice(1, -1).split("|").map((c) => c.trim());
      flushPara(); flushList();
      if (cells.every((c) => /^:?-{2,}:?$/.test(c))) continue; // 分隔行丢弃
      (table ||= []).push(cells);
      continue;
    }
    flushTable();
    if (!t) { flushPara(); flushList(); continue; }
    const h = t.match(/^(#{1,4})\s+(.*)/);
    if (h) { flushPara(); flushList(); out.push(`<div class="md-h" data-lv="${h[1].length}">${inlineMd(h[2])}</div>`); continue; }
    if (/^(-{3,}|\*{3,})$/.test(t)) { flushPara(); flushList(); out.push("<hr/>"); continue; }
    const bq = t.match(/^>\s?(.*)/);
    if (bq) { flushPara(); flushList(); out.push(`<blockquote>${inlineMd(bq[1])}</blockquote>`); continue; }
    const ul = t.match(/^[-*+]\s+(.*)/), ol = t.match(/^\d+[.)]\s+(.*)/);
    const li = ul || ol;
    if (li) {
      flushPara();
      const want = ul ? "ul" : "ol";
      if (!list || list.t !== want) { flushList(); list = { t: want, items: [] }; }
      list.items.push(li[1]);
      continue;
    }
    flushList();
    para.push(t);
  }
  flushPara(); flushList(); flushTable();
  return out.join("");
}
function renderMd(src: string): string {
  // 围栏代码块按 ``` 奇偶切段,段内整体转义,段间再走块级渲染
  return String(src)
    .split("```")
    .map((part, i) => {
      if (i % 2 === 0) return blockMd(part);
      const nl = part.indexOf("\n");
      const lang = nl > -1 ? part.slice(0, nl).trim() : "";
      const code = nl > -1 ? part.slice(nl + 1) : part;
      return `<pre class="md-pre"><span class="md-lang">${esc(lang || "code")}</span><button class="md-copy" type="button" data-code>复制</button><code>${esc(code.replace(/\n$/, ""))}</code></pre>`;
    })
    .join("");
}
async function onPanelClick(e: MouseEvent) {
  const btn = (e.target as HTMLElement).closest(".md-copy");
  if (!btn) return;
  const code = btn.parentElement?.querySelector("code")?.textContent || "";
  try { await navigator.clipboard.writeText(code); btn.textContent = "已复制"; window.setTimeout(() => (btn.textContent = "复制"), 1500); } catch { /* 无剪贴板权限就算了 */ }
}
// 模板里不许直接摸 navigator(不在 Vue 编译白名单,渲染会报 undefined)——包一层函数
async function copyAnswer(t: string) {
  try { await navigator.clipboard.writeText(t); } catch { /* 无剪贴板权限就算了 */ }
}

// ---------- 打字机 ----------

const typers = new Set<number>();
// 注意:typeInto 必须收到 messages.value 里的**代理对象**(push 后再按下标取),
// 直接写 push 前的原始对象不经过响应式陷阱,打字机就"只算不画"(验收实锤的卡字 bug)。
function typeInto(m: Msg, onDone?: () => void) {
  if (reduceMotion) { m.shown = m.text; m.html = renderMd(m.text); onDone?.(); return; }
  let i = 0;
  const timer = window.setInterval(() => {
    i = Math.min(m.text.length, i + 6);
    m.shown = m.text.slice(0, i);
    m.html = renderMd(m.shown);
    if (nearBottom.value && panelEl.value) panelEl.value.scrollTop = panelEl.value.scrollHeight; // 打字中贴着底走
    if (i >= m.text.length) {
      window.clearInterval(timer); typers.delete(timer); scrollPanel();
      onDone?.(); // 动作在吐完字之后才执行:用户正盯着气泡时页面不突跳
    }
  }, 28);
  typers.add(timer);
}
onBeforeUnmount(() => typers.forEach((t) => window.clearInterval(t)));

// ---------- 对话 ----------

async function checkHealth() {
  try {
    const r = await fetch(`${BASE}/health`, { signal: AbortSignal.timeout(2500) });
    health.value = await r.json();
    online.value = true;
    if (petState.value === "offline") petState.value = "idle"; // 恢复在线别留灰脸残留
  } catch {
    online.value = false;
    health.value = null;
  }
}

let ctrl: AbortController | null = null; // 停止按钮用的 AbortController

function pushNote(text: string, opts: { error?: boolean; retry?: string } = {}) {
  messages.value.push({ role: "assistant", text, shown: "", html: "", sources: [], ...opts });
  typeInto(messages.value[messages.value.length - 1]);
}
function flashFail() {
  // 失败脸(灰+X眼)与 success 对称:协议错/超时也看得见,1.4s 后在线则回 idle,
  // 绝不把宠物留在"永远 thinking"。
  petState.value = "offline";
  window.setTimeout(() => {
    if (petState.value === "offline" && online.value && !sending.value) petState.value = "idle";
  }, 1400);
}

// ---------- 页面动作(navigate 自动/fill 需确认,全部 fail-closed) ----------
let execBusy = false;
function voidPendingFills() {
  for (const x of messages.value) {
    if (x.actions?.some((a) => a.kind === "fill") && !x.fillState) x.fillState = "void";
  }
}
async function runNavs(m: Msg) {
  if (execBusy || m.naviDone) return;
  const navs = (m.actions || []).filter((a) => a.kind === "navigate");
  if (!navs.length) return;
  m.naviDone = true;
  execBusy = true;
  try {
    for (const a of navs) {
      try { await runAction(a, { doc: document }); pushNote(`已带你到「${a.label}」。`); }
      catch (e) { pushNote(`没能打开「${a.label}」:${e instanceof Error ? e.message : "页面状态不对"},可以手动点顶部导航。`, { error: true }); }
    }
  } finally { execBusy = false; }
}
async function runFill(m: Msg, a: PetAction) {
  if (execBusy || m.fillState) return;
  execBusy = true; // 同步置锁挡双击;成败之前芯片文案不变,不骗用户
  try {
    await runAction(a, { doc: document });
    m.fillState = "done";
    pushNote(`已在「${a.label}」填入「${a.value}」——未提交,请你亲手点查询确认。`);
  } catch (e) {
    m.fillState = "void";
    pushNote(`填入失败:${e instanceof Error ? e.message : "页面状态不对"},值没有改动。`, { error: true });
  } finally { execBusy = false; }
}

async function ask(text: string) {
  const q = String(text || "").trim();
  if (!q || sending.value) return;
  voidPendingFills(); // 新一轮对话开始,上一轮没确认的填入芯片作废
  question.value = "";
  messages.value.push({ role: "user", text: q, shown: q, html: "", sources: [] });
  sending.value = true;
  petState.value = "thinking";
  nearBottom.value = true;
  scrollPanel();
  ctrl = new AbortController();
  const watchdog = window.setTimeout(() => ctrl?.abort("timeout"), 120000);
  let gotResponse = false; // "连不上"与"服务端回了错误"的分水岭:看是否拿到过响应,不猜错误文本
  try {
    const r = await fetch(`${BASE}/advisor/ask`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        question: q,
        ...(backendChoice.value === "mock" ? { backend: "mock" } : {}),
      }),
      signal: ctrl.signal,
    });
    gotResponse = true;
    let data: Record<string, unknown> | null = null;
    try { data = await r.json(); } catch { data = null; }
    if (!r.ok || !data) {
      // 协议级:服务端明确回了错误(问题太大/后端配置等),气泡带 advice,可重试
      const errText = String((data && data.error) || `参谋服务返回 HTTP ${r.status}`);
      const advice = data && data.advice ? `\n${String(data.advice)}` : "";
      pushNote(errText + advice, { error: true, retry: q });
      flashFail();
    } else {
      const acts = resolveActions(data.actions); // 后端也不全信:本地注册表再过一遍
      const m: Msg = {
        role: "assistant",
        text: String(data.answer || "(空回答)"),
        shown: "",
        html: "",
        sources: (data.sources as string[] | undefined) || (data.citations as string[] | undefined) || [],
        actions: acts.length ? acts : undefined,
        meta: `${data.mode === "offline" ? "离线演示" : "在线查证"} · ${(data.rounds as number | undefined) || 1} 轮`,
      };
      messages.value.push(m);
      const proxy = messages.value[messages.value.length - 1];
      typeInto(proxy, () => { void runNavs(proxy); });
      online.value = true;
      petState.value = "success";
      window.setTimeout(() => { if (petState.value === "success") petState.value = "idle"; }, 1400);
    }
  } catch (err) {
    if (err instanceof Error && err.name === "AbortError") {
      if (ctrl && ctrl.signal.reason === "timeout") {
        pushNote("查询超时(120 秒),已自动停止——把问题拆小一点,或点重试。", { error: true, retry: q });
        flashFail();
      } else {
        pushNote("(已停止)");
      }
    } else if (!gotResponse) {
      // 网络级:fetch 直接 reject,才是真"连不上",给启动命令并可重试
      pushNote("连不上参谋服务(127.0.0.1:8765)。在仓库根目录跑:\n`python -m data_analysis.ml.advisor.serve`\n没起服务前也可切右上角「离线演示」看管线。",
               { error: true, retry: q });
      online.value = false;
      petState.value = "offline";
    } else {
      pushNote(err instanceof Error ? err.message : String(err), { error: true, retry: q });
      flashFail();
    }
  } finally {
    window.clearTimeout(watchdog);
    sending.value = false;
    ctrl = null;
    if (petState.value === "thinking") petState.value = online.value ? "idle" : "offline";
    scrollPanel();
  }
}
function stopAsk() {
  if (ctrl) ctrl.abort();
}

const state = computed<PetState>(() => (!online.value && !sending.value ? "offline" : petState.value));

// ---------- 拖动(阈值区分点/拖) ----------

const petEl = ref<HTMLElement | null>(null);
const panelEl = ref<HTMLElement | null>(null);
const squashing = ref(false);
const draggingNow = ref(false);
let dragging = false, moved = false, startX = 0, startY = 0, originLeft = 0, originTop = 0;

function onPointerDown(e: PointerEvent) {
  if (!petEl.value) return;
  dragging = true; moved = false; draggingNow.value = false;
  startX = e.clientX; startY = e.clientY;
  const rect = petEl.value.getBoundingClientRect();
  originLeft = rect.left; originTop = rect.top;
  try { petEl.value.setPointerCapture(e.pointerId); } catch { /* 合成事件等场景抓不住就算了 */ }
}
function onPointerMove(e: PointerEvent) {
  if (!dragging) return;
  const dx = e.clientX - startX, dy = e.clientY - startY;
  // 位移平方阈值区分"点一下"和"拖"(抄鲸鱼项目:平方距离 ≥9 才算动)
  if (!moved && dx * dx + dy * dy >= 9) { moved = true; draggingNow.value = true; }
  if (moved) {
    petLeft.value = Math.max(4, Math.min(winW.value - PET_SIZE - 4, originLeft + dx));
    petTop.value = Math.max(4, Math.min(winH.value - PET_SIZE - 8, originTop + dy));
  }
}
function onPointerUp(e: PointerEvent) {
  if (!dragging) return;
  dragging = false; draggingNow.value = false;
  try { petEl.value?.releasePointerCapture(e.pointerId); } catch { /* 指针已释放 */ }
  if (!moved) {
    squashing.value = true;
    window.setTimeout(() => (squashing.value = false), 360);
    toggle();
  } else {
    snapToEdge();
  }
}
// 四分吸附:中心落在横向/纵向两侧 1/4 外就吸到最近边;靠左边缘时镜像朝向(鲸鱼项目同款)
function snapToEdge() {
  if (petLeft.value == null || petTop.value == null) return;
  const cx = petLeft.value + PET_SIZE / 2, cy = petTop.value + PET_SIZE / 2;
  if (cx < winW.value * 0.25) petLeft.value = 6;
  else if (cx > winW.value * 0.75) petLeft.value = winW.value - PET_SIZE - 6;
  if (cy < winH.value * 0.2) petTop.value = 6;
  else if (cy > winH.value * 0.8) petTop.value = winH.value - PET_SIZE - 6;
  flipped.value = (petLeft.value ?? 999) <= 6;
}
// 触屏手势被系统接管(侧滑返回等)时浏览器发的是 pointercancel:不复位就会永久"抓着"
function onPointerCancel() {
  dragging = false; moved = false; draggingNow.value = false;
}
function toggle() {
  open.value = !open.value;
  if (open.value) { if (!online.value) checkHealth(); }
  else voidPendingFills(); // 关面板=放弃未确认的填入,不留悬空确认
}

// 窗口尺寸走响应式:resize 后面板翻转/夹取跟着重算,宠物本体也拉回视口内
function onResize() {
  winW.value = window.innerWidth; winH.value = window.innerHeight;
  if (petLeft.value != null) {
    petLeft.value = Math.max(4, Math.min(winW.value - PET_SIZE - 4, petLeft.value));
    flipped.value = petLeft.value <= 6;
  }
  if (petTop.value != null) petTop.value = Math.max(4, Math.min(winH.value - PET_SIZE - 8, petTop.value));
}

const panelStyle = computed(() => {
  if (petLeft.value == null) return { right: "22px", bottom: "104px" };
  const flipX = petLeft.value > winW.value - 420;
  const flipY = (petTop.value ?? 0) < winH.value - 520;
  return {
    left: flipX ? "auto" : `${petLeft.value + PET_SIZE + 6}px`,
    right: flipX ? `${winW.value - petLeft.value - PET_SIZE}px` : "auto",
    top: flipY ? "96px" : "auto", // 96 > 88px 粘性顶栏,贴顶展开也露得出头
    bottom: flipY ? "auto" : `${winH.value - (petTop.value ?? 0) - 8}px`,
  };
});

// 近底部判定(调研模式:自动滚只在用户贴底时发生,否则给"回到底部"按钮)
const nearBottom = ref(true);
const showJump = ref(false);
function onPanelScroll() {
  const el = panelEl.value;
  if (!el) return;
  nearBottom.value = el.scrollHeight - el.scrollTop - el.clientHeight < 80;
  if (nearBottom.value) showJump.value = false;
}
function jumpToBottom() {
  nearBottom.value = true;
  showJump.value = false;
  if (panelEl.value) panelEl.value.scrollTop = panelEl.value.scrollHeight;
}
function scrollPanel() {
  requestAnimationFrame(() => {
    const el = panelEl.value;
    if (!el) return;
    if (nearBottom.value) el.scrollTop = el.scrollHeight;
    else showJump.value = true;
  });
}
function onEnter(e: KeyboardEvent) {
  if (e.isComposing) return; // 中文输入法选词回车不发送
  e.preventDefault();
  ask(question.value);
}

let probe: number | undefined;
onMounted(() => {
  checkHealth();
  probe = window.setInterval(checkHealth, 30000);
  window.addEventListener("resize", onResize);
});
onBeforeUnmount(() => {
  if (probe !== undefined) window.clearInterval(probe);
  window.removeEventListener("resize", onResize);
});

const statusText = computed(() => {
  if (!online.value) return "服务未连接";
  return health.value?.mode === "offline" || backendChoice.value === "mock" ? "离线演示" : "在线查证";
});
</script>

<template>
  <!-- ===== 桌宠:鲸鱼娘立绘(MIT 化用),状态由 data-state 的 transform/filter 驱动 ===== -->
  <div
    ref="petEl"
    class="ai-pet"
    :class="{ squashing, grabbing: draggingNow, flip: flipped }"
    :data-state="state"
    :style="petTop == null ? { right: '20px', bottom: '20px' } : { left: petLeft + 'px', top: petTop + 'px' }"
    role="button"
    tabindex="0"
    aria-label="AI 运营参谋,点击对话"
    @pointerdown="onPointerDown"
    @pointermove="onPointerMove"
    @pointerup="onPointerUp"
    @pointercancel="onPointerCancel"
    @keydown.enter="toggle"
  >
    <div class="mascot-box">
      <img class="mascot-img" :src="whaleGirl" alt="鲸鱼娘桌宠" draggable="false" />
      <span v-show="state === 'thinking'" class="pet-think" aria-hidden="true"><b /><b /><b /></span>
    </div>
    <span class="pet-status" :class="{ ok: online }">{{ online ? "在线" : "离线" }}</span>
  </div>

  <!-- ===== 对话面板 ===== -->
  <Transition name="pp">
    <section v-if="open" class="pet-panel" :style="panelStyle" aria-label="AI 运营参谋对话">
      <div v-if="sending" class="pp-progress"><i></i></div>
      <header class="pp-head">
        <img class="pp-avatar" :src="whaleGirl" alt="" />
        <div class="pp-title">
          <b>AI 运营参谋</b>
          <small><i class="pp-dot" :class="{ on: online }"></i>{{ statusText }}</small>
        </div>
        <select v-model="backendChoice" class="pp-mode" title="离线演示不需要网络和 API,答辩断网兜底">
          <option value="auto">自动</option>
          <option value="mock">离线演示</option>
        </select>
        <button class="pp-close" aria-label="收起" @click="open = false">
          <svg width="14" height="14" viewBox="0 0 14 14"><path d="M3 3 L11 11 M11 3 L3 11" stroke="#64748b" stroke-width="1.8" stroke-linecap="round" /></svg>
        </button>
      </header>

      <div ref="panelEl" class="pp-body" @scroll.passive="onPanelScroll" @click="onPanelClick">
        <p v-if="online && !health?.alertsReady" class="pp-note">
          参谋账本还没生成,部分问题会答“无依据”。先在仓库根目录跑
          <code>python -m data_analysis.ml.advisor.driver --prepare</code>
        </p>
        <template v-if="!messages.length">
          <p class="pp-hello">鲸鱼娘报到!我只说<b>有出处</b>的话——数字都来自平台留档的工件,查不到就明说“无依据”。试试问:</p>
          <button v-for="c in chips" :key="c" class="pp-chip" @click="ask(c)">
            {{ c }}<svg width="13" height="13" viewBox="0 0 24 24" fill="none"><path d="M5 12h14m-6-6 6 6-6 6" stroke="#536ba9" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" /></svg>
          </button>
        </template>
        <template v-for="(m, i) in messages" :key="i">
          <div class="pp-row" :class="m.role">
            <div v-if="m.role === 'assistant'" class="pp-mini"><img :src="whaleGirl" alt="" /></div>
            <div class="pp-bubble" :class="{ error: m.error }">
              <span v-if="m.role === 'user'">{{ m.text }}</span>
              <span v-else class="md" v-html="m.html"></span>
              <span v-if="m.role === 'assistant' && m.shown.length < m.text.length" class="caret"></span>
              <button
                v-if="m.role === 'assistant' && m.shown.length >= m.text.length && !m.error"
                class="pp-bcopy"
                title="复制回答"
                @click="copyAnswer(m.text)"
              >
                <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="9" y="9" width="12" height="12" rx="2" /><path d="M5 15V5a2 2 0 0 1 2-2h10" /></svg>
              </button>
            </div>
          </div>
          <div v-if="m.role === 'assistant' && !m.error && m.text !== '(已停止)'" class="pp-sources">
            <span class="pp-src-label">依据</span>
            <span v-if="!m.sources.length" class="pp-src none">无出处</span>
            <span v-for="s in m.sources" :key="s" class="pp-src">{{ s }}</span>
          </div>
          <div v-if="m.actions?.length" class="pp-acts">
            <span v-for="a in m.actions" :key="a.target" class="pp-act">
              <button v-if="a.kind === 'navigate' && !m.naviDone" type="button" @click="runNavs(m)">↪ 打开「{{ a.label }}」</button>
              <span v-else-if="a.kind === 'navigate'">✓ 已打开「{{ a.label }}」</span>
              <button v-else-if="!m.fillState" type="button" @click="runFill(m, a)">✎ 填入「{{ a.value }}」</button>
              <span v-else-if="m.fillState === 'done'">✓ 已填入「{{ a.label }}」(未提交)</span>
              <span v-else>已作废</span>
            </span>
          </div>
          <div v-if="m.role === 'assistant' && m.meta" class="pp-meta">{{ m.meta }}</div>
          <button v-if="m.error && m.retry" class="pp-retry" @click="ask(m.retry || '')">重试</button>
        </template>
        <div v-if="sending" class="pp-row assistant">
          <div class="pp-mini"><img :src="whaleGirl" alt="" /></div>
          <div class="pp-bubble pp-thinking">鲸鱼娘正在翻证据<i class="dots"><b></b><b></b><b></b></i></div>
        </div>
      </div>

      <button v-if="showJump" class="pp-jump" @click="jumpToBottom">
        ↓ 回到底部
      </button>
      <footer class="pp-foot">
        <input
          v-model="question"
          maxlength="200"
          placeholder="问点运营数据上的事…"
          @keydown.enter="onEnter"
        />
        <button v-if="sending" class="pp-send stop" title="停止本次提问" @click="stopAsk">
          <svg width="13" height="13" viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="2" fill="currentColor" /></svg>
        </button>
        <button v-else class="pp-send" :disabled="!question.trim()" @click="ask(question)">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none"><path d="M4 12h14m-6-6 6 6-6 6" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" /></svg>
        </button>
      </footer>
    </section>
  </Transition>
</template>

<style scoped>
/* ---------- 桌宠本体(立绘 + transform/filter 状态机) ---------- */
.ai-pet {
  position: fixed;
  z-index: 1000;
  width: 88px;
  height: 88px;
  cursor: grab;
  user-select: none;
  touch-action: none;
}
.ai-pet.grabbing { cursor: grabbing; }
.mascot-box {
  position: relative;
  width: 100%;
  height: 100%;
  transform-origin: 50% 100%; /* 按压 Q 弹以脚底为轴(鲸鱼项目同款参数) */
  transition: transform .22s cubic-bezier(.34, 1.56, .64, 1);
}
.ai-pet:hover .mascot-box { transform: rotate(-5deg) scale(1.04); }
.ai-pet.squashing .mascot-box { animation: ap-squash .32s cubic-bezier(.34, 1.56, .64, 1); }
@keyframes ap-squash {
  0% { transform: scale(1, 1); } 30% { transform: scale(1.12, .88); }
  60% { transform: scale(.94, 1.06); } 100% { transform: scale(1, 1); }
}
.mascot-img {
  width: 100%;
  height: 100%;
  object-fit: contain;
  pointer-events: none;
  filter: drop-shadow(0 8px 16px rgba(32, 49, 112, .30));
  transition: filter .25s;
}
.ai-pet.flip .mascot-img { transform: scaleX(-1); } /* 靠左吸附:镜像朝向,面板文字不受影响 */

/* thinking:轻呼吸 + 头顶冒思考小泡(描边 #203170,呼应鲸鱼项目气泡) */
.ai-pet[data-state="thinking"] .mascot-box { animation: ap-breathe 1.1s ease-in-out infinite; }
@keyframes ap-breathe { 0%, 100% { transform: translateY(0) scale(1); } 50% { transform: translateY(-3px) scale(1.03); } }
.pet-think { position: absolute; top: -6px; right: -4px; display: flex; gap: 3px; }
.pet-think b {
  width: 7px; height: 7px; border-radius: 50%;
  background: #fff; border: 1.6px solid #203170;
  animation: ap-thinkpop 1.2s ease-in-out infinite;
}
.pet-think b:nth-child(2) { animation-delay: .18s; }
.pet-think b:nth-child(3) { animation-delay: .36s; }
@keyframes ap-thinkpop { 0%, 70%, 100% { transform: translateY(0); opacity: .55; } 35% { transform: translateY(-5px); opacity: 1; } }

/* idle:慢漂 + 微微摆尾式的轻晃 */
.ai-pet[data-state="idle"] .mascot-box { animation: ap-float 3.2s ease-in-out infinite; }
@keyframes ap-float { 0%, 100% { transform: translateY(0) rotate(0); } 50% { transform: translateY(-4px) rotate(1.6deg); } }

/* success:庆祝跳 + 增饱和亮一圈 */
.ai-pet[data-state="success"] .mascot-img { filter: drop-shadow(0 8px 16px rgba(32, 49, 112, .30)) saturate(1.25) brightness(1.05); }
.ai-pet[data-state="success"] .mascot-box { animation: ap-celebrate .5s cubic-bezier(.32, .72, .35, 1); }
@keyframes ap-celebrate { 0% { transform: scale(1, 1); } 30% { transform: scale(1.12, .9) translateY(2px); } 60% { transform: scale(.94, 1.08) translateY(-8px); } 100% { transform: scale(1, 1); } }

/* offline:整只去色压暗、耷拉一点,停掉一切动画 */
.ai-pet[data-state="offline"] .mascot-img { filter: grayscale(.95) brightness(.82) drop-shadow(0 6px 12px rgba(100, 116, 139, .3)); }
.ai-pet[data-state="offline"] .mascot-box { transform: rotate(4deg) translateY(2px); }
.ai-pet[data-state="offline"] * { animation: none !important; }

.pet-status {
  position: absolute;
  bottom: -3px;
  left: 50%;
  transform: translateX(-50%);
  padding: 1px 9px;
  border-radius: 9px;
  font-size: 10px;
  color: #fff;
  background: #94a3b8;
  white-space: nowrap;
  pointer-events: none;
}
.pet-status.ok { background: #0d9488; }

/* ---------- 面板(设计 token:边框+轻阴影双轨,8px 间距节奏,iOS 曲线入场) ---------- */
.pet-panel {
  position: fixed;
  z-index: 1001; /* 压过粘性顶栏(z≈90)与一切页面内容,宠物和面板永远浮在最上 */
  width: 384px;
  max-width: calc(100vw - 24px);
  display: flex;
  flex-direction: column;
  background: #fff;
  border: 1px solid rgba(32, 49, 112, .16);
  border-radius: 16px;
  box-shadow: 0 12px 32px -12px rgba(15, 23, 42, .18), 0 2px 6px rgba(15, 23, 42, .06);
  overflow: hidden;
  font-size: 14px;
  line-height: 1.6;
}
.pp-progress { position: absolute; top: 0; left: 0; right: 0; height: 2px; z-index: 2; overflow: hidden; }
.pp-progress i {
  position: absolute;
  inset: 0;
  width: 40%;
  background: linear-gradient(90deg, transparent, #536ba9, #9fb0d9, transparent);
  animation: ap-slide 1.1s ease-in-out infinite;
}
@keyframes ap-slide { 0% { left: -40%; } 100% { left: 100%; } }

.pp-head {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 12px 16px;
  border-bottom: 1px solid #f1f5f9;
  background: linear-gradient(180deg, #fbfdff, #fff);
}
.pp-avatar {
  width: 34px;
  height: 34px;
  border-radius: 50%;
  object-fit: cover;
  object-position: 50% 38%; /* 圆裁取鲸鱼娘脸部 */
  border: 1.5px solid #203170;
  background: #f0f4fc;
}
.pp-title { flex: 1; display: flex; flex-direction: column; line-height: 1.25; }
.pp-title b { font-size: 16px; font-weight: 600; color: #0f172a; }
.pp-title small { font-size: 12px; color: #64748b; display: flex; align-items: center; gap: 5px; }
.pp-dot { width: 7px; height: 7px; border-radius: 50%; background: #cbd5e1; display: inline-block; }
.pp-dot.on { background: #10b981; box-shadow: 0 0 0 3px rgba(16, 185, 129, .15); }
.pp-mode {
  border: 1px solid #e2e8f0;
  border-radius: 8px;
  background: #f8fafc;
  color: #334155;
  font-size: 12px;
  padding: 3px 6px;
  cursor: pointer;
}
.pp-close { border: 0; background: none; cursor: pointer; padding: 6px; border-radius: 8px; display: flex; }
.pp-close:hover { background: #f1f5f9; }

.pp-body {
  flex: 1;
  overflow-y: auto;
  max-height: min(52vh, calc(100vh - 250px)); /* 矮窗口也保证头部/输入框不被挤出屏幕 */
  padding: 16px;
  display: flex;
  flex-direction: column;
  gap: 12px;
}
.pp-note { color: #92400e; background: #fef3c7; border-radius: 10px; padding: 8px 12px; font-size: 12px; }
.pp-note code { word-break: break-all; }
.pp-hello { color: #334155; margin: 0; font-size: 13px; }
.pp-chip {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 8px;
  text-align: left;
  border: 1px solid #dbe3f4;
  background: #f7f9ff;
  color: #203170;
  border-radius: 12px;
  padding: 10px 12px;
  font-size: 13px;
  cursor: pointer;
  transition: border-color .18s, transform .18s, box-shadow .18s;
}
.pp-chip:hover { border-color: #9fb0d9; transform: translateY(-1px); box-shadow: 0 4px 10px -4px rgba(32, 49, 112, .25); }
.pp-chip svg { flex: none; opacity: .55; }

.pp-row { display: flex; gap: 8px; align-items: flex-end; }
.pp-row.user { justify-content: flex-end; }
.pp-mini { width: 24px; height: 24px; flex: none; }
.pp-mini img {
  width: 24px;
  height: 24px;
  border-radius: 50%;
  object-fit: cover;
  object-position: 50% 38%;
  border: 1px solid #dbe3f4;
  background: #f0f4fc;
}
.pp-bubble {
  position: relative;
  max-width: 84%;
  padding: 9px 13px;
  border-radius: 14px;
  white-space: pre-wrap;
  word-break: break-word;
}
.pp-row.user .pp-bubble { background: #203170; color: #fff; border-bottom-right-radius: 4px; white-space: normal; }
.pp-row.assistant .pp-bubble {
  background: #fff;
  color: #0f172a;
  border: 1px solid #dbe3f4;
  border-bottom-left-radius: 4px;
  white-space: normal;
  box-shadow: 0 1px 3px rgba(15, 23, 42, .04);
}
.pp-bubble.error { background: #fef2f2; border-color: #fecaca; color: #991b1b; }
.pp-bcopy {
  position: absolute;
  top: 6px;
  right: 6px;
  border: 1px solid #dbe3f4;
  background: #fff;
  color: #94a3b8;
  border-radius: 6px;
  padding: 3px;
  cursor: pointer;
  display: none;
  line-height: 0;
}
.pp-row.assistant:hover .pp-bcopy { display: flex; }
.pp-bcopy:hover { color: #536ba9; border-color: #9fb0d9; }
.pp-thinking { color: #64748b; font-size: 13px; }
.dots { display: inline-flex; gap: 3px; margin-left: 6px; }
.dots b { width: 4px; height: 4px; border-radius: 50%; background: #536ba9; animation: ap-jump 1s infinite; }
.dots b:nth-child(2) { animation-delay: .15s; }
.dots b:nth-child(3) { animation-delay: .3s; }
@keyframes ap-jump { 0%, 60%, 100% { transform: translateY(0); } 30% { transform: translateY(-4px); } }
.caret { display: inline-block; width: 2px; height: 1em; margin-left: 1px; background: #536ba9; vertical-align: -2px; animation: ap-blink-c .8s step-end infinite; }
@keyframes ap-blink-c { 50% { opacity: 0; } }

.pp-sources { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; padding-left: 32px; }
.pp-src-label { font-size: 11px; color: #94a3b8; }
.pp-src {
  font-size: 11px;
  color: #203170;
  background: #f0f4fc;
  border: 1px solid #dbe3f4;
  border-radius: 6px;
  padding: 1px 8px;
}
.pp-src.none { color: #94a3b8; background: #f8fafc; border-color: #e2e8f0; }
.pp-retry {
  align-self: flex-start;
  margin-left: 32px;
  border: 1px solid #e2e8f0;
  background: #fff;
  color: #203170;
  font-size: 12px;
  border-radius: 8px;
  padding: 3px 12px;
  cursor: pointer;
}
.pp-retry:hover { border-color: #9fb0d9; background: #f7f9ff; }
.pp-jump {
  position: absolute;
  bottom: 66px;
  left: 50%;
  transform: translateX(-50%);
  border: 1px solid #dbe3f4;
  background: #fff;
  color: #203170;
  font-size: 12px;
  border-radius: 999px;
  padding: 4px 14px;
  cursor: pointer;
  box-shadow: 0 4px 14px -4px rgba(15, 23, 42, .18);
}
.pp-jump:hover { background: #f0f6ff; }
.pp-acts { display: flex; flex-wrap: wrap; gap: 6px; padding-left: 32px; }
.pp-act button {
  font-size: 12px;
  color: #203170;
  background: #eef3fd;
  border: 1px solid #dbe3f4;
  border-radius: 999px;
  padding: 3px 12px;
  cursor: pointer;
}
.pp-act button:hover { border-color: #9fb0d9; background: #f7f9ff; }
.pp-act span { font-size: 12px; color: #64748b; padding: 3px 4px; }
.pp-meta { font-size: 11px; color: #94a3b8; padding-left: 30px; }

.pp-foot { display: flex; gap: 8px; padding: 12px 16px; border-top: 1px solid #f1f5f9; background: #fbfcfe; }
.pp-foot input {
  flex: 1;
  border: 1px solid #e2e8f0;
  border-radius: 12px;
  padding: 9px 13px;
  font-size: 14px;
  outline: none;
  transition: border-color .18s, box-shadow .18s;
}
.pp-foot input:focus { border-color: #536ba9; box-shadow: 0 0 0 3px rgba(83, 107, 169, .14); }
.pp-send {
  border: 0;
  border-radius: 12px;
  width: 42px;
  background: #203170;
  color: #fff;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  transition: background .18s, transform .12s;
}
.pp-send:hover:not(:disabled) { background: #31509e; }
.pp-send:active:not(:disabled) { transform: scale(.93); }
.pp-send:disabled { background: #b9c4e6; cursor: not-allowed; }
.pp-send.stop { background: #334155; }
.pp-send.stop:hover { background: #1e293b; }

.pp-enter-active, .pp-leave-active { transition: opacity .28s cubic-bezier(.32, .72, 0, 1), transform .28s cubic-bezier(.32, .72, 0, 1); }
.pp-enter-from, .pp-leave-to { opacity: 0; transform: translateY(10px) scale(.985); }

/* ---------- Markdown 内容区(回答渲染,去 debug 味) ---------- */
.md :deep(p) { margin: 0 0 8px; }
.md :deep(p:last-child) { margin-bottom: 0; }
.md :deep(.md-h) { font-weight: 650; color: #0f172a; margin: 10px 0 6px; }
.md :deep(.md-h[data-lv="1"]), .md :deep(.md-h[data-lv="2"]) { font-size: 15px; }
.md :deep(.md-h[data-lv="3"]), .md :deep(.md-h[data-lv="4"]) { font-size: 14px; }
.md :deep(hr) { border: 0; border-top: 1px solid #e8edf5; margin: 10px 0; }
.md :deep(blockquote) { margin: 6px 0; padding: 2px 0 2px 10px; border-left: 3px solid #536ba9; color: #475569; }
.md :deep(ul), .md :deep(ol) { margin: 4px 0 8px; padding-left: 20px; }
.md :deep(li) { margin: 2px 0; }
.md :deep(code) {
  font-family: ui-monospace, "Cascadia Mono", Consolas, monospace;
  font-size: 12px;
  background: #f1f5f9;
  border-radius: 5px;
  padding: 1px 5px;
  color: #0f172a;
}
.md :deep(.md-pre) {
  position: relative;
  background: #0f172a;
  color: #e2e8f0;
  border-radius: 10px;
  padding: 26px 12px 10px;
  margin: 8px 0;
  overflow-x: auto;
  white-space: pre-wrap; /* 小面板里长行折行,不撑破气泡 */
  word-break: break-all;
}
.md :deep(.md-pre code) { background: none; color: inherit; padding: 0; font-size: 12px; line-height: 1.55; }
.md :deep(.md-lang) {
  position: absolute;
  top: 6px;
  left: 12px;
  font-size: 10px;
  letter-spacing: .06em;
  text-transform: uppercase;
  color: #64748b;
}
.md :deep(.md-copy) {
  position: absolute;
  top: 5px;
  right: 8px;
  border: 1px solid #334155;
  background: #1e293b;
  color: #94a3b8;
  font-size: 10px;
  border-radius: 6px;
  padding: 1px 8px;
  cursor: pointer;
}
.md :deep(.md-copy:hover) { color: #e2e8f0; }
.md :deep(table) {
  border-collapse: collapse;
  width: auto;
  max-width: 100%;
  margin: 8px 0;
  font-size: 12.5px;
  display: block;
  overflow-x: auto;
}
.md :deep(th) {
  background: #f1f5f9;
  font-weight: 600;
  color: #334155;
  text-align: left;
}
.md :deep(th), .md :deep(td) { padding: 6px 13px; border: 0; border-bottom: 1px solid #e2e8f0; white-space: nowrap; }
.md :deep(tr:last-child td) { border-bottom: 0; }
.md :deep(.md-ref) { color: #475569; border-bottom: 1px dashed #cbd5e1; }

@media (prefers-reduced-motion: reduce) {
  .ai-pet *, .pet-panel, .pp-progress i, .dots b, .caret { animation: none !important; transition: none !important; }
}
@media (max-width: 640px) { .pet-panel { width: calc(100vw - 20px); } }
</style>
