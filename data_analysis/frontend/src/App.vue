<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { ApiError, request } from "./api";
import { submitAndReveal } from "./sectionNavigation";
import {
  finished,
  localTime,
  number,
  percent,
  probability,
  pretty,
  statusLabels,
} from "./display";
import type {
  Bootstrap,
  Candidate,
  City,
  Clock,
  Envelope,
  JsonObject,
  Location,
  Me,
  Recommendation,
  Station,
  Trip,
  TripStatus,
  User,
} from "./types";
import Icon from "./components/Icon.vue";
import StationMap from "./components/StationMap.vue";
import Chart from "./components/Chart.vue";
import AnalyticsDashboard from "./components/AnalyticsDashboard.vue";
import ForecastPanel from "./components/ForecastPanel.vue";
import ManagementInsights from "./components/ManagementInsights.vue";
import WorkspaceTabs from "./components/WorkspaceTabs.vue";
import AutoHideHeader from "./components/AutoHideHeader.vue";

type Tab = "dashboard" | "explore" | "trip" | "lab" | "admin";
const tabs: { id: Tab; label: string; icon: string }[] = [
  { id: "dashboard", label: "运营总览", icon: "chart" },
  { id: "explore", label: "智能找站", icon: "compass" },
  { id: "trip", label: "我的行程", icon: "trip" },
  { id: "lab", label: "智能分析", icon: "lab" },
  { id: "admin", label: "模拟控制台", icon: "sliders" },
];
const tripSteps: { label: string; statuses: TripStatus[] }[] = [
  { label: "前往电站", statuses: ["EN_ROUTE"] },
  { label: "到站 / 排队", statuses: ["QUEUED", "CALLED", "RESERVED"] },
  { label: "充电中", statuses: ["CHARGING"] },
  { label: "支付完成", statuses: ["PENDING_PAYMENT", "COMPLETED"] },
];
const tab = ref<Tab>("dashboard");
const dashboardPresentation = ref(false);
const labSection = ref('forecast');
const labSections = [
  { id: 'forecast', label: '负荷与空闲预测', description: '未来 1 / 6 / 24 小时' },
  { id: 'insights', label: '用户与异常', description: '回访风险 · 充电复核' },
  { id: 'arrival', label: '到站模型', description: '可用性与等待预测依据' },
  { id: 'experiments', label: '策略对比', description: '最近站 vs. 智能推荐' },
];
const adminSection = ref('scenario');
const adminSections = [
  { id: 'scenario', label: '场景控制', description: '时钟与电站资源' },
  { id: 'strategy', label: '推荐策略', description: '权重、积分与预约规则' },
  { id: 'experiments', label: '配对实验', description: '运行并比较两种策略' },
];
const boot = ref<Bootstrap>();
const stations = ref<Station[]>([]);
const cityId = ref("");
const origin = ref<Location>();
const picking = ref(false);
const highlighted = ref("");
const energyKwh = ref(20);
const maxEtaMinutes = ref(60);
const recommendation = ref<Recommendation>();
const me = ref<Me>();
const sessionName = ref("体验用户");
const token = ref("");
const sessionLoading = ref(false);
const initialLoading = ref(true);
const refreshLoading = ref(false);
const busyAction = ref("");
const errorMessage = ref("");
const toast = ref("");
const models = ref<JsonObject>();
const experiments = ref<JsonObject>();
const experimentCard = ref<HTMLElement | null>(null);
const clockTime = ref("");
const clock = ref<Clock>();
const latestRefresh = ref("");
const forecastExpanded = ref("");
const adminToken = ref("");
const admin = ref<JsonObject>();
const adminError = ref("");
const adminLoading = ref(false);
const adminSpeed = ref(60);
const advanceSeconds = ref(300);
const adminStationId = ref("");
const busyCount = ref(3);
const releaseAfterSeconds = ref(600);
const configForm = ref<JsonObject>({
  weights: {
    availability: 0.3,
    wait: 0.2,
    travel: 0.2,
    price: 0.1,
    power: 0.1,
    balance: 0.1,
  },
  rewards: { first: 100, second: 50 },
  minimumEnergyKwh: 5,
  minimumChargingSeconds: 300,
  dailyRewardBudget: 10000,
  callTimeoutSeconds: 60,
  reservationTimeoutSeconds: 900,
});
const experimentUsers = ref(1000);
const experimentSeed = ref(42);
const tripRoute = ref<JsonObject>();
const routeLoading = ref(false);
const routeError = ref("");
let pollTimer: number | undefined;
let toastTimer: number | undefined;
let polling = false;
let mounted = true;
let meSequence = 0;
let stationSequence = 0;
let lastAutoRecAttempt = 0;
try {
  token.value = localStorage.getItem("chargepilot-demo-session") || "";
} catch {
  /* Private browsing may disable storage. */
}
const selectedCity = computed<City | undefined>(() =>
  boot.value?.cities.find((c) => c.cityId === cityId.value),
);
const cityStations = computed(() =>
  stations.value.filter((s) => s.cityId === cityId.value),
);
const candidates = computed(() => recommendation.value?.candidates || []);
const focusedCandidate = computed(
  () =>
    candidates.value.find((c) => c.stationId === highlighted.value) ||
    candidates.value[0],
);
const activeTrip = computed(() =>
  me.value?.trips.find((t) => !finished(t.status)),
);
const activeTripStage = computed(() =>
  tripSteps.findIndex(
    (step) =>
      activeTrip.value && step.statuses.includes(activeTrip.value.status),
  ),
);
const previousTrips = computed(
  () => me.value?.trips.filter((t) => finished(t.status)) || [],
);
const freeCount = computed(() =>
  cityStations.value.reduce((sum, s) => sum + (s.currentFree || 0), 0),
);
const totalCapacity = computed(() =>
  cityStations.value.reduce((sum, s) => sum + (s.capacity || 0), 0),
);
const modelReady = computed(
  () =>
    models.value?.status === "READY" ||
    models.value?.arrival?.status === "READY" ||
    !!models.value?.arrival?.metadata,
);
const points = computed(() => me.value?.user.points ?? 0);
const scoreNames: Record<string, string> = {
  availability: "到站可用",
  wait: "等待时间",
  travel: "行驶时间",
  price: "充电价格",
  power: "充电功率",
  balance: "负荷均衡",
};
const eventNames: Record<string, string> = {
  SELECTED: "已选择推荐电站",
  EN_ROUTE: "行程已创建",
  ARRIVED: "已抵达电站",
  QUEUED: "已加入队列",
  CALLED: "电桩已为你预留",
  CONFIRMED: "叫号确认成功",
  RESERVED: "电桩预约成功",
  STARTED: "开始充电",
  CHARGING: "开始充电",
  STOPPED: "充电结束",
  PENDING_PAYMENT: "充电结束，等待支付",
  PAID: "支付完成",
  COMPLETED: "订单完成",
  REWARD: "推荐积分到账",
  REWARD_GRANTED: "推荐积分到账",
  CANCELLED: "行程已取消",
  EXPIRED: "行程已过期",
  ARRIVE: "已抵达电站",
  CONFIRM: "已确认叫号",
  START: "开始充电",
  STOP: "充电结束",
  PAY: "支付完成",
  CANCEL: "行程已取消",
};
const policyRows = computed<JsonObject[]>(() =>
  Array.isArray(experiments.value?.policies) ? experiments.value!.policies : [],
);
const testMetrics = computed<JsonObject>(
  () =>
    models.value?.arrival?.metrics?.test ||
    models.value?.arrival?.metadata?.metrics?.test ||
    {},
);
const modelMetricRows = computed(() =>
  [
    {
      key: "空闲数 MAE / 根",
      value: testMetrics.value.availability?.model?.countMae,
    },
    {
      key: "到站可用 Brier",
      value: testMetrics.value.availability?.model?.brier,
    },
    {
      key: "条件等待 MAE / 分钟",
      value: testMetrics.value.wait?.model?.maeMinutes,
    },
    { key: "可服务概率 AUC", value: testMetrics.value.service?.rocAuc },
  ].filter((row) => typeof row.value === "number"),
);
const horizonMetrics = computed(() =>
  Object.entries(testMetrics.value.availability?.byHorizon || {}).sort(
    ([a], [b]) => Number(a) - Number(b),
  ),
);
const recommendationExpired = computed(
  () =>
    !!recommendation.value &&
    !!clockTime.value &&
    Date.parse(clockTime.value) >= Date.parse(recommendation.value.expiresAt),
);
const tripStation = computed(() =>
  stations.value.find((s) => s.stationId === activeTrip.value?.stationId),
);
const tripCity = computed(() =>
  boot.value?.cities.find((c) => c.cityId === tripStation.value?.cityId),
);
const weightSum = computed(() =>
  Object.values(configForm.value.weights || {}).reduce<number>(
    (sum, value) => sum + Number(value || 0),
    0,
  ),
);
const clockLabel = computed(() =>
  localTime(clockTime.value || clock.value?.time, true),
);

