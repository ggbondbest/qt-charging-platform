<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { createQtLiveMonitor } from '../qtLiveMonitor';
import { fetchQtLiveSnapshot } from '../qtLiveApi';
import Chart from './Chart.vue';
import Icon from './Icon.vue';

const props = withDefaults(defineProps<{ active?: boolean; compact?: boolean; presentation?: boolean }>(), { active: true, compact: false, presentation: false });
const emit = defineEmits<{ 'update:presentation': [value: boolean] }>();
const trendDays = ref<7 | 30>(7);
const monitor = createQtLiveMonitor(signal => fetchQtLiveSnapshot(signal, trendDays.value));
const { snapshot, loading, error, active } = monitor;
const city = ref('');
const cities = computed(() => [...new Set(snapshot.value?.stations.map(station => station.city || '未设置城市') || [])].sort());
const stations = computed(() => snapshot.value?.stations.filter(station => !city.value || (station.city || '未设置城市') === city.value) || []);
const summary = computed(() => snapshot.value?.summary);
const overview = computed(() => snapshot.value?.overview);
const orderNames: Record<string, string> = { RESERVED: '已预约', CHARGING: '充电中', WAITING_PAYMENT: '待支付', COMPLETED: '已完成', CANCELLED: '已取消' };
const stationNames: Record<string, string> = { ACTIVE: '运营中', MAINTENANCE: '维护中', CLOSED: '已关闭', INACTIVE: '暂停运营' };
const chargerNames: Record<string, string> = { AVAILABLE: '空闲', RESERVED: '已预约', CHARGING: '充电中', FAULT: '故障', OFFLINE: '离线' };
const number = (value: number | undefined, digits = 0) => value == null ? '—' : value.toLocaleString('zh-CN', { minimumFractionDigits: digits, maximumFractionDigits: digits });
const time = (value?: string | null) => value && Number.isFinite(Date.parse(value)) ? new Date(value).toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai', hour12: false }) : '—';
const status = computed(() => error.value ? '数据暂不可用' : !active.value ? '刷新已暂停' : snapshot.value ? '已连接' : '正在连接');
const metrics = computed(() => {
  const s = summary.value;
  return [
    { label: '今日营收', value: s ? s.todayRevenueCents / 100 : undefined, digits: 2, unit: '元', note: '今日完成并支付' },
    { label: '本月营收', value: s ? s.monthRevenueCents / 100 : undefined, digits: 2, unit: '元', note: '本月完成并支付' },
    { label: '累计营收', value: s ? s.paidAmountCents / 100 : undefined, digits: 2, unit: '元', note: '已完成订单支付金额' },
    { label: '电桩在线率', value: s ? s.onlineRatio * 100 : undefined, digits: 1, unit: '%', note: `${number(s?.onlineChargers)} / ${number(s?.totalChargers)} 根 · 在线含故障` },
    { label: '今日新增订单', value: s?.todayOrders, digits: 0, unit: '笔', note: '按订单创建时间' },
    { label: '当前充电订单', value: s?.chargingOrders, digits: 0, unit: '笔', note: `${number(s?.activeOrders)} 笔进行中（含预约、待支付）` },
    { label: '注册用户', value: s?.totalUsers, digits: 0, unit: '人', note: '当前业务库累计' },
    { label: '累计充电量', value: s ? s.completedEnergyWh / 1000 : undefined, digits: 3, unit: 'kWh', note: '已完成订单充电量' },
  ];
});
const trendOption = computed(() => ({
  color: ['#2563eb', '#12a685'], tooltip: { trigger: 'axis', confine: true },
  legend: { bottom: 0, textStyle: { color: '#667085' }, data: ['营收', '完成订单'] },
  grid: { left: 12, right: 14, top: 34, bottom: 48, containLabel: true },
  xAxis: { type: 'category', data: overview.value?.revenueTrend.map(row => row.date.slice(5)) || [], axisTick: { show: false }, axisLine: { lineStyle: { color: '#e4e8ef' } } },
  yAxis: [{ type: 'value', name: '元', splitLine: { lineStyle: { color: '#edf0f5' } } }, { type: 'value', name: '笔', minInterval: 1, splitLine: { show: false } }],
  series: [
    { name: '营收', type: 'bar', barMaxWidth: 32, itemStyle: { borderRadius: [4, 4, 0, 0] }, data: overview.value?.revenueTrend.map(row => row.revenueCents / 100) || [] },
    { name: '完成订单', type: 'line', yAxisIndex: 1, symbolSize: 6, data: overview.value?.revenueTrend.map(row => row.completedOrderCount) || [] },
  ],
}));
function updateActivity() { monitor.setActive(props.active && !document.hidden); }
watch(() => props.active, updateActivity);
watch(trendDays, () => { monitor.setActive(false); updateActivity(); });
onMounted(() => { document.addEventListener('visibilitychange', updateActivity); updateActivity(); });
onBeforeUnmount(() => { document.removeEventListener('visibilitychange', updateActivity); monitor.dispose(); });
</script>

