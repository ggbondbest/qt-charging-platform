<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import L from "leaflet";
import "leaflet/dist/leaflet.css";
import type { Candidate, City, Location, Station } from "../types";
import Icon from "./Icon.vue";
const props = defineProps<{
  city?: City;
  stations: Station[];
  candidates: Candidate[];
  origin?: Location;
  highlighted: string;
  picking: boolean;
  routeCoordinates?: [number, number][];
}>();
const emit = defineEmits<{ origin: [Location]; station: [string] }>();
const mapElement = ref<HTMLElement>();
const tilesReady = ref(false);
const tileFailed = ref(false);
let map: L.Map | undefined;
let markers: L.LayerGroup | undefined;
let tileTimer: number;
let observer: ResizeObserver | undefined;
const fallback = computed(() => !tilesReady.value || tileFailed.value);
const center = computed(
  () => props.city || { latitude: 38.914, longitude: 121.6147 },
);
const extent = computed(() => {
  const points = [...props.stations, ...(props.origin ? [props.origin] : [])];
  const lats = points.map((s) => s.latitude);
  const lngs = points.map((s) => s.longitude);
  return {
    minLat: Math.min(center.value.latitude - 0.09, ...lats) - 0.025,
    maxLat: Math.max(center.value.latitude + 0.09, ...lats) + 0.025,
    minLng: Math.min(center.value.longitude - 0.12, ...lngs) - 0.045,
    maxLng: Math.max(center.value.longitude + 0.12, ...lngs) + 0.045,
  };
});
const xy = (p: Location) => ({
  x:
    40 +
    ((p.longitude - extent.value.minLng) /
      (extent.value.maxLng - extent.value.minLng)) *
      720,
  y:
    570 -
    ((p.latitude - extent.value.minLat) /
      (extent.value.maxLat - extent.value.minLat)) *
      500,
});
const rank = (id: string) =>
  props.candidates.find((c) => c.stationId === id)?.rank;
