import type { JsonObject, TripStatus } from "./types";
export const number = (value: unknown, digits = 0) =>
  typeof value === "number" && Number.isFinite(value)
    ? value.toLocaleString("zh-CN", {
        maximumFractionDigits: digits,
        minimumFractionDigits: digits,
      })
    : "—";
export const percent = (value: unknown) =>
  typeof value === "number" && Number.isFinite(value)
    ? `${number(value * 100)}%`
    : "—";
export const probability = (value: unknown) =>
  typeof value === "number" && Number.isFinite(value)
    ? value < 1 && value >= 0.9995
      ? ">99.9%"
      : `${number(value * 100, 1)}%`
    : "—";
export function localTime(value: unknown, date = false) {
  if (typeof value !== "string" || !Number.isFinite(Date.parse(value)))
    return "—";
  return new Intl.DateTimeFormat("zh-CN", {
    timeZone: "Asia/Shanghai",
    ...(date ? ({ month: "2-digit", day: "2-digit" } as const) : {}),
    hour: "2-digit",
    minute: "2-digit",
    hour12: false,
  }).format(new Date(value));
}
export const statusLabels: Record<TripStatus, string> = {
  EN_ROUTE: "前往电站",
  QUEUED: "等待叫号",
  CALLED: "已叫到你",
  RESERVED: "已保留电桩",
  CHARGING: "正在充电",
  PENDING_PAYMENT: "待支付",
  COMPLETED: "订单已完成",
  CANCELLED: "行程已取消",
  EXPIRED: "行程已过期",
};
export const finished = (status: string) =>
  ["COMPLETED", "CANCELLED", "EXPIRED"].includes(status);
export const pretty = (value: unknown) => JSON.stringify(value, null, 2);
export function metricLeaves(
  value: unknown,
  prefix = "",
): { key: string; value: number }[] {
  if (!value || typeof value !== "object") return [];
  return Object.entries(value as JsonObject).flatMap(([key, item]) => {
    const path = prefix ? `${prefix}.${key}` : key;
    if (
      typeof item === "number" &&
      Number.isFinite(item) &&
      /mae|rmse|brier|auc|coverage|precision|recall|meanWait|meanTotal|serviceRate|completionRate/i.test(
        path,
      )
    )
      return [{ key: path, value: item }];
    if (item && typeof item === "object" && !Array.isArray(item))
      return metricLeaves(item, path);
    return [];
  });
}
