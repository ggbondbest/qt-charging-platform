<script setup lang="ts">
import { computed } from "vue";
import { number } from "../display";
import type { Candidate, City, JsonObject, Location } from "../types";
import Icon from "./Icon.vue";
import StationMap from "./StationMap.vue";

const props = defineProps<{
  candidate?: Candidate;
  city?: City;
  origin?: Location;
  route?: JsonObject;
  loading: boolean;
  error: string;
}>();
const emit = defineEmits<{ close: []; retry: [] }>();
const validMetric = (value: unknown): value is number =>
  typeof value === "number" && Number.isFinite(value) && value >= 0;
const coordinates = computed<[number, number][]>(() => {
  const points: unknown = props.route?.coordinates;
  if (!Array.isArray(points) || points.length < 2) return [];
  return points.every(point => Array.isArray(point) && point.length === 2
    && typeof point[0] === "number" && Number.isFinite(point[0]) && Math.abs(point[0]) <= 90
    && typeof point[1] === "number" && Number.isFinite(point[1]) && Math.abs(point[1]) <= 180)
    ? points as [number, number][] : [];
});
const source = computed(() => {
  if (props.route?.routeSource === "TENCENT_CURRENT_TRAFFIC") return {
    label: "腾讯道路路线",
    notice: "腾讯当前道路路线；与历史回放日期的路况无关。",
    demo: false,
  };
  if (props.route?.routeSource === "STRAIGHT_LINE_DEMO") return {
    label: "DEMO · 直线示意",
    notice: "当前为出发点与电站之间的演示连线，并非道路导航；请勿用于实际驾驶。",
    demo: true,
  };
  return {
    label: "路线来源未确认",
    notice: "路线来源未确认，当前仅供位置参考。",
    demo: true,
  };
});
const hasRouteEta = computed(() => validMetric(props.route?.etaMinutes));
const hasRouteDistance = computed(() => validMetric(props.route?.distanceKm));
const eta = computed(() => hasRouteEta.value ? props.route!.etaMinutes : props.candidate?.etaMinutes);
const distance = computed(() => hasRouteDistance.value ? props.route!.distanceKm : props.candidate?.distanceKm);
const providerNotice = computed(() => {
  if (props.route?.routeSource === "TENCENT_CURRENT_TRAFFIC"
    || props.route?.routeSource === "STRAIGHT_LINE_DEMO") return "";
  const notice = typeof props.route?.notice === "string" ? props.route.notice.trim() : "";
  return notice !== source.value.notice ? notice : "";
});
</script>

<template>
  <section class="recommendation-route" aria-labelledby="recommendation-route-title" :aria-busy="loading">
    <header class="route-heading">
      <div>
        <p class="route-caption"><Icon name="road" :size="15" />路线预览</p>
        <h2 id="recommendation-route-title">{{ candidate ? `前往${candidate.stationName}` : '选择电站查看路线' }}</h2>
      </div>
      <button class="route-close" type="button" @click="emit('close')"><Icon name="close" :size="15" />收起路线</button>
    </header>

    <p v-if="!candidate" class="route-state" role="status">请先从推荐结果中选择一座电站。</p>
    <p v-else-if="!origin" class="route-state" role="status">请先设置出发点，再查看到电站的路线。</p>
    <p v-else-if="loading" class="route-state" role="status">正在获取到电站的路线…</p>
    <p v-else-if="error" class="route-state route-error" role="alert">路线加载失败：{{ error }}</p>
    <template v-else-if="route">
      <div class="route-summary">
        <span class="route-source" :class="{ demo: source.demo }">{{ source.label }}</span>
        <dl class="route-metrics">
          <div><dt>{{ hasRouteEta ? '预计行车时间' : '推荐估算行车时间' }}</dt><dd>{{ number(eta, 1) }}<small> 分钟</small></dd></div>
          <div><dt>{{ hasRouteDistance ? '路线距离' : '推荐估算距离' }}</dt><dd>{{ number(distance, 1) }}<small> km</small></dd></div>
        </dl>
      </div>
      <p class="route-notice">{{ source.notice }}</p>
      <p v-if="providerNotice" class="route-provider-notice">{{ providerNotice }}</p>
      <div v-if="coordinates.length" class="route-map">
        <StationMap :key="candidate.stationId" :city="city" :stations="[candidate]" :candidates="[candidate]"
          :origin="origin" :highlighted="candidate.stationId" :picking="false" :route-coordinates="coordinates" />
      </div>
      <p v-else class="route-state" role="status">暂未取得可展示的路线，请重试。</p>
    </template>
    <p v-else class="route-state" role="status">暂未取得路线，请重试。</p>

    <div v-if="candidate" class="route-actions">
      <button class="route-retry" type="button" :disabled="loading || !origin" @click="emit('retry')">
        <Icon name="refresh" :size="14" />{{ loading ? '正在请求路线…' : route && !error && coordinates.length ? '刷新路线' : '重试路线' }}
      </button>
    </div>
  </section>
