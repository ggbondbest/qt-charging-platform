/** Keep wheel gestures inside a floating panel without locking the page outside it. */
export function containPanelWheel(event: WheelEvent) {
  // Trackpad pinch-to-zoom must remain available.
  if (event.ctrlKey || event.defaultPrevented || (!event.deltaX && !event.deltaY)) return;
  const panel = event.currentTarget as HTMLElement | null;
  let element = event.target as HTMLElement | null;
  if (!panel || !element || !panel.contains(element)) return;

  event.stopPropagation();
  while (element && panel.contains(element)) {
    if (element.nodeType === 1) {
      const style = getComputedStyle(element);
      const vertical = canScroll(style.overflowY, element.scrollTop, element.scrollHeight - element.clientHeight, event.deltaY);
      const horizontal = canScroll(style.overflowX, element.scrollLeft, element.scrollWidth - element.clientWidth, event.deltaX);
      // Let the browser scroll the textarea, settings, or conversation normally.
      // Their overscroll-behavior contains any remainder at the boundary.
      if (vertical || horizontal) return;
    }
    if (element === panel) break;
    element = element.parentElement;
  }
  // Fixed headers/composers and exhausted scroll areas otherwise scroll the body.
  event.preventDefault();
}

function canScroll(overflow: string, position: number, maximum: number, delta: number) {
  if (!/^(auto|scroll|overlay)$/.test(overflow) || maximum <= 1 || !delta) return false;
  return delta < 0 ? position > 0 : position < maximum - 1;
}
