import { beforeEach, describe, expect, it, vi } from 'vitest';
import { h, ref, shallowRef } from 'vue';
import { renderToString } from 'vue/server-renderer';
import QtLivePanel from './components/QtLivePanel.vue';
import { createQtLiveMonitor } from './qtLiveMonitor';
import { qtLiveEnvelope } from './test/qtLiveFixture';

vi.mock('./qtLiveMonitor', () => ({ createQtLiveMonitor: vi.fn() }));
let monitor: ReturnType<typeof createQtLiveMonitor>;
beforeEach(() => {
  monitor = { snapshot: shallowRef(), loading: ref(false), error: ref(''), active: ref(true),
    refresh: vi.fn(async () => {}), setActive: vi.fn(), dispose: vi.fn() };
  vi.mocked(createQtLiveMonitor).mockReturnValue(monitor);
});
describe('Qt live visible data provenance', () => {
  it('renders unavailable metrics as missing instead of implying an empty business database', async () => {
    monitor.error.value = '业务库未配置';
    const html = await renderToString(h(QtLivePanel));
    expect(html).toContain('Qt 业务数据暂不可用');
    expect(html).toContain('尚未取得有效快照，指标不以 0 代替');
    expect(html).toContain('最后成功快照：—');
    expect(html).not.toContain('>0<small>');
  });
  it('shows the last successful timestamp and source units while marking retained data stale', async () => {
    monitor.snapshot.value = qtLiveEnvelope().data;
    monitor.error.value = '服务连接断开';
    const html = await renderToString(h(QtLivePanel));
    expect(html).toContain('下方保留最后一次成功快照');
    expect(html).toContain('最后成功快照：2026/10/17 12:00:00');
    expect(html).toContain('12.34');
    expect(html).toContain('7.890');
    expect(html).toContain('STA-CITY-BJ-001');
    expect(html).toContain('数据源：qt-sqlite');
    expect(html).toContain('支付金额与电量仅统计已完成订单');
    expect(html).toContain('不与历史批次合计');
    expect(html).toContain('今日营收');
    expect(html).toContain('本月营收');
    expect(html).toContain('100.0');
    expect(html).toContain('在线含故障');
    expect(html).toContain('经营趋势');
    expect(html).toContain('当前没有异常电桩');
  });
  it('keeps the source boundary and fullscreen entry without a duplicate embedded title', async () => {
    monitor.snapshot.value = qtLiveEnvelope().data;
    const html = await renderToString(h(QtLivePanel, { compact: true }));
    expect(html).not.toContain('<h1');
    expect(html).toContain('与历史运营分析分别统计');
    expect(html).toContain('大屏模式');
    const fullscreen = await renderToString(h(QtLivePanel, { compact: true, presentation: true }));
    expect(fullscreen).toContain('退出大屏模式');
    expect(fullscreen).toContain('Esc 返回窗口');
  });
  it('renders server-provided safe abnormal summaries as escaped text', async () => {
    const value = qtLiveEnvelope().data;
    value.overview.abnormalChargers = { total: 1, items: [{ qtChargerId: 2, qtStationId: 1,
      chargerCode: 'C002', qtStationCode: 'S001', stationName: '故障测试电站', status: 'FAULT',
      updatedAt: value.asOf, activeException: { qtExceptionId: 1, code: 'FAULT', severity: 'HIGH',
        status: 'OPEN', safeSummary: '<script>不能执行</script>', occurredAt: value.asOf } }] };
    monitor.snapshot.value = value;
    const html = await renderToString(h(QtLivePanel));
    expect(html).toContain('故障测试电站');
    expect(html).toContain('&lt;script&gt;不能执行&lt;/script&gt;');
    expect(html).not.toContain('<script>');
  });
});
