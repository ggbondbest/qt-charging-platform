<script setup>
import * as echarts from 'echarts'
import { computed, onMounted, ref } from 'vue'
import EChart from './components/EChart.vue'
import NetworkMap from './components/NetworkMap.vue'
import { getJson } from './api'
import { modelCapabilities } from './modelCapabilities'
import { cities, cityStats, hours, load, energy, week, revenue, stationRank, quality } from './mock'

const active = ref('dashboard')
const city = ref('ALL')
const apiOnline = ref(false)
const refreshedAt = ref('')
const busy = ref(false)
const theme = ref('ocean')
const showPresentation = ref(false)
const presentationStep = ref(0)
const presentationScenes = [
  { tag:'01 · 全域态势', title:'五城充电网络，一屏掌握', text:'从全国空间态势进入，观察 25 座模拟电站和 75 个充电桩的运营分布。' },
  { tag:'02 · 数据可信', title:'203,973 条原始数据如何变得可用', text:'展示 HDFS → PySpark → MySQL → FastAPI 的完整处理链路和 99.96% 清洗通过率。' },
  { tag:'03 · AI 决策', title:'从看见问题到预测下一步', text:'负荷、空闲桩、异常、流失与推荐五条模型线，严格区分已上线、待合并和待契约能力。' },
  { tag:'04 · 产品彩蛋', title:'低碳积分的终极梦想', text:'全网用户累计攒够 100,000,000 低碳积分，可兑换一辆“模拟 Audi A6”——最终解释权归课程答辩小组所有。' }
]
const dataset = ref('charging_sample_7d_v2')
const batch = ref('analytics-76be09e255f94277aa2613b4edc8d8f3')
const now = () => new Date().toLocaleTimeString('zh-CN', { hour12: false })
async function refresh() {
  busy.value = true
  try {
    const result = await getJson('/api/v1/datasets')
    const item = result.data.items[0]
    if (item) { dataset.value = item.datasetId; batch.value = item.publishedBatchId }
    apiOnline.value = true
  } catch { apiOnline.value = false }
  refreshedAt.value = now(); busy.value = false
}
onMounted(refresh)