function notify(message: string) {
  toast.value = message;
  window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => {
    toast.value = "";
  }, 4200);
}
function receive<T>(response: Envelope<T>): T {
  if (
    response.meta?.clockTime &&
    (!clockTime.value ||
      Date.parse(response.meta.clockTime) >= Date.parse(clockTime.value))
  )
    clockTime.value = response.meta.clockTime;
  return response.data;
}
function errorText(error: unknown) {
  return error instanceof Error ? error.message : "请求未完成，请重试。";
}
function forgetSession() {
  token.value = "";
  me.value = undefined;
  recommendation.value = undefined;
  try {
    localStorage.removeItem("chargepilot-demo-session");
  } catch {}
}
async function auth(): Promise<string> {
  if (token.value) return token.value;
  sessionLoading.value = true;
  try {
    const result = receive(
      await request<{ token: string; user: User }>("/sessions", {
        method: "POST",
        body: { name: sessionName.value.trim() || "体验用户" },
      }),
    );
    token.value = result.token;
    me.value = { user: result.user, trips: [], ledger: [] };
    try {
      localStorage.setItem("chargepilot-demo-session", result.token);
    } catch {}
    return result.token;
  } finally {
    sessionLoading.value = false;
  }
}
async function refreshMe() {
  if (!token.value) return;
  const seq = ++meSequence;
  try {
    const result = await request<Me>("/me", { token: token.value });
    if (mounted && seq === meSequence) me.value = receive(result);
  } catch (error) {
    if (error instanceof ApiError && error.status === 401) forgetSession();
    throw error;
  }
}
async function refreshStations() {
  const seq = ++stationSequence;
  const result = await request<Station[]>("/stations");
  if (mounted && seq === stationSequence) {
    stations.value = receive(result);
    latestRefresh.value = clockTime.value;
  }
}
async function refreshVisible() {
  if (tab.value === "dashboard" || document.hidden || polling || busyAction.value || initialLoading.value)
    return;
  polling = true;
  const results = await Promise.allSettled([
    refreshStations(),
    ...(token.value ? [refreshMe()] : []),
    ...(experiments.value?.status === "RUNNING" ? [refreshExperiments()] : []),
  ]);
  if (mounted) {
    const failure = results.find((result) => result.status === "rejected");
    if (failure?.status === "rejected")
      errorMessage.value = errorText(failure.reason);
  }
  polling = false;
  if (
    mounted &&
    tab.value === "explore" &&
    token.value &&
    !activeTrip.value &&
    recommendationExpired.value &&
    Date.now() - lastAutoRecAttempt > 10000
  ) {
    lastAutoRecAttempt = Date.now();
    await recommend();
  }
}
async function initialize() {
  initialLoading.value = true;
  errorMessage.value = "";
  try {
    const data = receive(await request<Bootstrap>("/bootstrap"));
    boot.value = data;
    stations.value = data.stations;
    clock.value = data.clock;
    clockTime.value ||= data.clock.time;
    if (!cityId.value || !data.cities.some((c) => c.cityId === cityId.value))
      cityId.value =
        data.cities.find((c) => c.cityId === "DL")?.cityId ||
        data.cities[0]?.cityId ||
        "";
    origin.value = selectedCity.value
      ? {
          latitude: selectedCity.value.latitude,
          longitude: selectedCity.value.longitude,
        }
      : undefined;
    adminStationId.value ||= data.stations[0]?.stationId || "";
    latestRefresh.value = clockTime.value;
    const results = await Promise.allSettled([
      refreshModels(),
      refreshExperiments(),
      ...(token.value ? [refreshMe()] : []),
    ]);
    for (const result of results)
      if (result.status === "rejected")
        errorMessage.value = errorText(result.reason);
  } catch (error) {
    errorMessage.value = errorText(error);
  } finally {
    initialLoading.value = false;
  }
}
async function run(action: string, work: () => Promise<void>) {
  if (busyAction.value) return;
  busyAction.value = action;
  errorMessage.value = "";
  try {
    await work();
  } catch (error) {
    errorMessage.value = errorText(error);
    if (error instanceof ApiError && error.status === 401) forgetSession();
  } finally {
    busyAction.value = "";
  }
}
function changeCity(id: string) {
  cityId.value = id;
  const city = boot.value?.cities.find((c) => c.cityId === id);
  if (city)
    origin.value = { latitude: city.latitude, longitude: city.longitude };
  recommendation.value = undefined;
  highlighted.value = "";
  picking.value = false;
}
function setOrigin(location: Location) {
  origin.value = location;
  picking.value = false;
  recommendation.value = undefined;
  notify("出发点已更新，重新推荐即可比较附近电站。");
}
function locate() {
  if (!navigator.geolocation) {
    errorMessage.value = "当前浏览器不支持定位，请点击地图选择出发点。";
    return;
  }
  navigator.geolocation.getCurrentPosition(
    (position) =>
      setOrigin({
        latitude: position.coords.latitude,
        longitude: position.coords.longitude,
      }),
    () => {
      errorMessage.value = "未获得当前位置，请点击地图设置出发点。";
    },
    { timeout: 8000, maximumAge: 60000 },
  );
}
function recommendationInput() {
  return {
    cityId: cityId.value,
    origin: origin.value ? { ...origin.value } : undefined,
    energyKwh: energyKwh.value,
    maxEtaMinutes: maxEtaMinutes.value,
  };
}
async function recommend() {
  await run("recommend", async () => {
    if (!origin.value || !cityId.value)
      throw new Error("请先选择城市和出发点。");
    const input = recommendationInput();
    const fingerprint = JSON.stringify(input);
    const stillCurrent = () =>
      mounted && fingerprint === JSON.stringify(recommendationInput());
    const session = await auth();
    if (!stillCurrent()) return;
    let response: Envelope<Recommendation>;
    try {
      response = await request<Recommendation>("/recommendations", {
        method: "POST",
        token: session,
        body: input,
      });
    } catch (error) {
      if (stillCurrent()) throw error;
      return;
    }
    if (!stillCurrent()) return;
    const result = receive(response);
    recommendation.value = result;
    highlighted.value = result.candidates[0]?.stationId || "";
    if (!result.candidates.length)
      notify("当前条件下没有候选电站，请调整可接受的行驶时间。");
  });
}
async function selectStation(candidate: Candidate) {
  await run(`select-${candidate.stationId}`, async () => {
    if (!recommendation.value) return;
    await request<Trip>(
      `/recommendations/${encodeURIComponent(recommendation.value.recommendationId)}/select`,
      {
        method: "POST",
        token: await auth(),
        body: { stationId: candidate.stationId },
      },
    );
    await refreshMe();
    await refreshStations();
    tab.value = "trip";
    notify("行程已创建。到达预计时刻后可确认抵达。");
  });
}
async function tripAction(trip: Trip, action: string) {
  await run(`trip-${action}`, async () => {
    receive(
      await request<Trip>(
        `/trips/${encodeURIComponent(trip.tripId)}/${action}`,
        {
          method: "POST",
          token: token.value,
          body: action === "stop" ? { reason: "MANUAL" } : {},
        },
      ),
    );
    await refreshMe();
    await refreshStations();
    notify(
      action === "pay"
        ? "模拟支付已完成，实际奖励以到账积分为准。"
        : "行程状态已更新。",
    );
  });
}
function remainingUntil(value?: string) {
  if (!value || !clockTime.value) return 0;
  return Math.max(
    0,
    Math.ceil((Date.parse(value) - Date.parse(clockTime.value)) / 1000),
  );
}
function duration(seconds: number) {
  const value = Math.max(0, Math.floor(seconds || 0));
  return `${Math.floor(value / 60)} 分 ${String(value % 60).padStart(2, "0")} 秒`;
}
async function refreshModels() {
  try {
    models.value = receive(await request<JsonObject>("/models"));
  } catch (error) {
    models.value = { status: "NOT_READY", error: errorText(error) };
  }
}
async function refreshExperiments() {
  try {
    const wasRunning = experiments.value?.status === "RUNNING";
    experiments.value = receive(await request<JsonObject>("/experiments"));
    if (wasRunning && experiments.value?.status === "READY")
      notify("配对实验已完成，可在智能分析的「最近站 vs. 智能推荐」查看结果。");
  } catch (error) {
    experiments.value = { status: "NOT_RUN", message: errorText(error) };
  }
}
async function showRoute() {
  if (!activeTrip.value?.origin || routeLoading.value) return;
  routeLoading.value = true;
  routeError.value = "";
  try {
    tripRoute.value = receive(
      await request<JsonObject>("/route", {
        method: "POST",
        token: token.value,
        body: {
          origin: activeTrip.value.origin,
          stationId: activeTrip.value.stationId,
        },
      }),
    );
  } catch (error) {
    routeError.value = errorText(error);
  } finally {
    routeLoading.value = false;
  }
}
const experimentOption = computed(() => ({
  color: ["#9aa5b5", "#2563eb"],
  legend: {
    bottom: 0,
    itemWidth: 12,
    itemHeight: 12,
    textStyle: { color: "#4b5563" },
  },
  tooltip: { trigger: "axis" },
  grid: { left: 48, right: 20, top: 30, bottom: 70 },
  xAxis: {
    type: "category",
    data: ["等待时间", "行驶时间", "总用时"],
    axisLine: { lineStyle: { color: "#e5e7eb" } },
    axisTick: { show: false },
    axisLabel: { color: "#4b5563" },
  },
  yAxis: {
    type: "value",
    name: "分钟",
    splitLine: { lineStyle: { color: "#f5f5f7" } },
    axisLabel: { color: "#6b7280" },
  },
  series: policyRows.value.map((row) => ({
    type: "bar",
    name: row.label || row.policy,
    data: [row.meanWaitMinutes, row.meanDrivingMinutes, row.meanTotalMinutes],
    barWidth: 30,
    itemStyle: { borderRadius: [5, 5, 0, 0] },
  })),
}));
const horizonOption = computed(() => ({
  color: ["#4b5563"],
  grid: { left: 45, right: 23, top: 30, bottom: 36 },
  tooltip: { trigger: "axis" },
  xAxis: {
    type: "category",
    name: "分钟",
    data: horizonMetrics.value.map(([key]) => key),
    axisLine: { lineStyle: { color: "#e5e7eb" } },
    axisTick: { show: false },
    axisLabel: { color: "#6b7280" },
  },
  yAxis: {
    type: "value",
    name: "Brier ↓",
    splitLine: { lineStyle: { color: "#f5f5f7" } },
    axisLabel: { color: "#6b7280" },
  },
  series: [
    {
      name: "TEST 可用性误差",
      type: "line",
      data: horizonMetrics.value.map(
        ([, value]) => (value as JsonObject).brier,
      ),
      symbolSize: 6,
      lineStyle: { width: 2 },
      areaStyle: { color: "#4b556310" },
    },
  ],
}));
async function adminRequest(path: string, method = "GET", body?: unknown) {
  if (!adminToken.value.trim()) throw new Error("请输入管理员令牌。");
  return receive(
    await request<JsonObject>(path, {
      method,
      body,
      admin: adminToken.value.trim(),
    }),
  );
}
async function getAdmin() {
  if (adminLoading.value) return;
  adminLoading.value = true;
  adminError.value = "";
  try {
    admin.value = await adminRequest("/admin");
    if (admin.value.clock) {
      clock.value = admin.value.clock;
      adminSpeed.value = admin.value.clock.speed;
    }
    if (admin.value.config)
      configForm.value = JSON.parse(JSON.stringify(admin.value.config));
  } catch (error) {
    adminError.value = errorText(error);
    admin.value = undefined;
  } finally {
    adminLoading.value = false;
  }
}
async function changeAdmin(path: string, body: unknown, method = "POST") {
  await run("admin", async () => {
    await adminRequest(path, method, body);
    await getAdmin();
    await refreshStations();
    if (token.value) await refreshMe();
    notify("模拟设置已更新。");
  });
}
async function runExperiment() {
  await run("experiment", async () => {
    await submitAndReveal(
      () => adminRequest("/admin/experiments", "POST", {
        users: experimentUsers.value,
        seed: experimentSeed.value,
      }),
      (result) => {
        experiments.value = result;
        labSection.value = 'experiments';
        tab.value = "lab";
        notify(
          result.status === "READY"
            ? "配对模拟实验已完成。"
            : "实验已提交，完成后会自动更新实际结果。",
        );
      },
      () => experimentCard.value,
      () => mounted && tab.value === "lab" && labSection.value === 'experiments',
    );
  });
}
async function exportFeedback() {
  await run("feedback", async () => {
    const report = await adminRequest("/admin/feedback");
    const url = URL.createObjectURL(
      new Blob([JSON.stringify(report, null, 2)], {
        type: "application/json;charset=utf-8",
      }),
    );
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = "chargepilot-feedback.json";
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
    notify("反馈数据已导出，用于后续离线分析。");
  });
}
function saveConfig() {
  if (Math.abs(weightSum.value - 1) > 0.00001) {
    adminError.value = "六项排序权重之和必须为 1。";
    return;
  }
  changeAdmin("/admin/config", configForm.value, "PATCH");
}
watch([energyKwh, maxEtaMinutes], () => {
  recommendation.value = undefined;
});
watch(tab, (value) => {
  errorMessage.value = "";
  if (value !== 'dashboard') dashboardPresentation.value = false;
  window.scrollTo({ top: 0, behavior: 'instant' });
  if (value === "lab") {
    refreshModels();
    refreshExperiments();
  }
});
watch(
  () => activeTrip.value?.tripId,
  () => {
    tripRoute.value = undefined;
    routeError.value = "";
  },
);
onMounted(() => {
  initialize();
  pollTimer = window.setInterval(refreshVisible, 2000);
  document.addEventListener("visibilitychange", refreshVisible);
});
onBeforeUnmount(() => {
  mounted = false;
  window.clearInterval(pollTimer);
  window.clearTimeout(toastTimer);
  document.removeEventListener("visibilitychange", refreshVisible);
});
</script>

