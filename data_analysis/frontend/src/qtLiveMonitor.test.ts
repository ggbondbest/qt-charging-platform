import { afterEach, describe, expect, it, vi } from 'vitest';
import { createQtLiveMonitor, QT_LIVE_POLL_MS } from './qtLiveMonitor';
import { qtLiveEnvelope } from './test/qtLiveFixture';

afterEach(() => vi.useRealTimers());
describe('Qt live polling lifecycle', () => {
  it('preserves the last successful snapshot and timestamp on outage, then recovers', async () => {
    vi.useFakeTimers();
    const original = qtLiveEnvelope().data;
    const newer = { ...original, asOf: '2026-10-17T04:01:00Z' };
    const load = vi.fn().mockResolvedValueOnce(original).mockRejectedValueOnce(new Error('Qt 服务已断开')).mockResolvedValueOnce(newer);
    const monitor = createQtLiveMonitor(load);
    monitor.setActive(true);
    await vi.advanceTimersByTimeAsync(0);
    expect(monitor.snapshot.value).toEqual(original);
    await vi.advanceTimersByTimeAsync(QT_LIVE_POLL_MS);
    expect(monitor.error.value).toBe('Qt 服务已断开');
    expect(monitor.snapshot.value?.asOf).toBe(original.asOf);
    expect(monitor.snapshot.value?.summary.paidAmountCents).toBe(1234);
    await vi.advanceTimersByTimeAsync(QT_LIVE_POLL_MS);
    expect(monitor.snapshot.value?.asOf).toBe(newer.asOf);
    expect(monitor.error.value).toBe('');
    monitor.dispose();
    expect(vi.getTimerCount()).toBe(0);
  });
  it('pauses when hidden or on another page, cancels inflight work, and ignores a late old response', async () => {
    vi.useFakeTimers();
    let resolveOld!: (value: ReturnType<typeof qtLiveEnvelope>['data']) => void;
    const fresh = { ...qtLiveEnvelope().data, asOf: '2026-10-17T04:02:00Z' };
    const load = vi.fn().mockImplementationOnce(() => new Promise(resolve => { resolveOld = resolve; })).mockResolvedValue(fresh);
    const monitor = createQtLiveMonitor(load);
    monitor.setActive(true);
    const signal = load.mock.calls[0]![0] as AbortSignal;
    await monitor.refresh();
    expect(load).toHaveBeenCalledTimes(1);
    monitor.setActive(false);
    expect(signal.aborted).toBe(true);
    await vi.advanceTimersByTimeAsync(QT_LIVE_POLL_MS * 3);
    expect(load).toHaveBeenCalledTimes(1);
    monitor.setActive(true);
    await vi.advanceTimersByTimeAsync(0);
    resolveOld(qtLiveEnvelope().data);
    await vi.advanceTimersByTimeAsync(0);
    expect(monitor.snapshot.value?.asOf).toBe(fresh.asOf);
    monitor.dispose();
    await vi.advanceTimersByTimeAsync(QT_LIVE_POLL_MS * 3);
    expect(load).toHaveBeenCalledTimes(2);
  });
  it('does not invent an initial snapshot when the service is unavailable', async () => {
    vi.useFakeTimers();
    const monitor = createQtLiveMonitor(vi.fn().mockRejectedValue(new Error('未配置数据库')));
    monitor.setActive(true);
    await vi.advanceTimersByTimeAsync(0);
    expect(monitor.snapshot.value).toBeUndefined();
    expect(monitor.error.value).toBe('未配置数据库');
    monitor.dispose();
  });
});