function pick(event: MouseEvent) {
  if (!props.picking) return;
  const box = (event.currentTarget as Element).getBoundingClientRect();
  const x = ((event.clientX - box.left) / box.width) * 800;
  const y = ((event.clientY - box.top) / box.height) * 620;
  emit("origin", {
    latitude:
      extent.value.minLat +
      ((570 - y) / 500) * (extent.value.maxLat - extent.value.minLat),
    longitude:
      extent.value.minLng +
      ((x - 40) / 720) * (extent.value.maxLng - extent.value.minLng),
  });
}
function drawMarkers() {
  if (!map || !markers) return;
  markers.clearLayers();
  if (props.routeCoordinates?.length)
    markers.addLayer(
      L.polyline(props.routeCoordinates, {
        color: "#267659",
        weight: 4,
        opacity: 0.8,
      }),
    );
  for (const s of props.stations) {
    const r = rank(s.stationId);
    const active = s.stationId === props.highlighted;
    const marker = L.marker([s.latitude, s.longitude], {
      icon: L.divIcon({
        className: "station-leaflet-icon",
        html: `<span class="leaflet-station ${r === 1 ? "best" : ""} ${active ? "selected" : ""}"><b>${r || "ϟ"}</b></span>`,
        iconSize: [40, 46],
        iconAnchor: [20, 42],
      }),
    });
    const tooltip = document.createElement("span");
    tooltip.textContent = s.stationName;
    marker.bindTooltip(tooltip, { direction: "top", offset: [0, -34] });
    marker.on("click", () => emit("station", s.stationId));
    markers.addLayer(marker);
  }
  if (props.origin)
    markers.addLayer(
      L.marker([props.origin.latitude, props.origin.longitude], {
        icon: L.divIcon({
          className: "origin-leaflet-icon",
          html: '<span class="leaflet-origin"></span>',
          iconSize: [24, 24],
          iconAnchor: [12, 12],
        }),
      }).bindTooltip("我的出发点"),
    );
}
function fit() {
  if (!map) return;
  if (props.stations.length)
    map.fitBounds(
      L.latLngBounds(
        [...props.stations, ...(props.origin ? [props.origin] : [])].map(
          (s) => [s.latitude, s.longitude] as [number, number],
        ),
      ),
      { padding: [80, 80], maxZoom: 13 },
    );
  else map.setView([center.value.latitude, center.value.longitude], 11);
}
function restore() {
  tileFailed.value = false;
  tilesReady.value = false;
  map?.eachLayer((layer) => {
    if (layer instanceof L.TileLayer) layer.redraw();
  });
  tileTimer = window.setTimeout(() => {
    if (!tilesReady.value) tileFailed.value = true;
  }, 7000);
}
onMounted(() => {
  if (!mapElement.value) return;
  map = L.map(mapElement.value, {
    zoomControl: false,
    attributionControl: true,
  }).setView([center.value.latitude, center.value.longitude], 11);
  const tile = L.tileLayer(
    "https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
    {
      attribution:
        '&copy; <a href="https://www.openstreetmap.org/copyright" target="_blank" rel="noopener noreferrer">OpenStreetMap</a> contributors',
      maxZoom: 18,
      crossOrigin: true,
    },
  );
  let loaded = 0;
  tile.on("tileload", () => {
    loaded++;
    if (loaded >= 3) {
      tilesReady.value = true;
      tileFailed.value = false;
    }
  });
  tile.addTo(map);
  tileTimer = window.setTimeout(() => {
    if (!tilesReady.value) tileFailed.value = true;
  }, 7000);
  markers = L.layerGroup().addTo(map);
  map.on("click", (event) => {
    if (props.picking)
      emit("origin", {
        latitude: event.latlng.lat,
        longitude: event.latlng.lng,
      });
  });
  observer = new ResizeObserver(() => {
    map?.invalidateSize();
  });
  observer.observe(mapElement.value);
  drawMarkers();
  fit();
});
watch(
  () => [
    props.stations,
    props.candidates,
    props.highlighted,
    props.origin,
    props.routeCoordinates,
  ],
  drawMarkers,
  { deep: true },
);
watch(
  () => props.city?.cityId,
  () => {
    drawMarkers();
    fit();
  },
);
onBeforeUnmount(() => {
  window.clearTimeout(tileTimer);
  observer?.disconnect();
  map?.remove();
});
</script>
<template>
  <div class="station-map" :class="{ picking }">
    <div
      ref="mapElement"
      class="leaflet-surface"
      :class="{ hidden: fallback }"
    ></div>
    <svg
      v-if="fallback"
      class="geographic-fallback"
      viewBox="0 0 800 620"
      preserveAspectRatio="none"
      @click="pick"
      role="img"
      aria-label="电站地理位置示意；点击站点选中，点击地图可设置出发点"
    >
      <defs>
        <pattern
          id="minor-grid"
          width="40"
          height="40"
          patternUnits="userSpaceOnUse"
        >
          <path d="M40 0H0V40" fill="none" stroke="#d5dfd5" stroke-width=".7" />
        </pattern>
        <radialGradient id="map-halo">
          <stop offset="0" stop-color="#c9dec8" stop-opacity=".65" />
          <stop offset="1" stop-color="#e9eee5" stop-opacity="0" />
        </radialGradient>
        <filter id="pin-shadow">
          <feDropShadow dx="0" dy="4" stdDeviation="4" flood-opacity=".14" />
        </filter>
      </defs>
      <rect width="800" height="620" fill="#eaf0e6" />
      <rect width="800" height="620" fill="url(#minor-grid)" />
      <ellipse cx="400" cy="320" rx="360" ry="280" fill="url(#map-halo)" />
      <g fill="none" stroke="#c7d6c7">
        <circle cx="400" cy="320" r="125" />
        <circle cx="400" cy="320" r="240" stroke-dasharray="4 6" />
        <path d="M0 320h800M400 0v620" stroke-dasharray="5 7" />
      </g>
      <text x="45" y="565" font-size="11" fill="#82968a" letter-spacing="3">
        {{ city?.cityName || "CHARGEPILOT" }} · GEOGRAPHIC OVERVIEW
      </text>
      <g v-if="origin && !routeCoordinates?.length">
        <line
          v-for="s in stations"
          :key="'line' + s.stationId"
          :x1="xy(origin).x"
          :y1="xy(origin).y"
          :x2="xy(s).x"
          :y2="xy(s).y"
          :stroke="s.stationId === highlighted ? '#db965b' : '#acbdb0'"
          :stroke-width="s.stationId === highlighted ? 2 : 1"
          stroke-dasharray="5 7"
        />
      </g>
      <polyline
        v-if="routeCoordinates?.length"
        :points="
          routeCoordinates
            .map(([latitude, longitude]) => {
              const p = xy({ latitude, longitude });
              return `${p.x},${p.y}`;
            })
            .join(' ')
        "
        fill="none"
        stroke="#267659"
        stroke-width="4"
        stroke-linecap="round"
        stroke-linejoin="round"
        opacity=".8"
      />
      <g
        v-for="s in stations"
        :key="s.stationId"
        :transform="`translate(${xy(s).x},${xy(s).y})`"
        class="svg-station"
        role="button"
        tabindex="0"
        :aria-label="s.stationName"
        @click.stop="emit('station', s.stationId)"
        @keydown.enter="emit('station', s.stationId)"
      >
        <circle
          v-if="s.stationId === highlighted"
          r="36"
          fill="#166b58"
          opacity=".12"
        />
        <path
          d="M0 7C-20-8-23-16-23-24a23 23 0 1 1 46 0C23-16 20-8 0 7Z"
          :fill="
            rank(s.stationId) === 1 || s.stationId === highlighted
              ? '#17685c'
              : '#fff'
          "
          :stroke="
            rank(s.stationId) === 1 || s.stationId === highlighted
              ? '#17685c'
              : '#cad5ca'
          "
          filter="url(#pin-shadow)"
        />
        <text
          text-anchor="middle"
          y="-16"
          :fill="
            rank(s.stationId) === 1 || s.stationId === highlighted
              ? '#e2f5c4'
              : '#17685c'
          "
          font-size="21"
          font-weight="700"
        >
          {{ rank(s.stationId) || "ϟ" }}
        </text>
        <rect
          x="-52"
          y="14"
          width="104"
          height="24"
          rx="7"
          fill="white"
          fill-opacity=".93"
        />
        <text text-anchor="middle" y="30" fill="#34483f" font-size="11">
          {{ s.stationName.replace(/模拟充电站|示范充电站|市/g, "").slice(-8) }}
        </text>
      </g>
      <g
        v-if="origin"
        :transform="`translate(${xy(origin).x},${xy(origin).y})`"
      >
        <circle r="23" fill="#538ce8" opacity=".15" />
        <circle r="10" fill="#4985d6" stroke="white" stroke-width="4" />
        <rect x="-37" y="18" width="74" height="24" rx="7" fill="white" />
        <text y="34" text-anchor="middle" fill="#426181" font-size="11">
          我的出发点
        </text>
      </g>
    </svg>
    <div class="map-top-badges">
      <span class="map-badge"
        ><span class="live-dot"></span>{{ city?.cityName || "选择城市" }} ·
        {{ stations.length }} 座模拟电站</span
      ><span v-if="picking" class="map-badge picking-badge"
        >点击地图设置出发点</span
      >
    </div>
    <div class="map-compass">
      <span>N</span><Icon name="compass" :size="27" />
    </div>
    <div class="map-bottom">
      <div class="map-source">
        <Icon name="info" :size="14" /><span>{{
          fallback
            ? "地理位置示意 · 虚线为直线距离，非道路导航"
            : "OSM 地图 · 电站为模拟位置"
        }}</span
        ><button v-if="tileFailed" @click="restore">重试底图</button>
      </div>
      <div v-if="!fallback" class="map-zoom">
        <button aria-label="放大地图" @click="map?.zoomIn()">+</button
        ><button aria-label="缩小地图" @click="map?.zoomOut()">−</button>
      </div>
    </div>
  </div>
</template>
