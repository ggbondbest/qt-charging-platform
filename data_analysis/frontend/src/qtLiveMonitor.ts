import { ref, shallowRef } from 'vue';
import { fetchQtLiveSnapshot, QtLiveError } from './qtLiveApi';
import type { QtLiveSnapshot } from './qtLiveApi';

export const QT_LIVE_POLL_MS = 7500;

export function createQtLiveMonitor(fetchSnapshot = fetchQtLiveSnapshot) {
  const snapshot = shallowRef<QtLiveSnapshot>();
  const loading = ref(false);
  const error = ref('');
  const active = ref(false);
  let sequence = 0;
  let disposed = false;
  let controller: AbortController | undefined;
  let timer: ReturnType<typeof setTimeout> | undefined;
  function clearTimer() { if (timer !== undefined) clearTimeout(timer); timer = undefined; }
  async function refresh() {
    if (!active.value || disposed || loading.value) return;
    clearTimer();
    const current = ++sequence;
    controller = new AbortController();
    loading.value = true;
    try {
      const data = await fetchSnapshot(controller.signal);
      if (current !== sequence || disposed || !active.value) return;
      snapshot.value = data;
      error.value = '';
    } catch (failure) {
      if (current !== sequence || disposed || !active.value) return;
      if (!(failure instanceof QtLiveError && failure.code === 'CANCELLED'))
        error.value = failure instanceof Error ? failure.message : 'Qt 业务快照读取失败。';
    } finally {
      if (current === sequence && !disposed) {
        loading.value = false;
        controller = undefined;
        if (active.value) timer = setTimeout(refresh, QT_LIVE_POLL_MS);
      }
    }
  }
  function setActive(value: boolean) {
    if (disposed || active.value === value) return;
    active.value = value;
    if (value) { void refresh(); return; }
    clearTimer();
    sequence++;
    controller?.abort();
    controller = undefined;
    loading.value = false;
  }
  function dispose() { setActive(false); disposed = true; }
  return { snapshot, loading, error, active, refresh, setActive, dispose };
}
