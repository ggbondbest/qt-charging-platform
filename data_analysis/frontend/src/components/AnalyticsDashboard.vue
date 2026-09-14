<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import Chart from './Chart.vue';
import Icon from './Icon.vue';
import AnalyticsMap from './AnalyticsMap.vue';
import { AnalyticsError, aggregateCities, converted, formatValue as n, fraction, publishedRequest, shiftDate, sumSnapshot } from '../analytics';
import type { Dataset, RecordData } from '../analytics';
const dataset = ref<Dataset>();
const cities = ref<RecordData[]>([]);
const city = ref(''); const start = ref(''); const end = ref('');
const snapshot = ref<RecordData>(); const loading = ref(false); const error = ref('');
const sequence = ref(0); let controller: AbortController | undefined;
const refreshing = ref(false);
let alive = true;
const metrics = computed(() => snapshot.value?.overview.metrics || {});
const stations = computed<RecordData[]>(() => snapshot.value?.stations || []);
const quality = computed(() => snapshot.value?.quality || {});
const cityRows = computed(() => aggregateCities(stations.value));
const stationRank = computed(() => [...stations.value].sort((a, b) => (b.periodMetrics?.chargingUtilizationRate ?? -1) - (a.periodMetrics?.chargingUtilizationRate ?? -1)).slice(0, 5));
const title = computed(() => snapshot.value?.cityName || '全域');
const colors = ['#176b58', '#76a18d', '#dbad63', '#647d9c', '#c47f70', '#a1aba4', '#d7ddd7'];
const grid = { left: 54, right: 24, top: 36, bottom: 34, containLabel: true };
const axis = { axisLine: { lineStyle: { color: '#dce4de' } }, axisTick: { show: false }, axisLabel: { color: '#748178', fontSize: 11 }, splitLine: { lineStyle: { color: '#edf1eb' } } };
const tooltip = { trigger: 'axis', confine: true };
const dayLabel = (value: string) => value.slice(5, 10);
function trend(items: RecordData[], fields: { key: string; name: string; divisor?: number; type?: string }[], unit: string) {
  return { color: colors, tooltip, legend: { bottom: 0, textStyle: { color: '#748178', fontSize: 11 } }, grid: { ...grid, bottom: 55 },
    xAxis: { ...axis, type: 'category', data: items.map(row => dayLabel(row.time)) }, yAxis: { ...axis, type: 'value', name: unit },
    series: fields.map(field => ({ name: field.name, type: field.type || 'line', data: items.map(row => converted(row[field.key], field.divisor || 1)), connectNulls: false, symbolSize: 5, barMaxWidth: 28, lineStyle: { width: 2.5 }, itemStyle: { borderRadius: [3, 3, 0, 0] } })) };
}
const energyOption = computed(() => trend(snapshot.value?.charts.energy.items || [], [{ key: 'energyWh', name: '充电量', divisor: 1000, type: 'bar' }], 'kWh'));
const revenueOption = computed(() => trend(snapshot.value?.charts.revenue.items || [], [{ key: 'netPaidCents', name: '净收款', divisor: 100 }, { key: 'refundCents', name: '退款', divisor: 100 }], '元'));
const queueOption = computed(() => trend(snapshot.value?.charts.cohorts.items || [], [{ key: 'queuesJoinedCount', name: '加入排队', type: 'bar' }, { key: 'queueServedCount', name: '最终服务', type: 'bar' }, { key: 'queueAbandonedCount', name: '主动离队', type: 'bar' }], '次'));
const loadOption = computed(() => ({ color: [colors[0]], tooltip, grid,
  xAxis: { ...axis, type: 'category', data: (snapshot.value?.charts.load.items || []).map((row: RecordData) => new Date(row.time).toLocaleTimeString('zh-CN', { timeZone: 'Asia/Shanghai', hour: '2-digit', minute: '2-digit', hour12: false })) },
  yAxis: { ...axis, type: 'value', name: 'kW' }, series: [{ name: '站点平均功率之和', type: 'line', data: (snapshot.value?.charts.load.items || []).map((row: RecordData) => row.meanPowerKw), connectNulls: false, symbolSize: 5, lineStyle: { width: 3 }, areaStyle: { color: '#176b5812' } }] }));
