import { describe, expect, it } from 'vitest';
import { h } from 'vue';
import { renderToString } from 'vue/server-renderer';
import AutoHideHeader from './components/AutoHideHeader.vue';

const navigation = () => h('nav', { 'aria-label': '主导航' }, [h('button', '运营总览')]);

describe('presentation navigation', () => {
  it('keeps the ordinary header visible without a reveal control', async () => {
    const html = await renderToString(h(AutoHideHeader, {}, navigation));
    expect(html).toContain('navigation-shell--expanded');
    expect(html).not.toContain('navigation-shell--auto');
    expect(html).not.toContain('inert');
    expect(html).not.toContain('aria-label="显示主导航"');
    expect(html).toContain('运营总览');
  });

  it('starts presentation with a hidden, inert header and an accessible reveal button', async () => {
    const html = await renderToString(h(AutoHideHeader, { autoHide: true }, navigation));
    expect(html).toContain('navigation-shell--auto');
    expect(html).not.toContain('navigation-shell--expanded');
    expect(html).toContain('inert aria-hidden="true"');
    expect(html).toContain('aria-label="显示主导航" aria-controls="primary-header" aria-expanded="false"');
    expect(html).toContain('aria-label="收起主导航"');
    expect(html).toContain('id="primary-header"');
  });
});
