import { describe, expect, it } from 'vitest';
import { h } from 'vue';
import { renderToString } from 'vue/server-renderer';
import WorkspaceTabs from './components/WorkspaceTabs.vue';

const items = [
  { id: 'forecast', label: '负荷与空闲预测', description: '未来 1 / 6 / 24 小时' },
  { id: 'experiments', label: '策略对比', description: '最近站 vs. 智能推荐' },
];
describe('workspace section navigation', () => {
  it('exposes one selected section and named navigation without hiding descriptions', async () => {
    const html = await renderToString(h(WorkspaceTabs, { modelValue: 'experiments', items, label: '智能分析分区' }));
    expect(html).toContain('aria-label="智能分析分区"');
    expect(html.match(/aria-pressed="true"/g)).toHaveLength(1);
    expect(html).toMatch(/aria-pressed="true"[^>]*><strong>策略对比/);
    expect(html).toContain('最近站 vs. 智能推荐');
    expect(html.match(/type="button"/g)).toHaveLength(2);
  });
});
