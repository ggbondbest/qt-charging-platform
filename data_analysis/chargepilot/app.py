"""Isolated Vue demo API. Analytics v1 and first-stage Qt are not mutated.

Run with the factory via chargepilot.cli, or uvicorn ...app:create_app --factory.
Operational APIs are authenticated; payment is explicitly simulated, never money.
"""
from __future__ import annotations
import asyncio
from collections import defaultdict, deque
from contextlib import asynccontextmanager
import hmac
import json
import logging
from pathlib import Path
import threading
import time
from typing import Annotated, Literal

from fastapi import Depends, FastAPI, Header, Request, Query
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, ConfigDict, Field

from .settings import Settings, ROOT
from .store import ChargePilotStore, DomainError
from .ranking import RecommendationService
from .routing import RoutePlanner
from .load_adapter import LoadAdapter


class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)


class Origin(StrictModel):
    latitude: float = Field(ge=-90, le=90)
    longitude: float = Field(ge=-180, le=180)


class SessionInput(StrictModel):
    name: str = Field(min_length=1, max_length=40)


class RecommendationInput(StrictModel):
    cityId: str = Field(min_length=1, max_length=128)
    origin: Origin
    energyKwh: float = Field(default=20, ge=1, le=100)
    maxEtaMinutes: float = Field(default=60, ge=5, le=60)


class Selection(StrictModel):
    stationId: str = Field(min_length=1, max_length=128)


class Background(StrictModel):
    busyCount: int = Field(ge=0, le=10000, strict=True)
    releaseAfterSeconds: int = Field(ge=1, le=86400, strict=True)


class ClockInput(StrictModel):
    action: Literal["advance", "configure"]
    seconds: int | None = Field(default=None, ge=1, le=86400, strict=True)
    speed: float | None = Field(default=None, ge=0, le=3600)
    paused: bool | None = None


class ExperimentInput(StrictModel):
    users: int = Field(default=1000, ge=100, le=2000, strict=True)
    seed: int = Field(default=42, ge=0, le=2**31-1, strict=True)


class PathInput(StrictModel):
    origin: Origin
    stationId: str = Field(min_length=1, max_length=128)


class WindowLimiter:
    """Bounded process-local demo abuse protection, not distributed auth."""
    def __init__(self):
        self.events = {}
        self.lock = threading.Lock()

    def check(self, key, maximum, period=60):
        now = time.monotonic()
        with self.lock:
            if key not in self.events and len(self.events) >= 4096:
                self.events = {k: q for k, q in self.events.items() if q and q[-1] > now-period}
                if len(self.events) >= 4096:
                    raise DomainError("RATE_LIMITED", "请求过多，请稍后重试", 429)
            queue = self.events.setdefault(key, deque())
            while queue and queue[0] <= now-period:
                queue.popleft()
            if len(queue) >= maximum:
                raise DomainError("RATE_LIMITED", "请求过多，请稍后重试", 429)
            queue.append(now)


