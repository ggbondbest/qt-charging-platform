import { describe, expect, it, vi } from "vitest";
import { h } from "vue";
import { renderToString } from "vue/server-renderer";
import RecommendationRoute from "./components/RecommendationRoute.vue";
import type { Candidate } from "./types";

vi.mock("./components/StationMap.vue", () => ({
  default: {
    props: ["stations", "candidates", "origin", "highlighted", "picking", "routeCoordinates"],
    setup: (props: Record<string, any>) => () => h("div", {
      "data-testid": "route-map",
      "data-stations": props.stations.map((station: Candidate) => station.stationId).join(","),
      "data-origin": JSON.stringify(props.origin),
      "data-highlighted": props.highlighted,
      "data-picking": String(props.picking),
      "data-coordinates": JSON.stringify(props.routeCoordinates),
    }),
  },
}));

const candidate: Candidate = {
  stationId: "station-1", stationName: "星海广场充电站", cityId: "dalian",
  latitude: 38.88, longitude: 121.59, capacity: 12, powerKw: 120, pricePerKwh: 1.4,
  rank: 1, score: 0.88, etaMinutes: 12.5, distanceKm: 5.8, routeSource: "ESTIMATED_DISTANCE_SPEED",
  currentFree: 4, expectedFree: 5, availableProbability: 0.9, waitMinutes: 2, waitP90Minutes: 6,
  serviceProbability: 0.95, arrivalTime: "2026-09-15T04:12:30Z", forecastTime: "2026-09-15T04:00:00Z",
  resolutionMinutes: 15, loadRatio: 0.4, rewardPoints: 100, scoreBreakdown: {}, reasons: [], totalMinutes: 14.5,
};
const origin = { latitude: 38.914, longitude: 121.6147 };
const coordinates = [[origin.latitude, origin.longitude], [candidate.latitude, candidate.longitude]];
const fallbackRoute = { routeSource: "STRAIGHT_LINE_DEMO", coordinates };
async function render(overrides: Record<string, unknown> = {}) {
  return renderToString(h(RecommendationRoute, { candidate, origin, loading: false, error: "", ...overrides }));
}

describe("recommendation route preview", () => {
  it("shows a source warning and recommendation estimates for the route API's metric-free fallback", async () => {
    const html = await render({ route: { ...fallbackRoute, notice: "演示连线，并非道路导航；请勿用于实际驾驶" } });
    expect(html).toContain("前往星海广场充电站");
    expect(html).toContain("DEMO · 直线示意");
    expect(html).toContain("并非道路导航");
    expect(html).toContain("推荐估算行车时间");
    expect(html).toContain("推荐估算距离");
    expect(html).toContain("12.5");
    expect(html).toContain("5.8");
    expect(html).not.toContain("route-estimate-notice");
    expect(html).not.toContain("route-provider-notice");
    expect(html.match(/并非道路导航/g)).toHaveLength(1);
    expect(html).toContain('data-stations="station-1"');
    expect(html).toContain('data-highlighted="station-1"');
    expect(html).toContain('data-picking="false"');
    expect(html).toContain('data-origin="{&quot;latitude&quot;:38.914,&quot;longitude&quot;:121.6147}"');
    expect(html).toContain('data-coordinates="[[38.914,121.6147],[38.88,121.59]]"');
    expect(html).toContain("刷新路线");
    expect(html).not.toMatch(/预约|支付|开始充电|积分到账|创建行程/);
  });

  it("distinguishes Tencent current roads from the historical replay without redundant provider notices", async () => {
    const html = await render({ route: { routeSource: "TENCENT_CURRENT_TRAFFIC", coordinates, notice: "路线服务说明" } });
    expect(html).toContain("腾讯道路路线");
    expect(html).toContain("与历史回放日期的路况无关");
    expect(html).not.toContain("路线服务说明");
    expect(html).not.toContain("route-provider-notice");
    expect(html).not.toContain("route-estimate-notice");
    expect(html).toContain("推荐估算行车时间");
    expect(html).not.toContain("DEMO · 直线示意");
  });

  it("uses route ETA and distance when available and marks only a missing metric as an estimate", async () => {
    const html = await render({ route: { ...fallbackRoute, etaMinutes: 8.2, distanceKm: 3.4 } });
    expect(html).toContain("预计行车时间");
    expect(html).toContain("路线距离");
    expect(html).toContain("8.2");
    expect(html).toContain("3.4");
    expect(html).not.toContain("推荐估算");
    const partial = await render({ route: { ...fallbackRoute, etaMinutes: 8.2 } });
    expect(partial).toContain("预计行车时间");
    expect(partial).toContain("推荐估算距离");
    expect(partial).not.toContain("推荐估算行车时间");
    expect(partial).not.toContain("route-estimate-notice");
  });

  it("hides stale routes during loading and offers an accessible loading state", async () => {
    const html = await render({ loading: true, route: fallbackRoute });
    expect(html).toContain('aria-busy="true"');
    expect(html).toContain('role="status"');
    expect(html).toContain("正在获取到电站的路线");
    expect(html).toMatch(/<button[^>]*class="route-retry"[^>]*disabled/);
    expect(html).not.toContain('data-testid="route-map"');
  });

  it("shows an error and retry without presenting stale geometry", async () => {
    const html = await render({ error: "连接超时", route: fallbackRoute });
    expect(html).toContain('role="alert"');
    expect(html).toContain("路线加载失败：连接超时");
    expect(html).toContain("重试路线");
    expect(html).toContain("收起路线");
    expect(html).not.toContain('data-testid="route-map"');
    expect(html).not.toMatch(/<button[^>]*disabled/);
  });

  it("handles missing selections, origins and responses with clear empty states", async () => {
    const empty = await render({ candidate: undefined });
    expect(empty).toContain("请先从推荐结果中选择一座电站");
    expect(empty).not.toContain("重试路线");
    const noOrigin = await render({ origin: undefined });
    expect(noOrigin).toContain("请先设置出发点");
    expect(noOrigin).toMatch(/<button[^>]*class="route-retry"[^>]*disabled/);
    expect(await render()).toContain("暂未取得路线，请重试");
  });

  it("does not pass malformed geometry to the map or label an unknown source as Tencent", async () => {
    const html = await render({ route: { coordinates: [[999, 121.6], [38.88, 121.59]], notice: "  路线数据暂不完整  " } });
    expect(html).toContain("路线来源未确认");
    expect(html).toContain("暂未取得可展示的路线，请重试");
    expect(html).not.toContain('data-testid="route-map"');
    expect(html).not.toContain("腾讯道路路线");
    expect(html).toMatch(/<p class="route-provider-notice"[^>]*>路线数据暂不完整<\/p>/);
    for (const notice of [undefined, "  ", {}, "路线来源未确认，当前仅供位置参考。"]) {
      expect(await render({ route: { coordinates, notice } })).not.toContain("route-provider-notice");
    }
  });
});