</template>

<style scoped>
.recommendation-route { grid-column: 1 / -1; width: 100%; min-width: 0; padding: 22px; border: 1px solid #e5e7eb; border-radius: 14px; background: #fff; }
.route-heading { display: flex; align-items: flex-start; justify-content: space-between; gap: 16px; }
.route-caption { display: flex; align-items: center; gap: 6px; margin: 0 0 8px; color: #6b7280; font-size: 12px; }
.route-heading h2 { margin: 0; color: #171a20; font-size: 18px; line-height: 1.5; overflow-wrap: anywhere; }
.route-close, .route-retry { display: inline-flex; align-items: center; justify-content: center; gap: 6px; min-height: 36px; padding: 8px 10px; border-radius: 7px; font-size: 12px; line-height: 1.5; white-space: nowrap; }
.route-close { color: #6b7280; border: 1px solid #e5e7eb; }
.route-close:hover { background: #f9fafb; }
.route-close:focus-visible, .route-retry:focus-visible { outline: 2px solid #2563eb; outline-offset: 3px; }
.route-summary { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 16px; margin: 20px 0 12px; }
.route-source { padding: 5px 8px; border-radius: 5px; color: #2563eb; background: #eff6ff; font-size: 11px; font-weight: 600; }
.route-source.demo { color: #856123; background: #fffbeb; }
.route-metrics { display: flex; flex-wrap: wrap; gap: 28px; margin: 0; }
.route-metrics dt { color: #6b7280; font-size: 11px; }
.route-metrics dd { margin: 4px 0 0; color: #171a20; font-size: 19px; font-weight: 600; font-variant-numeric: tabular-nums; }
.route-metrics small { color: #6b7280; font-size: 11px; font-weight: 400; }
.route-notice, .route-provider-notice { margin: 5px 0; color: #6b7280; font-size: 12px; line-height: 1.7; overflow-wrap: anywhere; }
.route-map { height: 320px; margin-top: 16px; overflow: hidden; border: 1px solid #e5e7eb; border-radius: 10px; }
.route-map :deep(.station-map) { height: 100%; min-height: 0; }
.route-state { margin: 20px 0 8px; padding: 18px; border-radius: 8px; color: #6b7280; background: #f9fafb; font-size: 13px; line-height: 1.7; }
.route-error { color: #b45309; background: #fffbeb; }
.route-actions { display: flex; justify-content: flex-end; margin-top: 12px; }
.route-retry { color: #2563eb; background: #eff6ff; }
.route-retry:not(:disabled):hover { background: #dbeafe; }
.route-retry:disabled { cursor: wait; opacity: .55; }
@media (max-width: 600px) {
  .recommendation-route { padding: 16px; }
  .route-heading { gap: 10px; }
  .route-heading h2 { font-size: 16px; }
  .route-close { padding: 7px; }
  .route-summary { align-items: flex-start; }
  .route-metrics { gap: 18px; }
  .route-map { height: 290px; }
}
</style>
