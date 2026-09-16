import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { containPanelWheel } from './panelScroll';

class ScrollNode {
  nodeType = 1;
  parentElement: ScrollNode | null = null;
  overflowY = 'visible'; overflowX = 'visible';
  scrollTop = 0; scrollLeft = 0;
  scrollHeight = 100; clientHeight = 100;
  scrollWidth = 100; clientWidth = 100;
  constructor(parent?: ScrollNode, metrics: Partial<ScrollNode> = {}) {
    this.parentElement = parent || null; Object.assign(this, metrics);
  }
  contains(node: ScrollNode): boolean {
    return node === this || !!node.parentElement && this.contains(node.parentElement);
  }
}
function wheel(panel: ScrollNode, target: ScrollNode, deltaY: number, extra: Partial<WheelEvent> = {}) {
  const event = { currentTarget: panel, target, deltaY, deltaX: 0, ctrlKey: false,
    defaultPrevented: false, preventDefault: vi.fn(), stopPropagation: vi.fn(), ...extra };
  containPanelWheel(event as unknown as WheelEvent);
  return event;
}
beforeEach(() => vi.stubGlobal('getComputedStyle', (node: ScrollNode) => node));
afterEach(() => vi.unstubAllGlobals());

describe('floating panel scroll isolation', () => {
  it('blocks background scrolling over a fixed header or unscrollable composer', () => {
    const panel = new ScrollNode(undefined, { overflowY: 'hidden' });
    const form = new ScrollNode(panel, { overflowY: 'auto' });
    for (const target of [new ScrollNode(panel), new ScrollNode(form), new ScrollNode(form, { overflowY: 'auto' })]) {
      for (const delta of [-120, 120]) {
        const event = wheel(panel, target, delta);
        expect(event.preventDefault).toHaveBeenCalledOnce();
        expect(event.stopPropagation).toHaveBeenCalledOnce();
      }
    }
  });

  it('allows native conversation scrolling, then blocks chaining at both ends', () => {
    const panel = new ScrollNode();
    const chat = new ScrollNode(panel, { overflowY: 'auto', scrollHeight: 600, scrollTop: 200 });
    const message = new ScrollNode(chat);
    expect(wheel(panel, message, 120).preventDefault).not.toHaveBeenCalled();
    expect(wheel(panel, message, -120).preventDefault).not.toHaveBeenCalled();
    chat.scrollTop = 500;
    expect(wheel(panel, message, 120).preventDefault).toHaveBeenCalledOnce();
    expect(wheel(panel, message, -120).preventDefault).not.toHaveBeenCalled();
    chat.scrollTop = 0;
    expect(wheel(panel, message, -120).preventDefault).toHaveBeenCalledOnce();
  });

  it('preserves scrolling of expanded settings and long textarea contents', () => {
    const panel = new ScrollNode();
    const form = new ScrollNode(panel, { overflowY: 'auto', scrollHeight: 450 });
    const textarea = new ScrollNode(form, { overflowY: 'auto', scrollHeight: 300 });
    expect(wheel(panel, textarea, 100).preventDefault).not.toHaveBeenCalled();
    textarea.scrollTop = 200;
    expect(wheel(panel, textarea, 100).preventDefault).not.toHaveBeenCalled();
    form.scrollTop = 350;
    expect(wheel(panel, textarea, 100).preventDefault).toHaveBeenCalledOnce();
    expect(wheel(panel, textarea, -100).preventDefault).not.toHaveBeenCalled();
  });

  it('contains horizontal wheel gestures without blocking a scrollable field', () => {
    const panel = new ScrollNode();
    const field = new ScrollNode(panel, { overflowX: 'auto', scrollWidth: 400 });
    expect(wheel(panel, field, 0, { deltaX: 100 }).preventDefault).not.toHaveBeenCalled();
    field.scrollLeft = 300;
    expect(wheel(panel, field, 0, { deltaX: 100 }).preventDefault).toHaveBeenCalledOnce();
  });

  it('does not interfere with zoom, already-handled events, or the page outside the panel', () => {
    const panel = new ScrollNode(); const content = new ScrollNode(panel);
    for (const event of [wheel(panel, content, 100, { ctrlKey: true }),
      wheel(panel, content, 100, { defaultPrevented: true }), wheel(panel, content, 0),
      wheel(panel, new ScrollNode(), 100)]) {
      expect(event.preventDefault).not.toHaveBeenCalled();
      expect(event.stopPropagation).not.toHaveBeenCalled();
    }
  });
});
