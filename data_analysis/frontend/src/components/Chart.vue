<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, watch } from "vue";
import * as echarts from "echarts/core";
import { LineChart, BarChart, PieChart, HeatmapChart, ScatterChart, SankeyChart, SunburstChart } from "echarts/charts";
import {
  GridComponent,
  TooltipComponent,
  LegendComponent,
  DatasetComponent,
  VisualMapComponent,
  AriaComponent,
} from "echarts/components";
import { CanvasRenderer } from "echarts/renderers";
import type { EChartsCoreOption } from "echarts/core";
import { chartOptionForMotion, chartTheme } from "../chartTheme";
echarts.use([
  LineChart,
  BarChart,
  PieChart,
  HeatmapChart,
  ScatterChart,
  SankeyChart,
  SunburstChart,
  GridComponent,
  TooltipComponent,
  LegendComponent,
  DatasetComponent,
  VisualMapComponent,
  AriaComponent,
  CanvasRenderer,
]);
const props = defineProps<{ option: EChartsCoreOption; label: string }>();
const emit = defineEmits<{ select: [event: { name?: string; data?: unknown; seriesName?: string }] }>();
const element = ref<HTMLElement>();
let instance: echarts.ECharts | undefined;
let observer: ResizeObserver | undefined;
let motionPreference: MediaQueryList | undefined;
let resizeFrame: number | undefined;
function updateChart() {
  instance?.setOption(
    chartOptionForMotion(props.option, motionPreference?.matches ?? false),
    { notMerge: true },
  );
}
onMounted(() => {
  if (!element.value) return;
  motionPreference = window.matchMedia("(prefers-reduced-motion: reduce)");
  motionPreference.addEventListener("change", updateChart);
  instance = echarts.init(element.value, chartTheme);
  instance.on("click", event => emit("select", event as { name?: string; data?: unknown; seriesName?: string }));
  updateChart();
  observer = new ResizeObserver(() => {
    if (resizeFrame !== undefined) window.cancelAnimationFrame(resizeFrame);
    resizeFrame = window.requestAnimationFrame(() => {
      instance?.resize();
      resizeFrame = undefined;
    });
  });
  observer.observe(element.value);
});
watch(
  () => props.option,
  updateChart,
  { deep: true },
);
onBeforeUnmount(() => {
  motionPreference?.removeEventListener("change", updateChart);
  if (resizeFrame !== undefined) window.cancelAnimationFrame(resizeFrame);
  observer?.disconnect();
  instance?.dispose();
  instance = undefined;
});
</script>
<template>
  <div ref="element" class="chart" role="img" :aria-label="label"></div>
</template>
