import { afterEach, describe, expect, it, vi } from 'vitest';
import { createWorkspaceFullscreen } from './workspaceFullscreen';

afterEach(() => vi.unstubAllGlobals());
function environment() {
  class FullscreenDocument extends EventTarget {
    fullscreenElement: unknown = null;
    exitFullscreen = vi.fn(async () => { this.fullscreenElement = null; this.dispatchEvent(new Event('fullscreenchange')); });
  }
  const doc = new FullscreenDocument();
  const element = { requestFullscreen: vi.fn(async () => {
    doc.fullscreenElement = element; doc.dispatchEvent(new Event('fullscreenchange'));
  }) } as unknown as HTMLElement;
  vi.stubGlobal('document', doc);
  const fullscreen = createWorkspaceFullscreen(() => element);
  fullscreen.connect();
  return { doc, element, fullscreen };
}
describe('native browser and Qt fullscreen lifecycle', () => {
  it('requests actual fullscreen and tracks native Esc exits', async () => {
    const { doc, element, fullscreen } = environment();
    expect(fullscreen.active.value).toBe(false);
    await fullscreen.setActive(true);
    expect(element.requestFullscreen).toHaveBeenCalledTimes(1);
    expect(fullscreen.active.value).toBe(true);
    doc.fullscreenElement = null;
    doc.dispatchEvent(new Event('fullscreenchange'));
    expect(fullscreen.active.value).toBe(false);
    fullscreen.dispose();
  });
  it('never reports fullscreen for a rejected or unsupported request and permits retry', async () => {
    const { element, fullscreen } = environment();
    vi.mocked(element.requestFullscreen).mockRejectedValueOnce(new Error('Denied'));
    await fullscreen.setActive(true);
    expect(fullscreen.active.value).toBe(false);
    expect(fullscreen.error.value).toContain('无法切换全屏');
    await fullscreen.setActive(true);
    expect(fullscreen.active.value).toBe(true);
    expect(fullscreen.error.value).toBe('');
    fullscreen.dispose();
    const unsupported = createWorkspaceFullscreen(() => ({} as HTMLElement));
    await unsupported.setActive(true);
    expect(unsupported.active.value).toBe(false);
    expect(unsupported.error.value).toContain('无法切换全屏');
  });
  it('exits its own fullscreen element and cleans up listeners on disposal', async () => {
    const { doc, fullscreen } = environment();
    const remove = vi.spyOn(doc, 'removeEventListener');
    await fullscreen.setActive(true);
    await fullscreen.setActive(false);
    expect(doc.exitFullscreen).toHaveBeenCalledTimes(1);
    expect(fullscreen.active.value).toBe(false);
    await fullscreen.setActive(true);
    fullscreen.dispose();
    expect(doc.exitFullscreen).toHaveBeenCalledTimes(2);
    expect(remove).toHaveBeenCalledWith('fullscreenchange', expect.any(Function));
    expect(remove).toHaveBeenCalledWith('fullscreenerror', expect.any(Function));
  });
  it('does not close fullscreen owned by another element', async () => {
    const { doc, fullscreen } = environment();
    doc.fullscreenElement = {};
    await fullscreen.setActive(false);
    fullscreen.dispose();
    expect(doc.exitFullscreen).not.toHaveBeenCalled();
  });
});
