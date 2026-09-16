<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { AnalyticsError, formatValue, publishedRequest, shiftDate } from '../analytics';
import type { Dataset } from '../analytics';
import { advisorDatesValid, advisorDestinations, validateAdvisorResponse } from '../advisor';
import type { AdvisorCapabilities, AdvisorRequest, AdvisorResponse, AdvisorTarget } from '../advisor';
import Icon from './Icon.vue';
import '../intelligence-layout.css';

const emit = defineEmits<{ navigate: [target: AdvisorTarget] }>();
const dataset = ref<Dataset>();
const capabilities = ref<AdvisorCapabilities>();
const cities = ref<{ cityId: string; cityName: string }[]>([]);
const question = ref(''); const cityId = ref(''); const startDate = ref(''); const endDate = ref('');
const mode = ref<'offline' | 'online'>('offline'); const consent = ref(false);
const preparing = ref(false); const loading = ref(false); const error = ref(''); const status = ref('');
const result = ref<AdvisorResponse>(); const answeredQuestion = ref('');
let alive = true; let sequence = 0; let setupSequence = 0;
let controller: AbortController | undefined; let setupController: AbortController | undefined;
const maxLength = computed(() => Math.min(capabilities.value?.maxQuestionLength || 300, 300));
const validDates = computed(() => !!dataset.value && advisorDatesValid(dataset.value, startDate.value, endDate.value));
const canAsk = computed(() => !preparing.value && !loading.value && !!capabilities.value && validDates.value
  && !!question.value.trim() && question.value.trim().length <= maxLength.value
  && (mode.value === 'offline' || (capabilities.value.onlineAvailable && consent.value)));
const scopeCity = computed(() => cities.value.find(city => city.cityId === result.value?.scope.cityId)?.cityName || result.value?.scope.cityId || '全部城市');
const evidenceValue = (value: unknown) => value == null ? '暂无数据' : typeof value === 'number' ? formatValue(value, Number.isInteger(value) ? 0 : 2) : String(value);

function invalidate() {
  const wasLoading = loading.value; const hadResult = !!result.value;
  sequence++; controller?.abort(); loading.value = false; result.value = undefined; consent.value = false;
  if (dataset.value && capabilities.value) error.value = '';
  status.value = wasLoading ? '已停止接收上次答复。修改完成后可重新提问。' : hadResult ? '问题或范围已变化，请重新生成答复。' : '';
}
watch([question, cityId, startDate, endDate, mode], invalidate, { flush: 'sync' });

async function initialize() {
  invalidate(); const current = ++setupSequence; setupController?.abort(); setupController = new AbortController();
  const signal = setupController.signal; preparing.value = true; error.value = ''; dataset.value = undefined; capabilities.value = undefined;
  try {
    const [datasets, config] = await Promise.all([
      publishedRequest<{ items: Dataset[] }>('/datasets', {}, { signal }),
      publishedRequest<AdvisorCapabilities>('/intelligence/advisor', {}, { signal }),
    ]);
    if (!alive || current !== setupSequence) return;
    const source = datasets.data.items?.[0];
    if (!source) throw new Error('尚无已发布统计数据，请先完成数据发布。');
    if (!source.datasetId || !source.publishedBatchId) throw new Error('数据集缺少发布批次，暂时无法生成可核对的答复。');
    const cityResponse = await publishedRequest<{ items: { cityId: string; cityName: string }[] }>('/cities', {
      datasetId: source.datasetId, publishedBatchId: source.publishedBatchId, pageSize: 100,
    }, { signal });
    if (!alive || current !== setupSequence) return;
    dataset.value = source; capabilities.value = config.data; cities.value = cityResponse.data.items;
    cityId.value = ''; endDate.value = source.endDate; startDate.value = source.startDate > shiftDate(source.endDate, -7) ? source.startDate : shiftDate(source.endDate, -7);
    mode.value = 'offline'; status.value = '';
  } catch (failure) {
    if (alive && current === setupSequence && !(failure instanceof AnalyticsError && failure.code === 'CANCELLED')) error.value = failure instanceof Error ? failure.message : '无法载入参谋，请重试。';
  } finally { if (alive && current === setupSequence) preparing.value = false; }
}