const axis = { axisLine: { lineStyle: { color: '#29415b' } }, axisLabel: { color: '#7690aa' }, splitLine: { lineStyle: { color: 'rgba(83,116,145,.13)' } } }
const tooltip = { trigger: 'axis', backgroundColor: '#0b1c2e', borderColor: '#24445f', textStyle: { color: '#dcefff' } }
const loadOption = computed(() => ({
  tooltip, grid: { left: 42, right: 16, top: 32, bottom: 22 },
  legend: { right: 8, top: 3, textStyle: { color: '#7f9bb3', fontSize: 9 }, itemWidth: 14, itemHeight: 5, data: ['负荷功率', '充电电量'] },
  xAxis: { ...axis, type: 'category', data: hours }, yAxis: { ...axis, type: 'value', name: 'kW', nameTextStyle: { color: '#6f8aa3' } },
  series: [
    { name:'负荷功率', type:'line', smooth:true, symbol:'none', data:load, lineStyle:{width:3,color:'#23d5e5'}, areaStyle:{color:new echarts.graphic.LinearGradient(0,0,0,1,[{offset:0,color:'rgba(35,213,229,.35)'},{offset:1,color:'rgba(35,213,229,0)'}])}},
    { name:'充电电量', type:'line', smooth:true, symbol:'none', data:energy, lineStyle:{width:2,color:'#7c6cf2'} }
  ]
}))
const revenueOption = { tooltip, grid:{left:42,right:14,top:24,bottom:30}, xAxis:{...axis,type:'category',data:week}, yAxis:{...axis,type:'value'}, series:[{type:'bar',data:revenue,barWidth:16,itemStyle:{borderRadius:[5,5,0,0],color:new echarts.graphic.LinearGradient(0,0,0,1,[{offset:0,color:'#22d3c5'},{offset:1,color:'#176b80'}])}}] }
const cityOption = { tooltip:{...tooltip,trigger:'item'}, legend:{bottom:0,left:'center',textStyle:{color:'#7793a7',fontSize:8},itemWidth:7,itemHeight:7,itemGap:8,data:cityStats.map(x=>x.name)}, series:[{type:'pie',radius:['48%','72%'],center:['50%','43%'],label:{position:'inside',color:'#e9ffff',fontSize:8,formatter:'{d}%'},labelLine:{show:false},itemStyle:{borderColor:'#0b1726',borderWidth:3},data:cityStats.map((x,i)=>({name:x.name,value:x.value,itemStyle:{color:['#21d4d0','#5278f5','#8b6bf1','#f3a94a','#24b981'][i]}}))}] }
const stateOption = { tooltip:{...tooltip,trigger:'item'}, legend:{bottom:0,textStyle:{color:'#8da4b7'},itemWidth:10,itemHeight:10},series:[{type:'pie',radius:['50%','72%'],center:['50%','42%'],label:{show:false},data:[{name:'空闲',value:41,itemStyle:{color:'#28d7ad'}},{name:'充电中',value:24,itemStyle:{color:'#21a8ee'}},{name:'预约占用',value:5,itemStyle:{color:'#8a6ff1'}},{name:'维护',value:3,itemStyle:{color:'#f4ad52'}},{name:'离线',value:2,itemStyle:{color:'#53677b'}}]}] }
const forecastActual = [44,51,59,68,81,94,108,121,132,126,112,96]
const forecastPred = [46,49,62,71,78,91,104,117,128,123,109,99]
const forecastOption = { tooltip,grid:{left:46,right:18,top:38,bottom:28},legend:{top:2,right:8,textStyle:{color:'#7995aa',fontSize:9},data:['实际负荷','模型预测','80%风险区间']},xAxis:{...axis,type:'category',data:['08:00','09:00','10:00','11:00','12:00','13:00','14:00','15:00','16:00','17:00','18:00','19:00']},yAxis:{...axis,type:'value',name:'kW',nameTextStyle:{color:'#6f8aa3'}},series:[{name:'80%风险区间',type:'line',symbol:'none',data:forecastPred.map(v=>v-13),lineStyle:{opacity:0},stack:'band'},{name:'80%风险区间',type:'line',symbol:'none',data:forecastPred.map(()=>26),lineStyle:{opacity:0},areaStyle:{color:'rgba(130,105,245,.18)'},stack:'band'},{name:'实际负荷',type:'line',smooth:true,symbol:'circle',symbolSize:5,data:forecastActual,lineStyle:{width:2,color:'#36e4df'},itemStyle:{color:'#36e4df'}},{name:'模型预测',type:'line',smooth:true,symbol:'none',data:forecastPred,lineStyle:{width:2,color:'#9c7cf5',type:'dashed'}}]}
const qualityOption = { tooltip:{...tooltip,trigger:'item'},series:[{type:'gauge',startAngle:210,endAngle:-30,min:0,max:100,splitNumber:5,axisLine:{lineStyle:{width:12,color:[[.9996,'#26d7bc'],[1,'#23394c']]}},pointer:{show:false},axisTick:{show:false},splitLine:{show:false},axisLabel:{show:false},detail:{valueAnimation:true,formatter:'99.96%\n{caption|清洗通过率}',color:'#eafcff',fontSize:28,offsetCenter:[0,'8%'],rich:{caption:{fontSize:12,color:'#7390a8',lineHeight:30}}},data:[{value:99.96}]}] }
</script>