<template>
  <section class="qt-live-workspace" :class="{ 'is-compact': compact, 'is-presentation': presentation }" aria-label="当前模拟业务">
    <header class="qt-live-heading">
      <div>
        <h1 v-if="!compact">当前模拟业务</h1>
        <div class="qt-live-connection" :class="{ 'is-offline': !!error }" role="status"><b>{{ status }}</b><span>最后成功快照：{{ time(snapshot?.asOf) }}</span></div>
        <p>Qt 当前业务 · 北京时间 · 与历史运营分析分别统计</p>
      </div>
      <div class="qt-live-actions">
        <button type="button" class="button secondary" :disabled="loading || !active" @click="monitor.refresh"><Icon name="refresh" :size="16"/>{{ loading ? '读取中…' : '刷新' }}</button>
        <button type="button" class="button secondary" :aria-pressed="presentation" @click="emit('update:presentation', !presentation)"><Icon name="chart" :size="16"/>{{ presentation ? '退出大屏模式' : '大屏模式' }}</button>
        <small v-if="presentation">Esc 返回窗口</small>
      </div>
    </header>
    <div v-if="error" class="qt-live-error" role="alert"><strong>Qt 业务数据暂不可用</strong><span>{{ error }}</span><span>{{ snapshot ? '下方保留最后一次成功快照，当前值可能已变化。' : '尚未取得有效快照，指标不以 0 代替。请检查服务连接。' }}</span></div>
    <div class="qt-live-metrics" :class="{ 'is-stale': !!error }">
      <article v-for="metric in metrics" :key="metric.label"><span>{{ metric.label }}</span><strong>{{ number(metric.value, metric.digits) }}<small> {{ metric.unit }}</small></strong><p>{{ metric.note }}</p></article>
    </div>
    <div class="qt-live-business-totals">
      <span>运营电站 <b>{{ number(summary?.activeStations) }} / {{ number(summary?.totalStations) }}</b></span>
      <span>累计完成 <b>{{ number(summary?.completedOrders) }} 笔</b></span>
      <span>今日完成 <b>{{ number(summary?.todayCompletedOrders) }} 笔</b></span>
      <span>今日充电 <b>{{ number(summary ? summary.todayCompletedEnergyWh / 1000 : undefined, 3) }} kWh</b></span>
    </div>
    <div class="qt-live-overview-grid">
      <section class="qt-live-card qt-live-trend">
        <div class="qt-live-card-heading"><div><h2>经营趋势</h2><p>{{ overview ? `截至 ${overview.today} · 最近 ${overview.trendDays} 天` : '等待经营快照' }} · 按支付日期</p></div><label>周期<select v-model.number="trendDays" aria-label="经营趋势周期"><option :value="7">近 7 天</option><option :value="30">近 30 天</option></select></label></div>
        <Chart v-if="overview" :option="trendOption" label="Qt 当前业务每日营收与完成订单，单位元和笔"/>
        <p v-else class="qt-live-empty">{{ loading ? '正在读取经营趋势…' : '暂无有效经营快照。' }}</p>
      </section>
      <section class="qt-live-card qt-live-abnormal">
        <div class="qt-live-card-heading"><div><h2>异常电桩</h2><p>故障、离线或尚未恢复的异常</p></div><span>{{ number(overview?.abnormalChargers.total) }} 根</span></div>
        <ul v-if="overview?.abnormalChargers.items.length" class="qt-live-alert-list"><li v-for="item in overview.abnormalChargers.items" :key="item.qtChargerId"><div><strong>{{ item.stationName }}</strong><span class="qt-live-fault">{{ chargerNames[item.status] || item.status }}</span></div><small>{{ item.chargerCode }}</small><p>{{ item.activeException?.safeSummary || (item.status === 'OFFLINE' ? '设备离线，等待恢复连接。' : '设备故障，等待检查。') }}</p></li></ul>
        <p v-else class="qt-live-empty">{{ overview ? '当前没有异常电桩。' : '暂无有效异常快照。' }}</p>
        <p v-if="overview && overview.abnormalChargers.total > overview.abnormalChargers.items.length" class="qt-live-note">展示最近 {{ overview.abnormalChargers.items.length }} 根，请到电桩管理查看全部。</p>
      </section>
    </div>
    <section class="qt-live-card">
      <div class="qt-live-card-heading"><div><h2>电站与电桩状态</h2><p>{{ number(summary?.totalStations) }} 座电站 · {{ number(summary?.totalChargers) }} 根电桩</p></div><label>城市<select v-model="city" aria-label="Qt 电站城市" :disabled="!snapshot"><option value="">全部城市</option><option v-for="item in cities" :key="item">{{ item }}</option></select></label></div>
      <div v-if="summary" class="qt-live-statuses"><span>空闲 <b>{{ summary.chargerStatusCounts.AVAILABLE }}</b></span><span>已预约 <b>{{ summary.chargerStatusCounts.RESERVED }}</b></span><span>充电 <b>{{ summary.chargerStatusCounts.CHARGING }}</b></span><span>故障 <b>{{ summary.chargerStatusCounts.FAULT }}</b></span><span>离线 <b>{{ summary.chargerStatusCounts.OFFLINE }}</b></span></div>
      <div class="qt-live-table-scroll" tabindex="0" aria-label="电站状态表，可横向滚动"><table><thead><tr><th>Qt 电站</th><th>城市 / 状态</th><th>电价 元/kWh</th><th>空闲</th><th>预约</th><th>充电</th><th>故障</th><th>离线</th></tr></thead><tbody><tr v-for="station in stations" :key="station.qtStationId"><td><strong>{{ station.name }}</strong><small>{{ station.qtStationCode }}</small></td><td>{{ station.city || '未设置城市' }}<small>{{ stationNames[station.status] || station.status }}</small></td><td>{{ number(station.priceCentsPerKwh / 100, 2) }}</td><td class="qt-live-available">{{ station.chargerStatusCounts.AVAILABLE }}</td><td>{{ station.chargerStatusCounts.RESERVED }}</td><td>{{ station.chargerStatusCounts.CHARGING }}</td><td>{{ station.chargerStatusCounts.FAULT }}</td><td>{{ station.chargerStatusCounts.OFFLINE }}</td></tr></tbody></table></div>
      <p v-if="!stations.length" class="qt-live-empty">{{ snapshot ? '当前筛选下没有电站。' : loading ? '正在读取电站快照…' : '暂无有效电站快照。' }}</p>
      <p v-if="snapshot?.truncated.stations" class="qt-live-note">仅展示前 {{ snapshot.limits.stations }} 座电站；全库汇总仍涵盖全部电站。</p>
    </section>
    <section class="qt-live-card">
      <div class="qt-live-card-heading"><div><h2>最近订单</h2><p>按订单创建序号倒序</p></div><span>全库 {{ number(summary?.totalOrders) }} 笔</span></div>
      <div class="qt-live-table-scroll" tabindex="0" aria-label="最近订单表，可横向滚动"><table><thead><tr><th>业务订单</th><th>电站 / 电桩</th><th>状态</th><th>金额 元</th><th>电量 kWh</th><th>创建时间 · 北京时间</th></tr></thead><tbody><tr v-for="order in snapshot?.orders || []" :key="order.qtOrderId"><td>#{{ order.qtOrderId }}</td><td><strong>{{ order.stationName }}</strong><small>{{ order.qtStationCode }} · {{ order.chargerCode }}</small></td><td><span class="qt-live-order-status" :class="{ 'is-charging': order.status === 'CHARGING' }">{{ orderNames[order.status] || order.status }}</span></td><td>{{ number(order.amountCents / 100, 2) }}</td><td>{{ number(order.energyWh / 1000, 3) }}</td><td>{{ time(order.createdAt) }}</td></tr></tbody></table></div>
      <p v-if="!snapshot?.orders.length" class="qt-live-empty">{{ snapshot ? '当前 Qt 数据库暂无订单。' : '暂无有效订单快照。' }}</p>
      <p v-if="snapshot?.truncated.orders" class="qt-live-note">显示最近 {{ snapshot.limits.orders }} 笔，汇总仍涵盖全库。</p>
    </section>
    <details class="qt-live-definitions"><summary>统计口径与刷新</summary><p>数据源：qt-sqlite · 只读快照，每 7.5 秒刷新；切页或窗口进入后台时暂停。支付金额与电量仅统计已完成订单，不与历史批次合计。今日、本月与趋势按北京时间支付日期归属，新增订单按创建日期统计；在线电桩包含故障状态，不表示设备可充电。金额分转元，电量 Wh 转 kWh。</p></details>
  </section>
