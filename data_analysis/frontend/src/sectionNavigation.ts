import { nextTick } from "vue";

/** Navigate only after a request succeeds, then reveal its rendered result. */
export async function submitAndReveal<T>(
  submit: () => Promise<T>,
  showResult: (result: T) => void,
  getTarget: () => HTMLElement | null,
  isCurrent: () => boolean,
): Promise<void> {
  const result = await submit();
  showResult(result);
  // A tab change mounts the result section on Vue's next render, not immediately.
  await nextTick();
  const target = getTarget();
  if (!isCurrent() || !target?.isConnected) return;
  for (let parent = target.parentElement; parent; parent = parent.parentElement) {
    if (parent.tagName === "DETAILS") (parent as HTMLDetailsElement).open = true;
  }
  await nextTick();
  // Do not steal focus if the user has navigated elsewhere in the meantime.
  if (!isCurrent() || getTarget() !== target || !target.isConnected) return;
  target.focus({ preventScroll: true });
  target.scrollIntoView({ behavior: "auto", block: "start" });
}
