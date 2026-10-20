import { ref } from 'vue';

// Presentation follows the browser's actual fullscreen element, including Esc
// and Qt WebEngine's native exit, rather than treating a layout toggle as fullscreen.
export function createWorkspaceFullscreen(target: () => HTMLElement | undefined) {
  const active = ref(false);
  const error = ref('');
  const pending = ref(false);
  let connected = false;
  function sync() {
    active.value = !!target() && document.fullscreenElement === target();
    if (active.value) error.value = '';
  }
  function failed() { error.value = '无法切换全屏，请在 Qt 或支持全屏的浏览器中重试。'; sync(); }
  async function setActive(value: boolean) {
    if (pending.value) return;
    error.value = '';
    pending.value = true;
    try {
      if (value) {
        const element = target();
        if (!element?.requestFullscreen) throw new Error('Fullscreen unavailable');
        if (document.fullscreenElement !== element) await element.requestFullscreen();
      } else if (document.fullscreenElement === target() && document.exitFullscreen) {
        await document.exitFullscreen();
      }
      sync();
    } catch { failed(); }
    finally { pending.value = false; }
  }
  function connect() {
    if (connected) return;
    connected = true;
    document.addEventListener('fullscreenchange', sync);
    document.addEventListener('fullscreenerror', failed);
    sync();
  }
  function dispose() {
    if (!connected) return;
    connected = false;
    document.removeEventListener('fullscreenchange', sync);
    document.removeEventListener('fullscreenerror', failed);
    if (document.fullscreenElement === target() && document.exitFullscreen)
      void document.exitFullscreen().catch(() => {});
  }
  return { active, error, pending, setActive, connect, dispose };
}
