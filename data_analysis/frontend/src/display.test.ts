import { describe, expect, it } from "vitest";
import { finished, localTime, number, percent, probability } from "./display";

describe("public display semantics", () => {
  it("does not turn missing or invalid measurements into observed zeros", () => {
    for (const value of [undefined, null, NaN, Infinity, "0"]) {
      expect(number(value)).toBe("—");
      expect(percent(value)).toBe("—");
    }
    expect(number(0)).toBe("0");
    expect(percent(0)).toBe("0%");
  });
  it("always displays wire UTC timestamps in Shanghai business time", () => {
    expect(localTime("2026-05-05T00:00:00Z")).toBe("08:00");
    expect(localTime("invalid")).toBe("—");
  });
  it("keeps pending payment active and only closes terminal statuses", () => {
    expect(finished("PENDING_PAYMENT")).toBe(false);
    for (const status of ["COMPLETED", "EXPIRED", "CANCELLED"])
      expect(finished(status)).toBe(true);
  });
  it("does not round uncertain near-one availability to a guarantee", () => {
    expect(probability(0.997)).toBe("99.7%");
    expect(probability(0.99999)).toBe(">99.9%");
  });
});
