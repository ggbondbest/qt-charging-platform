"""Same-origin apps for current Qt business and historical model analysis.

Run with ``python -m data_analysis.delivery.cli serve``. No iframe, second
frontend server or silent SQLite/mock fallback is needed.
"""
from contextlib import asynccontextmanager
from typing import Annotated, Literal

from pathlib import Path

from fastapi import Depends, FastAPI, Query, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

from .qt_live_adapter import PREFIX, qt_envelope, register_qt_live


def create_app(*, settings=None, mysql_settings=None, database_path=None, provider=None,
               operational_app=None, qt_database_path=None, initialize_operational_store=True):
    # Keep the Qt-only factory usable with FastAPI + stdlib, without loading
    # ML libraries, model artifacts, MySQL configuration or replay services.
    from data_analysis.backend.app import create_app as analytics_app, envelope
    from data_analysis.backend.database import open_snapshot
    from data_analysis.backend.models import BatchQuery, PredictionRequest
    from data_analysis.backend import service
    from data_analysis.chargepilot.app import create_app as chargepilot_app, WindowLimiter
    from data_analysis.chargepilot.settings import Settings
    from data_analysis.chargepilot.store import DomainError
    from data_analysis.backend.errors import ApiError
    from .models import ForecastService

    class DetailedPredictionRequest(PredictionRequest):
        target: Literal["load", "availability"]

    class InsightQuery(BatchQuery):
        limit: int = 20

    settings = settings or Settings.from_env()
    provider = provider or ForecastService.from_local(settings)
    application = analytics_app(database_path, mysql_settings=mysql_settings, prediction_provider=provider)
    application.title = "充能智析 · 运营分析与智能找桩"
    application.description = "Qt当前模拟业务只读快照、历史统计批次、CPU机器学习和独立MySQL策略演示。三类数据保持隔离；历史预测不保证Qt库存。"
    # The outer CORS layer sees mounted-app preflights first. Include the
    # operational PATCH/auth headers as well as the read-only statistics API.
    existing_cors = next(m for m in application.user_middleware if m.cls is CORSMiddleware)
    origins = set(existing_cors.kwargs["allow_origins"]) | set(settings.allowed_origins)
    application.user_middleware = [m for m in application.user_middleware if m.cls is not CORSMiddleware]
    application.add_middleware(CORSMiddleware, allow_origins=sorted(origins), allow_credentials=False,
                              allow_methods=["GET", "POST", "PATCH"],
                              allow_headers=["Content-Type", "Authorization"],
                              expose_headers=["X-Request-ID"])
    cp_app = operational_app or chargepilot_app(settings, predictor=provider.arrival, load_adapter=provider.load,
                                               initialize_store=initialize_operational_store)
    application.state.models = provider
    application.state.operational_app = cp_app
    limiter = WindowLimiter()
    from .advisor import register as register_advisor
    advisor_runner = register_advisor(application, provider)

    @asynccontextmanager
    async def lifespan(app):
        try:
            async with cp_app.router.lifespan_context(cp_app):
                yield
        finally:
            advisor_runner.close()
    application.router.lifespan_context = lifespan

    def snapshot(request: Request):
        with open_snapshot(application.state.database_path, mysql_settings=application.state.mysql_settings) as data:
            request.state.publication = data.metadata
            yield data

    @application.get("/api/v1/intelligence/models")
    def models(request: Request, query: Annotated[BatchQuery, Query()], data=Depends(snapshot)):
        service.check_batch(data, query)
        return envelope(request, provider.registry(data.metadata))

    @application.post("/api/v1/intelligence/forecast")
    def forecast(request: Request, body: DetailedPredictionRequest, data=Depends(snapshot)):
        service.validate_prediction(data, body, require_unavailable=False)
        try:
            limiter.check(("forecast", request.client.host if request.client else "local"), 60)
        except DomainError as exc:
            raise ApiError(429, "RATE_LIMITED", "预测请求过多，请稍后重试") from exc
        return envelope(request, provider.predict(body.target, body, data.metadata))

    @application.get("/api/v1/intelligence/insights/{kind}")
    def insights(request: Request, kind: Literal["churn", "anomalies"],
                 query: Annotated[InsightQuery, Query()], data=Depends(snapshot)):
        service.check_batch(data, query)
        if not 1 <= query.limit <= 100:
            raise ApiError(422, "INVALID_ARGUMENT", "条数须在1至100之间")
        return envelope(request, provider.insight(kind, data.metadata, limit=query.limit))

    @application.get("/api/v1/intelligence/insights/{kind}/{entity_id}")
    def insight_detail(request: Request, kind: Literal["churn", "anomalies"], entity_id: str,
                       query: Annotated[BatchQuery, Query()], data=Depends(snapshot)):
        service.check_batch(data, query)
        if len(entity_id) > 128:
            raise ApiError(422, "INVALID_ARGUMENT", "编号过长")
        return envelope(request, provider.insight(kind, data.metadata, entity_id=entity_id))

    register_qt_live(application, qt_database_path)

    # Existing analytics routes win; ChargePilot keeps its error/auth boundary
    # and serves Vue at /. Its lifespan is explicitly entered above.
    application.mount("/", cp_app)
    return application


def create_qt_live_app(*, qt_database_path=None, frontend_dist=None):
    """Current-business demo without requiring historical MySQL or models."""
    application = FastAPI(title="充能智析 · Qt当前模拟业务", version="1.0.0",
                          description="只读Qt SQLite；不合并历史统计，不提供历史模型预测。", redoc_url=None)
    register_qt_live(application, qt_database_path)
    application.add_middleware(CORSMiddleware,
        allow_origins=["http://localhost:5173", "http://127.0.0.1:5173"], allow_credentials=False,
        allow_methods=["GET"], allow_headers=["Content-Type"], expose_headers=["X-Request-ID"])

    @application.api_route("/api/{path:path}", methods=["GET", "POST", "PATCH", "PUT", "DELETE"], include_in_schema=False)
    def historical_services_unavailable(request: Request, path: str):
        is_qt = request.url.path.startswith(PREFIX + "/")
        known_qt_route = request.url.path in {PREFIX + "/" + endpoint for endpoint in
                                             ("health", "snapshot", "summary", "stations", "orders")}
        status, code, message = (503, "HISTORICAL_SERVICES_DISABLED", "当前仅启动Qt业务监控；历史分析与模型服务未启用")
        if known_qt_route:
            status, code, message = 405, "METHOD_NOT_ALLOWED", "Qt业务监控仅支持只读GET请求"
        elif is_qt:
            status, code, message = 404, "NOT_FOUND", "接口不存在"
        payload = qt_envelope(request, code=code, message=message)
        return JSONResponse(status_code=status,
            content=payload, headers={"Cache-Control": "no-store", "X-Request-ID": payload["meta"]["requestId"]})

    dist = Path(frontend_dist) if frontend_dist is not None else Path(__file__).resolve().parents[1] / "frontend" / "dist"
    if (dist / "index.html").is_file():
        application.mount("/", StaticFiles(directory=dist, html=True), name="frontend")
    else:
        @application.get("/", include_in_schema=False)
        def frontend_not_built(request: Request):
            payload = qt_envelope(request, code="FRONTEND_NOT_BUILT", message="Vue页面尚未构建，请在前端目录运行npm ci和npm run build")
            return JSONResponse(status_code=503, content=payload,
                                headers={"Cache-Control": "no-store", "X-Request-ID": payload["meta"]["requestId"]})
    return application
