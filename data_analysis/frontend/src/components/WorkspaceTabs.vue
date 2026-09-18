<script setup lang="ts">
defineProps<{ modelValue: string; label: string; items: { id: string; label: string; description: string }[] }>();
defineEmits<{ 'update:modelValue': [value: string] }>();
</script>
<template>
  <nav class="workspace-tabs" :aria-label="label">
    <button v-for="item in items" :key="item.id" type="button"
      :class="{ selected: modelValue === item.id }" :aria-pressed="modelValue === item.id"
      @click="$emit('update:modelValue', item.id)">
      <strong>{{ item.label }}</strong><span>{{ item.description }}</span>
    </button>
  </nav>
</template>
