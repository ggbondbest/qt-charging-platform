import type { EChartsCoreOption } from "echarts/core";

const fontFamily = 'Inter, -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif';
const axis = {
  axisLine: { lineStyle: { color: "#d9dfe7", width: 1 } },
  axisTick: { show: false },
  axisLabel: { color: "#697586", fontFamily, fontSize: 11, margin: 12 },
  nameTextStyle: { color: "#697586", fontFamily, fontSize: 11 },
  splitLine: { lineStyle: { color: "#edf0f4", width: 1 } },
  splitArea: { show: false },
};

// Theme defaults only: each chart retains its own axes, units, series and formatters.
// In particular, forecasts must keep gaps rather than drawing invented observations.
export const chartTheme = {
  color: ["#2463eb", "#152438", "#8aa4cf", "#70a8c4", "#a1abba", "#c4cedc"],
  backgroundColor: "transparent",
  textStyle: { color: "#344054", fontFamily, fontSize: 12 },
  animationDuration: 650,
  animationDurationUpdate: 350,
  animationEasing: "cubicOut",
  animationEasingUpdate: "cubicOut",
  categoryAxis: axis,
  valueAxis: axis,
  timeAxis: axis,
  logAxis: axis,
  legend: {
    textStyle: { color: "#697586", fontFamily, fontSize: 11 },
    icon: "roundRect",
    itemWidth: 13,
    itemHeight: 4,
    itemGap: 18,
  },
  tooltip: {
    confine: true,
    backgroundColor: "rgba(255, 255, 255, .98)",
    borderColor: "#e4e8ee",
    borderWidth: 1,
    padding: [12, 16],
    textStyle: { color: "#1d2939", fontFamily, fontSize: 12 },
    extraCssText: "border-radius:12px;box-shadow:0 10px 32px rgba(16,24,40,.10);line-height:1.8;",
    axisPointer: {
      lineStyle: { color: "#98a6b9", type: "dashed", width: 1 },
      crossStyle: { color: "#98a6b9", width: 1 },
      shadowStyle: { color: "rgba(36,99,235,.04)" },
    },
  },
};

export function chartOptionForMotion(
  option: EChartsCoreOption,
  reducedMotion: boolean,
): EChartsCoreOption {
  if (!reducedMotion) return option;
  return {
    ...option,
    animation: false,
    animationDuration: 0,
    animationDurationUpdate: 0,
  };
}
