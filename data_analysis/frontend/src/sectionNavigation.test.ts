import { nextTick } from "vue";
import { describe, expect, it, vi } from "vitest";
import { submitAndReveal } from "./sectionNavigation";

function resultElement(parentElement: HTMLElement | null = null) {
  return {
    parentElement,
    isConnected: true,
    focus: vi.fn(),
    scrollIntoView: vi.fn(),
  } as unknown as HTMLElement;
}

describe("paired experiment result navigation", () => {
  it.each(["READY", "RUNNING"])(
    "reveals the exact result section after the %s response and tab render",
    async (status) => {
      const target = resultElement();
      let rendered: HTMLElement | null = null;
      let selectedTab = "admin";
      const showResult = vi.fn(() => {
        selectedTab = "lab";
        nextTick(() => { rendered = target; });
      });
      await submitAndReveal(
        async () => ({ status }),
        showResult,
        () => rendered,
        () => selectedTab === "lab",
      );
      expect(showResult).toHaveBeenCalledWith({ status });
      expect(target.focus).toHaveBeenCalledWith({ preventScroll: true });
      expect(target.scrollIntoView).toHaveBeenCalledWith({ behavior: "auto", block: "start" });
    },
  );

  it("opens folded evidence ancestors before focusing and scrolling", async () => {
    const outer = { tagName: "DETAILS", open: false, parentElement: null };
    const inner = { tagName: "DETAILS", open: false, parentElement: outer };
    const target = resultElement(inner as unknown as HTMLElement);
    vi.mocked(target.scrollIntoView).mockImplementation(() => {
      expect(outer.open).toBe(true);
      expect(inner.open).toBe(true);
    });
    await submitAndReveal(async () => ({}), () => {}, () => target, () => true);
    expect(target.scrollIntoView).toHaveBeenCalledOnce();
  });

  it("keeps the control console and focus unchanged when submission fails", async () => {
    const target = resultElement();
    const showResult = vi.fn();
    const failure = new Error("管理员认证失败");
    await expect(submitAndReveal(
      async () => { throw failure; },
      showResult,
      () => target,
      () => true,
    )).rejects.toBe(failure);
    expect(showResult).not.toHaveBeenCalled();
    expect(target.focus).not.toHaveBeenCalled();
    expect(target.scrollIntoView).not.toHaveBeenCalled();
  });

  it.each(["changed-tab", "unmounted", "replaced"])(
    "does not scroll to a stale target after %s",
    async (reason) => {
      const target = resultElement();
      let current = true;
      let rendered = target;
      // A second render/navigation can occur after the result section is found.
      const getTarget = vi.fn(() => {
        nextTick(() => {
          if (reason === "changed-tab") current = false;
          if (reason === "unmounted") Object.assign(target, { isConnected: false });
          if (reason === "replaced") rendered = resultElement();
        });
        return rendered;
      });
      await submitAndReveal(async () => ({}), () => {}, getTarget, () => current);
      expect(target.focus).not.toHaveBeenCalled();
      expect(target.scrollIntoView).not.toHaveBeenCalled();
    },
  );

  it("safely skips an absent section", async () => {
    await expect(submitAndReveal(
      async () => ({}), () => {}, () => null, () => true,
    )).resolves.toBeUndefined();
  });
});