def create_app(settings=None, *, predictor=None, store=None, routing=None, load_adapter=None):
    settings = settings or Settings.from_env()
    if predictor is None:
        from .ml.predict import ArrivalPredictor
        predictor = ArrivalPredictor(settings.model_dir)
    store = store or ChargePilotStore(settings.mysql)
    store.initialize(predictor.catalog, predictor.cities)
    initial = store.admin()
    if not initial.get("baselineInitialized", False):
        store.set_baseline(predictor.snapshot(initial["clock"]["time"]))
    routing = routing or RoutePlanner(settings.tencent_key, settings.tencent_secret)
    if load_adapter is None:
        try:
            load_adapter = LoadAdapter(settings.load_dir)
        except (OSError, ValueError, KeyError, TypeError):
            # A corrupt/absent optional load bundle must not lock users out of trips.
            load_adapter = LoadAdapter.unavailable("负荷模型校验失败，请重新准备受信任工件；其他功能可继续使用")
    service = RecommendationService(predictor, store, routing)
    limiter = WindowLimiter()
    experiment_state = {"running": False, "error": None}
    experiment_lock = threading.Lock()
    tick_state = {"healthy": True, "clock": initial["clock"]}
    experiment_path = settings.model_dir / "experiment.json"

    @asynccontextmanager
    async def lifespan(app):
        stop = asyncio.Event()

        async def ticker():
            while not stop.is_set():
                try:
                    tick_state["clock"] = await asyncio.to_thread(store.clock)
                    tick_state["healthy"] = True
                except Exception:
                    tick_state["healthy"] = False
                    logging.getLogger(__name__).warning("ChargePilot clock synchronization unavailable")
                try:
                    await asyncio.wait_for(stop.wait(), timeout=1)
                except asyncio.TimeoutError:
                    pass
        task = asyncio.create_task(ticker())
        yield
        stop.set()
        await task
        routing.close()

    app = FastAPI(title="ChargePilot 智能找桩实验室", version="1.0.0", lifespan=lifespan,
                  description="独立MySQL、历史模拟回放、CPU机器学习；支付与积分均为演示，不发生真实资金交易。")
    app.state.store, app.state.predictor = store, predictor
    app.add_middleware(CORSMiddleware, allow_origins=list(settings.allowed_origins),
        allow_credentials=False, allow_methods=["GET", "POST", "PATCH"],
        allow_headers=["Content-Type", "Authorization", "X-Admin-Token"])

    @app.middleware("http")
    async def safe_headers(request, call_next):
        length = request.headers.get("content-length")
        if length and (not length.isdigit() or int(length) > 65536):
            return JSONResponse({"error": {"code": "BODY_TOO_LARGE", "message": "请求过大"}}, status_code=413)
        response = await call_next(request)
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "strict-origin-when-cross-origin"
        return response

    @app.exception_handler(DomainError)
    async def domain_error(_, exc):
        return JSONResponse({"error": {"code": exc.code, "message": exc.message}}, status_code=exc.status)

    @app.exception_handler(RequestValidationError)
    async def validation_error(_, exc):
        # Avoid echoing arbitrary request bodies, tokens or upstream values.
        fields = ", ".join(".".join(str(part) for part in e["loc"]) for e in exc.errors())
        return JSONResponse({"error": {"code": "INVALID_REQUEST", "message": f"请检查输入字段：{fields}"}}, status_code=422)

    @app.exception_handler(Exception)
    async def unexpected_error(_, exc):
        logging.getLogger(__name__).error("ChargePilot request failed (%s)", type(exc).__name__)
        return JSONResponse({"error": {"code": "SERVICE_UNAVAILABLE", "message": "服务暂不可用，请检查服务端状态"}}, status_code=503)

    def envelope(data):
        observed = store.observed_clock() or tick_state["clock"]
        return {"data": data, "meta": {"dataSource": "SIMULATED", "clockTime": observed["time"],
                                       "mode": "REPLAY_DEMO"}}

    def user(authorization: Annotated[str | None, Header()] = None):
        if not authorization or not authorization.startswith("Bearer ") or len(authorization) > 512:
            raise DomainError("UNAUTHORIZED", "请先创建演示会话", 401)
        return store.authenticate(authorization[7:])

    def administrator(x_admin_token: Annotated[str | None, Header()] = None):
        if not settings.admin_token:
            raise DomainError("ADMIN_DISABLED", "管理员功能未配置；设置CHARGEPILOT_ADMIN_TOKEN后启动", 503)
        if not x_admin_token or not hmac.compare_digest(x_admin_token, settings.admin_token):
            raise DomainError("FORBIDDEN", "管理员凭证无效", 403)

    prefix = "/api/v1/chargepilot"
    provenance = {"dataSource": "SIMULATED", "description": "既有五城市模拟数据清洗结果；没有冒充ACN实测数据",
        "timeRange": predictor.bounds, "stations": len(predictor.catalog),
        "chargers": sum(s["capacity"] for s in predictor.catalog), "cleanRows": 5830923,
        "datasetId": predictor.metadata.get("provenance", {}).get("datasetId"),
        "scope": "运营沙盒与历史预测分离，订单承诺采用透明保守修正；未连接Qt或真实电桩。"}

    @app.get(prefix+"/health")
    def health():
        return envelope({"status": "ok" if tick_state["healthy"] else "degraded", "database": "MySQL",
                         "arrivalModel": "READY", "loadModel": load_adapter.report["status"]})

    @app.get(prefix+"/bootstrap")
    def bootstrap():
        state = store.admin()
        return envelope({"cities": predictor.cities, "stations": state["stations"], "clock": state["clock"],
                         "modelStatus": "READY", "provenance": provenance})

    @app.post(prefix+"/sessions")
    def session(payload: SessionInput, request: Request):
        limiter.check(("session", request.client.host if request.client else "unknown"), 20)
        if not payload.name.strip():
            raise DomainError("INVALID_NAME", "请输入演示昵称", 422)
        return envelope(store.create_session(payload.name.strip()))

    @app.post(prefix+"/recommendations")
    def recommendations(payload: RecommendationInput, uid=Depends(user)):
        limiter.check(("recommend", uid), 20)
        return envelope(service.recommend(uid, payload.cityId, payload.origin.model_dump(),
                                          payload.energyKwh, payload.maxEtaMinutes))

    @app.post(prefix+"/recommendations/{recommendation_id}/select")
    def select(recommendation_id: str, payload: Selection, uid=Depends(user)):
        return envelope(store.select(uid, recommendation_id, payload.stationId))

    @app.get(prefix+"/me")
    def me(uid=Depends(user)):
        return envelope(store.me(uid))

    @app.post(prefix+"/trips/{trip_id}/{action}")
    def trip_action(trip_id: str, action: Literal["arrive", "confirm", "start", "stop", "pay", "cancel"],
                    payload: dict | None = None, uid=Depends(user)):
        if payload and (set(payload) != {"reason"} or action != "stop" or payload["reason"] != "MANUAL"):
            raise DomainError("INVALID_REQUEST", "不能由客户端指定金额、积分或充电量", 422)
        return envelope(store.act(uid, trip_id, action))

    @app.get(prefix+"/stations")
    def stations(cityId: str | None = Query(default=None, max_length=128)):
        return envelope(store.stations(cityId))

    @app.post(prefix+"/route")
    def route(payload: PathInput, uid=Depends(user)):
        limiter.check(("route", uid), 20)
        station = next((s for s in predictor.catalog if s["stationId"] == payload.stationId), None)
        if station is None:
            raise DomainError("STATION_NOT_FOUND", "电站不存在", 404)
        return envelope(routing.path(payload.origin.model_dump(), station))

    @app.get(prefix+"/models")
    def models():
        return envelope({"status": "READY", "arrival": {"name": "到站空桩与等待模型",
            "metadata": predictor.metadata, "metrics": predictor.metadata.get("metrics", {})},
            "load": load_adapter.report, "provenance": provenance})

    @app.get(prefix+"/load")
    def load(stationId: str = Query(max_length=128), horizonHours: int = 6):
        return envelope(load_adapter.predict(stationId, store.clock()["time"], horizonHours))

    @app.get(prefix+"/experiments")
    def experiments():
        if experiment_state["running"]:
            return envelope({"status": "RUNNING", "message": "正在计算同一批请求的两种策略"})
        if experiment_state["error"]:
            return envelope({"status": "FAILED", "message": "实验计算失败，请查看服务端测试/重新运行CLI"})
        if not experiment_path.exists():
            return envelope({"status": "NOT_RUN", "message": "尚未运行对照仿真，没有预填效果数据"})
        report = json.loads(experiment_path.read_text(encoding="utf8"))
        expected_hash = predictor.metadata.get("artifacts", {}).get("arrival.joblib", {}).get("sha256")
        if report.get("modelArtifactSha256") != expected_hash:
            return envelope({"status": "STALE", "message": "实验结果对应旧模型，请重新计算"})
        return envelope(report)

    @app.get(prefix+"/admin", dependencies=[Depends(administrator)])
    def admin():
        return envelope(store.admin())

    @app.patch(prefix+"/admin/config", dependencies=[Depends(administrator)])
    def configure(payload: dict):
        return envelope(store.configure(payload))

    @app.post(prefix+"/admin/clock", dependencies=[Depends(administrator)])
    def clock(payload: ClockInput):
        if payload.action == "advance" and payload.seconds is None:
            raise DomainError("INVALID_REQUEST", "advance需要seconds", 422)
        return envelope(store.control_clock(payload.model_dump(exclude_none=True)))

    @app.post(prefix+"/admin/stations/{station_id}/background", dependencies=[Depends(administrator)])
    def background(station_id: str, payload: Background):
        return envelope(store.set_background(station_id, payload.busyCount, payload.releaseAfterSeconds))

    @app.get(prefix+"/admin/feedback", dependencies=[Depends(administrator)])
    def feedback():
        return envelope({"records": store.feedback(), "dataSource": "SIMULATED_OPERATIONAL_FEEDBACK",
                         "note": "用于后续独立离线训练；当前不会自动改变已评估模型。"})

    @app.post(prefix+"/admin/experiments", dependencies=[Depends(administrator)], status_code=202)
    def start_experiment(payload: ExperimentInput):
        from .experiment import run_experiment
        frozen_config = store.admin()["config"]
        with experiment_lock:
            if experiment_state["running"]:
                raise DomainError("EXPERIMENT_RUNNING", "已有实验正在运行", 409)
            experiment_state.update(running=True, error=None)

        def work():
            try:
                run_experiment(predictor, payload.users, payload.seed, experiment_path, frozen_config)
            except Exception as exc:
                experiment_state["error"] = type(exc).__name__
                logging.getLogger(__name__).error("ChargePilot experiment failed (%s)", type(exc).__name__)
            finally:
                experiment_state["running"] = False
        threading.Thread(target=work, daemon=True, name="chargepilot-paired-experiment").start()
        return envelope({"status": "RUNNING"})

    # Built frontend is same-origin; Vite uses its /api proxy in development.
    dist = ROOT / "frontend" / "dist"
    if dist.is_dir():
        app.mount("/", StaticFiles(directory=dist, html=True), name="frontend")
    return app
