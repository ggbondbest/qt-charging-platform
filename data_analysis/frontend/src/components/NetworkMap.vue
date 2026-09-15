<script setup>
const nodes = [
  { name:'北京', x:67, y:25, energy:'12.7 MWh', level:3 }, { name:'大连', x:78, y:36, energy:'8.9 MWh', level:2 },
  { name:'沈阳', x:82, y:20, energy:'9.8 MWh', level:2 }, { name:'上海', x:74, y:61, energy:'14.9 MWh', level:3 },
  { name:'深圳', x:57, y:82, energy:'15.7 MWh', level:3 }
]
</script>
<template><div class="network-map"><div class="map-grid"></div>
  <svg viewBox="0 0 100 100" preserveAspectRatio="xMidYMid meet" aria-label="五城市充电网络空间示意图">
    <defs><linearGradient id="land" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#0b4260"/><stop offset="1" stop-color="#071b35"/></linearGradient><filter id="glow"><feGaussianBlur stdDeviation="1.2" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>
    <path class="land-shadow" d="M28 8 44 10 51 17 65 12 83 19 91 32 83 42 86 54 77 65 72 80 60 88 49 83 42 91 34 82 25 75 29 62 20 52 27 43 20 31Z"/>
    <path class="land" d="M28 5 44 7 51 14 65 9 83 16 91 29 83 39 86 51 77 62 72 77 60 85 49 80 42 88 34 79 25 72 29 59 20 49 27 40 20 28Z"/>
    <path class="contour" d="M29 18 Q46 27 68 17M25 39 Q49 48 84 34M28 58 Q52 66 81 53M36 76 Q54 73 70 69"/>
    <g class="routes"><path d="M67 25 Q75 20 82 20"/><path d="M67 25 Q76 28 78 36"/><path d="M67 25 Q79 42 74 61"/><path d="M74 61 Q66 70 57 82"/></g>
    <g v-for="node in nodes" :key="node.name" class="node" :transform="`translate(${node.x} ${node.y})`"><circle class="sonar s1" r="4"/><circle class="sonar s2" r="6"/><circle class="point" :r="node.level" filter="url(#glow)"/><text x="5" y="-1">{{node.name}}</text><text class="value" x="5" y="4">{{node.energy}}</text></g>
  </svg>
  <div class="map-corner tl"></div><div class="map-corner tr"></div><div class="map-corner bl"></div><div class="map-corner br"></div>
  <div class="map-caption"><span>●</span> 五城市 · 25 站 · 75 桩 <em>空间坐标示意</em></div>
</div></template>
