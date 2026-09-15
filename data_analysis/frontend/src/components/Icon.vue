<script setup lang="ts">
withDefaults(defineProps<{ name: string; size?: number }>(), { size: 20 });
const paths: Record<string, string> = {
  bolt: "m13 2-8 12h6l-1 8 9-12h-6l1-8Z",
  map: "m9 3-6 3v15l6-3 6 3 6-3V3l-6 3-6-3Z M9 3v15 M15 6v15",
  compass: "m16 8-3 5-5 3 3-5 5-3Z M22 12a10 10 0 1 1-20 0 10 10 0 0 1 20 0",
  arrow: "M4 12h16 m-6-6 6 6-6 6",
  clock: "M12 8v4l3 2 M22 12a10 10 0 1 1-20 0 10 10 0 0 1 20 0",
  pin: "M20 10c0 6-8 12-8 12S4 16 4 10a8 8 0 1 1 16 0Z M15 10a3 3 0 1 1-6 0 3 3 0 0 1 6 0",
  chart: "M4 3v17h17 M8 15v-4 M13 15V6 M18 15V9",
  trip: "M5 6h9a4 4 0 0 1 0 8H9a4 4 0 0 0 0 8 M8 6a3 3 0 1 1-6 0 3 3 0 0 1 6 0 M22 18a3 3 0 1 1-6 0 3 3 0 0 1 6 0",
  sliders: "M4 7h9 M17 7h3 M4 17h3 M11 17h9 M13 4v6 M7 14v6",
  check: "m5 12 4 4L19 6",
  chevron: "m9 5 7 7-7 7",
  down: "m6 9 6 6 6-6",
  refresh: "M20 8a8 8 0 1 0 0 8 M20 3v5h-5",
  user: "M16 7a4 4 0 1 1-8 0 4 4 0 0 1 8 0 M4 22v-3a8 8 0 0 1 16 0v3",
  location:
    "M12 2v3 M12 19v3 M2 12h3 M19 12h3 M19 12a7 7 0 1 1-14 0 7 7 0 0 1 14 0 M14 12a2 2 0 1 1-4 0 2 2 0 0 1 4 0",
  gift: "M3 9h18v4H3z M5 13v8h14v-8 M12 9v12 M12 9H8a3 3 0 1 1 3-3l1 3Z M12 9h4a3 3 0 1 0-3-3l-1 3Z",
  shield: "m12 2 8 4v6c0 6-8 10-8 10s-8-4-8-10V6l8-4Z m-4 10 3 3 5-6",
  info: "M12 11v6 M12 7v.1 M22 12a10 10 0 1 1-20 0 10 10 0 0 1 20 0",
  close: "m6 6 12 12 M6 18 18 6",
  leaf: "M20 3C9 2 2 7 5 15c3 8 14 4 15-12Z M6 17l9-9",
  play: "m8 4 12 8-12 8V4Z",
  pause: "M8 4v16 M16 4v16",
  card: "M3 5h18v14H3z M3 9h18 M7 15h3",
  users:
    "M15 7a3 3 0 1 1-6 0 3 3 0 0 1 6 0 M5 21v-2a7 7 0 0 1 14 0v2 M19 4a3 3 0 0 1 0 6 M21 14a5 5 0 0 1 2 5",
  signal: "M3 20v-3 M8 20v-7 M13 20V9 M18 20V4",
  star: "m12 2 3 6 7 1-5 5 1 7-6-3-6 3 1-7-5-5 7-1 3-6Z",
  exit: "M9 3H3v18h6 M9 12h12 m-5-5 5 5-5 5",
  lab: "M9 3h6 M10 3v6L4 20h16L14 9V3 M7 15h10",
  road: "m6 3-3 18 M18 3l3 18 M12 3v4 M12 11v3 M12 18v3",
};
</script>
<template>
  <svg
    :width="size"
    :height="size"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="1.7"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
  >
    <path :d="paths[name] || paths.info" />
  </svg>
</template>
