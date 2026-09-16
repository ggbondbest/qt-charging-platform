<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { AnalyticsError, formatValue, publishedRequest, shiftDate } from '../analytics';
import type { Dataset } from '../analytics';
import type { DashboardScope } from '../dashboardScope';
import { advisorDatesValid, advisorDestinations, advisorHistory, validateAdvisorResponse } from '../advisor';
import type { AdvisorCapabilities, AdvisorRequest, AdvisorResponse, AdvisorTarget, ValidatedAdvisorResponse } from '../advisor';
import Icon from './Icon.vue';
import '../intelligence-layout.css';

const props = withDefaults(defineProps<{ compact?: boolean; active?: boolean }>(), { compact: false, active: true });
const emit = defineEmits<{ navigate: [target: AdvisorTarget, scope: DashboardScope]; busy: [value: boolean] }>();
const dataset = ref<Dataset>();
const capabilities = ref<AdvisorCapabilities>();
const cities = ref<{ cityId: string; cityName: string }[]>([]);
const question = ref(''); const cityId = ref(''); const startDate = ref(''); const endDate = ref('');
const mode = ref<'offline' | 'online'>('offline'); const consent = ref(false);
const preparing = ref(false); const loading = ref(false); const error = ref(''); const status = ref('');
const result = ref<ValidatedAdvisorResponse>(); const answeredQuestion = ref('');
const pendingQuestion = ref(''); const composing = ref(false);
type CompletedAnswer = { id: number; question: string; cityName: string; response: ValidatedAdvisorResponse };
const history = ref<CompletedAnswer[]>([]); const currentAnswerId = ref<number>();
const archivedAnswers = computed(() => history.value.filter(item => item.id !== currentAnswerId.value));
const conversation = ref<HTMLElement>(); const currentAnswerElement = ref<HTMLElement>();
let followLatest = true; let automaticScrollTop: number | undefined;
let alive = true; let sequence = 0; let setupSequence = 0;
let controller: AbortController | undefined; let setupController: AbortController | undefined;
const maxLength = computed(() => Math.min(capabilities.value?.maxQuestionLength || 300, 300));
const validDates = computed(() => !!dataset.value && advisorDatesValid(dataset.value, startDate.value, endDate.value));
const canAsk = computed(() => props.active && !preparing.value && !loading.value && !!capabilities.value && validDates.value
  && !!question.value.trim() && question.value.trim().length <= maxLength.value
  && (mode.value === 'offline' || (capabilities.value.onlineAvailable && consent.value)));
const scopeCity = computed(() => cities.value.find(city => city.cityId === result.value?.scope.cityId)?.cityName || result.value?.scope.cityId || '全部城市');
const selectedCity = computed(() => cities.value.find(city => city.cityId === cityId.value)?.cityName || '全部城市');
const evidenceValue = (value: unknown) => value == null ? '暂无数据' : typeof value === 'number' ? formatValue(value, Number.isInteger(value) ? 0 : 2) : String(value);
const answerLabel = (answer: AdvisorResponse) => answer.status === 'chat' ? '在线对话' : answer.mode === 'online' ? '模型解读 · 请结合来源复核' : '本地证据答复';
const answerStatus = (answer: AdvisorResponse) => answer.status === 'chat' ? '' : answer.status === 'answered' ? (answer.mode === 'offline' ? '已核对统计范围' : '来源引用见下方') : answer.status === 'no_evidence' ? '当前范围证据不足' : '此问题暂不支持';
function citationLabel(answer: ValidatedAdvisorResponse, id: string) {
  const evidence = answer.evidence.find(item => item.id === id);
  if (evidence) return `${id} · ${evidence.label} · ${evidence.source.endpoint} · ${evidence.source.field}`;
  const knowledge = answer.knowledge.find(item => item.id === id);
  return knowledge ? `${id} · ${knowledge.title} · ${knowledge.source}` : id;
}
function navigate(target: AdvisorTarget) {
  if (!result.value) return;
  const { datasetId, publishedBatchId, cityId, startDate, endDate } = result.value.scope;
  emit('navigate', target, { datasetId, publishedBatchId, cityId, startDate, endDate });
}

