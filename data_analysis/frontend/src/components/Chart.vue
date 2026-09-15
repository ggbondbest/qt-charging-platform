<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, watch } from "vue";
import * as echarts from "echarts/core";
import { LineChart, BarChart, PieChart } from "echarts/charts";
import {
  GridComponent,
  TooltipComponent,
  LegendComponent,
  DatasetComponent,
} from "echarts/components";
import { CanvasRenderer } from "echarts/renderers";
import type { EChartsCoreOption } from "echarts/core";
import { chartOptionForMotion, chartTheme } from "../chartTheme";
echarts.use([
  LineChart,
  BarChart,
  PieChart,
  GridComponent,
  TooltipComponent,
  LegendComponent,
  DatasetComponent,
  CanvasRenderer,
]);
const props = defineProps<{ option: EChartsCoreOption; label: string }>();
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
