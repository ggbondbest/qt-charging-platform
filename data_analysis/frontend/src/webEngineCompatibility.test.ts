import { describe, expect, it } from 'vitest';
import { advisorTarget } from './advisor';

describe('Qt 6.2 WebEngine navigation', () => {
  it('validates historical advisor destinations without Object.hasOwn (Chromium 93+)', () => {
    const original = Object.hasOwn;
    try {
      Object.defineProperty(Object, 'hasOwn', { configurable: true, writable: true, value: undefined });
      expect(advisorTarget('overview')).toBe(true);
      expect(advisorTarget('anomalies')).toBe(true);
      expect(advisorTarget('constructor')).toBe(false);
      expect(advisorTarget('__proto__')).toBe(false);
      expect(advisorTarget(null)).toBe(false);
    } finally {
      Object.defineProperty(Object, 'hasOwn', { configurable: true, writable: true, value: original });
    }
  });
});