function invalidate() {
  const wasLoading = loading.value; const hadResult = !!result.value;
  sequence++; controller?.abort(); loading.value = false; pendingQuestion.value = ''; result.value = undefined; currentAnswerId.value = undefined; consent.value = false;
  if (dataset.value && capabilities.value) error.value = '';
  status.value = wasLoading ? '已停止接收上次答复。修改完成后可重新提问。' : hadResult && !props.compact ? '问题或范围已变化，请重新生成答复。' : '';
}
watch([cityId, startDate, endDate, mode], invalidate, { flush: 'sync' });
watch(question, () => {
  // Chat drafts belong to the next request; the in-flight request already has its own snapshot.
  if (props.compact) consent.value = false;
  else invalidate();
}, { flush: 'sync' });

async function initialize() {
  if (!props.active) return;
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
    mode.value = config.data.defaultMode === 'online' && config.data.onlineAvailable ? 'online' : 'offline'; status.value = '';
  } catch (failure) {
    if (alive && current === setupSequence && !(failure instanceof AnalyticsError && failure.code === 'CANCELLED')) error.value = failure instanceof Error ? failure.message : '无法载入参谋，请重试。';
  } finally { if (alive && current === setupSequence) preparing.value = false; }
}

function rememberAnswer(answer: CompletedAnswer) {
  currentAnswerId.value = answer.id;
  history.value = [...history.value, answer].slice(-10);
}

async function ask() {
  if (!canAsk.value || !dataset.value) return;
  const body: AdvisorRequest = {
    question: question.value.trim(), datasetId: dataset.value.datasetId, publishedBatchId: dataset.value.publishedBatchId,
    ...(cityId.value ? { cityId: cityId.value } : {}), startDate: startDate.value, endDate: endDate.value,
    mode: mode.value, consent: mode.value === 'online' && consent.value,
  };
  if (body.mode === 'online') body.history = advisorHistory(history.value, body);
  // Capture the submitted question and consent before clearing the next chat draft.
  if (props.compact) question.value = '';
  const current = ++sequence; controller?.abort();
  error.value = ''; status.value = ''; result.value = undefined; currentAnswerId.value = undefined;
  consent.value = false;
  controller = new AbortController();
  pendingQuestion.value = body.question; loading.value = true;
  try {
    const response = await publishedRequest<AdvisorResponse>('/intelligence/advisor', {}, { method: 'POST', body, signal: controller.signal, timeoutMs: 70000 });
    if (!alive || current !== sequence) return;
    result.value = validateAdvisorResponse(response.data, body); answeredQuestion.value = body.question;
    rememberAnswer({ id: current, question: body.question, cityName: selectedCity.value, response: result.value });
  } catch (failure) {
    if (alive && current === sequence && !(failure instanceof AnalyticsError && failure.code === 'CANCELLED')) error.value = failure instanceof Error ? failure.message : '答复未完成，请重试。';
  } finally { if (alive && current === sequence) { loading.value = false; pendingQuestion.value = ''; } }
}
function stopReceiving() {
  sequence++; controller?.abort(); loading.value = false; pendingQuestion.value = ''; consent.value = false;
  status.value = '已停止接收答复。服务器可能仍在处理本次请求。';
}
function onComposerKeydown(event: KeyboardEvent) {
  if (!props.compact || event.key !== 'Enter' || event.shiftKey || event.isComposing || composing.value || event.keyCode === 229) return;
  event.preventDefault(); void ask();
}
function trackConversationScroll() {
  const element = conversation.value;
  if (!element) return;
  if (automaticScrollTop !== undefined && Math.abs(element.scrollTop - automaticScrollTop) < 1) {
    automaticScrollTop = undefined; return;
  }
  automaticScrollTop = undefined;
  followLatest = element.scrollHeight - element.scrollTop - element.clientHeight < 64;
}
watch(loading, value => emit('busy', value), { flush: 'sync' });
watch([result, loading, error], async ([answer], [previousAnswer]) => {
  await nextTick();
  const element = conversation.value;
  if (!props.compact || !props.active || !followLatest || !element) return;
  if (answer && answer !== previousAnswer && currentAnswerElement.value) {
    const offset = currentAnswerElement.value.getBoundingClientRect().top - element.getBoundingClientRect().top;
    element.scrollTop = Math.max(0, element.scrollTop + offset - 8);
  } else if (loading.value || error.value) element.scrollTop = element.scrollHeight;
  else return;
  automaticScrollTop = element.scrollTop;
}, { flush: 'post' });
function pause() {
  if (loading.value) stopReceiving();
  consent.value = false;
  if (preparing.value) { setupSequence++; setupController?.abort(); preparing.value = false; }
}
watch(() => props.active, active => {
  if (!active) pause();
  else if (!dataset.value && !preparing.value) initialize();
}, { flush: 'sync' });
defineExpose({ pause });
onMounted(() => { if (props.active) initialize(); });
onBeforeUnmount(() => { alive = false; sequence++; setupSequence++; controller?.abort(); setupController?.abort(); loading.value = false; });
</script>