async function ask() {
  if (!canAsk.value || !dataset.value) return;
  const current = ++sequence; controller?.abort(); controller = new AbortController();
  const body: AdvisorRequest = {
    question: question.value.trim(), datasetId: dataset.value.datasetId, publishedBatchId: dataset.value.publishedBatchId,
    ...(cityId.value ? { cityId: cityId.value } : {}), startDate: startDate.value, endDate: endDate.value,
    mode: mode.value, consent: mode.value === 'online' && consent.value,
  };
  loading.value = true; error.value = ''; status.value = ''; result.value = undefined;
  consent.value = false;
  try {
    const response = await publishedRequest<AdvisorResponse>('/intelligence/advisor', {}, { method: 'POST', body, signal: controller.signal });
    if (!alive || current !== sequence) return;
    result.value = validateAdvisorResponse(response.data, body); answeredQuestion.value = body.question;
  } catch (failure) {
    if (alive && current === sequence && !(failure instanceof AnalyticsError && failure.code === 'CANCELLED')) error.value = failure instanceof Error ? failure.message : '答复未完成，请重试。';
  } finally { if (alive && current === sequence) loading.value = false; }
}
function stopReceiving() {
  sequence++; controller?.abort(); loading.value = false; consent.value = false;
  status.value = '已停止接收答复。服务器可能仍在处理本次请求。';
}
onMounted(initialize);
onBeforeUnmount(() => { alive = false; sequence++; setupSequence++; controller?.abort(); setupController?.abort(); });
</script>

<template>
  <section class="card intelligence-section intelligence-workspace advisor-workspace" aria-label="AI运营参谋">
    <div class="intelligence-header advisor-heading">
      <div><div class="advisor-eyebrow">OPERATIONS ADVISOR</div><h2>把数据，变成下一步</h2><p>提出一个运营问题，从已发布统计中找到依据。</p></div>
      <span class="advisor-mode-label"><i aria-hidden="true"/>{{ mode === 'online' ? '在线辅助 · 单次授权' : '本地证据问答' }}</span>
    </div>

    <div v-if="capabilities" class="advisor-examples" aria-label="运营问题示例">
      <button v-for="item in capabilities.supportedQuestions.slice(0, 4)" :key="item.id" type="button" @click="question = item.question"><Icon name="arrow" :size="15"/><span>{{ item.label }}</span></button>
    </div>

    <form class="advisor-form" @submit.prevent="ask">
      <label class="advisor-question-label" for="advisor-question">你的运营问题</label>
      <textarea id="advisor-question" v-model="question" :maxlength="maxLength" rows="3" placeholder="例如：当前范围内，哪些电站需要优先关注？" :disabled="preparing"/>
      <div class="advisor-scope-controls">
        <label>城市<select v-model="cityId" aria-label="参谋城市" :disabled="preparing || !dataset"><option value="">全部城市</option><option v-for="city in cities" :key="city.cityId" :value="city.cityId">{{ city.cityName }}</option></select></label>
        <label>开始日期<input v-model="startDate" type="date" aria-label="参谋开始日期" :min="dataset?.startDate" :max="dataset?.endDate" :disabled="preparing || !dataset"/></label>
        <label>结束日期 · 不含当日<input v-model="endDate" type="date" aria-label="参谋结束日期" :min="dataset?.startDate" :max="dataset?.endDate" :disabled="preparing || !dataset"/></label>
      </div>
      <p v-if="dataset && !validDates" class="advisor-validation" role="alert">请在 {{ dataset.startDate }} 至 {{ dataset.endDate }} 内选择起止日期，开始日期须早于结束日期。</p>
      <div class="advisor-form-footer">
        <label class="advisor-mode-control">答复方式<select v-model="mode" aria-label="参谋答复方式" :disabled="preparing || !capabilities"><option value="offline">本地证据问答</option><option value="online" :disabled="!capabilities?.onlineAvailable">{{ capabilities?.onlineAvailable ? '在线模型辅助' : '在线模型 · 未配置' }}</option></select></label>
        <button v-if="loading" type="button" class="button secondary" @click="stopReceiving"><Icon name="pause" :size="15"/>停止接收</button>
        <button v-else type="submit" class="button primary" :disabled="!canAsk"><Icon name="arrow" :size="16"/>生成运营答复</button>
      </div>
      <label v-if="mode === 'online' && capabilities?.onlineAvailable" class="advisor-consent"><input v-model="consent" type="checkbox" :disabled="loading" aria-label="允许本次发送问题意图和聚合统计"/><span>允许本次向 {{ capabilities.onlineProvider || '已配置的在线模型' }} 发送问题意图和聚合统计。{{ capabilities.disclosure }} 每次提问需重新勾选。</span></label>
    </form>

    <div v-if="error" class="intelligence-inline-error" role="alert">{{ error }} <button type="button" class="text-button" @click="initialize">重新载入</button></div>
    <p v-if="status" class="advisor-status" role="status">{{ status }}</p>
    <div v-if="preparing || loading" class="advisor-loading" role="status"><span class="advisor-loading-dot"/>{{ preparing ? '正在读取已发布数据范围…' : '正在核对当前范围的统计证据…' }}</div>

    <article v-if="result" class="advisor-answer" aria-live="polite">
      <div class="advisor-answer-top"><span>{{ result.mode === 'online' ? '在线辅助 · 数据证据答复' : '本地证据答复' }}</span><span>{{ result.status === 'answered' ? '已核对统计范围' : result.status === 'no_evidence' ? '当前范围证据不足' : '此问题暂不支持' }}</span></div>
      <h3>{{ answeredQuestion }}</h3><p class="advisor-answer-text">{{ result.answer }}</p>
      <p class="advisor-answer-scope">{{ scopeCity }} · {{ result.scope.startDate }} 至 {{ result.scope.endDate }}（不含结束日）· 北京时间 · 模拟数据</p>
      <div v-if="result.evidence.length" class="advisor-evidence" aria-label="答复依据">
        <div v-for="item in result.evidence.slice(0, 6)" :key="item.id"><span>{{ item.label }}</span><strong>{{ evidenceValue(item.value) }} <small v-if="item.value != null">{{ item.unit }}</small></strong></div>
      </div>
      <details v-if="result.evidence.length > 6" class="advisor-extra-evidence"><summary>查看全部 {{ result.evidence.length }} 项证据</summary><div class="advisor-evidence" aria-label="补充答复依据"><div v-for="item in result.evidence.slice(6)" :key="item.id"><span>{{ item.label }}</span><strong>{{ evidenceValue(item.value) }} <small v-if="item.value != null">{{ item.unit }}</small></strong></div></div></details>
      <div v-if="result.suggestions.length" class="advisor-next" aria-label="建议查看"><button v-for="item in result.suggestions" :key="item.target" type="button" @click="emit('navigate', item.target)">{{ advisorDestinations[item.target] }}<Icon name="arrow" :size="14"/></button></div>
      <div v-if="result.limitations.length" class="advisor-limits"><span>使用边界</span><ul><li v-for="(item, index) in result.limitations" :key="index">{{ item }}</li></ul></div>
      <details class="advisor-provenance"><summary>数据来源与发布批次</summary><p>数据集 {{ result.scope.datasetId }}<br/>发布批次 {{ result.scope.publishedBatchId }}</p><ul><li v-for="item in result.evidence" :key="item.id"><span>{{ item.label }}</span><code>{{ item.source.endpoint }} · {{ item.source.field }}</code></li></ul></details>
    </article>
    <p v-else-if="!loading && !preparing && !error && !status" class="advisor-hint">先选一个问题，或写下你想了解的运营情况。答复会注明数据依据和适用范围。</p>
  </section>