<template>
  <div class="app-shell" :class="`workspace-${tab}`">
    <AutoHideHeader :auto-hide="tab === 'dashboard' && dashboardPresentation">
      <button
        class="brand"
        @click="tab = 'dashboard'"
        aria-label="充能智析首页"
      >
        <span class="brand-mark"><Icon name="bolt" :size="25" /></span
        ><span
          >充能<span class="brand-light">智析</span
          ><small>CHARGE INSIGHT · 运营与决策</small></span
        >
      </button>
      <nav class="main-nav" aria-label="主导航">
        <button
          v-for="item in tabs"
          :key="item.id"
          :class="{ active: tab === item.id }"
          :aria-current="tab === item.id ? 'page' : undefined"
          @click="tab = item.id"
        >
          <Icon :name="item.icon" :size="17" /><span>{{ item.label }}</span
          ><i v-if="item.id === 'trip' && activeTrip" class="nav-dot"></i>
        </button>
      </nav>
      <div class="topbar-right">
        <span class="replay-label"><span class="live-dot"></span>模拟回放</span>
        <div class="user-avatar" :title="me?.user.name || '演示会话'">
          <Icon name="user" :size="18" />
        </div>
      </div>
    </AutoHideHeader>

    <main>
      <div v-if="errorMessage && tab !== 'dashboard'" class="error-banner" role="alert">
        <Icon name="info" :size="18" /><span>{{ errorMessage }}</span
        ><button v-if="!boot" class="text-button" @click="initialize">
          重新连接</button
        ><button aria-label="关闭错误提示" @click="errorMessage = ''">
          <Icon name="close" :size="16" />
        </button>
      </div>

      <AnalyticsDashboard v-if="tab === 'dashboard'" v-model:presentation="dashboardPresentation" />
      <template v-if="tab === 'explore'">
        <section class="explore-hero">
          <div class="hero-copy">
            <div class="eyebrow light">
              CHARGING NETWORK
            </div>
            <h1>智能找站</h1>
            <p>
              选择出发点，比较到站空闲、等待与价格。
            </p>
            <div class="hero-proof">
              <Icon name="shield" :size="15" />模拟场景 · 预测不等于资源预约
            </div>
          </div>
          <div class="hero-clock">
            <div class="hero-clock-label">
              <Icon name="clock" :size="16" />当前模拟时刻
            </div>
            <strong>{{ clockLabel }}</strong
            ><span
              >北京时间 ·
              {{
                clock?.paused ? "已暂停" : `${clock?.speed || "—"} 倍回放`
              }}</span
            >
            <div class="clock-divider"></div>
            <p>基于历史模拟场景<br />不代表真实电站实时状态</p>
          </div>
        </section>

        <section class="explore-toolbar">
          <div class="city-switcher">
            <span class="small-label"
              ><Icon name="pin" :size="15" />探索城市</span
            >
            <div
              v-if="initialLoading && !boot"
              class="skeleton city-skeleton"
            ></div>
            <button
              v-for="city in boot?.cities || []"
              :key="city.cityId"
              :class="{ selected: cityId === city.cityId }"
              @click="changeCity(city.cityId)"
            >
              {{ city.cityName.replace(/市$/, "") }}
            </button>
          </div>
          <div class="area-stats">
            <span
              ><b>{{ number(cityStations.length) }}</b> 座电站</span
            ><i></i
            ><span
              ><b class="teal">{{ number(freeCount) }}</b> /
              {{ number(totalCapacity) }} 根当前空闲</span
            ><span class="subtle">每 2 秒刷新</span>
          </div>
        </section>

        <section class="discovery-layout">
          <div class="map-column">
            <div class="map-panel">
              <StationMap
                :city="selectedCity"
                :stations="cityStations"
                :candidates="candidates"
                :origin="origin"
                :highlighted="highlighted"
                :picking="picking"
                @origin="setOrigin"
                @station="highlighted = $event"
              />
              <div v-if="initialLoading && !boot" class="map-loading">
                <span class="spinner"></span>正在连接模拟场景…
              </div>
            </div>
            <div class="journey-form card">
              <div class="origin-field">
                <span class="input-icon"
                  ><Icon name="location" :size="20"
                /></span>
                <div>
                  <label>我的出发点</label
                  ><button class="origin-value" @click="picking = !picking">
                    {{
                      origin
                        ? `${number(origin.latitude, 4)}° N，${number(origin.longitude, 4)}° E`
                        : "请先选择城市"
                    }}<Icon name="down" :size="13" />
                  </button>
                </div>
                <button
                  class="icon-button"
                  title="使用设备定位"
                  aria-label="使用设备定位"
                  @click="locate"
                >
                  <Icon name="location" :size="17" />
                </button>
              </div>
              <div class="form-divider"></div>
              <div class="energy-field">
                <label for="energy">计划补电</label
                ><select id="energy" v-model.number="energyKwh">
                  <option
                    v-for="value in [5, 10, 20, 30, 40, 60]"
                    :key="value"
                    :value="value"
                  >
                    {{ value }} kWh
                  </option>
                </select>
              </div>
              <div class="eta-field">
                <label for="max-eta">最远行驶</label
                ><select id="max-eta" v-model.number="maxEtaMinutes">
                  <option
                    v-for="value in [15, 30, 60]"
                    :key="value"
                    :value="value"
                  >
                    {{ value }} 分钟
                  </option>
                </select>
              </div>
              <button
                class="button primary recommend-button"
                :disabled="!!busyAction || initialLoading || !origin"
                @click="recommend"
              >
                <span v-if="busyAction === 'recommend'" class="spinner"></span
                ><Icon v-else name="bolt" :size="18" />{{
                  busyAction === "recommend" ? "正在计算推荐" : "为我推荐"
                }}<Icon
                  v-if="busyAction !== 'recommend'"
                  name="arrow"
                  :size="18"
                />
              </button>
            </div>
            <div class="map-footnote">
              <span
                ><Icon
                  name="info"
                  :size="14"
                />先点出发点坐标，再点地图修改位置；点电站可高亮位置。</span
              ><span>数据截至 {{ localTime(latestRefresh) }}</span>
            </div>
            <div class="trust-strip">
              <div>
                <span class="trust-icon"
                  ><Icon name="signal" :size="19"
                /></span>
                <p>
                  <b>预测你抵达之后</b><span>以 ETA 对齐可用性与等待时间</span>
                </p>
              </div>
              <div>
                <span class="trust-icon"><Icon name="leaf" :size="19" /></span>
                <p>
                  <b>让空闲资源被看见</b><span>将负荷均衡纳入推荐评分</span>
                </p>
              </div>
              <div>
                <span class="trust-icon peach"
                  ><Icon name="gift" :size="19"
                /></span>
                <p>
                  <b>充电完成，再领积分</b><span>服务端确认有效订单后发放</span>
                </p>
              </div>
            </div>
          </div>

          <aside class="recommendations-panel">
            <div class="section-top">
              <div>
                <div class="eyebrow">YOUR NEXT CHARGE</div>
                <h2>
                  {{ recommendation ? "推荐电站" : "选择充电站" }}
                </h2>
              </div>
              <span class="count-pill">{{
                recommendation ? `${candidates.length} 个推荐` : "到站预测"
              }}</span>
            </div>
            <div v-if="!recommendation" class="recommendation-empty">
              <div class="empty-orbit">
                <Icon name="compass" :size="44" /><i></i>
              </div>
              <h3>从出发点开始</h3>
              <p>
                选好出发点和补电量，<br />比较到站空闲概率、等待时间和充电价格。
              </p>
              <div class="empty-factor-grid">
                <span><Icon name="pin" :size="15" />到站时间</span
                ><span><Icon name="users" :size="15" />排队预估</span
                ><span><Icon name="bolt" :size="15" />充电功率</span
                ><span><Icon name="gift" :size="15" />推荐积分</span>
              </div>
              <button
                class="button secondary"
                :disabled="!!busyAction || initialLoading || !origin"
                @click="recommend"
              >
                开始智能推荐<Icon name="arrow" :size="17" />
              </button>
            </div>
            <div v-else-if="!candidates.length" class="empty-state compact">
              <Icon name="pin" :size="34" />
              <h3>暂时没有合适的电站</h3>
              <p>试试增加最远行驶时间，或调整出发点。</p>
            </div>
            <div
              v-if="recommendationExpired"
              class="expired-offer"
              role="status"
            >
              <Icon name="clock" :size="15" /><span
                >推荐已过期，正在等待刷新。</span
              ><button :disabled="!!busyAction" @click="recommend">
                重新推荐
              </button>
            </div>
            <div
              v-if="recommendation && candidates.length"
              class="candidate-list"
            >
              <article
                v-for="candidate in candidates"
                :key="candidate.stationId"
                class="candidate-card"
                :class="{
                  best: candidate.rank === 1,
                  focused: highlighted === candidate.stationId,
                }"
                @mouseenter="highlighted = candidate.stationId"
              >
                <div class="candidate-header">
                  <span class="rank-badge">{{
                    String(candidate.rank).padStart(2, "0")
                  }}</span>
                  <div>
                    <h3>
                      <button @click="highlighted = candidate.stationId">
                        {{
                          candidate.stationName.replace("模拟充电站", "充电站")
                        }}
                      </button>
                    </h3>
                    <div class="candidate-subline">
                      <span v-if="candidate.rank === 1" class="best-label"
                        ><Icon name="star" :size="11" />综合推荐</span
                      ><span>{{ number(candidate.distanceKm, 1) }} km</span
                      ><span>¥{{ number(candidate.pricePerKwh, 2) }}/kWh</span
                      ><span>{{ number(candidate.powerKw) }} kW</span>
                    </div>
                  </div>
                  <span v-if="candidate.rewardPoints > 0" class="reward-tag"
                    ><Icon name="gift" :size="12" />{{
                      number(candidate.rewardPoints)
                    }}
                    积分</span
                  >
                </div>
                <div class="candidate-metrics">
                  <div>
                    <label>预计到站</label
                    ><strong
                      >{{ number(candidate.etaMinutes)
                      }}<small> 分钟</small></strong
                    >
                  </div>
                  <div>
                    <label>到站有空桩</label
                    ><strong class="teal">{{
                      probability(candidate.availableProbability)
                    }}</strong>
                  </div>
                  <div>
                    <label>预计等待</label
                    ><strong
                      >{{ number(candidate.waitMinutes)
                      }}<small> 分钟</small></strong
                    >
                  </div>
                </div>
                <div class="reason-line">
                  <Icon name="check" :size="14" /><span>{{
                    candidate.reasons?.[0] ||
                    "综合可用性、等待、行驶、价格、功率与负荷评分"
                  }}</span>
                </div>
                <div class="candidate-footer">
                  <button
                    class="explain-button"
                    @click="
                      forecastExpanded =
                        forecastExpanded === candidate.stationId
                          ? ''
                          : candidate.stationId
                    "
                  >
                    推荐依据<Icon name="down" :size="13" /></button
                  ><span class="score-caption"
                    >综合 <b>{{ number(candidate.score, 1) }}</b> 分</span
                  ><button
                    class="button small"
                    :class="candidate.rank === 1 ? 'primary' : 'secondary'"
                    :disabled="
                      !!busyAction || !!activeTrip || recommendationExpired
                    "
                    @click="selectStation(candidate)"
                  >
                    {{
                      busyAction === `select-${candidate.stationId}`
                        ? "正在创建…"
                        : activeTrip
                          ? "已有进行中行程"
                          : recommendationExpired
                            ? "请刷新推荐"
                            : "选择并出发"
                    }}<Icon name="arrow" :size="15" />
                  </button>
                </div>
                <div
                  v-if="forecastExpanded === candidate.stationId"
                  class="candidate-explanation"
                >
                  <div class="detail-grid">
                    <span
                      >当前空闲
                      <b>{{ number(candidate.currentFree) }} 根</b></span
                    ><span
                      >预计空闲
                      <b>{{ number(candidate.expectedFree, 1) }} 根</b></span
                    ><span
                      >等待 P90
                      <b>{{ number(candidate.waitP90Minutes) }} 分钟</b></span
                    ><span
                      >可服务概率
                      <b>{{
                        probability(candidate.serviceProbability)
                      }}</b></span
                    ><span
                      >当前资源占用率
                      <b>{{ percent(candidate.loadRatio) }}</b></span
                    ><span v-if="candidate.loadForecast"
                      >到站所在小时负荷
                      <b>{{ number(candidate.loadForecast.meanPowerKw, 1) }} kW</b></span
                    ><span
                      >预计总用时
                      <b>{{ number(candidate.totalMinutes) }} 分钟</b></span
                    >
                  </div>
                  <div class="score-bars">
                    <div
                      v-for="(value, key) in candidate.scoreBreakdown"
                      :key="key"
                    >
                      <span>{{ scoreNames[key] || key }}</span
                      ><i
                        ><b
                          :style="{
                            width: `${Math.max(0, Math.min(100, value))}%`,
                          }"
                        ></b></i
                      ><small>{{ number(value, 1) }} 分</small>
                    </div>
                  </div>
                  <p
                    v-for="reason in candidate.reasons?.slice(1)"
                    :key="reason"
                  >
                    {{ reason }}
                  </p>
                  <p>
                    预计 {{ localTime(candidate.arrivalTime) }} 抵达 ·
                    {{ candidate.resolutionMinutes }} 分钟预测分辨率
                  </p>
                  <p v-if="candidate.loadForecast">
                    均衡项同时参考资源占用和小时预测负荷：
                    {{ localTime(candidate.loadForecast.timestamp) }} 起的小时均值
                    {{ number(candidate.loadForecast.meanPowerKw, 1) }} kW /
                    额定 {{ number(candidate.loadForecast.ratedCapacityKw, 1) }} kW。
                    小时功率不替代分钟级空桩预测。
                  </p>
                  <p>
                    {{
                      /tencent/i.test(candidate.routeSource)
                        ? "腾讯路线估计"
                        : "按距离与速度估算行驶时间，非实时道路导航"
                    }}
                  </p>
                  <p class="subtle">
                    积分为预估承诺，完成符合资格的充电并支付后结算。
                  </p>
                </div>
              </article>
            </div>
            <div
              v-if="recommendation?.nearestComparison"
              class="comparison-note"
            >
              <Icon name="compass" :size="18" />
              <div>
                <b>距离之外，多想一步</b>
                <p>
                  相较最近站，预计总用时{{
                    (recommendation.nearestComparison.savedMinutes || 0) >= 0
                      ? "减少"
                      : "增加"
                  }}
                  {{
                    number(
                      Math.abs(
                        recommendation.nearestComparison.savedMinutes || 0,
                      ),
                      1,
                    )
                  }}
                  分钟。综合推荐同时考虑可用性和负荷。
                </p>
              </div>
            </div>
            <div v-if="recommendation?.warnings?.length" class="warning-notes">
              <p v-for="warning in recommendation.warnings" :key="warning">
                <Icon name="info" :size="13" />{{ warning }}
              </p>
            </div>
            <div v-if="recommendation" class="recommendation-meta">
              预测基准 {{ localTime(recommendation.referenceTime) }} ·
              推荐有效至 {{ localTime(recommendation.expiresAt) }}
            </div>
          </aside>
        </section>
      </template>

      <template v-if="tab === 'trip'">
        <section class="page-heading">
          <div>
            <div class="eyebrow">EVERY STEP, CONNECTED</div>
            <h1>我的充电行程</h1>
            <p>从选站到积分到账，每一步都在这里。</p>
          </div>
          <span class="time-chip"
            ><Icon name="clock" :size="16" />模拟时刻 {{ clockLabel }}</span
          >
        </section>
        <div class="trip-layout">
          <div class="trip-main">
            <section v-if="activeTrip" class="card active-trip">
              <div class="section-top">
                <div>
                  <span
                    class="status-tag"
                    :class="activeTrip.status.toLowerCase()"
                    ><span class="live-dot"></span
                    >{{ statusLabels[activeTrip.status] }}</span
                  >
                  <h2>{{ activeTrip.stationName }}</h2>
                  <p class="subtle mono">{{ activeTrip.tripId }}</p>
                </div>
                <div class="trip-station-symbol">
                  <Icon
                    :name="activeTrip.status === 'CHARGING' ? 'bolt' : 'pin'"
                    :size="31"
                  />
                </div>
              </div>
              <div class="trip-stepper">
                <div
                  v-for="(step, index) in tripSteps"
                  :key="step.label"
                  :class="{
                    current: step.statuses.includes(activeTrip.status),
                    done: index < activeTripStage,
                  }"
                >
                  <span>{{ index + 1 }}</span>
                  <p>{{ step.label }}</p>
                </div>
              </div>
              <div
                class="trip-state-panel"
                :class="activeTrip.status.toLowerCase()"
              >
                <template v-if="activeTrip.status === 'EN_ROUTE'"
                  ><Icon name="road" :size="36" />
                  <div>
                    <h3>路上不着急，电站已选好</h3>
                    <p v-if="activeTrip.arrivalEligibleAt">
                      预计 {{ localTime(activeTrip.arrivalEligibleAt) }} 抵达 ·
                      {{
                        remainingUntil(activeTrip.arrivalEligibleAt) > 0
                          ? `模拟时间还需 ${duration(remainingUntil(activeTrip.arrivalEligibleAt))}`
                          : "已到达预计抵达时刻，可以确认到站。"
                      }}
                    </p>
                    <p v-else>抵达后确认到站，系统将分配空闲电桩或加入队列。</p>
                  </div></template
                >
                <template v-else-if="activeTrip.status === 'QUEUED'"
                  ><strong class="queue-number">{{
                    number(activeTrip.queuePosition)
                  }}</strong>
                  <div>
                    <h3>你已在队列中</h3>
                    <p>
                      前面还有
                      {{ number(activeTrip.peopleAhead) }}
                      人，叫号后请及时确认。页面会自动刷新。
                    </p>
                  </div></template
                >
                <template v-else-if="activeTrip.status === 'CALLED'"
                  ><Icon name="bolt" :size="36" />
                  <div>
                    <h3>轮到你了，请确认叫号</h3>
                    <p>
                      剩余
                      {{ duration(remainingUntil(activeTrip.callExpiresAt)) }} ·
                      超时将释放名额。
                    </p>
                  </div></template
                >
                <template v-else-if="activeTrip.status === 'RESERVED'"
                  ><Icon name="check" :size="36" />
                  <div>
                    <h3>电桩已为你预留</h3>
                    <p>
                      请在
                      {{ localTime(activeTrip.reservationExpiresAt) }}
                      前开始充电，剩余
                      {{
                        duration(
                          remainingUntil(activeTrip.reservationExpiresAt),
                        )
                      }}。
                    </p>
                  </div></template
                >
                <template v-else-if="activeTrip.status === 'CHARGING'"
                  ><div class="charging-symbol">
                    <Icon name="bolt" :size="38" />
                  </div>
                  <div>
                    <h3>正在补充下一程的能量</h3>
                    <p>
                      已充电 {{ duration(activeTrip.chargingSeconds) }} ·
                      用量与费用由服务端计量。
                    </p>
                  </div></template
                >
                <template v-else-if="activeTrip.status === 'PENDING_PAYMENT'"
                  ><Icon name="card" :size="36" />
                  <div>
                    <h3>充电结束，等待模拟结算</h3>
                    <p>
                      电桩已释放。此为模拟支付，不产生真实扣款；完成后核验并发放推荐积分。
                    </p>
                  </div></template
                >
              </div>
              <div class="trip-values">
                <div>
                  <label>已充电量</label
                  ><strong
                    >{{ number(activeTrip.energyKwh, 2)
                    }}<small> kWh</small></strong
                  >
                </div>
                <div>
                  <label>{{
                    activeTrip.status === "CHARGING" ? "当前费用" : "订单金额"
                  }}</label
                  ><strong
                    ><small>¥ </small>{{ number(activeTrip.amount, 2) }}</strong
                  >
                </div>
                <div>
                  <label>推荐承诺积分</label
                  ><strong class="orange"
                    >{{ number(activeTrip.rewardPoints)
                    }}<small> 分</small></strong
                  >
                </div>
              </div>
              <div class="trip-actions">
                <button
                  v-if="activeTrip.status === 'EN_ROUTE'"
                  class="button primary"
                  :disabled="
                    !!busyAction ||
                    remainingUntil(activeTrip.arrivalEligibleAt) > 0
                  "
                  @click="tripAction(activeTrip, 'arrive')"
                >
                  <Icon name="pin" :size="17" />确认已到站</button
                ><button
                  v-if="activeTrip.status === 'CALLED'"
                  class="button primary"
                  :disabled="!!busyAction"
                  @click="tripAction(activeTrip, 'confirm')"
                >
                  确认叫号<Icon name="arrow" :size="17" /></button
                ><button
                  v-if="activeTrip.status === 'RESERVED'"
                  class="button primary"
                  :disabled="!!busyAction"
                  @click="tripAction(activeTrip, 'start')"
                >
                  <Icon name="bolt" :size="18" />开始充电</button
                ><button
                  v-if="activeTrip.status === 'CHARGING'"
                  class="button primary"
                  :disabled="!!busyAction"
                  @click="tripAction(activeTrip, 'stop')"
                >
                  结束充电并结算<Icon name="arrow" :size="17" /></button
                ><button
                  v-if="activeTrip.status === 'PENDING_PAYMENT'"
                  class="button primary"
                  :disabled="!!busyAction"
                  @click="tripAction(activeTrip, 'pay')"
                >
                  模拟支付 ¥{{ number(activeTrip.amount, 2)
                  }}<Icon name="arrow" :size="17" /></button
                ><button
                  v-if="
                    ['EN_ROUTE', 'QUEUED', 'CALLED', 'RESERVED'].includes(
                      activeTrip.status,
                    )
                  "
                  class="button ghost"
                  :disabled="!!busyAction"
                  @click="tripAction(activeTrip, 'cancel')"
                >
                  取消行程</button
                ><span
                  v-if="busyAction.startsWith('trip-')"
                  class="action-feedback"
                  ><span class="spinner dark"></span>正在处理…</span
                >
              </div>
              <div
                v-if="activeTrip.origin && activeTrip.status === 'EN_ROUTE'"
                class="trip-route-section"
              >
                <button
                  class="button secondary"
                  :disabled="routeLoading"
                  @click="showRoute"
                >
                  <Icon name="map" :size="17" />{{
                    routeLoading
                      ? "正在请求路线…"
                      : tripRoute
                        ? "刷新行程路线"
                        : "查看行程路线"
                  }}
                </button>
                <p v-if="routeError" class="route-notice">{{ routeError }}</p>
                <template v-if="tripRoute"
                  ><p class="route-notice">
                    {{
                      tripRoute.notice ||
                      (/tencent/i.test(tripRoute.routeSource)
                        ? "腾讯路线规划结果"
                        : "直线位置示意，不是道路导航")
                    }}
                  </p>
                  <div class="trip-route-map">
                    <StationMap
                      :city="tripCity"
                      :stations="tripStation ? [tripStation] : []"
                      :candidates="[]"
                      :origin="activeTrip.origin"
                      :highlighted="activeTrip.stationId"
                      :picking="false"
                      :route-coordinates="tripRoute.coordinates"
                    /></div
                ></template>
              </div>
              <div class="timeline-section">
                <h3>行程动态</h3>
                <ol class="timeline">
                  <li
                    v-for="(event, index) in activeTrip.events"
                    :key="event.eventId || index"
                  >
                    <span class="timeline-dot"></span>
                    <div>
                      <b>{{
                        eventNames[event.type] ||
                        statusLabels[
                          event.status as keyof typeof statusLabels
                        ] ||
                        event.type
                      }}</b>
                      <p v-if="event.details?.message">
                        {{ event.details.message }}
                      </p>
                    </div>
                    <time>{{ localTime(event.createdAt, true) }}</time>
                  </li>
                </ol>
              </div>
            </section>
            <section v-else class="card empty-trip">
              <div class="empty-orbit"><Icon name="trip" :size="43" /></div>
              <h2>下一次充电，从好选择开始</h2>
              <p>
                你目前没有进行中的行程。选一座合适的电站，<br />到站、排队、充电和结算将自动串联。
              </p>
              <button class="button primary" @click="tab = 'explore'">
                去智能找站<Icon name="arrow" :size="18" />
              </button>
            </section>
            <section v-if="previousTrips.length" class="card history-card">
              <div class="section-top">
                <h2>最近的行程</h2>
                <span class="subtle">{{ previousTrips.length }} 笔</span>
              </div>
              <article
                v-for="trip in previousTrips"
                :key="trip.tripId"
                class="history-row"
              >
                <span class="history-icon"
                  ><Icon
                    :name="trip.status === 'COMPLETED' ? 'check' : 'trip'"
                    :size="20"
                /></span>
                <div>
                  <h3>{{ trip.stationName }}</h3>
                  <p>
                    {{ statusLabels[trip.status] }} ·
                    {{ number(trip.energyKwh, 2) }} kWh ·
                    {{ number(trip.chargingSeconds / 60, 1) }} 分钟
                  </p>
                </div>
                <div class="history-value">
                  <b>¥{{ number(trip.amount, 2) }}</b
                  ><span :class="{ orange: trip.awardedPoints > 0 }">{{
                    trip.awardedPoints > 0
                      ? `+${number(trip.awardedPoints)} 积分已到账`
                      : "未发放推荐积分"
                  }}</span>
                </div>
              </article>
            </section>
          </div>
          <aside class="trip-aside">
            <section class="wallet-card">
              <div class="wallet-top">
                <span class="wallet-icon"><Icon name="gift" :size="23" /></span
                ><span>我的推荐积分</span><Icon name="star" :size="18" />
              </div>
              <strong>{{ number(points) }}<small>分</small></strong>
              <p>每一次更好的选择，<br />都有机会带来一点回馈。</p>
              <div class="wallet-rule">
                <Icon
                  name="shield"
                  :size="16"
                />完成有效充电并支付后发放，实际到账以服务端结算为准。
              </div>
            </section>
            <section class="card session-card">
              <div class="section-top">
                <h3>演示身份</h3>
                <span class="tiny-tag">DEMO</span>
              </div>
              <template v-if="me"
                ><div class="session-person">
                  <span class="user-avatar"
                    ><Icon name="user" :size="22"
                  /></span>
                  <div>
                    <b>{{ me.user.name }}</b
                    ><span>独立会话 · 自动保存</span>
                  </div>
                </div>
                <button
                  class="text-button"
                  @click="
                    forgetSession();
                    notify('已退出本地演示会话。');
                  "
                >
                  退出当前会话<Icon name="exit" :size="14" /></button></template
              ><template v-else
                ><label for="session-name">你的演示昵称</label
                ><input
                  id="session-name"
                  v-model="sessionName"
                  maxlength="40"
                  placeholder="输入昵称"
                /><button
                  class="button secondary full"
                  :disabled="sessionLoading || !!busyAction"
                  @click="
                    run('login', async () => {
                      await auth();
                      notify('演示会话已创建。');
                    })
                  "
                >
                  {{ sessionLoading ? "正在创建…" : "创建演示会话" }}
                </button></template
              >
              <p class="tiny-note">仅用于此模拟场景，不代表真实充电账户。</p>
            </section>
            <section class="card ledger-card">
              <h3>积分动态</h3>
              <div v-if="!me?.ledger?.length" class="ledger-empty">
                <Icon name="gift" :size="23" />
                <p>还没有积分入账记录</p>
              </div>
              <div
                v-for="(entry, index) in me?.ledger || []"
                :key="entry.tripId || index"
                class="ledger-row"
              >
                <div>
                  <b>完成推荐充电</b
                  ><span>{{ localTime(entry.createdAt, true) }}</span>
                </div>
                <strong>+{{ number(entry.points) }}</strong>
              </div>
            </section>
          </aside>
        </div>
      </template>

      <template v-if="tab === 'lab'">
        <section class="page-heading">
          <div>
            <div class="eyebrow">PREDICTIONS WITH EVIDENCE</div>
            <h1>智能分析</h1>
            <p>预测供需，识别风险，验证每一次推荐。</p>
          </div>
          <span class="simulation-chip"
            ><Icon name="lab" :size="16" />模拟数据实验</span
          >
        </section>
        <WorkspaceTabs v-model="labSection" :items="labSections" label="智能分析分区" />
        <KeepAlive><ForecastPanel v-if="labSection === 'forecast'" /></KeepAlive>
        <KeepAlive><ManagementInsights v-if="labSection === 'insights'" /></KeepAlive>
        <div v-if="labSection === 'arrival'" class="lab-grid single-evidence workspace-content">
          <section class="card model-card">
            <div class="section-top">
              <span class="model-icon"><Icon name="compass" :size="25" /></span
              ><span class="status-tag" :class="{ ready: modelReady }">{{
                modelReady ? "模型已就绪" : "模型尚未就绪"
              }}</span>
            </div>
            <div class="eyebrow">ARRIVAL INTELLIGENCE · TEST</div>
            <h2>到站空闲与等待预测</h2>
            <p>
              以出发时刻可用的历史信息，估计抵达后空闲数、等待时间和可服务概率。以下均为实际
              TEST 结果。
            </p>
            <div v-if="modelMetricRows.length" class="model-metric-grid">
              <div v-for="metric in modelMetricRows" :key="metric.key">
                <label>{{ metric.key }}</label
                ><strong>{{ number(metric.value, 3) }}</strong>
              </div>
            </div>
            <div v-else class="model-pending">
              <Icon name="info" :size="17" />{{
                models?.error || "尚无可展示的模型评价数值；等待实际训练产物。"
              }}
            </div>
            <template v-if="testMetrics.availability"
              ><div class="model-chart-title">
                预测时距与可用性误差<span>TEST · Brier 越低越好</span>
              </div>
              <Chart
                v-if="horizonMetrics.length"
                :option="horizonOption"
                label="测试集不同到站时距的可用性Brier误差"
              />
              <div class="baseline-comparison">
                <div>
                  <span>空闲数 MAE / 根</span
                  ><b
                    >模型
                    {{ number(testMetrics.availability.model?.countMae, 3) }}</b
                  ><span
                    >持久性基线
                    {{
                      number(testMetrics.availability.persistence?.countMae, 3)
                    }}</span
                  >
                </div>
                <div>
                  <span>条件等待 MAE / 分钟</span
                  ><b
                    >模型
                    {{ number(testMetrics.wait?.model?.maeMinutes, 3) }}</b
                  ><span
                    >零等待基线
                    {{
                      number(testMetrics.wait?.zeroWait?.maeMinutes, 3)
                    }}</span
                  >
                </div>
                <div>
                  <span>条件等待 RMSE / 分钟</span
                  ><b>{{ number(testMetrics.wait?.model?.rmseMinutes, 3) }}</b
                  ><span
                    >P90 覆盖率
                    {{ percent(testMetrics.wait?.p90Coverage) }}</span
                  >
                </div>
                <div>
                  <span>可服务概率 Brier</span
                  ><b>{{ number(testMetrics.service?.brier, 3) }}</b
                  ><span
                    >恒定概率基线
                    {{
                      number(testMetrics.service?.constantTrainRateBrier, 3)
                    }}</span
                  >
                </div>
              </div>
              <p class="model-caveat">
                等待模型仅评价成功、非预约充电样本；失败等待未知。模型并非所有指标都优于简单基线，MAE
                的差异如实保留。概率校准、长尾误差与服务概率需一起判断。
              </p>
            </template>
            <details v-if="models?.arrival">
              <summary>
                查看完整模型与评价记录<Icon name="down" :size="15" />
              </summary>
              <pre>{{ pretty(models.arrival) }}</pre>
            </details>
          </section>
        </div>
        <section
          v-if="labSection === 'experiments'"
          id="paired-experiment-results"
          ref="experimentCard"
          class="card experiment-card"
          tabindex="-1"
          aria-labelledby="paired-experiment-title"
        >
          <div class="section-top">
            <div>
              <div class="eyebrow">SAME JOURNEYS. DIFFERENT DECISIONS.</div>
              <h2 id="paired-experiment-title">最近站 vs. 智能推荐</h2>
              <p class="subtle">
                相同请求、相同随机种子，比较推荐策略的实际模拟结果。
              </p>
            </div>
            <span class="tiny-tag">{{
              experiments?.status === "READY"
                ? `${number(experiments.users)} 个模拟用户`
                : experiments?.status === "RUNNING"
                  ? "实验进行中"
                  : experiments?.status === "FAILED"
                    ? "实验失败"
                    : "尚未运行"
            }}</span>
          </div>
          <template v-if="experiments?.status === 'READY' && policyRows.length"
            ><div class="experiment-body">
              <div class="experiment-chart">
                <Chart
                  :option="experimentOption"
                  label="最近站与智能推荐的平均等待、行驶和总用时对比"
                />
              </div>
              <div class="experiment-highlights">
                <div>
                  <label>平均等待时间变化</label
                  ><strong
                    :class="{
                      teal: experiments.changes?.waitReductionPercent >= 0,
                    }"
                    >{{
                      experiments.changes?.waitReductionPercent >= 0
                        ? "−"
                        : "+"
                    }}{{
                      number(
                        Math.abs(experiments.changes?.waitReductionPercent),
                        1,
                      )
                    }}<small>%</small></strong
                  ><span>相对最近站策略</span>
                </div>
                <div>
                  <label>平均总用时变化</label
                  ><strong
                    :class="{
                      teal: experiments.changes?.totalTimeReductionPercent >= 0,
                    }"
                    >{{
                      experiments.changes?.totalTimeReductionPercent >= 0
                        ? "−"
                        : "+"
                    }}{{
                      number(
                        Math.abs(
                          experiments.changes?.totalTimeReductionPercent,
                        ),
                        1,
                      )
                    }}<small>%</small></strong
                  ><span>保留所有实际结果，包括退化</span>
                </div>
              </div>
            </div>
            <div class="table-scroll">
              <table class="data-table">
                <thead>
                  <tr>
                    <th>策略</th>
                    <th>到站可用率</th>
                    <th>成功服务率</th>
                    <th>平均等待</th>
                    <th>总用时</th>
                    <th>资源占用率标准差</th>
                    <th>推荐积分</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="row in policyRows" :key="row.policy">
                    <td>
                      <span
                        class="policy-dot"
                        :class="{ ai: row.policy === 'ai_incentive' }"
                      ></span
                      >{{ row.label || row.policy }}
                    </td>
                    <td>{{ percent(row.availabilitySuccessRate) }}</td>
                    <td>{{ percent(row.serviceSuccessRate) }}</td>
                    <td>{{ number(row.meanWaitMinutes, 1) }} 分钟</td>
                    <td>{{ number(row.meanTotalMinutes, 1) }} 分钟</td>
                    <td>{{ number(row.utilizationStd, 3) }}</td>
                    <td>{{ number(row.rewardPoints) }}</td>
                  </tr>
                </tbody>
              </table>
            </div>
            <details class="experiment-notes"><summary>实验口径与适用边界<Icon name="down" :size="16" /></summary>
              <p v-for="note in experiments.notes || []" :key="note">
                <Icon name="info" :size="14" />{{ note }}
              </p>
              <p>
                随机种子 {{ experiments.seed }} · 生成于
                {{ localTime(experiments.generatedAt, true) }} ·
                仅用于模拟场景对比，不代表真实运营效果。
              </p>
            </details></template
          >
          <div v-else class="empty-state">
            <Icon name="lab" :size="37" />
            <h3>
              {{
                experiments?.status === "RUNNING"
                  ? "配对实验正在计算"
                  : experiments?.status === "FAILED"
                    ? "配对实验计算失败"
                    : "还没有实验结果"
              }}
            </h3>
            <p>
              {{
                experiments?.status === "RUNNING"
                  ? "已提交到后台，页面将每 2 秒检查结果。"
                  : experiments?.message ||
                    "管理员可在运营控制台运行配对实验，完成后这里会显示实际对比。"
              }}
            </p>
            <button class="button secondary" @click="adminSection = 'experiments'; tab = 'admin'">
              前往运营控制台<Icon name="arrow" :size="16" />
            </button>
          </div>
        </section>
        <section
          v-if="labSection === 'experiments' && experiments?.status === 'READY' && policyRows.length"
          class="card resource-comparison"
        >
          <div class="section-top">
            <div>
              <h2>服务与资源对比</h2>
              <p class="subtle">
                推荐可能增加行驶时间或局部拥挤；以下保留完整对照。
              </p>
            </div>
          </div>
          <div class="table-scroll">
            <table class="data-table">
              <thead>
                <tr>
                  <th>策略</th>
                  <th>服务 / 放弃人数</th>
                  <th>行驶时间</th>
                  <th>拥挤站点占比</th>
                  <th>空闲浪费占比</th>
                  <th>最高资源占用率</th>
                  <th>完成行程平均用时</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="row in policyRows" :key="row.policy">
                  <td>{{ row.label || row.policy }}</td>
                  <td>
                    {{ number(row.served) }} / {{ number(row.abandoned) }}
                  </td>
                  <td>{{ number(row.meanDrivingMinutes, 1) }} 分钟</td>
                  <td>{{ percent(row.congestedStationRate) }}</td>
                  <td>{{ percent(row.idleWasteRate) }}</td>
                  <td>{{ percent(row.maxUtilization) }}</td>
                  <td>{{ number(row.meanCompletedJourneyMinutes, 1) }} 分钟</td>
                </tr>
              </tbody>
            </table>
          </div>
          <p class="tiny-note">
            资源占用包括维护、预留和占位。上方总用时计入失败请求的行驶与 120
            分钟等待；这里的完成行程用时只统计成功服务请求。
          </p>
        </section>
        <details v-if="labSection === 'arrival' || labSection === 'experiments'" class="card provenance-card">
          <summary>
            <span><Icon name="shield" :size="18" />数据来源与回放边界</span
            ><Icon name="down" :size="16" />
          </summary>
          <p>
            所有展示均来自模拟数据。排序中的实时承诺修正属于场景估计，离线指标评价原始模型。历史真实结果只用于离线实验，不作为出发时的预测输入。
          </p>
          <pre>{{
            pretty(
              models?.provenance ||
                boot?.provenance || {
                  dataSource: "SIMULATED",
                  status: "尚未获取来源元数据",
                },
            )
          }}</pre>
        </details>
      </template>
      <template v-if="tab === 'admin'">
        <section class="page-heading">
          <div>
            <div class="eyebrow">REPLAY CONTROL ROOM</div>
            <h1>模拟控制台</h1>
            <p>调节模拟时钟、资源占用和推荐激励，仅作用于演示业务库。</p>
          </div>
          <span class="simulation-chip"
            ><Icon name="sliders" :size="16" />运营实验台</span
          >
        </section>
        <section class="card admin-auth">
          <span class="admin-auth-icon"><Icon name="shield" :size="24" /></span>
          <div>
            <h3>管理员身份验证</h3>
            <p>输入后端配置的管理员令牌，仅保留在当前页面内存中。</p>
          </div>
          <div class="admin-token-field">
            <input
              v-model="adminToken"
              type="password"
              autocomplete="off"
              aria-label="管理员令牌"
              placeholder="输入管理员令牌"
              @keyup.enter="getAdmin"
            /><button
              class="button primary"
              :disabled="adminLoading || !adminToken.trim()"
              @click="getAdmin"
            >
              {{ adminLoading ? "验证中…" : admin ? "刷新状态" : "连接控制台" }}
            </button>
          </div>
        </section>
        <div v-if="adminError" class="error-banner" role="alert">
          <Icon name="info" :size="18" />{{ adminError }}
        </div>
        <WorkspaceTabs v-if="admin" v-model="adminSection" :items="adminSections" label="控制台分区" />
        <div v-if="admin" class="admin-grid workspace-content" :class="`admin-grid--${adminSection}`">
          <section v-show="adminSection === 'scenario'" class="card admin-clock-card">
            <div class="section-top">
              <div>
                <div class="eyebrow">SIMULATION CLOCK</div>
                <h2>模拟时钟</h2>
              </div>
              <span class="status-tag ready">{{
                admin.clock?.paused ? "已暂停" : "运行中"
              }}</span>
            </div>
            <div class="big-clock">
              {{ localTime(admin.clock?.time, true) }}
            </div>
            <p class="subtle">北京时间 · 单向前进，无法回拨</p>
            <div class="admin-clock-controls">
              <label
                >运行倍速<select v-model.number="adminSpeed">
                  <option
                    v-for="speed in [1, 10, 30, 60, 120, 300]"
                    :key="speed"
                    :value="speed"
                  >
                    {{ speed }}×
                  </option>
                </select></label
              ><button
                class="button secondary"
                :disabled="!!busyAction"
                @click="
                  changeAdmin('/admin/clock', {
                    action: 'configure',
                    speed: adminSpeed,
                    paused: !!admin.clock?.paused,
                  })
                "
              >
                应用倍速</button
              ><button
                class="button secondary"
                :disabled="!!busyAction"
                @click="
                  changeAdmin('/admin/clock', {
                    action: 'configure',
                    speed: adminSpeed,
                    paused: !admin.clock?.paused,
                  })
                "
              >
                <Icon
                  :name="admin.clock?.paused ? 'play' : 'pause'"
                  :size="17"
                />{{ admin.clock?.paused ? "继续" : "暂停" }}
              </button>
            </div>
            <div class="advance-controls">
              <select v-model.number="advanceSeconds" aria-label="推进模拟时间">
                <option
                  v-for="seconds in [60, 300, 600, 900, 1800, 3600]"
                  :key="seconds"
                  :value="seconds"
                >
                  推进 {{ seconds / 60 }} 分钟
                </option></select
              ><button
                class="button primary"
                :disabled="!!busyAction"
                @click="
                  changeAdmin('/admin/clock', {
                    action: 'advance',
                    seconds: advanceSeconds,
                  })
                "
              >
                推进时钟<Icon name="arrow" :size="17" />
              </button>
            </div>
            <p class="tiny-note">
              时钟推进将触发到站期限、叫号超时与充电计量。
            </p>
          </section>
          <section v-show="adminSection === 'scenario'" class="card busy-card">
            <div class="section-top">
              <div>
                <div class="eyebrow">RESOURCE SCENARIO</div>
                <h2>构建一个排队场景</h2>
              </div>
              <Icon name="users" :size="24" />
            </div>
            <label for="busy-station">选择电站</label
            ><select id="busy-station" v-model="adminStationId">
              <option
                v-for="station in stations"
                :key="station.stationId"
                :value="station.stationId"
              >
                {{ station.stationName }}
              </option>
            </select>
            <div class="two-fields">
              <label
                >背景占用桩数<input
                  v-model.number="busyCount"
                  type="number"
                  min="0"
                  :max="
                    stations.find((s) => s.stationId === adminStationId)
                      ?.capacity || 3
                  " /></label
              ><label
                >多少秒后释放<input
                  v-model.number="releaseAfterSeconds"
                  type="number"
                  min="1"
                  step="60"
              /></label>
            </div>
            <button
              class="button secondary full"
              :disabled="!!busyAction"
              @click="
                changeAdmin(
                  `/admin/stations/${encodeURIComponent(adminStationId)}/background`,
                  { busyCount, releaseAfterSeconds },
                )
              "
            >
              应用背景占用<Icon name="arrow" :size="17" />
            </button>
            <p class="tiny-note">
              只控制模拟背景占用，不覆盖用户预约和充电分配。
            </p>
          </section>
          <section v-show="adminSection === 'strategy'" class="card weights-card">
            <div class="section-top">
              <div>
                <div class="eyebrow">RANKING & INCENTIVES</div>
                <h2>推荐策略与奖励</h2>
              </div>
              <span
                class="tiny-tag"
                :class="{ invalid: Math.abs(weightSum - 1) > 0.00001 }"
                >权重总和 {{ number(weightSum, 2) }}</span
              >
            </div>
            <div class="weights-grid">
              <label v-for="(label, key) in scoreNames" :key="key"
                ><span>{{ label }}</span
                ><input
                  v-model.number="configForm.weights[key]"
                  type="number"
                  min="0"
                  max="1"
                  step="0.05"
              /></label>
            </div>
            <div class="two-fields">
              <label
                >首选奖励积分<input
                  v-model.number="configForm.rewards.first"
                  type="number"
                  min="0"
                  step="10" /></label
              ><label
                >次选奖励积分<input
                  v-model.number="configForm.rewards.second"
                  type="number"
                  min="0"
                  step="10" /></label
              ><label
                >最低补电量 / kWh<input
                  v-model.number="configForm.minimumEnergyKwh"
                  type="number"
                  min="0"
                  step="1" /></label
              ><label
                >最低充电时长 / 秒<input
                  v-model.number="configForm.minimumChargingSeconds"
                  type="number"
                  min="0"
                  step="60" /></label
              ><label
                >每日积分预算<input
                  v-model.number="configForm.dailyRewardBudget"
                  type="number"
                  min="0"
                  step="100" /></label
              ><label
                >叫号确认期限 / 秒<input
                  v-model.number="configForm.callTimeoutSeconds"
                  type="number"
                  min="1" /></label
              ><label
                >预约有效期 / 秒<input
                  v-model.number="configForm.reservationTimeoutSeconds"
                  type="number"
                  min="1"
              /></label>
            </div>
            <button
              class="button primary"
              :disabled="!!busyAction || Math.abs(weightSum - 1) > 0.00001"
              @click="saveConfig"
            >
              保存策略配置<Icon name="check" :size="17" />
            </button>
            <p class="tiny-note">
              六项权重之和须为 1。奖励仅按服务端保存的推荐资格结算。
            </p>
          </section>
          <div v-show="adminSection === 'experiments'" class="admin-side-stack">
            <section class="card experiment-run-card">
              <span class="model-icon peach"
                ><Icon name="lab" :size="25"
              /></span>
              <h2>运行配对实验</h2>
              <p class="subtle">
                用同一批请求比较最近站与智能推荐，指标来自实际模拟计算。
              </p>
              <div class="two-fields">
                <label
                  >模拟用户数<input
                    v-model.number="experimentUsers"
                    type="number"
                    min="1"
                    max="10000"
                    step="100" /></label
                ><label
                  >随机种子<input
                    v-model.number="experimentSeed"
                    type="number"
                    min="0"
                /></label>
              </div>
              <button
                class="button primary full"
                :disabled="!!busyAction"
                @click="runExperiment"
              >
                <span v-if="busyAction === 'experiment'" class="spinner"></span
                ><Icon v-else name="play" :size="17" />{{
                  busyAction === "experiment" ? "实验计算中…" : "运行并查看结果"
                }}
              </button>
            </section>
            <section class="card admin-counts">
              <h3>业务场景概况</h3>
              <div v-for="(value, key) in admin.counts || {}" :key="key">
                <span>{{ key }}</span
                ><b>{{ value }}</b>
              </div>
              <p class="tiny-note">
                当前模拟业务库统计，与只读分析批次相互独立。
              </p>
            </section>
          </div>
          <section v-show="adminSection === 'scenario'" class="card admin-stations">
            <div class="section-top">
              <h2>电站资源状态</h2>
              <button
                class="button secondary small"
                :disabled="!!busyAction"
                @click="exportFeedback"
              >
                <Icon name="chart" :size="15" />{{
                  busyAction === "feedback" ? "正在导出…" : "导出行程反馈"
                }}
              </button>
            </div>
            <div class="table-scroll">
              <table class="data-table">
                <thead>
                  <tr>
                    <th>电站</th>
                    <th>总桩数</th>
                    <th>空闲</th>
                    <th>排队</th>
                    <th>已承诺</th>
                    <th>充电中</th>
                    <th>背景占用</th>
                  </tr>
                </thead>
                <tbody>
                  <tr
                    v-for="station in admin.stations || stations"
                    :key="station.stationId"
                  >
                    <td>{{ station.stationName }}</td>
                    <td>{{ station.capacity }}</td>
                    <td class="teal">{{ station.currentFree }}</td>
                    <td>{{ station.queued }}</td>
                    <td>{{ station.committed }}</td>
                    <td>{{ station.charging }}</td>
                    <td>{{ station.backgroundBusy }}</td>
                  </tr>
                </tbody>
              </table>
            </div>
          </section>
        </div>
        <section v-else class="admin-locked">
          <Icon name="shield" :size="43" />
          <h2>先连接，再调整场景</h2>
          <p>
            控制台需要独立的管理员令牌。<br />无需令牌也可使用智能推荐和个人充电行程。
          </p>
          <div>
            <span><Icon name="clock" :size="18" />模拟时钟</span
            ><span><Icon name="users" :size="18" />排队场景</span
            ><span><Icon name="gift" :size="18" />激励策略</span
            ><span><Icon name="chart" :size="18" />配对实验</span>
          </div>
        </section>
      </template>
    </main>
    <footer class="footer">
      <div>
        <span class="footer-brand"
          ><Icon name="bolt" :size="16" />充能智析</span
        ><span>从运营数据，到智能决策。</span>
      </div>
      <div>
        <span>模拟运营数据</span><i></i><span>Asia/Shanghai</span><i></i
        ><span>预测不等于资源预约</span>
      </div>
    </footer>
    <Transition name="toast"
      ><div v-if="toast" class="toast-message" role="status">
        <Icon name="check" :size="18" />{{ toast }}
      </div></Transition
    >
  </div>
</template>

<style scoped>
.experiment-card {
  scroll-margin-top: 24px;
}
.experiment-card:focus {
  outline: 2px solid #2563eb;
  outline-offset: 4px;
}
</style>
