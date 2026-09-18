import { describe, expect, it } from "vitest";
import { chartOptionForMotion, chartTheme } from "./chartTheme";

describe("shared chart presentation", () => {
  it("uses neutral, readable defaults without defining chart business series", () => {
    expect(chartTheme.backgroundColor).toBe("transparent");
    expect(chartTheme.tooltip.confine).toBe(true);
    expect(chartTheme).not.toHaveProperty("series");
    expect(chartTheme).not.toHaveProperty("xAxis");
    expect(chartTheme).not.toHaveProperty("yAxis");
  });

  it("preserves data, missing observations, units and formatters with reduced motion", () => {
    const formatter = (value: number) => `${value} kWh`;
    const option = {
      animation: true,
      yAxis: { name: "kWh", axisLabel: { formatter } },
      tooltip: { valueFormatter: formatter },
      series: [{ type: "line", data: [0, null, 12.5], connectNulls: false }],
    };
    const rendered = chartOptionForMotion(option, true);
    expect(rendered.animation).toBe(false);
    expect(rendered.animationDuration).toBe(0);
    expect(rendered.animationDurationUpdate).toBe(0);
    expect(rendered.series).toBe(option.series);
    expect(rendered.yAxis).toBe(option.yAxis);
    expect(rendered.tooltip).toBe(option.tooltip);
    expect(option.animation).toBe(true);
  });

  it("does not override explicit business animation settings otherwise", () => {
    const option = { animation: false, animationDuration: 200, series: [] };
    expect(chartOptionForMotion(option, false)).toBe(option);
  });
});
