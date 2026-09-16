<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import Icon from './Icon.vue';
import { AnalyticsError, formatValue as n, fraction, publishedRequest } from '../analytics';
import type { Dataset, RecordData } from '../analytics';
import '../intelligence-layout.css';
const props = withDefaults(defineProps<{ initialTarget?: 'churn' | 'anomalies' }>(), { initialTarget: 'churn' });
const target = ref<'churn' | 'anomalies'>(props.initialTarget); const dataset = ref<Dataset>(); const report = ref<RecordData>();
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
watch(() => props.initialTarget, value => { target.value = value; });
watch(target, load); onMounted(load); onBeforeUnmount(() => { alive = false; sequence++; detailSequence++; controller?.abort(); detailController?.abort(); });
</script>
<template>
  <section class="card intelligence-section intelligence-workspace management-workspace">
    <div class="intelligence-header"><div><h2>用户与充电分析</h2><p>从历史行为中识别值得关注的用户与充电记录。</p></div><span class="insights-data-label">历史模拟数据 · TEST 样本</span></div>
    <div class="insights-switch" aria-label="分析类型"><button :class="{ active: target === 'churn' }" :aria-pressed="target === 'churn'" @click="target = 'churn'">用户回访风险</button><button :class="{ active: target === 'anomalies' }" :aria-pressed="target === 'anomalies'" @click="target = 'anomalies'">充电异常筛查</button></div>
    <div class="insights-purpose"><p>{{ target === 'churn' ? '预测用户未来 14 天不再发起充电请求的风险。分数越高，越值得关注。' : '筛查已结束的充电会话，优先呈现需要人工复核的温度异常。' }}</p><span>{{ target === 'churn' ? '分数是风险排序，不是流失概率。' : '告警不是故障诊断，不能作为设备安全保障。' }}</span></div>
    <form class="intelligence-form insights-query" @submit.prevent="inspect(query)"><label class="wide">{{ target === 'churn' ? '模拟用户编号' : '已结束会话编号' }}<input v-model="query" :aria-label="target === 'churn' ? '模拟用户编号' : '已结束会话编号'" :placeholder="target === 'churn' ? '输入编号或从下方选择用户' : '输入编号或从下方选择会话'"/></label><button class="button primary" :disabled="detailLoading || !query.trim()">{{ detailLoading ? '推理中…' : '查询分析' }}</button><button type="button" class="button secondary" :disabled="loading" @click="load"><Icon name="refresh" :size="15"/>刷新</button></form>
    <div v-if="error" role="alert" class="intelligence-inline-error">{{ error }}</div>
    <div v-if="loading" class="intelligence-empty" role="status">正在加载模型与历史样本…</div>
    <div v-else-if="listing" class="insights-grid">
      <div class="insights-list-panel"><div class="insights-list-heading"><h3>{{ target === 'churn' ? '关注列表' : '复核列表' }}</h3><span>前 {{ listing.items.length }} / {{ n(listing.total) }} {{ target === 'churn' ? '位用户' : '笔会话' }}</span></div>
        <div class="insights-table-wrap"><table class="insights-table"><caption class="intelligence-sr-only">{{ title }}，按模型分数排序，点击编号查看详情</caption><thead><tr><th scope="col">{{ target === 'churn' ? '用户编号' : '会话编号' }}</th><th scope="col">分数</th><th scope="col">{{ target === 'churn' ? '相对风险' : '告警状态' }}</th></tr></thead><tbody><tr v-for="row in listing.items" :key="row.userId || row.sessionId" :class="{ selected: (selected?.userId || selected?.sessionId) === (row.userId || row.sessionId) }"><td><button :aria-pressed="(selected?.userId || selected?.sessionId) === (row.userId || row.sessionId)" @click="inspect(row.userId || row.sessionId)">{{ row.userId || row.sessionId }}</button></td><td class="insights-number">{{ n(target === 'churn' ? row.riskScore : row.anomalyScore, 4) }}</td><td><span class="insights-risk" :class="{ high: target === 'churn' ? row.riskLevel === 'HIGH' : row.flagged }">{{ target === 'churn' ? (riskLabel[row.riskLevel] || row.riskLevel) : (row.flagged ? '需复核' : '未触发') }}</span></td></tr></tbody></table></div>
        <p v-if="!listing.items.length" class="insights-list-note">此批次没有符合条件的告警样本。</p><p class="insights-list-note">参考时刻 {{ time(listing.referenceTime) }} · 北京时间</p>
      </div>
      <aside class="insights-detail" aria-live="polite">
        <template v-if="selected"><div class="insights-detail-heading"><small>{{ target === 'churn' ? '用户分析' : '会话分析' }}</small><h3>{{ selected.userId || selected.sessionId }}</h3></div>
          <div class="insights-score"><span>{{ target === 'churn' ? '风险分数' : '异常分数' }}</span><strong>{{ n(target === 'churn' ? selected.riskScore : selected.anomalyScore, 4) }}</strong><small>相对评分 · 非概率</small></div>
          <dl><template v-if="target === 'anomalies'"><dt>固定阈值</dt><dd>{{ n(selected.threshold, 4) }}</dd><dt>站点 / 电桩</dt><dd>{{ selected.stationId }} / {{ selected.chargerId }}</dd></template><template v-for="(value, key) in selected.features" :key="key"><dt>{{ features[key] || key }}</dt><dd>{{ value == null ? '无历史记录' : n(value, 2) }}</dd></template></dl>
          <details class="insights-facts" open><summary>查看观察到的{{ target === 'churn' ? '用户行为' : '充电事实' }}</summary><ul><li v-for="(reason,index) in selected.explanations || []" :key="index">{{ reason }}</li></ul><p>这些是输入事实，不是因果解释。不会据此自动冻结用户、停桩或发券。</p></details>
        </template><p v-else class="subtle">{{ detailLoading ? '正在读取对象分析…' : '选择左侧记录，查看分析结果。' }}</p>
      </aside>
    </div>
    <div v-else-if="!loading" class="intelligence-empty"><Icon name="info" :size="26"/>暂无可展示的模型样本。请先完成模型训练及发布。</div>
    <details class="intelligence-evidence"><summary>预测说明与模型评价<span>了解适用范围</span></summary><div class="intelligence-evidence-body">
      <template v-if="target === 'churn'"><p>根据观察日前的充电频率、距上次请求天数、充电量和排队经历等历史行为评分。预测的是用户未来 14 天不再发起充电请求的风险，不表示用户永久流失，也不是设备故障风险。</p><p>这里的“回访”指再次发起充电请求，不是打开网页或已经完成充电。结果是历史模拟数据上的风险排序，未经概率校准；例如分数 0.8 不等于 80% 的流失概率，也不表示发券收益。</p><div v-if="evaluation.model" class="intelligence-metrics"><span><small>TEST AUC</small><b>{{ n(evaluation.model.auc, 4) }}</b></span><span><small>距上次请求基线 AUC</small><b>{{ n(evaluation.recencyBaseline?.auc, 4) }}</b></span><span><small>TEST PR-AUC</small><b>{{ n(evaluation.model.prAuc, 4) }}</b></span></div></template>
      <template v-else><p>使用固定训练参照分数与阈值筛查已结束的会话。告警需人工复核；当前主要识别温度异常，不能覆盖所有提前停止或功率降额。</p><div v-if="Object.keys(evaluation).length" class="intelligence-metrics"><span><small>查准率</small><b>{{ fraction(evaluation.precision) }}</b></span><span><small>召回率</small><b>{{ fraction(evaluation.recall) }}</b></span><span><small>F1</small><b>{{ n(evaluation.f1, 4) }}</b></span></div><p>低召回意味着仍有漏报，不可作为设备安全保障。</p></template>
      <p v-if="listing">模型：{{ listing.modelId }}</p>
    </div></details>
  </section>
</template>
