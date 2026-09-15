<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, watch } from 'vue';
import L from 'leaflet';
import type { RecordData } from '../analytics';
import { formatValue, converted } from '../analytics';
const props = defineProps<{ stations: RecordData[] }>();
const element = ref<HTMLElement>();
let map: L.Map | undefined;
let markers: L.LayerGroup | undefined;
let observer: ResizeObserver | undefined;
function update() {
  if (!map || !markers) return;
  markers.clearLayers();
  const points: L.LatLngTuple[] = [];
  for (const station of props.stations) {
    if (!Number.isFinite(station.latitude) || !Number.isFinite(station.longitude)) continue;
    const point: L.LatLngTuple = [station.latitude, station.longitude]; points.push(point);
    const content = document.createElement('div'); content.className = 'analytics-map-popup';
    const title = document.createElement('strong'); title.textContent = station.stationName;
    const detail = document.createElement('p'); detail.textContent = `${station.cityName} · ${station.capacity} 桩 · 期间 ${formatValue(converted(station.periodMetrics?.energyWh, 1000), 1)} kWh`;
    content.append(title, detail);
    L.circleMarker(point, { radius: 7, color: '#fff', weight: 2, fillColor: '#2563eb', fillOpacity: .95 }).bindPopup(content).addTo(markers);
  }
  if (points.length) map.fitBounds(L.latLngBounds(points), { padding: [28, 28], maxZoom: 12, animate: false });
}
onMounted(() => {
  if (!element.value) return;
  map = L.map(element.value, { scrollWheelZoom: false, zoomAnimation: false, fadeAnimation: false, markerZoomAnimation: false }).setView([35, 116], 4);
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', { maxZoom: 18, attribution: '&copy; OpenStreetMap contributors' }).addTo(map);
  markers = L.layerGroup().addTo(map); update();
  observer = new ResizeObserver(() => map?.invalidateSize()); observer.observe(element.value);
});
watch(() => props.stations, update);
onBeforeUnmount(() => { observer?.disconnect(); map?.stop(); map?.remove(); map = undefined; });
</script>
<template><div class="analytics-map-wrap"><div ref="element" class="analytics-map" role="img" aria-label="当前统计范围电站地理分布，可点击查看电站信息"></div><span class="analytics-map-note">模拟电站位置 · 底图需联网</span></div></template>