<template>
  <div class="shell" :class="'theme-'+theme">
    <aside>
      <div class="brand"><span class="brand-mark">⚡</span><div><b>充能智析</b><small>CHARGE INSIGHT</small></div></div>
      <nav>
        <button :class="{active:active==='dashboard'}" @click="active='dashboard'"><i>◫</i><span>智慧运营大屏</span></button>
        <button :class="{active:active==='quality'}" @click="active='quality'"><i>◈</i><span>运营与数据质量</span></button>
        <button :class="{active:active==='model'}" @click="active='model'"><i>⌁</i><span>智能分析中心</span></button>
      </nav>
      <div class="side-foot"><span class="pulse"></span><div><b>{{ apiOnline ? '数据服务已连接' : '演示数据模式' }}</b><small>FastAPI · {{ apiOnline ? 'ONLINE' : 'OFFLINE' }}</small></div></div>
    </aside>
    <main>
      <header>
        <div><h1>{{ active==='dashboard'?'智慧充电运营中心':active==='quality'?'运营与数据质量':'AI 负荷预测中心' }}</h1><p>多城市充电网络 · 数据驱动运营决策</p></div>
        <div class="header-actions"><button class="theme-button" @click="theme=theme==='ocean'?'aurora':'ocean'">◐ {{theme==='ocean'?'极光紫':'深海蓝'}}</button><button class="demo-button" @click="showPresentation=true;presentationStep=0">◎ 全景导览</button><span class="mock-badge">SIMULATED 模拟数据</span><span class="clock">数据更新 {{ refreshedAt || '--:--:--' }}</span><button class="refresh" :class="{spin:busy}" @click="refresh">↻</button></div>
      </header>
      <div class="filterbar">
        <label>区域范围<select v-model="city"><option v-for="c in cities" :key="c.id" :value="c.id">{{ c.name }}</option></select></label>
        <label>统计周期<input type="date" value="2025-12-01"><span>至</span><input type="date" value="2025-12-08"></label>
        <div class="batch"><span>发布批次</span><b>{{ batch.slice(0,24) }}…</b></div>
      </div>

      <template v-if="active==='dashboard'">
        <section class="kpis">
          <article><div class="kpi-icon cyan">⌁</div><div><span>累计充电量</span><strong>71,960.4 <em>kWh</em></strong><small class="up">↗ 12.6% 较上周期</small></div></article>
          <article><div class="kpi-icon violet">¥</div><div><span>净收款</span><strong>¥ 186,420</strong><small class="up">↗ 8.3% 较上周期</small></div></article>
          <article><div class="kpi-icon green">◎</div><div><span>平均利用率</span><strong>68.7 <em>%</em></strong><small class="up">↗ 5.2% 较上周期</small></div></article>
          <article><div class="kpi-icon amber">ϟ</div><div><span>服务会话</span><strong>4,619 <em>次</em></strong><small class="up">↗ 9.8% 较上周期</small></div></article>
        </section>
        <section class="command-grid">
          <div class="left-rail"><article class="panel mini-chart"><div class="panel-title"><div><h3>城市电量贡献</h3><p>五城市充电量占比</p></div></div><EChart :option="cityOption"/></article><article class="panel mini-chart"><div class="panel-title"><div><h3>近 7 日净收款</h3><p>万元 · 按业务日</p></div><b class="sum">¥112.3万</b></div><EChart :option="revenueOption"/></article></div>
          <article class="panel map-panel"><div class="map-title"><span></span><div><small>NETWORK SITUATION</small><h2>全国充电网络态势</h2></div><span></span></div><NetworkMap/></article>
          <div class="right-rail"><article class="panel state-compact"><div class="panel-title"><div><h3>设备状态快照</h3><p>批次末状态 · 非实时</p></div><strong class="online">97.3%</strong></div><EChart :option="stateOption"/></article><article class="panel rank-panel"><div class="panel-title"><div><h3>站点效能 TOP 5</h3><p>利用率与充电量</p></div></div><div class="ranking"><div v-for="(s,i) in stationRank" :key="s[0]"><span class="rank" :class="'r'+i">{{i+1}}</span><p><b>{{s[0]}}</b><small>{{s[2].toLocaleString()}} kWh</small></p><div class="bar"><i :style="{width:s[1]+'%'}"></i></div><strong>{{s[1]}}%</strong></div></div></article></div>
          <article class="panel load-strip"><div class="panel-title"><div><h3>全网 24 小时负荷脉冲</h3><p>负荷功率 / 充电电量</p></div><span class="legend-pill">峰值 137 kW · 18:00</span></div><EChart :option="loadOption"/></article>
        </section>
      </template>

      <template v-else-if="active==='quality'">
        <section class="kpis quality-kpis"><article><div><span>原始数据总量</span><strong>{{quality.raw.toLocaleString()}}</strong><small>23 张业务表</small></div></article><article><div><span>清洗有效数据</span><strong>{{quality.clean.toLocaleString()}}</strong><small class="up">99.96% 通过</small></div></article><article><div><span>隔离数据</span><strong>{{quality.rejected}}</strong><small class="warn">需追溯检查</small></div></article><article><div><span>规范化记录</span><strong>{{quality.normalized}}</strong><small>枚举值修正</small></div></article></section>
        <section class="quality-grid"><article class="panel gauge-panel"><div class="panel-title"><div><h3>Spark 清洗质量</h3><p>本次发布批次综合质量</p></div><span class="ok">● PUBLISHED</span></div><EChart :option="qualityOption"/></article><article class="panel reason-panel"><div class="panel-title"><div><h3>隔离原因分布</h3><p>共 86 条拒绝记录</p></div></div><div class="reasons"><div><b>无效费用</b><span><i style="width:100%"></i></span><strong>25</strong></div><div><b>会话 ID 缺失</b><span><i style="width:88%"></i></span><strong>22</strong></div><div><b>未知站点</b><span><i style="width:88%"></i></span><strong>22</strong></div><div><b>重复会话</b><span><i style="width:68%"></i></span><strong>17</strong></div></div></article><article class="panel flow-panel"><div class="panel-title"><div><h3>数据处理链路</h3><p>从原始数据到查询服务的完整血缘</p></div></div><div class="pipeline"><div><i>01</i><b>CSV / HDFS</b><small>原始数据</small></div><span>→</span><div><i>02</i><b>PySpark</b><small>清洗计算</small></div><span>→</span><div><i>03</i><b>MySQL 8.4</b><small>统计发布</small></div><span>→</span><div><i>04</i><b>FastAPI</b><small>只读服务</small></div><span>→</span><div class="current"><i>05</i><b>Vue 3</b><small>可视化大屏</small></div></div></article></section>
      </template>

      <template v-else>
        <section class="intelligence-grid">
          <article class="panel forecast-panel"><div class="panel-title"><div><h3>深圳·南山科技园站｜未来 12 小时负荷</h3><p>实际值、模型预测与 80% 风险区间 · 模拟展示</p></div><span class="model-version">hgb-q50 · v0.4</span></div><EChart :option="forecastOption"/></article>
          <article class="panel forecast-summary"><div class="panel-title"><div><h3>预测摘要</h3><p>当前选定站点</p></div><span class="ok">模型可用</span></div><div class="prediction-number"><small>下一时段预测负荷</small><strong>46.0 <em>kW</em></strong><span>区间 33.0—59.0 kW</span></div><div class="metric-pair"><div><small>TEST MAE</small><b>12.71 kW</b></div><div><small>预计空闲桩</small><b>2 个</b></div></div><div class="risk-line"><span>容量风险</span><i><b style="width:68%"></b></i><em>中等</em></div></article>
          <article class="panel capability-list"><div class="panel-title"><div><h3>智能能力目录</h3><p>已合入与待接入模型状态</p></div></div><div class="cap-row" v-for="(m,i) in modelCapabilities" :key="m.key"><span>{{['⌁','▦','◉','◇','⌖'][i]}}</span><div><b>{{m.name}}</b><small>{{m.output}}</small></div><em :class="m.state">{{m.state==='integrating'?'联调中':m.state==='pendingMerge'?'待合并':'待契约'}}</em></div></article>
          <article class="panel insight-panel"><div class="panel-title"><div><h3>模型洞察</h3><p>基于当前发布批次的运营提示</p></div></div><div class="insight-cards"><div><i>峰</i><p><b>18:00 进入负荷峰值</b><small>建议提前释放 2 个预约桩位</small></p></div><div><i>险</i><p><b>南山站容量风险中等</b><small>预测区间上沿达到额定容量 86%</small></p></div><div><i>客</i><p><b>高流失风险用户需召回</b><small>流失模型接口待合并后显示人数</small></p></div></div></article>
        </section>
      </template>
      <footer><span>数据集 {{ dataset }}</span><span>统计时段 2025-12-01 至 2025-12-08（右端不含）</span><span>© 2026 充能智析课程项目</span></footer>
    </main>
    <div v-if="showPresentation" class="presentation-mask" @click.self="showPresentation=false">
      <section class="presentation-card"><button class="presentation-close" @click="showPresentation=false">×</button>
        <div class="presentation-visual"><div class="orbit"><i></i><i></i><i></i><b>{{ presentationStep===3?'A6':'⚡' }}</b></div><span>{{presentationScenes[presentationStep].tag}}</span></div>
        <div class="presentation-copy"><small>PRESENTATION MODE</small><h2>{{presentationScenes[presentationStep].title}}</h2><p>{{presentationScenes[presentationStep].text}}</p>
          <div v-if="presentationStep===3" class="points-joke"><strong>100,000,000</strong><span>CARBON POINTS</span><em>模拟奖品 · 请勿当场要求提车</em></div>
          <div class="presentation-nav"><div><i v-for="(_,i) in presentationScenes" :key="i" :class="{active:i===presentationStep}"></i></div><button v-if="presentationStep>0" @click="presentationStep--">上一步</button><button v-if="presentationStep<presentationScenes.length-1" @click="presentationStep++">继续探索 →</button><button v-else @click="showPresentation=false">进入驾驶舱</button></div>
        </div>
      </section>
    </div>
  </div>
</template>