</template>

<style scoped>
.advisor-workspace { max-width: 1080px; margin-inline: auto; }
.advisor-heading { display: flex; justify-content: space-between; gap: 24px; }
.advisor-eyebrow { color: #8b929e; font-size: 10px; letter-spacing: 1.9px; margin-bottom: 11px; }
.advisor-mode-label { display: flex; align-items: center; gap: 7px; color: #687281; font-size: 11px; white-space: nowrap; }
.advisor-mode-label i { width: 6px; height: 6px; border-radius: 50%; background: #72978a; }
.advisor-examples { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 10px; margin-bottom: 28px; }
.advisor-examples button { display: flex; align-items: center; gap: 11px; text-align: left; border: 1px solid #e6e9ee; border-radius: 10px; padding: 14px 17px; color: #465262; background: #fafbfd; font-size: 12px; line-height: 1.5; }
.advisor-examples button:hover { border-color: #bfcbdc; background: #f4f7fc; }
.advisor-examples svg { color: #8695aa; flex-shrink: 0; }
.advisor-form label { display: flex; flex-direction: column; gap: 8px; font-size: 11px; color: #66707e; }
.advisor-form .advisor-question-label { margin-bottom: 10px; color: #424b57; font-size: 12px; }
.advisor-form textarea { display: block; width: 100%; min-height: 106px; resize: vertical; border: 1px solid #dce2ea; border-radius: 11px; padding: 16px; background: white; color: #252d38; font: inherit; font-size: 14px; line-height: 1.8; }
.advisor-form textarea::placeholder { color: #9da6b2; }
.advisor-form textarea:focus,.advisor-form select:focus,.advisor-form input:focus { outline: 2px solid #cdddf7; outline-offset: 2px; }
.advisor-scope-controls { display: grid; grid-template-columns: 1.2fr 1fr 1fr; gap: 15px; margin-top: 17px; }
.advisor-form select,.advisor-form input[type=date] { width: 100%; min-width: 0; height: 42px; padding: 9px 11px; border: 1px solid #e0e5eb; border-radius: 8px; background: #fff; color: #485260; font-size: 12px; }
.advisor-form-footer { display: flex; align-items: end; justify-content: space-between; gap: 20px; margin-top: 21px; }
.advisor-mode-control { width: 190px; }
.advisor-form-footer .button { border-radius: 9px; min-height: 42px; font-size: 12px; }
.advisor-form-footer .primary { background: #1c2430; border-color: #1c2430; color: white; }
.advisor-form .advisor-consent { margin-top: 17px; display: flex; flex-direction: row; align-items: flex-start; gap: 10px; background: #f6f8fb; padding: 13px; border-radius: 8px; line-height: 1.8; }
.advisor-consent input { width: 15px; height: 15px; margin-top: 3px; flex-shrink: 0; accent-color: #314768; }
.advisor-validation { font-size: 11px; color: #9c4f39; margin-top: 12px; }
.advisor-status,.advisor-hint { color: #828b98; font-size: 12px; line-height: 1.8; margin-top: 24px; }
.advisor-loading { display: flex; align-items: center; gap: 9px; margin-top: 28px; padding: 24px 0; font-size: 12px; color: #627289; }
.advisor-loading-dot { width: 7px; height: 7px; border-radius: 50%; background: #8196b5; }
.advisor-answer { border-top: 1px solid #e7ebf0; padding-top: 29px; margin-top: 31px; }
.advisor-answer-top { display: flex; justify-content: space-between; gap: 14px; font-size: 10px; color: #778395; }
.advisor-answer h3 { margin: 16px 0 12px; color: #273140; font-size: 17px; font-weight: 500; line-height: 1.6; }
.advisor-answer-text { white-space: pre-wrap; overflow-wrap: anywhere; font-size: 15px; color: #475260; line-height: 1.95; }
.advisor-answer-scope { margin-top: 18px; color: #838c98; font-size: 12px; line-height: 1.8; }
.advisor-evidence { display: grid; grid-template-columns: repeat(auto-fit,minmax(155px,1fr)); gap: 1px; background: #e9edf2; border: 1px solid #e9edf2; border-radius: 11px; overflow: hidden; margin-top: 20px; }
.advisor-evidence > div { padding: 18px; background: #fafbfd; min-width: 0; }
.advisor-evidence span { display: block; color: #7a8594; font-size: 12px; line-height: 1.6; }
.advisor-evidence strong { display: block; color: #364456; font-size: 22px; font-weight: 500; margin-top: 10px; overflow-wrap: anywhere; font-variant-numeric: tabular-nums; }
.advisor-evidence small { font-size: 12px; color: #7a8594; font-weight: 400; }
.advisor-extra-evidence { margin-top: 14px; font-size: 12px; color: #62758f; }.advisor-extra-evidence summary { cursor: pointer; }.advisor-extra-evidence .advisor-evidence { margin-top: 14px; }
.advisor-next { display: flex; flex-wrap: wrap; gap: 15px; margin-top: 23px; }
.advisor-next button { display: inline-flex; align-items: center; gap: 8px; color: #4e6b95; font-size: 12px; padding: 4px 0; }
.advisor-next button:hover { color: #244d89; }
.advisor-limits { display: flex; gap: 18px; border-top: 1px solid #edf0f4; padding-top: 19px; margin-top: 23px; font-size: 12px; color: #808996; line-height: 1.9; }
.advisor-limits > span { white-space: nowrap; color: #606b79; }
.advisor-limits ul { margin: 0; padding-left: 14px; }
.advisor-provenance { margin-top: 22px; color: #87909d; font-size: 10px; line-height: 1.8; overflow-wrap: anywhere; }
.advisor-provenance summary { cursor: pointer; }
.advisor-provenance p { margin-top: 12px; }
.advisor-provenance ul { padding-left: 15px; }.advisor-provenance code { display: block; font-size: 10px; }
@media(max-width:680px) { .advisor-heading { display: block; }.advisor-mode-label { margin-top: 17px; }.advisor-examples { grid-template-columns: 1fr; }.advisor-scope-controls { grid-template-columns: 1fr 1fr; }.advisor-scope-controls label:first-child { grid-column: 1 / -1; }.advisor-form-footer { gap: 12px; }.advisor-mode-control { width: 155px; }.advisor-answer-top { flex-direction: column; gap: 6px; }.advisor-limits { display: block; }.advisor-limits ul { margin-top: 8px; } }
</style>
