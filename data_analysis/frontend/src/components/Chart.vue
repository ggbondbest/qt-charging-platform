<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, watch } from "vue";
import * as echarts from "echarts/core";
import { LineChart, BarChart } from "echarts/charts";
import {
  GridComponent,
  TooltipComponent,
  LegendComponent,
  DatasetComponent,
} from "echarts/components";
import { CanvasRenderer } from "echarts/renderers";
import type { EChartsCoreOption } from "echarts/core";
echarts.use([
  LineChart,
  BarChart,
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
onMounted(() => {
  if (!element.value) return;
  instance = echarts.init(element.value);
  instance.setOption(props.option);
  observer = new ResizeObserver(() => instance?.resize());
  observer.observe(element.value);
});
watch(
  () => props.option,
  (option) => instance?.setOption(option, { notMerge: true }),
  { deep: true },
);
onBeforeUnmount(() => {
  observer?.disconnect();
  instance?.dispose();
});
</script>
<template>
  <div ref="element" class="chart" role="img" :aria-label="label"></div>
</template>
