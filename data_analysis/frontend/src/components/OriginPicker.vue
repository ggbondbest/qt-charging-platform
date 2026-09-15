<script setup lang="ts">
import Icon from './Icon.vue';
import { number } from '../display';
import type { Location } from '../types';
defineProps<{ origin?: Location; picking: boolean; disabled?: boolean }>();
defineEmits<{ toggle: [] }>();
</script>

<template>
  <div class="origin-field origin-picker">
    <Icon name="location" :size="20" />
    <div class="origin-picker__content">
      <span class="origin-picker__label">我的出发点</span>
      <div class="origin-picker__row">
        <span class="origin-picker__coordinates">{{ origin ? `${number(origin.latitude, 4)}° N，${number(origin.longitude, 4)}° E` : '请先选择城市' }}</span>
        <button type="button" class="button secondary" :disabled="disabled" :aria-pressed="picking" aria-describedby="origin-picker-hint" @click="$emit('toggle')">
          <Icon :name="picking ? 'close' : 'pin'" :size="15" />{{ picking ? '取消设置' : '设置出发点' }}
        </button>
      </div>
      <p id="origin-picker-hint" class="origin-picker__hint" :class="{ 'is-picking': picking }" role="status">{{ picking ? '请在地图中点击新位置，红色标记将更新为出发点。' : '点击“设置出发点”，然后在地图中点击。' }}</p>
    </div>
  </div>
</template>

<style scoped>
.origin-picker { flex-basis:100%; min-width:0; display:flex; align-items:flex-start; gap:12px; }
.origin-picker>svg { flex-shrink:0; margin-top:23px; color:#6b7280; }
.origin-picker>.origin-picker__content { flex:1; min-width:0; }
.origin-picker__label { font-size:11px; color:#6b7280; }
.origin-picker__row { display:flex; flex-wrap:wrap; align-items:center; gap:8px 16px; margin-top:7px; }
.origin-picker__coordinates { font-size:13px; font-variant-numeric:tabular-nums; color:#171a20; }
.origin-picker__row>.button { min-height:34px; padding:7px 11px; font-size:12px; gap:6px; }
.origin-picker__row>.button[aria-pressed=true] { border-color:#2563eb; color:#2563eb; background:#eff4ff; }
.origin-picker__hint { margin:8px 0 0; font-size:11px; line-height:1.6; color:#6b7280; }
.origin-picker__hint.is-picking { color:#2563eb; }
</style>
