import type { Envelope } from "./types";
export class ApiError extends Error {
  constructor(
    public code: string,
    message: string,
    public status: number,
  ) {
    super(message);
  }
}
const businessErrors: Record<string, string> = {
  UNAUTHORIZED: "演示访问已失效，请重新推荐。",
  FORBIDDEN: "管理员令牌无效，请检查后重试。",
  RECOMMENDATION_EXPIRED: "这次推荐已过期，请刷新推荐后重新选择。",
  RECOMMENDATION_NOT_FOUND: "这次推荐已不可用，请重新计算推荐。",
  INELIGIBLE_STATION: "此电站不属于当前有效推荐，请重新推荐。",
  INVALID_NAME: "请输入 1–40 个可见字符作为演示昵称。",
  NOT_INITIALIZED: "模拟业务库尚未初始化，请联系管理员。",
  INVALID_CLOCK: "模拟时钟参数无效，请使用正数推进时间和有效倍速。",
  INVALID_TIMESTAMP: "时间格式无效，请刷新页面后重试。",
  STATION_NOT_FOUND: "电站不存在，请刷新站点列表。",
  EXPERIMENT_RUNNING: "已有实验正在计算，请在智能分析的策略对比页面等待结果。",
};
export async function request<T>(
  path: string,
  options: {
    method?: string;
    body?: unknown;
    token?: string;
    admin?: string;
    signal?: AbortSignal;
  } = {},
): Promise<Envelope<T>> {
  const controller = new AbortController();
  let timedOut = false;
  const abortFromCaller = () => controller.abort();
  if (options.signal?.aborted) abortFromCaller();
  else
    options.signal?.addEventListener("abort", abortFromCaller, { once: true });
  const timeout = window.setTimeout(() => {
    timedOut = true;
    controller.abort();
  }, 30000);
  const headers: Record<string, string> = {
    "Content-Type": "application/json",
  };
  if (options.token) headers.Authorization = `Bearer ${options.token}`;
  if (options.admin) headers["X-Admin-Token"] = options.admin;
  try {
    const response = await fetch(`/api/v1/chargepilot${path}`, {
      method: options.method || "GET",
      headers,
      body:
        options.body === undefined ? undefined : JSON.stringify(options.body),
      signal: controller.signal,
    });
    const text = await response.text();
    let result: any;
    try {
      result = JSON.parse(text);
    } catch {
      throw new ApiError(
        "INVALID_RESPONSE",
        "服务返回了无法解析的响应，请确认 API 服务已启动。",
        response.status,
      );
    }
    if (result === null || typeof result !== "object" || Array.isArray(result))
      throw new ApiError(
        "INVALID_RESPONSE",
        "服务响应必须为 JSON 对象，请检查服务版本。",
        response.status,
      );
    if (!response.ok || result.error) {
      const code = result.error?.code || result.code || "REQUEST_FAILED";
      throw new ApiError(
        code,
        businessErrors[code] ||
          result.error?.message ||
          result.message ||
          "请求未完成，请稍后重试。",
        response.status,
      );
    }
    if (!("data" in result))
      throw new ApiError(
        "INVALID_RESPONSE",
        "响应缺少 data 字段，请检查服务版本。",
        response.status,
      );
    return result;
  } catch (error) {
    if (error instanceof ApiError) throw error;
    if (error instanceof Error && error.name === "AbortError")
      throw new ApiError(
        timedOut ? "TIMEOUT" : "REQUEST_CANCELLED",
        timedOut ? "请求超时，请稍后重试。" : "请求已取消。",
        0,
      );
    throw new ApiError(
      "NETWORK_ERROR",
      "暂时无法连接服务。请检查后端是否已启动。",
      0,
    );
  } finally {
    window.clearTimeout(timeout);
    options.signal?.removeEventListener("abort", abortFromCaller);
  }
}
