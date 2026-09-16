"""Choose one verified aggregate metric; never ask a model to write an answer."""
import json
import math
import socket
from http.client import HTTPException
from urllib.error import URLError
from urllib.request import HTTPRedirectHandler, Request, build_opener

from data_analysis.backend.errors import ApiError

ALLOWED_METRICS = frozenset({
    "attempts", "successful", "failed", "failure_share", "failure_count",
    "sessions", "intervals", "first_observed", "mean_interval", "mean_energy",
    "energy", "utilization", "queue_wait", "station_count",
})


def external_payload(intent, evidence):
    rows = []
    for item in evidence:
        value = item["value"]
        if item["id"] in ALLOWED_METRICS and type(value) in (int, float) and math.isfinite(value):
            rows.append({"id": item["id"], "value": value, "unit": item["unit"]})
    return {"intent": intent, "metrics": rows[:12]}


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        # Never carry Authorization to a redirected host.
        return None


def choose_focus(settings, payload, *, timeout=None):
    allowed = {row["id"] for row in payload["metrics"]}
    if not allowed:
        return None
    prompt = ("从给定的模拟运营聚合指标中选择一个值得用户优先复核的指标。"
              "只返回 JSON 对象 {\"focus\":\"指标id\"}，id 必须来自输入。"
              "不要返回数字、结论、建议、工具调用或其他字段。")
    body = {"model": settings.model, "messages": [
        {"role": "system", "content": prompt},
        {"role": "user", "content": json.dumps(payload, ensure_ascii=False, allow_nan=False)}],
        "max_tokens": 80, "temperature": 0}
    request = Request(settings.base_url + "/chat/completions", method="POST",
        data=json.dumps(body, ensure_ascii=False).encode("utf8"),
        headers={"Authorization": "Bearer " + settings.api_key, "Content-Type": "application/json"})
    try:
        with build_opener(NoRedirect()).open(request, timeout=min(settings.timeout, timeout or settings.timeout)) as response:
            raw = response.read(32769)
        if len(raw) > 32768:
            raise ValueError("Oversized response")
        result = json.loads(raw)
        message = result["choices"][0]["message"]
        if not isinstance(message, dict):
            raise ValueError("Unexpected message shape")
        if message.get("tool_calls"):
            raise ValueError("Unexpected tools")
        content = json.loads(message["content"])
        if not isinstance(content, dict) or set(content) != {"focus"} or content["focus"] not in allowed:
            raise ValueError("Unexpected output")
        return content["focus"]
    except (TimeoutError, socket.timeout):
        raise ApiError(504, "ADVISOR_TIMEOUT", "在线模型响应超时，可切回离线问答") from None
    except URLError as exc:
        if isinstance(exc.reason, (TimeoutError, socket.timeout)):
            raise ApiError(504, "ADVISOR_TIMEOUT", "在线模型响应超时，可切回离线问答") from None
        raise ApiError(502, "ONLINE_UNAVAILABLE", "在线模型暂不可用，可切回离线问答") from None
    except (HTTPException, OSError):
        raise ApiError(502, "ONLINE_UNAVAILABLE", "在线模型连接中断，可切回离线问答") from None
    except (ValueError, KeyError, IndexError, TypeError):
        raise ApiError(502, "ONLINE_INVALID_RESPONSE", "在线模型未返回有效的证据选择，可切回离线问答") from None