const cityOption = computed(() => ({ color: colors, tooltip: { trigger: 'axis', confine: true }, grid: { left: 65, right: 26, top: 15, bottom: 24, containLabel: true },
  xAxis: { ...axis, type: 'value', splitNumber: 3, axisLabel: { ...axis.axisLabel, formatter: (value: number) => value >= 1000 ? `${n(value / 1000, 0)}k` : String(value) } }, yAxis: { ...axis, type: 'category', inverse: true, data: cityRows.value.map(row => row.name) },
  series: [{ name: '充电量', type: 'bar', barMaxWidth: 18, data: cityRows.value.map((row, index) => ({ value: row.energyKwh, itemStyle: { color: colors[index % colors.length], borderRadius: [0, 4, 4, 0] } })) }] }));
const stateOption = computed(() => ({ color: colors, tooltip: { trigger: 'item', confine: true, formatter: '{b}: {c} 桩' }, legend: { bottom: 0, textStyle: { color: '#748178', fontSize: 11 }, itemWidth: 10, itemHeight: 10 },
  series: [{ type: 'pie', radius: ['49%', '72%'], center: ['50%', '43%'], label: { show: false }, data: sumSnapshot(stations.value), itemStyle: { borderColor: '#fff', borderWidth: 3 } }] }));
