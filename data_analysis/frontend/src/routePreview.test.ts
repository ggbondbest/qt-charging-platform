import { describe, expect, it, vi } from "vitest";
import { createRoutePreview } from "./routePreview";
import type { Candidate, JsonObject, Location } from "./types";

type FetchRoute = Parameters<typeof createRoutePreview>[0];
const start: Location = { latitude: 38.9, longitude: 121.6 };
function station(stationId: string): Candidate {
  return {
    stationId, stationName: stationId, cityId: "DL", latitude: 38.91, longitude: 121.61,
    capacity: 10, powerKw: 120, pricePerKwh: 1.2, rank: 1, score: 90,
    etaMinutes: 10, distanceKm: 3, routeSource: "STRAIGHT_LINE_DEMO", currentFree: 4,
    expectedFree: 3, availableProbability: 0.9, waitMinutes: 2, waitP90Minutes: 5,
    serviceProbability: 0.95, arrivalTime: "2026-09-15T04:10:00Z",
    forecastTime: "2026-09-15T04:00:00Z", resolutionMinutes: 5, loadRatio: 0.6,
    rewardPoints: 100, scoreBreakdown: {}, reasons: [], totalMinutes: 22,
  };
}
function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (cause: unknown) => void;
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
function route(stationId: string): JsonObject {
  return { stationId, coordinates: [[38.9, 121.6], [38.91, 121.61]] };
}

describe("route preview", () => {
  it("previews a station with only its ID and a copied origin", async () => {
    const pending = deferred<JsonObject>();
    const fetchRoute = vi.fn<FetchRoute>(() => pending.promise);
    const preview = createRoutePreview(fetchRoute);
    const selected = station("A");
    const source = { ...start };
    const result = preview.open(selected, source);

    const [input, signal] = fetchRoute.mock.calls[0];
    expect(input).toEqual({ stationId: "A", origin: start });
    expect(input.origin).not.toBe(source);
    expect(signal.aborted).toBe(false);
    source.latitude = 40;
    expect(input.origin).toEqual(start);
    expect(preview.origin.value).toEqual(start);
    expect(preview.candidate.value).toEqual(selected);
    expect(preview.loading.value).toBe(true);

    pending.resolve(route("A"));
    expect(await result).toBe(true);
    expect(preview.route.value).toEqual(route("A"));
    expect(preview.loading.value).toBe(false);
    expect(preview.error.value).toBe("");
  });

  it.each(["success", "error"])(
    "ignores a late %s from A after B succeeds",
    async outcome => {
      const a = deferred<JsonObject>();
      const b = deferred<JsonObject>();
      const fetchRoute = vi.fn<FetchRoute>()
        .mockReturnValueOnce(a.promise)
        .mockReturnValueOnce(b.promise);
      const preview = createRoutePreview(fetchRoute);
      const first = preview.open(station("A"), start);
      const second = preview.open(station("B"), { ...start, latitude: 38.8 });
      expect(fetchRoute.mock.calls[0][1].aborted).toBe(true);
      expect(fetchRoute.mock.calls[1][1].aborted).toBe(false);

      b.resolve(route("B"));
      expect(await second).toBe(true);
      if (outcome === "success") a.resolve(route("A"));
      else a.reject(new Error("old route failed"));
      expect(await first).toBe(false);
      expect(preview.candidate.value?.stationId).toBe("B");
      expect(preview.origin.value?.latitude).toBe(38.8);
      expect(preview.route.value).toEqual(route("B"));
      expect(preview.error.value).toBe("");
      expect(preview.loading.value).toBe(false);
    },
  );

  it.each(["success", "error"])(
    "keeps B loading when superseded A finishes with %s",
    async outcome => {
      const a = deferred<JsonObject>();
      const b = deferred<JsonObject>();
      const fetchRoute = vi.fn<FetchRoute>()
        .mockReturnValueOnce(a.promise)
        .mockReturnValueOnce(b.promise);
      const preview = createRoutePreview(fetchRoute);
      const first = preview.open(station("A"), start);
      const second = preview.open(station("B"), start);

      if (outcome === "success") a.resolve(route("A"));
      else a.reject(new Error("REQUEST_CANCELLED"));
      expect(await first).toBe(false);
      expect(preview.loading.value).toBe(true);
      expect(preview.route.value).toBeUndefined();
      expect(preview.error.value).toBe("");
      b.resolve(route("B"));
      expect(await second).toBe(true);
      expect(preview.loading.value).toBe(false);
      expect(preview.route.value).toEqual(route("B"));
    },
  );

  it.each(["success", "error"])("clears and cancels a closed preview before a late %s", async outcome => {
    const pending = deferred<JsonObject>();
    const fetchRoute = vi.fn<FetchRoute>().mockReturnValue(pending.promise);
    const preview = createRoutePreview(fetchRoute);
    const result = preview.open(station("A"), start);
    preview.close();
    expect(fetchRoute.mock.calls[0][1].aborted).toBe(true);
    if (outcome === "success") pending.resolve(route("A"));
    else pending.reject(new Error("REQUEST_CANCELLED"));
    expect(await result).toBe(false);
    expect(preview.candidate.value).toBeUndefined();
    expect(preview.origin.value).toBeUndefined();
    expect(preview.route.value).toBeUndefined();
    expect(preview.loading.value).toBe(false);
    expect(preview.error.value).toBe("");
  });

  it("retains the target on failure and copies the origin again on retry", async () => {
    const retry = deferred<JsonObject>();
    const fetchRoute = vi.fn<FetchRoute>()
      .mockRejectedValueOnce(new Error("路线服务暂不可用"))
      .mockReturnValueOnce(retry.promise);
    const preview = createRoutePreview(fetchRoute);
    const selected = station("A");
    const source = { ...start };
    expect(await preview.open(selected, source)).toBe(false);
    expect(preview.candidate.value).toEqual(selected);
    expect(preview.origin.value).toEqual(start);
    expect(preview.error.value).toBe("路线服务暂不可用");
    expect(preview.loading.value).toBe(false);
    expect(preview.route.value).toBeUndefined();

    source.latitude = 38.8;
    const result = preview.open(selected, source);
    const retryInput = fetchRoute.mock.calls[1][0];
    expect(retryInput.origin).toEqual({ ...start, latitude: 38.8 });
    expect(retryInput.origin).not.toBe(source);
    expect(retryInput.origin).not.toBe(fetchRoute.mock.calls[0][0].origin);
    expect(preview.error.value).toBe("");
    expect(preview.loading.value).toBe(true);
    source.latitude = 40;
    expect(retryInput.origin.latitude).toBe(38.8);
    expect(preview.origin.value?.latitude).toBe(38.8);
    retry.resolve(route("A"));
    expect(await result).toBe(true);
  });

  it("clears an existing route on reopening and handles non-Error failures", async () => {
    const fetchRoute = vi.fn<FetchRoute>()
      .mockResolvedValueOnce(route("A"))
      .mockRejectedValueOnce(null);
    const preview = createRoutePreview(fetchRoute);
    expect(await preview.open(station("A"), start)).toBe(true);
    expect(await preview.open(station("B"), start)).toBe(false);
    expect(preview.route.value).toBeUndefined();
    expect(preview.error.value).toBe("路线预览失败，请重试。");
    expect(preview.loading.value).toBe(false);
    preview.close();
    expect(preview.error.value).toBe("");
  });
});