<template>
  <section class="card intelligence-section intelligence-workspace advisor-workspace" :class="{ 'advisor-compact': compact }" aria-label="AI运营参谋">
    <div ref="conversation" class="advisor-conversation" tabindex="0" aria-label="参谋对话" @scroll="trackConversationScroll">
    <div v-if="!compact" class="intelligence-header advisor-heading">
      <div><div class="advisor-eyebrow">OPERATIONS ADVISOR</div><h2>把数据，变成下一步</h2><p>提出一个运营问题，从已发布统计中找到依据。</p></div>
      <span class="advisor-mode-label"><i aria-hidden="true"/>{{ mode === 'online' ? '在线辅助 · 单次授权' : '本地证据问答' }}</span>
    </div>
    <div v-else class="advisor-greeting"><p>运营参谋</p><span>{{ capabilities?.onlineAvailable ? '结合当前统计和知识来源，与你一起分析。' : '在线模型尚未配置，当前可查询本地统计。' }}</span></div>

    <div v-if="capabilities" class="advisor-examples" aria-label="运营问题示例">
      <button v-for="item in capabilities.supportedQuestions.slice(0, 4)" :key="item.id" type="button" @click="question = item.question"><Icon name="arrow" :size="15"/><span>{{ item.label }}</span></button>
    </div>

    <template v-if="compact">
      <article v-for="item in archivedAnswers" :key="item.id" class="advisor-history-item" aria-label="历史问答">
        <h3 class="advisor-user-message">{{ item.question }}</h3>
        <div class="advisor-history-reply">
          <div class="advisor-answer-top"><span>{{ answerLabel(item.response) }}</span></div>
          <p v-if="item.response.status !== 'chat'" class="advisor-answer-scope">{{ item.cityName }} · {{ item.response.scope.startDate }} 至 {{ item.response.scope.endDate }}（不含结束日）· 北京时间 · 模拟数据</p>
          <details class="advisor-history-answer">
            <summary>查看历史答复</summary>
            <p class="advisor-answer-text">{{ item.response.answer }}</p>
            <div v-if="item.response.status !== 'chat' && item.response.evidence.length" class="advisor-evidence"><div v-for="evidence in item.response.evidence" :key="evidence.id"><span>{{ evidence.label }}</span><strong>{{ evidenceValue(evidence.value) }} <small v-if="evidence.value != null">{{ evidence.unit }}</small></strong></div></div>
            <div v-if="item.response.citations.length" class="advisor-citations" aria-label="历史答复引用"><p>引用来源</p><ul><li v-for="id in item.response.citations" :key="id">{{ citationLabel(item.response, id) }}</li></ul></div>
            <div v-if="item.response.knowledge.length" class="advisor-knowledge" aria-label="历史知识来源"><div v-for="knowledge in item.response.knowledge" :key="knowledge.id"><strong>{{ knowledge.id }} · {{ knowledge.title }}</strong><p>{{ knowledge.text }}</p><span>来源：{{ knowledge.source }}</span></div></div>
            <div v-if="item.response.limitations.length" class="advisor-limits"><ul><li v-for="(limitation, index) in item.response.limitations" :key="index">{{ limitation }}</li></ul></div>
            <p v-if="item.response.status !== 'chat'" class="advisor-provenance">数据集 {{ item.response.scope.datasetId }} · 发布批次 {{ item.response.scope.publishedBatchId }}</p>
          </details>
        </div>
      </article>
    </template>
    <div v-if="error" class="intelligence-inline-error" role="alert">{{ error }} <button type="button" class="text-button" @click="initialize">重新载入</button></div>
    <p v-if="status" class="advisor-status" role="status">{{ status }}</p>
    <p v-if="compact && pendingQuestion" class="advisor-user-message advisor-pending-question">{{ pendingQuestion }}</p>
    <div v-if="preparing || loading" class="advisor-loading" role="status"><span class="advisor-typing-dots" aria-hidden="true"><i/><i/><i/></span>{{ preparing ? '正在读取已发布数据范围…' : mode === 'online' ? '正在检索来源并等待模型答复…' : '正在核对当前范围的统计证据…' }}</div>

    <article v-if="result" ref="currentAnswerElement" class="advisor-answer" aria-live="polite">
      <h3 class="advisor-user-message">{{ answeredQuestion }}</h3>
      <div class="advisor-reply">
      <div class="advisor-answer-top"><span>{{ answerLabel(result) }}</span><span v-if="answerStatus(result)">{{ answerStatus(result) }}</span></div>
      <p class="advisor-answer-text">{{ result.answer }}</p>
      <template v-if="result.status !== 'chat'">
      <p class="advisor-answer-scope">{{ scopeCity }} · {{ result.scope.startDate }} 至 {{ result.scope.endDate }}（不含结束日）· 北京时间 · 模拟数据</p>
      <details v-if="compact && result.evidence.length" class="advisor-compact-evidence"><summary>查看 {{ result.evidence.length }} 项数据依据</summary><div class="advisor-evidence" aria-label="答复依据"><div v-for="item in result.evidence" :key="item.id"><span>{{ item.label }}</span><strong>{{ evidenceValue(item.value) }} <small v-if="item.value != null">{{ item.unit }}</small></strong></div></div></details>
      <template v-if="!compact">
      <div v-if="result.evidence.length" class="advisor-evidence" aria-label="答复依据">
        <div v-for="item in result.evidence.slice(0, 6)" :key="item.id"><span>{{ item.label }}</span><strong>{{ evidenceValue(item.value) }} <small v-if="item.value != null">{{ item.unit }}</small></strong></div>
      </div>
      <details v-if="result.evidence.length > 6" class="advisor-extra-evidence"><summary>查看全部 {{ result.evidence.length }} 项证据</summary><div class="advisor-evidence" aria-label="补充答复依据"><div v-for="item in result.evidence.slice(6)" :key="item.id"><span>{{ item.label }}</span><strong>{{ evidenceValue(item.value) }} <small v-if="item.value != null">{{ item.unit }}</small></strong></div></div></details>
      </template>
      <div v-if="result.suggestions.length" class="advisor-next" aria-label="建议查看"><button v-for="item in result.suggestions" :key="item.target" type="button" @click="navigate(item.target)">{{ advisorDestinations[item.target] }}<Icon name="arrow" :size="14"/></button></div>
      </template>
      <div v-if="result.citations.length" class="advisor-citations" aria-label="答复引用"><p>引用来源</p><ul><li v-for="id in result.citations" :key="id">{{ citationLabel(result, id) }}</li></ul></div>
      <details v-if="result.knowledge.length" class="advisor-knowledge" aria-label="知识来源"><summary>查看 {{ result.knowledge.length }} 项知识来源</summary><div v-for="item in result.knowledge" :key="item.id"><strong>{{ item.id }} · {{ item.title }}</strong><p>{{ item.text }}</p><span>来源：{{ item.source }}</span></div></details>
      <details v-if="compact && result.limitations.length" class="advisor-compact-limits"><summary>使用边界</summary><ul><li v-for="(item, index) in result.limitations" :key="index">{{ item }}</li></ul></details>
      <div v-else-if="result.limitations.length" class="advisor-limits"><span>使用边界</span><ul><li v-for="(item, index) in result.limitations" :key="index">{{ item }}</li></ul></div>
      <details v-if="result.status !== 'chat'" class="advisor-provenance"><summary>数据来源与发布批次</summary><p>数据集 {{ result.scope.datasetId }}<br/>发布批次 {{ result.scope.publishedBatchId }}</p><ul><li v-for="item in result.evidence" :key="item.id"><span>{{ item.label }}</span><code>{{ item.source.endpoint }} · {{ item.source.field }}</code></li></ul></details>
      </div>
    </article>
    <p v-else-if="!loading && !preparing && !error && !status && !history.length" class="advisor-hint">先选一个问题，或写下你想了解的运营情况。答复会注明数据依据和适用范围。</p>
    </div>

    <form class="advisor-form" @submit.prevent="ask">
      <details class="advisor-settings" :open="!compact">
        <summary><span>{{ selectedCity }} · {{ mode === 'online' ? '在线辅助' : '本地问答' }}</span><span>范围与设置</span></summary>
        <div class="advisor-scope-controls">
          <label>城市<select v-model="cityId" aria-label="参谋城市" :disabled="preparing || !dataset"><option value="">全部城市</option><option v-for="city in cities" :key="city.cityId" :value="city.cityId">{{ city.cityName }}</option></select></label>
          <label>开始日期<input v-model="startDate" type="date" aria-label="参谋开始日期" :min="dataset?.startDate" :max="dataset?.endDate" :disabled="preparing || !dataset"/></label>
          <label>结束日期 · 不含当日<input v-model="endDate" type="date" aria-label="参谋结束日期" :min="dataset?.startDate" :max="dataset?.endDate" :disabled="preparing || !dataset"/></label>
        </div>
        <label class="advisor-mode-control">答复方式<select v-model="mode" aria-label="参谋答复方式" :disabled="preparing || !capabilities"><option value="offline">本地证据问答</option><option value="online" :disabled="!capabilities?.onlineAvailable">{{ capabilities?.onlineAvailable ? '在线模型辅助' : '在线模型 · 未配置' }}</option></select></label>
      </details>
      <p v-if="dataset && !validDates" class="advisor-validation" role="alert">请在 {{ dataset.startDate }} 至 {{ dataset.endDate }} 内选择起止日期，开始日期须早于结束日期。</p>
      <p v-if="capabilities && !capabilities.onlineAvailable" class="advisor-configuration" role="status">在线聊天需要先在后端配置 AIPing API Key、DeepSeek-V4.1-Flash 模型及服务地址，然后重新载入。密钥仅保存在后端；当前为本地统计问答。</p>
      <label v-if="mode === 'online' && capabilities?.onlineAvailable" class="advisor-consent"><input v-model="consent" type="checkbox" :disabled="loading" aria-label="允许本次向在线模型发送问题、最近对话、当前筛选聚合和知识片段"/><span>允许本次向 {{ capabilities.onlineProvider || '已配置的在线模型' }} 发送问题原文、同一范围最近三组对话、当前筛选聚合统计和检索到的知识片段。{{ capabilities.disclosure }} 每次提问需重新勾选。</span></label>
      <label class="advisor-question-label" for="advisor-question">{{ compact ? '和参谋聊聊' : '你的运营问题' }}</label>
      <textarea id="advisor-question" v-model="question" :maxlength="maxLength" :rows="compact ? 2 : 3" placeholder="哪些电站需要优先关注？" :disabled="preparing" @keydown="onComposerKeydown" @compositionstart="composing = true" @compositionend="composing = false"/>
      <div class="advisor-form-footer">
        <span class="advisor-composer-note">{{ compact ? 'Enter 发送 · Shift + Enter 换行' : '基于已发布统计 · 模拟数据' }}</span>
        <button v-if="loading" type="button" class="button secondary" @click="stopReceiving"><Icon name="pause" :size="15"/>停止接收</button>
        <button v-else type="submit" class="button primary" :disabled="!canAsk"><Icon name="arrow" :size="16"/>{{ compact ? '发送' : '生成运营答复' }}</button>
      </div>
    </form>
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
.advisor-typing-dots { display: inline-flex; align-items: center; gap: 3px; flex-shrink: 0; }
.advisor-typing-dots i { width: 4px; height: 4px; border-radius: 50%; background: #7194cc; animation: advisor-typing 1.2s ease-in-out infinite; }
.advisor-typing-dots i:nth-child(2) { animation-delay: .16s; }.advisor-typing-dots i:nth-child(3) { animation-delay: .32s; }
@keyframes advisor-typing { 0%,70%,100% { opacity: .35; transform: translateY(0); } 35% { opacity: 1; transform: translateY(-3px); } }
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
.advisor-citations,.advisor-knowledge { margin-top: 18px; color: #65758b; font-size: 11px; line-height: 1.8; overflow-wrap: anywhere; }
.advisor-citations p { margin-bottom: 6px; }.advisor-citations ul { margin: 0; padding-left: 17px; }
.advisor-knowledge summary { cursor: pointer; }.advisor-knowledge > div { margin-top: 12px; padding: 12px; border: 1px solid #e8edf4; border-radius: 8px; }
.advisor-knowledge strong { font-weight: 550; }.advisor-knowledge p { white-space: pre-wrap; margin: 8px 0; }.advisor-knowledge span { color: #87909d; }
.advisor-configuration { margin: 0 0 13px; color: #7b6c51; font-size: 11px; line-height: 1.8; }
.advisor-settings { margin-bottom: 18px; }
.advisor-settings > summary { display: flex; align-items: center; justify-content: space-between; gap: 12px; color: #6d7684; font-size: 11px; cursor: pointer; list-style: none; }
.advisor-settings > summary::-webkit-details-marker { display: none; }
.advisor-settings > summary span:last-child { color: #3867ae; }
.advisor-settings > .advisor-mode-control { margin-top: 14px; }
.advisor-composer-note { font-size: 10px; color: #9299a5; }
.advisor-greeting { padding: 5px 0 22px; }
.advisor-greeting p { margin: 0 0 8px; font-size: 17px; line-height: 1.5; color: #172132; font-weight: 550; }
.advisor-greeting > span { font-size: 12px; color: #8a929e; line-height: 1.8; }
.advisor-workspace.advisor-compact { display: flex; flex-direction: column; width: 100%; max-width: none; height: 100%; min-height: 0; margin: 0; padding: 0; border: 0; border-radius: 0; background: #fff; overflow: hidden; }
.advisor-compact .advisor-conversation { flex: 1 1 0; min-height: 0; overflow-y: auto; overscroll-behavior: contain; scrollbar-width: thin; padding: 22px 24px 20px; }
.advisor-compact .advisor-conversation:focus-visible { outline: 2px solid #bfd3f7; outline-offset: -3px; }
.advisor-compact .advisor-examples { grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; margin-bottom: 20px; }
.advisor-compact .advisor-examples button { align-items: flex-start; gap: 8px; min-height: 58px; border-radius: 12px; padding: 11px; font-size: 11px; background: #f8faff; border-color: #e9edf5; color: #4d596b; }
.advisor-compact .advisor-examples svg { margin-top: 2px; color: #5e85c7; }
.advisor-compact .advisor-form { flex: 0 0 auto; max-height: 64%; overflow-y: auto; overscroll-behavior: contain; border-top: 1px solid #edf0f5; padding: 15px 20px 18px; background: white; }
.advisor-compact .advisor-settings { margin-bottom: 14px; }
.advisor-compact .advisor-settings > summary { gap: 6px; font-size: 10px; }
.advisor-compact .advisor-scope-controls { grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 15px; }
.advisor-compact .advisor-scope-controls label:first-child { grid-column: 1 / -1; }
.advisor-compact .advisor-mode-control { width: 100%; }
.advisor-compact .advisor-form select,.advisor-compact .advisor-form input[type=date] { height: 36px; font-size: 11px; }
.advisor-compact .advisor-question-label { margin-bottom: 8px; font-size: 11px; color: #7f8897; }
.advisor-compact .advisor-form textarea { min-height: 76px; max-height: 130px; resize: none; overscroll-behavior: contain; padding: 12px 14px; border-radius: 13px; background: #fafbfd; font-size: 12px; line-height: 1.8; }
.advisor-compact .advisor-form-footer { margin-top: 10px; align-items: center; gap: 8px; }
.advisor-compact .advisor-form-footer .button { min-height: 34px; border-radius: 9px; padding: 7px 12px; font-size: 11px; }
.advisor-compact .advisor-form-footer .primary { background: #2864dc; border-color: #2864dc; }
.advisor-compact .advisor-form-footer .primary:hover:not(:disabled) { background: #1b54c6; }
.advisor-compact .advisor-consent { margin: 0 0 13px; padding: 10px; font-size: 10px; }
.advisor-compact .advisor-status,.advisor-compact .advisor-hint { margin-top: 14px; font-size: 11px; line-height: 1.9; }
.advisor-compact .advisor-loading { margin-top: 12px; padding: 16px 0; font-size: 11px; }
.advisor-compact .advisor-answer { margin-top: 22px; padding-top: 0; border: 0; }
.advisor-compact .advisor-user-message { width: fit-content; max-width: 92%; margin: 0 0 22px auto; padding: 12px 15px; border-radius: 14px 14px 3px 14px; background: #edf3ff; color: #334a71; font-size: 12px; line-height: 1.8; overflow-wrap: anywhere; }
.advisor-compact .advisor-reply,.advisor-history-reply { padding: 15px; border-radius: 3px 15px 15px 15px; background: #f7f9fc; }
.advisor-history-item { margin-top: 24px; }
.advisor-compact .advisor-history-item .advisor-user-message { margin-bottom: 12px; font-weight: 500; }
.advisor-history-reply .advisor-answer-scope { margin-top: 0; }
.advisor-history-answer { margin-top: 10px; font-size: 11px; color: #637ea7; }
.advisor-history-answer > summary { cursor: pointer; }
.advisor-compact .advisor-pending-question { margin-top: 24px; margin-bottom: 8px; }
.advisor-compact .advisor-answer-top { flex-direction: column; gap: 5px; font-size: 9px; }
.advisor-compact .advisor-answer-text { margin-top: 11px; font-size: 13px; line-height: 1.95; color: #364152; }
.advisor-compact .advisor-answer-scope { margin-top: 12px; font-size: 10px; line-height: 1.8; }
.advisor-compact-evidence { margin-top: 16px; color: #55749f; font-size: 11px; }
.advisor-compact-evidence summary { cursor: pointer; }
.advisor-compact-limits { margin-top: 14px; font-size: 10px; line-height: 1.8; color: #87909d; }
.advisor-compact-limits summary { cursor: pointer; }
.advisor-compact-limits ul { margin: 9px 0 0; padding-left: 15px; }
.advisor-compact .advisor-evidence { grid-template-columns: repeat(2,minmax(0,1fr)); margin-top: 12px; border-radius: 9px; }
.advisor-compact .advisor-evidence > div { padding: 12px; }
.advisor-compact .advisor-evidence span,.advisor-compact .advisor-evidence small { font-size: 10px; }
.advisor-compact .advisor-evidence strong { font-size: 19px; margin-top: 7px; }
.advisor-compact .advisor-next { margin-top: 17px; gap: 10px; }
.advisor-compact .advisor-next button { font-size: 11px; }
.advisor-compact .advisor-limits { display: block; margin-top: 18px; padding-top: 13px; font-size: 10px; }
.advisor-compact .advisor-limits ul { margin-top: 5px; }
.advisor-compact .advisor-provenance { margin-top: 14px; font-size: 9px; }
@media(prefers-reduced-motion:reduce) { .advisor-typing-dots i { animation: none; opacity: .7; } }
@media(max-width:680px) { .advisor-heading { display: block; }.advisor-mode-label { margin-top: 17px; }.advisor-examples { grid-template-columns: 1fr; }.advisor-scope-controls { grid-template-columns: 1fr 1fr; }.advisor-scope-controls label:first-child { grid-column: 1 / -1; }.advisor-form-footer { gap: 12px; }.advisor-mode-control { width: 155px; }.advisor-answer-top { flex-direction: column; gap: 6px; }.advisor-limits { display: block; }.advisor-limits ul { margin-top: 8px; } }
</style>
