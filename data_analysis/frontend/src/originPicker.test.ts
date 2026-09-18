import { describe, expect, it } from 'vitest';
import { h } from 'vue';
import { renderToString } from 'vue/server-renderer';
import OriginPicker from './components/OriginPicker.vue';

describe('map origin setting', () => {
  it('pairs read-only coordinates with one clearly named map-setting button', async () => {
    const html = await renderToString(h(OriginPicker, { origin: { latitude:38.914, longitude:121.6147 }, picking:false }));
    expect(html).toContain('38.9140° N，121.6147° E');
    expect(html.match(/<button/g)).toHaveLength(1);
    expect(html).toContain('设置出发点');
    expect(html).toContain('然后在地图中点击');
    expect(html).toContain('aria-describedby="origin-picker-hint"');
    expect(html).toContain('aria-pressed="false"');
  });
  it('exposes the active picking state with a cancellation action and updated instruction', async () => {
    const html = await renderToString(h(OriginPicker, { picking:true }));
    expect(html).toContain('取消设置');
    expect(html).toContain('aria-pressed="true"');
    expect(html).toContain('请在地图中点击新位置');
  });
  it('disables origin setting before a city is available', async () => {
    const html = await renderToString(h(OriginPicker, { picking:false, disabled:true }));
    expect(html).toContain('请先选择城市');
    expect(html).toMatch(/<button[^>]+disabled/);
  });
});