const cleanRate = computed(() => quality.value.rawRows > 0 && quality.value.cleanRows != null ? quality.value.cleanRows / quality.value.rawRows : null);
const stateIncomplete = computed(() => stations.value.filter(station => !station.isComplete).length);
async function refresh() {
  const current = ++sequence.value; controller?.abort(); controller = new AbortController(); const signal = controller.signal;
  loading.value = true; error.value = ''; snapshot.value = undefined;
  try {
    if (!dataset.value) throw new Error('尚未发布数据集。');
    if (start.value < dataset.value.startDate || end.value > dataset.value.endDate || start.value >= end.value) throw new Error('日期须位于数据集范围内，且开始日期早于结束日期。');
    const filters = { datasetId: dataset.value.datasetId, publishedBatchId: dataset.value.publishedBatchId, cityId: city.value, startDate: start.value, endDate: end.value };
    const chartNames = ['energy', 'revenue', 'cohorts'];
    const responses = await Promise.all([
      publishedRequest<RecordData>('/dashboard/overview', filters, { signal }),
      publishedRequest<RecordData>('/stations', { ...filters, pageSize: 100 }, { signal }),
      publishedRequest<RecordData>('/pipeline/runs', { datasetId: filters.datasetId, publishedBatchId: filters.publishedBatchId }, { signal }),
      ...chartNames.map(chart => publishedRequest<RecordData>('/dashboard/charts', { ...filters, chart, granularity: 'day', limit: 1000 }, { signal })),
      publishedRequest<RecordData>('/dashboard/charts', { ...filters, startDate: shiftDate(filters.endDate, -1), chart: 'load', granularity: 'hour', limit: 1000 }, { signal }),
    ]);
    if (!alive || current !== sequence.value) return;
    if (responses[1]!.data.hasNext) throw new Error('电站列表超过单页容量，请缩小城市范围。');
    if (responses.slice(3).some(response => response.data.truncated)) throw new Error('图表范围过长，请缩短日期后重试。');
    snapshot.value = { filters, cityName: cities.value.find(item => item.cityId === filters.cityId)?.cityName || '全域', overview: responses[0]!.data, stations: responses[1]!.data.items, quality: responses[2]!.data.items[0]?.quality,
      charts: Object.fromEntries([...chartNames, 'load'].map((name, i) => [name, responses[i + 3]!.data])), loadedAt: new Date().toLocaleTimeString('zh-CN', { hour12: false }) };
  } catch (failure) {
    if (!alive || current !== sequence.value) return;
    if (!(failure instanceof AnalyticsError && failure.code === 'CANCELLED')) error.value = failure instanceof Error ? failure.message : '数据加载失败。';
  } finally { if (alive && current === sequence.value) loading.value = false; }
}
async function initialize() {
  if (refreshing.value) return;
  refreshing.value = true; error.value = ''; snapshot.value = undefined; const current = ++sequence.value; controller?.abort(); controller = new AbortController(); const signal = controller.signal;
  try {
    const response = await publishedRequest<RecordData>('/datasets', {}, { signal });
    if (!alive || current !== sequence.value) return;
    const first = response.data.items?.[0];
    if (!first) throw new Error('尚无已发布统计批次。请先完成数据清洗与发布。');
    dataset.value = first;
    const cityResponse = await publishedRequest<RecordData>('/cities', { datasetId: first.datasetId, publishedBatchId: first.publishedBatchId, pageSize: 100 }, { signal });
    if (!alive || current !== sequence.value) return;
    cities.value = cityResponse.data.items;
    start.value = first.startDate > shiftDate(first.endDate, -7) ? first.startDate : shiftDate(first.endDate, -7);
    end.value = first.endDate; city.value = ''; await refresh();
  } catch (failure) { if (alive && !(failure instanceof AnalyticsError && failure.code === 'CANCELLED')) error.value = failure instanceof Error ? failure.message : '加载数据集失败。'; }
  finally { if (alive) refreshing.value = false; }
}
onMounted(initialize); onBeforeUnmount(() => { alive = false; sequence.value++; controller?.abort(); });
</script>
<template>
  <div class="analytics-page">
    <section class="analytics-heading"><div><div class="eyebrow">CHARGING NETWORK · ANALYTICS</div><h1>把运营，看清楚。</h1><p>从五城网络到一座电站，用同一份可信数据做判断。</p></div><div class="analytics-heading-note"><span class="simulation-chip"><Icon name="shield" :size="15"/>SIMULATED · 模拟数据</span><small>已发布历史批次，不代表实时运营</small></div></section>
    <section class="card analytics-filter"><label>城市范围<select v-model="city" aria-label="统计城市" :disabled="!dataset || loading"><option value="">全部城市</option><option v-for="item in cities" :key="item.cityId" :value="item.cityId">{{ item.cityName }}</option></select></label><label>开始日期<input v-model="start" type="date" aria-label="统计开始日期" :min="dataset?.startDate" :max="dataset?.endDate"/></label><label>结束日期（不含当天）<input v-model="end" type="date" aria-label="统计结束日期" :min="dataset?.startDate" :max="dataset?.endDate"/></label><button class="button primary" :disabled="loading || !dataset" @click="refresh"><Icon name="refresh" :size="16"/>{{ loading ? '正在查询…' : '应用筛选' }}</button><button class="text-button" :disabled="refreshing" @click="initialize">重新载入批次</button><span v-if="dataset" class="analytics-range">可用 {{ dataset.startDate }} — {{ dataset.endDate }}<small>北京时间 · 右端不含</small></span></section>
    <div v-if="error" role="alert" class="analytics-error"><Icon name="info" :size="20"/><div><strong>统计暂不可用</strong><p>{{ error }}</p><small>不会使用静态数值替代真实响应。</small></div><button class="button secondary" @click="initialize">重新载入</button></div>
    <div v-if="loading || (refreshing && !snapshot)" class="analytics-loading"><span class="spinner dark"></span>正在读取同一发布批次的统计结果…</div>
    <template v-if="snapshot">
      <div class="analytics-context"><b>{{ title }} · {{ snapshot.filters.startDate }} 至 {{ snapshot.filters.endDate }}（右端不含）</b><span>查询于 {{ snapshot.loadedAt }}</span></div>
      <section class="analytics-kpis">
        <article class="card"><span><Icon name="bolt" :size="17"/>充电量</span><strong>{{ n(converted(metrics.energyWh, 1000), 1) }}<em>kWh</em></strong><small>按遥测积分统计</small></article>
        <article class="card"><span><Icon name="card" :size="17"/>净收款</span><strong>{{ n(converted(metrics.netPaidCents, 100), 2) }}<em>元</em></strong><small>收款减退款，不等于净利润</small></article>
        <article class="card"><span><Icon name="signal" :size="17"/>充电利用率</span><strong>{{ fraction(metrics.chargingUtilizationRate) }}</strong><small>仅完整小时的充电采样占比</small></article>
        <article class="card"><span><Icon name="users" :size="17"/>活跃用户</span><strong>{{ n(metrics.activeUsers) }}<em>人</em></strong><small>跨站去重 · 复充 {{ fraction(metrics.repeatUserRate) }}</small></article>
      </section>
      <section class="analytics-overview-grid">
        <article class="card analytics-chart-card"><div class="analytics-card-title"><div><h3>城市电量贡献</h3><p>当前范围 · kWh</p></div><span class="tiny-tag">{{ cityRows.length }} 城市</span></div><Chart :option="cityOption" label="各城市充电量对比，单位千瓦时"/></article>
        <article class="card analytics-geography"><div class="analytics-card-title"><div><h3>{{ title }}电站网络</h3><p>点击站点查看期间电量</p></div><span class="tiny-tag">{{ n(metrics.stationCount) }} 站 / {{ n(metrics.chargerCount) }} 桩</span></div><AnalyticsMap :stations="stations"/></article>
        <article class="card analytics-chart-card"><div class="analytics-card-title"><div><h3>设备状态</h3><p>批次最新采样 · 非实时</p></div></div><Chart :option="stateOption" label="批次最新采样设备状态，单位电桩数"/><small v-if="stateIncomplete" class="analytics-warning">{{ stateIncomplete }} 个电站快照不完整，未知状态单独显示</small></article>
      </section>
      <section class="analytics-dual-grid">
        <article class="card analytics-chart-card"><div class="analytics-card-title"><div><h3>最后一个统计日 · 小时负荷</h3><p>{{ shiftDate(snapshot.filters.endDate, -1) }} · 站点小时平均功率之和 · 缺测保留断点</p></div><span class="tiny-tag">kW</span></div><Chart :option="loadOption" label="最后统计日逐小时负荷曲线，单位千瓦"/></article>
        <article class="card analytics-chart-card"><div class="analytics-card-title"><div><h3>每日充电量</h3><p>电量与功率分开呈现，不混用单位</p></div><span class="tiny-tag">kWh</span></div><Chart :option="energyOption" label="按业务日统计的充电电量，单位千瓦时"/></article>
        <article class="card analytics-chart-card"><div class="analytics-card-title"><div><h3>收款与退款</h3><p>按资金事件所属业务日归集</p></div><span class="tiny-tag">元</span></div><Chart :option="revenueOption" label="每日净收款和退款趋势，单位元"/></article>
        <article class="card analytics-chart-card"><div class="analytics-card-title"><div><h3>排队服务表现</h3><p>按加入日期归类 · 结果为批次末最终状态</p></div><span class="tiny-tag">服务率 {{ fraction(metrics.queueServedRate) }}</span></div><Chart :option="queueOption" label="每日加入排队、最终服务和主动离队次数"/></article>
      </section>
      <section class="analytics-bottom-grid">
        <article class="card analytics-ranking"><div class="analytics-card-title"><div><h3>站点效能 TOP 5</h3><p>按完整小时充电利用率排序</p></div></div><div v-for="(station,index) in stationRank" :key="station.stationId" class="analytics-rank-row"><b class="analytics-rank-number">{{ index + 1 }}</b><div><strong>{{ station.stationName }}</strong><small>{{ station.cityName }} · {{ n(converted(station.periodMetrics?.energyWh, 1000), 1) }} kWh</small><div class="analytics-track"><i :style="{ width: `${(station.periodMetrics?.chargingUtilizationRate || 0) * 100}%` }"></i></div></div><b>{{ fraction(station.periodMetrics?.chargingUtilizationRate) }}</b></div></article>
        <article class="card analytics-service"><div class="analytics-card-title"><div><h3>服务质量与用户体验</h3><p>完整链路，而不只是充了多少电</p></div></div><dl><div><dt>完成充电会话</dt><dd>{{ n(metrics.completedSessions) }}<small>次</small></dd></div><div><dt>平均排队等待</dt><dd>{{ n(converted(metrics.queueMeanWaitSeconds, 60), 1) }}<small>分钟</small></dd></div><div><dt>平均故障恢复时长</dt><dd>{{ n(converted(metrics.meanRepairResolutionSeconds, 3600), 1) }}<small>小时</small></dd></div><div><dt>用户平均评分</dt><dd>{{ n(metrics.meanRating, 2) }}<small>/ 5</small></dd></div><div><dt>预约创建 / 已取消</dt><dd>{{ n(metrics.reservationsCreatedCount) }} / {{ n(metrics.reservationCancelledCount) }}</dd></div><div><dt>恢复维修单 / 评价数</dt><dd>{{ n(metrics.repairsRestoredCount) }} / {{ n(metrics.ratingCount) }}</dd></div></dl><p class="tiny-note">排队等待按已解决事件统计：加入到首次叫号，未叫号则计算到离队；恢复时长为报障到恢复。</p></article>
      </section>
      <details class="card analytics-quality" open><summary><div><div class="eyebrow">DATA TRUST & LINEAGE</div><h3>数据质量与处理血缘</h3></div><span class="tiny-tag">展开 / 收起</span></summary><div class="analytics-quality-kpis"><div><small>原始记录</small><strong>{{ n(quality.rawRows) }}</strong></div><div><small>清洗后记录</small><strong>{{ n(quality.cleanRows) }}</strong></div><div><small>隔离记录</small><strong>{{ n(quality.rejectedRows) }}</strong></div><div><small>清洗保留比例</small><strong>{{ fraction(cleanRate, 2) }}</strong></div></div><p class="tiny-note">质量指标是整个发布批次的结果，不随城市或日期筛选变化；保留比例不代表所有业务规则都已验证。</p><div class="analytics-quality-grid"><div><h4>隔离原因</h4><div v-for="reason in quality.rejectionReasons || []" :key="reason.reason" class="analytics-rejection"><span>{{ reason.reason }}</span><b>{{ n(reason.rowCount) }}</b></div><p v-if="!quality.rejectionReasons?.length" class="subtle">本批次未报告隔离原因。</p></div><div><h4>来源与发布</h4><dl class="analytics-provenance"><dt>数据集</dt><dd>{{ dataset?.datasetId }}</dd><dt>发布批次</dt><dd>{{ dataset?.publishedBatchId }}</dd><dt>流水线</dt><dd>{{ dataset?.pipelineRunId }}</dd><dt>规范化记录</dt><dd>{{ n(quality.normalizedRows) }} · {{ quality.normalizationSemantics }}</dd></dl></div></div><div class="analytics-pipeline"><span>原始 / 脏数据</span><Icon name="arrow" :size="15"/><span>PySpark 清洗与统计</span><Icon name="arrow" :size="15"/><span>MySQL 批次发布</span><Icon name="arrow" :size="15"/><span>FastAPI</span><Icon name="arrow" :size="15"/><b>Vue 可视化</b></div><p class="tiny-note">HDFS 为可选存储与验收路径；这里不凭页面连接成功宣称 HDFS 已同步。</p></details>
    </template>
  </div>
</template>