</template>

<style scoped>
.qt-live-workspace { padding:28px 0 16px; margin:auto; width:100%; }
.qt-live-workspace.is-compact { padding-top:18px; }
.qt-live-heading,.qt-live-card-heading { display:flex; align-items:center; justify-content:space-between; gap:18px; }
.qt-live-heading { margin-bottom:18px; }.qt-live-heading h1 { font-size:30px; margin:0 0 10px; }.qt-live-heading p,.qt-live-card-heading p { color:#667085; font-size:12px; margin-top:5px; line-height:1.6; }
.qt-live-actions { display:flex; align-items:center; justify-content:flex-end; flex-wrap:wrap; gap:8px; flex-shrink:0; }.qt-live-actions small { font-size:11px; color:#667085; }
.qt-live-connection { display:flex; align-items:center; gap:12px; flex-wrap:wrap; font-size:12px; color:#667085; }.qt-live-connection b { color:#166b4b; font-weight:600; }.qt-live-connection.is-offline b { color:#a04612; }
.qt-live-note,.qt-live-definitions { font-size:12px; color:#667085; margin:14px 0; line-height:1.8; }.qt-live-definitions summary { cursor:pointer; }.qt-live-definitions p { margin-top:8px; }
.qt-live-error { display:flex; flex-direction:column; gap:6px; background:#fff7ed; color:#92400e; border:1px solid #fed7aa; border-radius:10px; padding:16px 20px; margin-top:14px; font-size:13px; }
.qt-live-metrics { display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); gap:12px; margin:18px 0; }.qt-live-metrics article { border:1px solid #e4e8ef; padding:17px 18px; border-radius:10px; background:#fff; min-width:0; }.qt-live-metrics article>span { color:#596579; font-size:12px; }.qt-live-metrics strong { display:block; margin:10px 0 8px; font-size:27px; font-weight:650; letter-spacing:-1px; overflow-wrap:anywhere; }.qt-live-metrics small { font-size:12px; font-weight:400; color:#667085; letter-spacing:0; }.qt-live-metrics p { font-size:11px; color:#7a8496; line-height:1.5; }.qt-live-metrics.is-stale { background:#fffbf5; }
.qt-live-business-totals { display:flex; gap:12px 28px; flex-wrap:wrap; font-size:12px; color:#667085; }.qt-live-business-totals b { color:#344054; margin-left:5px; font-weight:500; }
.qt-live-overview-grid { display:grid; grid-template-columns:minmax(0,1.6fr) minmax(280px,1fr); gap:16px; }
.qt-live-card { border:1px solid #e4e8ef; border-radius:12px; margin-top:18px; padding:20px; background:#fff; min-width:0; }.qt-live-card h2 { font-size:17px; }.qt-live-card-heading label { display:flex; align-items:center; gap:8px; font-size:12px; flex-shrink:0; }.qt-live-card-heading select { padding:7px; border:1px solid #e4e8ef; border-radius:6px; background:white; }.qt-live-card-heading>span { color:#667085; font-size:12px; white-space:nowrap; }
.qt-live-trend .chart { height:270px; margin-top:14px; }.qt-live-alert-list { list-style:none; padding:0; margin:10px 0 0; max-height:280px; overflow:auto; }.qt-live-alert-list li { padding:12px 0; border-bottom:1px solid #edf0f5; }.qt-live-alert-list li:last-child { border:0; }.qt-live-alert-list li>div { display:flex; justify-content:space-between; gap:10px; }.qt-live-alert-list strong { font-size:12px; font-weight:500; }.qt-live-alert-list small { font-size:10px; color:#8490a3; }.qt-live-alert-list p { font-size:12px; color:#667085; margin-top:5px; }.qt-live-fault { font-size:11px; color:#a04612; white-space:nowrap; }
.qt-live-statuses { display:flex; gap:20px; flex-wrap:wrap; margin:18px 0; font-size:12px; color:#667085; }.qt-live-statuses b { color:#344054; margin-left:4px; }
.qt-live-table-scroll { overflow:auto; margin-top:16px; }.qt-live-table-scroll table { width:100%; border-collapse:collapse; font-size:12px; min-width:690px; }.qt-live-table-scroll th { text-align:left; font-weight:500; color:#667085; background:#f8fafc; white-space:nowrap; }.qt-live-table-scroll th,.qt-live-table-scroll td { padding:12px 10px; border-bottom:1px solid #edf0f5; }.qt-live-table-scroll td strong { font-weight:500; }.qt-live-table-scroll td small { display:block; color:#8490a3; font-size:10px; margin-top:4px; }.qt-live-available { color:#16734f; font-weight:600; }.qt-live-order-status { display:inline-block; padding:4px 8px; border-radius:5px; background:#f1f4f9; white-space:nowrap; }.qt-live-order-status.is-charging { background:#eaf3ff; color:#245ac2; }.qt-live-empty { padding:36px 0; color:#8490a3; text-align:center; font-size:13px; }
.is-presentation .qt-live-metrics strong { font-size:32px; }.is-presentation .qt-live-trend .chart { height:310px; }
@media(max-width:1000px) { .qt-live-metrics { grid-template-columns:repeat(4,minmax(0,1fr)); }.qt-live-metrics article { padding:14px; }.qt-live-metrics strong { font-size:23px; }.qt-live-overview-grid { grid-template-columns:minmax(0,1.4fr) minmax(250px,1fr); } }
@media(max-width:760px) { .qt-live-overview-grid { grid-template-columns:minmax(0,1fr); }.qt-live-metrics { grid-template-columns:repeat(2,minmax(0,1fr)); } }
@media(max-width:640px) { .qt-live-workspace { padding-top:20px; }.qt-live-heading { align-items:flex-start; flex-direction:column; gap:12px; }.qt-live-actions { justify-content:flex-start; }.qt-live-heading h1 { font-size:26px; }.qt-live-card { padding:15px; }.qt-live-card-heading { align-items:flex-start; }.qt-live-metrics { gap:10px; }.qt-live-metrics strong { font-size:22px; } }
</style>
