"""Unified same-origin app: published MySQL analytics + isolated live demo.

Run with ``python -m data_analysis.delivery.cli serve``. No iframe, second
frontend server or silent SQLite/mock fallback is needed.
"""
from contextlib import asynccontextmanager
from typing import Annotated, Literal

from fastapi import Depends, Query, Request
from fastapi.middleware.cors import CORSMiddleware
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


def create_app(*, settings=None, mysql_settings=None, database_path=None, provider=None, operational_app=None):
    settings = settings or Settings.from_env()
    provider = provider or ForecastService.from_local(settings)
    application = analytics_app(database_path, mysql_settings=mysql_settings, prediction_provider=provider)
    application.title = "充能智析 · 运营分析与智能找桩"
    application.description = "已发布统计批次、CPU机器学习和独立MySQL业务演示。所有运营数据均为模拟；预测不保证实际库存。"
    # The outer CORS layer sees mounted-app preflights first. Include the
    # operational PATCH/auth headers as well as the read-only statistics API.
    existing_cors = next(m for m in application.user_middleware if m.cls is CORSMiddleware)
    origins = set(existing_cors.kwargs["allow_origins"]) | set(settings.allowed_origins)
    application.user_middleware = [m for m in application.user_middleware if m.cls is not CORSMiddleware]
    application.add_middleware(CORSMiddleware, allow_origins=sorted(origins), allow_credentials=False,
                              allow_methods=["GET", "POST", "PATCH"],
                              allow_headers=["Content-Type", "Authorization", "X-Admin-Token"],
                              expose_headers=["X-Request-ID"])
    cp_app = operational_app or chargepilot_app(settings, predictor=provider.arrival, load_adapter=provider.load)
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

    # Existing analytics routes win; ChargePilot keeps its error/auth boundary
    # and serves Vue at /. Its lifespan is explicitly entered above.
    application.mount("/", cp_app)
    return application
