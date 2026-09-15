import { afterEach, describe, expect, it, vi } from "vitest";
import { ApiError, request } from "./api";

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

function pendingFetch() {
  const fetchMock = vi.fn(
    (_url: string, init: RequestInit) =>
      new Promise<Response>((_resolve, reject) => {
        const signal = init.signal!;
        const abort = () => reject(new DOMException("Aborted", "AbortError"));
        if (signal.aborted) abort();
        else signal.addEventListener("abort", abort, { once: true });
      }),
  );
  vi.stubGlobal("window", globalThis);
  vi.stubGlobal("fetch", fetchMock);
  return fetchMock;
}
describe("API boundary", () => {
  it("keeps unavailable model responses as errors rather than predictions", async () => {
    vi.stubGlobal("window", globalThis);
    vi.stubGlobal(
      "fetch",
      vi.fn().mockResolvedValue(
        new Response(
          JSON.stringify({
            error: { code: "MODEL_NOT_READY", message: "模型未就绪" },
          }),
          { status: 503 },
        ),
      ),
    );
    await expect(request("/load")).rejects.toMatchObject({
      code: "MODEL_NOT_READY",
      status: 503,
    });
  });
  it("preserves backend RUNNING instead of claiming completion", async () => {
    vi.stubGlobal("window", globalThis);
    vi.stubGlobal(
      "fetch",
      vi.fn().mockResolvedValue(
        new Response(JSON.stringify({ data: { status: "RUNNING" } }), {
          status: 202,
        }),
      ),
    );
    expect(
      (
        await request<{ status: string }>("/admin/experiments", {
          method: "POST",
          admin: "unit-test-token",
        })
      ).data.status,
    ).toBe("RUNNING");
  });
  it("translates state conflicts without swallowing rejection", async () => {
    vi.stubGlobal("window", globalThis);
    vi.stubGlobal(
      "fetch",
      vi.fn().mockResolvedValue(
        new Response(
          JSON.stringify({
            error: { code: "EXPERIMENT_RUNNING", message: "already running" },
          }),
          { status: 409 },
        ),
      ),
    );
    await expect(
      request("/admin/experiments", { method: "POST" }),
    ).rejects.toThrow("已有实验正在计算");
  });
  it("rejects HTML/proxy responses safely", async () => {
    vi.stubGlobal("window", globalThis);
    vi.stubGlobal(
      "fetch",
      vi
        .fn()
        .mockResolvedValue(
          new Response("<html>Proxy error</html>", { status: 502 }),
        ),
    );
    await expect(request("/bootstrap")).rejects.toBeInstanceOf(ApiError);
  });
  it.each(["null", "[]", "42", '"response"'])(
    "rejects non-object JSON %s as a response error",
    async (body) => {
      vi.stubGlobal("window", globalThis);
      vi.stubGlobal(
        "fetch",
        vi.fn().mockResolvedValue(new Response(body, { status: 200 })),
      );
      await expect(request("/bootstrap")).rejects.toMatchObject({
        code: "INVALID_RESPONSE",
        status: 200,
      });
    },
  );
  it("enforces the 30-second timeout even with a caller signal", async () => {
    vi.useFakeTimers();
    const fetchMock = pendingFetch();
    const caller = new AbortController();
    const remove = vi.spyOn(caller.signal, "removeEventListener");
    const pending = request("/bootstrap", { signal: caller.signal });
    const rejection = expect(pending).rejects.toMatchObject({
      code: "TIMEOUT",
      status: 0,
    });
    expect(fetchMock.mock.calls[0][1].signal).not.toBe(caller.signal);
    await vi.advanceTimersByTimeAsync(30000);
    await rejection;
    expect(caller.signal.aborted).toBe(false);
    expect(fetchMock.mock.calls[0][1].signal?.aborted).toBe(true);
    expect(remove).toHaveBeenCalledWith("abort", expect.any(Function));
    expect(vi.getTimerCount()).toBe(0);
  });
  it("forwards caller cancellation immediately and cleans up the timeout", async () => {
    vi.useFakeTimers();
    const fetchMock = pendingFetch();
    const caller = new AbortController();
    const remove = vi.spyOn(caller.signal, "removeEventListener");
    const pending = request("/bootstrap", { signal: caller.signal });
    const rejection = expect(pending).rejects.toMatchObject({
      code: "REQUEST_CANCELLED",
      status: 0,
    });
    caller.abort();
    await rejection;
    expect(fetchMock.mock.calls[0][1].signal?.aborted).toBe(true);
    expect(remove).toHaveBeenCalledWith("abort", expect.any(Function));
    expect(vi.getTimerCount()).toBe(0);
  });
  it("honors an already-aborted caller signal", async () => {
    vi.useFakeTimers();
    const fetchMock = pendingFetch();
    const caller = new AbortController();
    caller.abort();
    await expect(
      request("/bootstrap", { signal: caller.signal }),
    ).rejects.toMatchObject({ code: "REQUEST_CANCELLED" });
    expect(fetchMock.mock.calls[0][1].signal?.aborted).toBe(true);
    expect(vi.getTimerCount()).toBe(0);
  });
  it("removes the caller listener after a successful response", async () => {
    vi.useFakeTimers();
    vi.stubGlobal("window", globalThis);
    vi.stubGlobal(
      "fetch",
      vi
        .fn()
        .mockResolvedValue(
          new Response(JSON.stringify({ data: {} }), { status: 200 }),
        ),
    );
    const caller = new AbortController();
    const remove = vi.spyOn(caller.signal, "removeEventListener");
    await request("/bootstrap", { signal: caller.signal });
    expect(remove).toHaveBeenCalledWith("abort", expect.any(Function));
    expect(vi.getTimerCount()).toBe(0);
  });
  it("matches the HTTP nickname limit in the translated error", async () => {
    vi.stubGlobal("window", globalThis);
    vi.stubGlobal(
      "fetch",
      vi.fn().mockResolvedValue(
        new Response(JSON.stringify({ error: { code: "INVALID_NAME" } }), {
          status: 422,
        }),
      ),
    );
    await expect(request("/sessions")).rejects.toThrow("1–40");
  });
});
