"""参谋的本地 HTTP 桥:桌宠(前端)↔ agent.ask 之间那一层薄薄的壳。

刻意不动队友的 backend/(Java/8000 端口),自己用一个标准库端口:
  python -m data_analysis.ml.advisor.serve        # 默认 127.0.0.1:8765
  ML_ADVISOR_PORT=9000 python -m ...             # 换端口

路由:
  GET  /health          → 后端/模型/工件就绪状态(桌宠启动时探测)
  POST /advisor/ask     → {"question":"...", "backend":"mock"(可选,只允许降级到离线演示)}

红线与 driver 一致:只读工具、引用收集、无依据就说无依据。
CORS 放开是为了 vite(5173)能直连;服务只绑 127.0.0.1,不出机器。

CSRF 闸门(评审实锤:CORS 只挡"读回答",挡不住恶意网页静默"执行"):
 - GET /advisor/ask 快捷通道已删除——<img src> 是无预检、无 Origin 的普通子资源
   请求,任何防头策略都拦不住它;curl 便车改走 driver CLI。
 - POST 必须 Content-Type: application/json —— form(text/plain) 简单请求不触发
   预检,收紧类型后跨站 form 拼不出合法请求体。
 - 带 Origin 头的请求必须是本机来源(无 Origin 的非浏览器客户端如 curl 不受影响)。
 - backend 参数只认 "mock":请求方可以把在线降为离线演示,绝不允许反过来拿
   .env/环境残留凭据强制起在线计费调用(在线/离线由操作员在 .env/面板下拉里定)。
"""
from __future__ import annotations

import json
import os
import re
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

from . import config

_PORT = int(os.environ.get("ML_ADVISOR_PORT", "8765"))
_LOCAL_ORIGIN = re.compile(r"^https?://(localhost|127\.0\.0\.1)(:\d+)?$")


def origin_allowed(origin: str | None) -> bool:
    """无 Origin(非浏览器客户端)放行;带 Origin 必须是本机来源——
    浏览器跨站请求一定带 Origin,<img>/<form> 都伪造不了也省不掉它。"""
    return not origin or bool(_LOCAL_ORIGIN.match(origin))


def request_backend(value) -> str | None:
    """客户端 backend 参数只允许把在线降为 mock(离线演示),其余一律忽略回落
    操作员配置——防拿 .env/环境残留凭据被外部强制起真实计费调用。"""
    return "mock" if value == "mock" else None


class Handler(BaseHTTPRequestHandler):
    server_version = "MLAdvisor/1.0"

    # ---------- 小工具 ----------

    def _cors(self) -> None:
        # 只给本机来源发 CORS 头(挡"读回答");"挡执行"由 do_POST 的
        # Origin/Content-Type 闸门负责——两者缺一不可,见模块头 CSRF 闸门注释。
        origin = self.headers.get("Origin") or ""
        if _LOCAL_ORIGIN.match(origin):
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def _json(self, code: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self._cors()
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):  # 安静点,演示时别刷屏
        if os.environ.get("ML_ADVISOR_SERVE_VERBOSE"):
            super().log_message(fmt, *args)

    # ---------- 路由 ----------

    def do_OPTIONS(self):  # vite 的预检
        self._json(204, {})

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == "/health":
            body = {
                "status": "ok",
                "mode": "offline" if config.backend_name() == "mock" else "online",
                "alertsReady": config.ALERTS_CSV.exists(),
                "indexReady": config.KNOWLEDGE_DB.exists(),
                "hint": "若工件未就绪:python -m data_analysis.ml.advisor.driver --prepare",
            }
            if os.environ.get("ML_ADVISOR_SERVE_DEBUG"):
                body["backend"] = config.backend_name()
                body["model"] = config.model_id()
            self._json(200, body)
        elif url.path == "/advisor/ask":
            # GET 便车已删:<img src="/advisor/ask?..."> 是无预检简单请求,CSRF 防不住
            self._json(405, {"error": "请用 POST /advisor/ask(命令行请走 driver)"})
        else:
            self._json(404, {"error": f"未知路径 {url.path}"})

    def do_POST(self):
        url = urlparse(self.path)
        if url.path != "/advisor/ask":
            self._json(404, {"error": f"未知路径 {url.path}"})
            return
        if not origin_allowed(self.headers.get("Origin")):
            self._json(403, {"error": "拒绝跨站来源的页面动作/提问请求"})
            return
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        if ctype != "application/json":
            # <form enctype=text/plain> 类简单请求的兜底:类型不对根本不解析 body
            self._json(415, {"error": "Content-Type 必须是 application/json"})
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
            payload = json.loads(self.rfile.read(length) or b"{}")
        except Exception as e:  # JSONDecodeError/UnicodeDecodeError 都不许吞连接
            self._json(400, {"error": f"请求体不是合法 UTF-8 JSON:{type(e).__name__}"})
            return
        question = str(payload.get("question") or "").strip()
        if not question:
            self._json(400, {"error": "question 不能为空"})
            return
        self._ask(question, request_backend(payload.get("backend")))

    def _ask(self, question: str, backend: str | None) -> None:
        from .agent import ask  # 懒导入:pandas/sklearn 在首次提问才加载
        from .friendly import humanize, scrub
        try:
            result = ask(question, backend=backend)
            self._json(200, humanize(result))
        except Exception as e:  # 绝不给前端裸 500:错误也 JSON 化
            # 原始异常常含供应商报文/本机路径/凭据残影:全量只进服务端日志,
            # 出口给 scrub 消毒过的类型名+短消息(验收实锤的泄露面在此收口)
            print(f"[advisor] 处理失败:{type(e).__name__}: {e}", file=sys.stderr)
            self._json(500, {"error": f"{type(e).__name__}:{scrub(str(e))[:160] or '内部错误'}",
                             "advice": "可重试一次;若为缺少 key/网络问题,切到「离线演示」仍可完整演示管线"})


class _NoDoubleBind(ThreadingHTTPServer):
    # Windows 的 SO_REUSEADDR 语义是"允许抢绑":旧坑实锤——重复启动不报错,
    # 两个 serve 静默共存同一端口,演示时请求随机落到僵尸实例上。关掉复用。
    allow_reuse_address = False


def main() -> None:
    # 先探一口:端口上已有人应答就明确拒绝启动,绝不与旧实例共存(双绑=演示事故)
    try:
        with socket.create_connection(("127.0.0.1", _PORT), timeout=0.5):
            print(f"[advisor] {_PORT} 端口已有参谋服务在应答,不必双开。"
                  f"确需重启:netstat -ano | findstr {_PORT} 找 PID → taskkill /PID <pid> /F")
            sys.exit(1)
    except OSError:
        pass
    try:
        httpd = _NoDoubleBind(("127.0.0.1", _PORT), Handler)
    except OSError as e:
        print(f"[advisor] 起不来:{e}。端口被占可换 ML_ADVISOR_PORT=9000 再试")
        sys.exit(1)
    print(f"[advisor] http://127.0.0.1:{_PORT}  backend={config.backend_name()}"
          f"  (Ctrl+C 停止)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
