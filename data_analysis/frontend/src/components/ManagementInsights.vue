<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import Icon from './Icon.vue';
import { AnalyticsError, formatValue as n, fraction, publishedRequest } from '../analytics';
import type { Dataset, RecordData } from '../analytics';
const target = ref<'churn' | 'anomalies'>('churn'); const dataset = ref<Dataset>(); const report = ref<RecordData>();
const listing = ref<RecordData>(); const selected = ref<RecordData>(); const query = ref(''); const error = ref('');
const loading = ref(false); const detailLoading = ref(false); let sequence = 0; let detailSequence = 0; let alive = true;
let controller: AbortController | undefined; let detailController: AbortController | undefined;
const title = computed(() => target.value === 'churn' ? '用户回访风险' : '已结束会话异常筛查');
const metric = computed(() => report.value?.[target.value === 'churn' ? 'churn' : 'anomaly'] || {});
const evaluation = computed(() => metric.value.test || {});
const features: Record<string, string> = { daysSinceLastAttempt: '距上次请求 / 天', attempts30d: '30 天请求 / 次', energyKwh90d: '90 天电量 / kWh', feesYuan90d: '90 天费用 / 元', queueJoins90d: '90 天排队 / 次', durationMinutes: '会话时长 / 分钟', maxTemperatureC: '最高温度 / ℃', temperatureAboveContextC: '高于工况基线 / ℃', maxCellDifferenceV: '最大电芯压差 / V', averagePowerKw: '平均功率 / kW' };
const riskLabel: Record<string, string> = { HIGH: '较高', MEDIUM: '中等', LOW: '较低' };
const time = (value?: string) => value ? new Date(value).toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai', hour12: false }) : '—';
function pin() { return { datasetId: dataset.value?.datasetId, publishedBatchId: dataset.value?.publishedBatchId }; }
async function load() {
  const current = ++sequence; detailSequence++; controller?.abort(); detailController?.abort(); controller = new AbortController();
  loading.value = true; detailLoading.value = false; listing.value = undefined; selected.value = undefined; query.value = ''; error.value = '';
  try {
    if (!dataset.value) {
      const response = await publishedRequest<RecordData>('/datasets', {}, { signal: controller.signal });
      if (!alive || current !== sequence) return;
      dataset.value = response.data.items?.[0];
      if (!dataset.value) throw new Error('尚无已发布数据集。');
    }
    const [modelResponse, listResponse] = await Promise.all([
      publishedRequest<RecordData>('/intelligence/models', pin(), { signal: controller.signal }),
      publishedRequest<RecordData>(`/intelligence/insights/${target.value}`, { ...pin(), limit: 20 }, { signal: controller.signal }),
    ]);
    if (!alive || current !== sequence) return;
    report.value = modelResponse.data.insights; listing.value = listResponse.data;
    if (listing.value.publishedBatchId !== dataset.value.publishedBatchId) throw new Error('样本与统计发布批次不一致，请重新载入。');
    query.value = listing.value.items[0]?.userId || listing.value.items[0]?.sessionId || '';
    if (query.value) await inspect(query.value);
  } catch (failure) {
    if (alive && current === sequence && !(failure instanceof AnalyticsError && failure.code === 'CANCELLED')) { error.value = failure instanceof Error ? failure.message : '模型样本加载失败。'; listing.value = undefined; }
  } finally { if (alive && current === sequence) loading.value = false; }
}
async function inspect(id: string) {
  if (!id.trim() || !dataset.value) return;
  const current = ++detailSequence; detailController?.abort(); detailController = new AbortController(); selected.value = undefined; error.value = ''; detailLoading.value = true; query.value = id;
  try {
    const response = await publishedRequest<RecordData>(`/intelligence/insights/${target.value}/${encodeURIComponent(id.trim())}`, pin(), { signal: detailController.signal });
    if (alive && current === detailSequence) {
      if (response.data.publishedBatchId !== dataset.value.publishedBatchId) throw new Error('详情模型来源批次不一致。');
      selected.value = response.data;
    }
  } catch (failure) { if (alive && current === detailSequence && !(failure instanceof AnalyticsError && failure.code === 'CANCELLED')) error.value = failure instanceof Error ? failure.message : '详情加载失败。'; }
  finally { if (alive && current === detailSequence) detailLoading.value = false; }
}
watch(target, load); onMounted(load); onBeforeUnmount(() => { alive = false; sequence++; detailSequence++; controller?.abort(); detailController?.abort(); });
</script>
<template><section class="card intelligence-section"><div class="intelligence-header"><div><div class="eyebrow">OPERATIONS INTELLIGENCE</div><h2>把注意力，放在值得复核的地方</h2><p>对历史留出样本实际推理，用风险排序支持运营复核，不冒充自动诊断。</p></div><span class="tiny-tag">TEST 样本 · 模拟数据</span></div>
  <div class="insights-switch"><button :class="{ active: target === 'churn' }" @click="target = 'churn'">用户回访风险</button><button :class="{ active: target === 'anomalies' }" @click="target = 'anomalies'">充电异常筛查</button></div>
  <p class="tiny-note">{{ target === 'churn' ? '固定观察日前的行为 → 未来 14 天是否回访。分数是未经概率校准的风险排序，不是流失百分比，也不表示发券收益。' : '已结束充电会话 → 固定训练参照分数与阈值。告警需人工复核；当前主要识别温度异常，不能覆盖所有提前停止或功率降额。' }}</p>
  <div class="intelligence-metrics" v-if="target === 'churn' && evaluation.model"><span>TEST AUC <b>{{ n(evaluation.model.auc, 4) }}</b></span><span>距上次请求基线 AUC <b>{{ n(evaluation.recencyBaseline?.auc, 4) }}</b></span><span>TEST PR-AUC <b>{{ n(evaluation.model.prAuc, 4) }}</b></span></div>
  <div class="intelligence-metrics" v-if="target === 'anomalies' && Object.keys(evaluation).length"><span>查准率 <b>{{ fraction(evaluation.precision) }}</b></span><span>召回率 <b>{{ fraction(evaluation.recall) }}</b></span><span>F1 <b>{{ n(evaluation.f1, 4) }}</b></span><span>低召回意味着仍有漏报，不可作为设备安全保障</span></div>
  <form class="intelligence-form" style="margin-top:18px" @submit.prevent="inspect(query)"><label class="wide">{{ target === 'churn' ? '模拟用户编号' : '已结束会话编号' }}<input v-model="query" :aria-label="target === 'churn' ? '模拟用户编号' : '已结束会话编号'" :placeholder="target === 'churn' ? '从下方留出样本选择用户' : '从下方告警样本选择会话'"/></label><button class="button secondary" :disabled="detailLoading || !query.trim()">{{ detailLoading ? '推理中…' : '查询并解释' }}</button><button type="button" class="text-button" :disabled="loading" @click="load">刷新样本</button></form>
  <div v-if="error" role="alert" class="intelligence-inline-error">{{ error }}</div>
  <div v-if="loading" class="intelligence-empty">正在加载实际模型与留出样本…</div>
  <div v-else-if="listing" class="insights-grid" style="margin-top:20px"><div><p class="subtle" style="margin-bottom:12px">{{ title }} · {{ target === 'churn' ? '风险排序前' : '告警排序前' }} {{ listing.items.length }} / {{ n(listing.total) }} {{ target === 'churn' ? '位留出用户' : '笔告警会话' }}</p><div class="insights-table-wrap"><table class="insights-table"><thead><tr><th>对象</th><th>模型分数</th><th>{{ target === 'churn' ? '相对风险' : '告警标记' }}</th></tr></thead><tbody><tr v-for="row in listing.items" :key="row.userId || row.sessionId" :class="{ selected: (selected?.userId || selected?.sessionId) === (row.userId || row.sessionId) }"><td><button @click="inspect(row.userId || row.sessionId)">{{ row.userId || row.sessionId }}</button></td><td>{{ n(target === 'churn' ? row.riskScore : row.anomalyScore, 4) }}</td><td>{{ target === 'churn' ? (riskLabel[row.riskLevel] || row.riskLevel) : (row.flagged ? '需复核' : '未触发') }}</td></tr></tbody></table></div><p v-if="!listing.items.length" class="subtle">此批次没有符合条件的告警样本。</p><p class="tiny-note">参考时刻 {{ time(listing.referenceTime) }} · 北京时间<br/>{{ listing.modelId }}</p></div>
    <aside class="insights-detail"><template v-if="selected"><h3>{{ selected.userId || selected.sessionId }}</h3><dl><dt>{{ target === 'churn' ? '风险分数' : '异常分数' }}</dt><dd>{{ n(target === 'churn' ? selected.riskScore : selected.anomalyScore, 4) }} <small>（非概率）</small></dd><template v-if="target === 'anomalies'"><dt>固定阈值</dt><dd>{{ n(selected.threshold, 4) }}</dd><dt>站点 / 电桩</dt><dd>{{ selected.stationId }} / {{ selected.chargerId }}</dd></template><template v-for="(value, key) in selected.features" :key="key"><dt>{{ features[key] || key }}</dt><dd>{{ value == null ? '无历史记录' : n(value, 2) }}</dd></template></dl><h4>已观察到的行为 / 充电事实</h4><ul><li v-for="(reason,index) in selected.explanations || []" :key="index">{{ reason }}</li></ul><p class="tiny-note">这些是输入事实，不是因果解释。不会据此自动冻结用户、停桩或发券。</p></template><p v-else class="subtle">{{ detailLoading ? '正在读取对象分析…' : '选择一行，查看模型输入事实与解释。' }}</p></aside></div>
  <div v-else-if="!loading" class="intelligence-empty"><Icon name="info" :size="26"/>暂无可展示的模型样本。请先完成模型训练及发布。</div>
</section></template>
