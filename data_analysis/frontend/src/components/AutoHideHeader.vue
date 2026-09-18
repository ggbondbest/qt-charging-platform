<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue';
import Icon from './Icon.vue';

const props = withDefaults(defineProps<{ autoHide?: boolean }>(), { autoHide: false });
const revealed = ref(false);
const expanded = computed(() => !props.autoHide || revealed.value);
const shell = ref<HTMLElement>();
const header = ref<HTMLElement>();
const trigger = ref<HTMLButtonElement>();

watch(() => props.autoHide, () => { revealed.value = false; });

function revealOnHover(event: PointerEvent) {
  if (props.autoHide && event.pointerType === 'mouse') revealed.value = true;
}
function hideOnLeave() {
  // Keep keyboard navigation visible until focus actually leaves the header.
  if (!header.value?.contains(document.activeElement)) revealed.value = false;
}
function hideOnBlur(event: FocusEvent) {
  if (!shell.value?.contains(event.relatedTarget as Node | null)) revealed.value = false;
}
async function openFromButton() {
  revealed.value = true;
  await nextTick();
  header.value?.querySelector<HTMLElement>('button, a[href]')?.focus({ preventScroll: true });
}
async function closeFromButton() {
  if (!props.autoHide) return;
  revealed.value = false;
  await nextTick();
  trigger.value?.focus({ preventScroll: true });
}
</script>

<template>
  <div ref="shell" class="navigation-shell" :class="{ 'navigation-shell--auto': autoHide, 'navigation-shell--expanded': expanded }"
    @pointerenter="revealOnHover" @pointerleave="hideOnLeave" @keydown.esc.stop="closeFromButton">
    <button v-if="autoHide" v-show="!expanded" ref="trigger" type="button" class="navigation-reveal"
      aria-label="显示主导航" aria-controls="primary-header" :aria-expanded="expanded"
      title="移到顶部或点击显示导航" @click="openFromButton"><span aria-hidden="true"></span></button>
    <header id="primary-header" ref="header" class="topbar" :inert="!expanded" :aria-hidden="!expanded ? true : undefined" @focusout="hideOnBlur">
      <slot />
      <button v-if="autoHide" type="button" class="navigation-collapse" aria-label="收起主导航" title="收起导航（Esc）" @click="closeFromButton">
        <Icon name="down" :size="18" />
      </button>
    </header>
  </div>
</template>

<style scoped>
.navigation-shell { position:sticky; top:0; z-index:1000; }
.navigation-shell>.topbar { position:relative; top:auto; }
.navigation-shell--auto { position:fixed; inset:0 0 auto; height:16px; }
.navigation-shell--auto.navigation-shell--expanded { height:auto; }
.navigation-shell--auto>.topbar { transform:translateY(-110%); visibility:hidden; pointer-events:none; transition:transform .22s ease; }
.navigation-shell--auto.navigation-shell--expanded>.topbar { transform:translateY(0); visibility:visible; pointer-events:auto; box-shadow:0 8px 24px #171a2010; }
.navigation-reveal { position:absolute; inset:0 0 auto; height:16px; padding:0; display:flex; align-items:flex-start; justify-content:center; pointer-events:auto; background:transparent; border:0; z-index:1; }
.navigation-reveal>span { width:64px; height:4px; margin-top:3px; border-radius:4px; background:#9aa5b580; transition:background .2s; }
.navigation-reveal:focus-visible { outline-offset:-2px; }
.navigation-reveal:focus-visible>span { background:#2563eb; }
.navigation-collapse { display:grid; place-items:center; flex:0 0 32px; height:32px; border:1px solid #e5e7eb; border-radius:6px; color:#6b7280; background:#fff; }
.navigation-collapse:hover { color:#171a20; background:#f5f5f7; }
.navigation-collapse>svg { transform:rotate(180deg); }
@media(hover:none),(pointer:coarse) {
  .navigation-reveal { inset:0 12px auto auto; width:64px; height:32px; background:#fffffff2; border:1px solid #e5e7eb; border-top:0; border-radius:0 0 8px 8px; align-items:center; }
  .navigation-reveal>span { width:28px; margin:0; }
}
@media(prefers-reduced-motion:reduce) { .navigation-shell--auto>.topbar,.navigation-reveal>span { transition:none; } }
</style>
