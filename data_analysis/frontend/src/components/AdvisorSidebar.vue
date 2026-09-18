<script setup lang="ts">
import { nextTick, ref } from 'vue';
import OperationsAdvisor from './OperationsAdvisor.vue';
import Icon from './Icon.vue';
import character from '../assets/whale-girl.png';
import { containPanelWheel } from '../panelScroll';
// Original character: MeteorNOX/DeepSeek-Balance-Whale-Widget (MIT).
// Attribution is retained in assets/whale-girl.LICENSE.txt, as in PR #79.
import type { AdvisorTarget } from '../advisor';
import type { DashboardScope } from '../dashboardScope';

const emit = defineEmits<{ navigate: [target: AdvisorTarget, scope: DashboardScope] }>();
const open = ref(false);
const visited = ref(false);
const thinking = ref(false);
const launcher = ref<HTMLButtonElement>();
const panel = ref<HTMLElement>();

async function toggle() {
  if (open.value) { close(); return; }
  visited.value = true;
  open.value = true;
  await nextTick();
  panel.value?.focus({ preventScroll: true });
}
function close() {
  open.value = false;
  // Non-modal: the dashboard remains usable; Escape returns to the pet.
  launcher.value?.focus({ preventScroll: true });
}
function navigate(target: AdvisorTarget, scope: DashboardScope) {
  emit('navigate', target, scope);
  close();
}
</script>

<template>
  <div class="advisor-companion">
    <section v-if="visited" v-show="open" id="advisor-conversation" ref="panel" class="companion-panel"
      role="dialog" aria-labelledby="companion-title" :aria-hidden="!open" :inert="!open" tabindex="-1"
      @keydown.esc.stop.prevent="close" @wheel="containPanelWheel">
      <header class="companion-header">
        <div class="companion-portrait"><img :src="character" alt="" width="50" height="50"/></div>
        <div class="companion-heading"><h2 id="companion-title">AI 运营参谋</h2><p><span aria-hidden="true"/>陪你读懂每一份数据</p></div>
        <button type="button" class="companion-close" aria-label="收起运营参谋" title="收起（Esc）" @click="close"><Icon name="close" :size="18"/></button>
      </header>
      <div class="companion-content"><OperationsAdvisor compact :active="open" @navigate="navigate" @busy="thinking = $event"/></div>
    </section>

    <button ref="launcher" type="button" class="companion-launcher" :class="{ 'is-open': open, 'is-thinking': thinking }"
      :aria-label="open ? '收起运营参谋对话' : '打开运营参谋对话'" :aria-expanded="open"
      :aria-controls="visited ? 'advisor-conversation' : undefined" @click="toggle">
      <span class="companion-label">{{ thinking ? '正在看数据…' : open ? '收起对话' : '问问运营参谋' }}<span aria-hidden="true">{{ open ? '−' : '↗' }}</span></span>
      <img :src="character" alt="蓝发小助手" width="96" height="96" draggable="false"/>
      <span class="companion-shadow" aria-hidden="true"/>
    </button>
  </div>
</template>

<style scoped>
.advisor-companion { position:fixed; inset:auto 24px 16px auto; z-index:1100; color:#202631; }
.companion-launcher { position:relative; display:flex; align-items:center; gap:8px; padding:0 3px 5px 0; background:transparent; border:0; border-radius:18px; cursor:pointer; }
.companion-launcher>img { display:block; width:96px; height:96px; object-fit:contain; position:relative; z-index:1; filter:drop-shadow(0 4px 6px #18233c1a); transform-origin:50% 90%; transition:transform .2s ease; }
.companion-launcher:hover>img { transform:translateY(-4px) rotate(-3deg); }
.companion-launcher.is-open>img { transform:rotate(3deg); }
.companion-launcher:active>img { transform:scale(.94,.88); }
.companion-launcher.is-thinking>img { animation:companion-think 1.7s ease-in-out infinite; }
.companion-shadow { position:absolute; bottom:2px; right:14px; width:72px; height:9px; border-radius:50%; background:#24375918; filter:blur(3px); }
.companion-label { display:flex; align-items:center; gap:14px; padding:11px 14px; border:1px solid #e6e9ef; border-radius:14px 14px 4px 14px; background:#fffffff5; box-shadow:0 4px 18px #1625450a; font-size:12px; font-weight:500; letter-spacing:.2px; color:#465369; white-space:nowrap; }
.companion-label>span { font-size:17px; color:#7c89a0; }
.companion-panel { position:absolute; right:0; bottom:115px; width:min(440px,calc(100vw - 32px)); height:min(720px,calc(100vh - 151px)); height:min(720px,calc(100dvh - 151px)); display:flex; flex-direction:column; border:1px solid #e0e5ef; border-radius:22px; background:#fff; overflow:hidden; box-shadow:0 20px 75px #1a2b4c24,0 4px 16px #1a2b4c0b; animation:companion-appear .2s ease-out; }
.companion-panel { overscroll-behavior:contain; }
.companion-panel:focus { outline:none; }
.companion-header { display:flex; align-items:center; gap:12px; padding:17px 19px; flex-shrink:0; border-bottom:1px solid #edf0f5; background:linear-gradient(115deg,#f6f8fd,#fff 80%); }
.companion-portrait { width:48px; height:48px; flex-shrink:0; overflow:hidden; border:1px solid #e5eafa; border-radius:15px; background:#edf1fb; }
.companion-portrait img { width:48px; height:48px; object-fit:cover; transform:scale(1.14) translateY(4px); }
.companion-heading { flex:1; min-width:0; }.companion-heading h2 { margin:0; font-size:16px; font-weight:650; letter-spacing:.2px; }.companion-heading p { display:flex; align-items:center; gap:6px; margin:6px 0 0; color:#8993a3; font-size:11px; }.companion-heading p span { width:5px; height:5px; border-radius:50%; background:#7094d5; }
.companion-close { display:grid; place-items:center; width:32px; height:32px; border:1px solid #e5e9f0; border-radius:50%; color:#758195; background:#fff; flex-shrink:0; }.companion-close:hover { background:#f1f4f9; color:#29374b; }
.companion-content { flex:1; min-height:0; overflow:hidden; }
.companion-launcher:focus-visible,.companion-close:focus-visible { outline:2px solid #3564d8; outline-offset:4px; }
@keyframes companion-appear { from { opacity:0; transform:translateY(12px) scale(.98); } to { opacity:1; transform:none; } }
@keyframes companion-think { 0%,100% { transform:translateY(0) rotate(2deg); } 50% { transform:translateY(-4px) rotate(-2deg); } }
@media(max-width:600px) { .advisor-companion { right:12px; bottom:max(10px,env(safe-area-inset-bottom)); }.companion-launcher>img { width:76px; height:76px; }.companion-label { padding:9px 12px; font-size:11px; }.companion-panel { bottom:94px; width:calc(100vw - 24px); height:calc(100vh - 120px); height:calc(100dvh - 120px - env(safe-area-inset-bottom)); border-radius:18px; }.companion-header { padding:13px 16px; }.companion-shadow { width:54px; right:12px; } }
@media(prefers-reduced-motion:reduce) { .companion-panel { animation:none; }.companion-launcher>img { transition:none; animation:none !important; }.companion-launcher:hover>img { transform:none; } }
</style>
