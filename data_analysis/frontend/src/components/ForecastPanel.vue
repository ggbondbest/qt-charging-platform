<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import Chart from './Chart.vue';
import Icon from './Icon.vue';
import { AnalyticsError, formatValue as n, fraction, publishedRequest, shiftDate } from '../analytics';
import type { Dataset, RecordData } from '../analytics';
import { forecastDisplayTimestamp, modelForHorizon, shanghaiInputToUtc, utcToShanghaiInput } from '../forecast';
const registry = ref<RecordData>(); const dataset = ref<Dataset>(); const stations = ref<RecordData[]>([]);
const target = ref('load'); const stationId = ref(''); const horizon = ref(6); const reference = ref('');
const loading = ref(false); const preparing = ref(false); const error = ref(''); const result = ref<RecordData>();
let controller: AbortController | undefined; let sequence = 0; let alive = true;
const capability = computed(() => registry.value?.[target.value]);
const selectedModel = computed(() => modelForHorizon(capability.value, horizon.value));
const first = computed(() => result.value?.points?.[0]);
const evidence = computed(() => capability.value?.metrics || capability.value?.evaluation || capability.value?.testMetrics || {});
const metricRows = computed(() => {
  const key = `h${String(horizon.value).padStart(2, '0')}`;
  const source = target.value === 'load' ? evidence.value.test?.[key] : evidence.value[key];
  if (!source) return [];
  const values = target.value === 'load'
    ? [{ key: 'TEST MAE / kW', value: source.gbdt?.mae }, { key: '持久性基线 MAE / kW', value: source.persistence?.mae }, { key: 'TEST RMSE / kW', value: source.gbdt?.rmse }]
    : [{ key: 'TEST 点预测 MAE / 桩', value: source.mae }, { key: '持久性基线 MAE / 桩', value: source.baselinePersistenceMae }, { key: '期望值 RMSE / 桩', value: source.rmse }, { key: '80% 区间实际覆盖率', value: source.coverage == null ? undefined : source.coverage * 100 }];
  return values.filter(row => typeof row.value === 'number');
});
const chartOption = computed(() => ({ color: ['#176b58', '#be7847', '#91aca0'], tooltip: { trigger: 'axis', confine: true }, legend: { bottom: 0, textStyle: { color: '#748178', fontSize: 11 } },
  grid: { left: 50, right: 22, top: 36, bottom: 62, containLabel: true },
  xAxis: { type: 'category', data: (result.value?.points || []).map((point: RecordData) => new Date(forecastDisplayTimestamp(point, result.value!.target || target.value)).toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', hour12: false })), axisLine: { lineStyle: { color: '#dce4de' } }, axisTick: { show: false }, axisLabel: { color: '#748178', fontSize: 10 } },
  yAxis: { type: 'value', name: result.value?.unit === 'kW' ? 'kW' : '桩', min: 0, splitLine: { lineStyle: { color: '#edf1eb' } }, axisLabel: { color: '#748178' } },
  series: [ { name: '模型预测', type: 'line', data: (result.value?.points || []).map((point: RecordData) => point.value), symbolSize: 7, connectNulls: false, lineStyle: { width: 3 }, areaStyle: { color: '#176b5810' } },
    ...(['lower', 'upper'] as const).filter(key => result.value?.points?.some((point: RecordData) => typeof point[key] === 'number')).map(key => ({ name: key === 'lower' ? '模型下界' : '模型上界', type: 'line', data: result.value!.points.map((point: RecordData) => point[key] ?? null), symbol: 'none', lineStyle: { width: 1.5, type: 'dashed' }, connectNulls: false })) ] }));
function clearResult() { sequence++; controller?.abort(); result.value = undefined; error.value = ''; loading.value = false; }
watch([target, stationId, horizon, reference], clearResult);
async function initialize() {
  preparing.value = true; error.value = '';
  try {
    const datasetResponse = await publishedRequest<RecordData>('/datasets');
    if (!alive) return;
    const source = datasetResponse.data.items?.[0];
    if (!source) throw new Error('暂无已发布数据集。');
    const pin = { datasetId: source.datasetId, publishedBatchId: source.publishedBatchId };
    const [modelResponse, stationResponse] = await Promise.all([publishedRequest<RecordData>('/intelligence/models', pin), publishedRequest<RecordData>('/stations', { ...pin, pageSize: 100 })]);
    if (!alive) return;
    dataset.value = source; registry.value = modelResponse.data; stations.value = stationResponse.data.items;
    stationId.value = stations.value[0]?.stationId || '';
    reference.value = utcToShanghaiInput(registry.value.defaultReferenceTime || registry.value.load?.defaultReferenceTime || `${shiftDate(source.endDate, -2)}T00:00:00Z`);
  } catch (failure) { if (alive) error.value = failure instanceof Error ? failure.message : '模型能力加载失败。'; }
  finally { if (alive) preparing.value = false; }
}
async function predict() {
  if (!dataset.value || !selectedModel.value || loading.value) return;
  const current = ++sequence; controller?.abort(); controller = new AbortController(); loading.value = true; error.value = ''; result.value = undefined;
  try {
    const request = { target: target.value, datasetId: dataset.value.datasetId, publishedBatchId: dataset.value.publishedBatchId, stationId: stationId.value, referenceTime: shanghaiInputToUtc(reference.value), horizonHours: Number(horizon.value), modelId: selectedModel.value.modelId };
    const response = await publishedRequest<RecordData>('/intelligence/forecast', {}, { method: 'POST', body: request, signal: controller.signal });
    if (current !== sequence || !alive) return;
    if (!Array.isArray(response.data.points) || !response.data.points.length) throw new Error('模型未返回预测点。');
    if (response.data.stationId && response.data.stationId !== request.stationId) throw new Error('预测站点与请求不一致。');
    if (response.data.modelId !== request.modelId) throw new Error('预测模型与请求不一致。');
    result.value = response.data;
  } catch (failure) {
    if (current === sequence && alive && !(failure instanceof AnalyticsError && failure.code === 'CANCELLED')) error.value = failure instanceof Error ? failure.message : '预测请求失败。';
  } finally { if (current === sequence && alive) loading.value = false; }
}
onMounted(initialize); onBeforeUnmount(() => { alive = false; sequence++; controller?.abort(); });
</script>
<template><section class="card intelligence-section"><div class="intelligence-header"><div><div class="eyebrow">ONE FORECAST WORKSPACE</div><h2>站点预测工作台</h2><p>同一入口查询负荷与空闲桩；模型缺失时明确提示，不用静态曲线替代。</p></div><span class="status-tag" :class="{ ready: capability?.status === 'READY' }">{{ preparing ? '检查模型…' : capability?.status === 'READY' ? '模型已就绪' : '等待模型产物' }}</span></div>
  <div class="intelligence-form"><label>预测目标<select v-model="target" aria-label="预测目标"><option value="load">负荷功率</option><option value="availability">空闲电桩</option></select></label><label class="wide">电站<select v-model="stationId" aria-label="预测电站"><option v-for="station in stations" :key="station.stationId" :value="station.stationId">{{ station.stationName }}</option></select></label><label>预测跨度<select v-model="horizon" aria-label="预测跨度"><option :value="1">未来 1 小时</option><option :value="6">未来 6 小时</option><option :value="24">未来 24 小时</option></select></label><label>预测起点（北京时间）<input v-model="reference" type="datetime-local" step="3600" aria-label="预测起点"/></label><button class="button primary" :disabled="loading || preparing || !selectedModel || !stationId" @click="predict"><Icon name="play" :size="15"/>{{ loading ? '模型推理中…' : '运行预测' }}</button></div>
  <p class="tiny-note">{{ target === 'load' ? '负荷目标为未来小时区间内平均功率（kW）；不等于同小时用电量。' : '空闲桩目标为各目标小时最后采样时刻 hh:55 的空闲数；不等同于行程 ETA 到站概率。' }} 起点与历史窗口须在已发布数据范围内。</p>
  <div v-if="error" role="alert" class="intelligence-inline-error">{{ error }} <button class="text-button" @click="initialize">重新检查模型</button></div>
  <div v-if="result" class="intelligence-output"><Chart :option="chartOption" :label="`${target === 'load' ? '负荷功率' : '空闲电桩'}模型预测曲线`"/><aside class="intelligence-summary"><small>首个预测点</small><strong>{{ n(first?.value, 2) }} <small>{{ result.unit === 'kW' ? 'kW' : '桩' }}</small></strong><span v-if="first?.probabilityNoFree != null">无空闲概率 {{ fraction(first.probabilityNoFree) }}</span><dl><dt>站点 / 模型</dt><dd>{{ stations.find(item => item.stationId === result!.stationId)?.stationName || result.stationId }}<br/>{{ result.modelId }}</dd><dt>目标含义</dt><dd>{{ target === 'load' ? '未来小时区间平均功率' : '目标小时末 hh:55 空闲桩数' }}</dd><dt>预测起点</dt><dd>{{ reference.replace('T', ' ') }} · 北京时间</dd></dl></aside></div>
  <div v-else class="intelligence-empty"><Icon name="chart" :size="28"/>{{ loading ? '正在使用实际模型计算预测…' : '选择电站、目标和时间，查看模型输出。' }}<p v-if="!selectedModel && !preparing">{{ capability?.message || capability?.reason || '当前跨度没有已发布的可用模型。' }}</p></div>
  <div v-if="metricRows.length" class="intelligence-metrics"><span v-for="metric in metricRows" :key="metric.key">{{ metric.key }} <b>{{ n(metric.value, 3) }}{{ metric.key.includes('覆盖率') ? '%' : '' }}</b></span></div>
  <p v-if="metricRows.length" class="tiny-note">{{ target === 'load' ? `负荷评价针对第 ${horizon} 小时目标；单位 kW。` : `空闲评价汇总未来 ${horizon} 小时各点；MAE 评价离散点预测，RMSE 评价分布期望，二者不可混读。` }}</p>
  <details v-if="capability" class="forecast-evidence"><summary class="text-button">查看实际模型信息与评价记录</summary><pre>{{ JSON.stringify(capability, null, 2) }}</pre></details>
</section></template>
