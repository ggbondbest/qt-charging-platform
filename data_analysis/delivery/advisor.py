"""Same-origin advisor with bounded bodies, work, and explicit online consent."""
import asyncio
from concurrent.futures import ThreadPoolExecutor
import json
import threading
import time
from typing import Literal

from fastapi import Request
from fastapi.exceptions import RequestValidationError
from pydantic import BaseModel, ConfigDict, Field, ValidationError, field_validator

from data_analysis.backend.app import envelope
from data_analysis.backend.database import open_snapshot
from data_analysis.backend.errors import ApiError
from data_analysis.backend.models import Envelope, FilterQuery
from data_analysis.chargepilot.app import WindowLimiter
from data_analysis.chargepilot.store import DomainError
from data_analysis.ml.advisor import config, service

PATH = "/api/v1/intelligence/advisor"


class AdvisorRequest(FilterQuery):
    model_config = ConfigDict(extra="forbid", strict=True)
    datasetId: str = Field(min_length=1, max_length=128)
    publishedBatchId: str = Field(min_length=1, max_length=128)
    question: str = Field(min_length=1, max_length=300)
    mode: Literal["offline", "online"] = "offline"
    consent: bool = False

    @field_validator("question", "datasetId", "publishedBatchId")
    @classmethod
    def not_blank(cls, value):
        if not value.strip():
            raise ValueError("Must not be blank")
        return value.strip()


class Source(BaseModel):
    endpoint: str
    field: str


class Evidence(BaseModel):
    id: str
    label: str
    value: int | float | str | None
    unit: str
    source: Source


class Scope(BaseModel):
    datasetId: str
    publishedBatchId: str
    startDate: str
    endDate: str
    cityId: str | None
    stationId: str | None
    timeZone: Literal["Asia/Shanghai"]
    dataKind: Literal["SIMULATED"]


class Suggestion(BaseModel):
    target: Literal["overview", "advanced", "models", "anomalies"]
    label: str


class AdvisorAnswer(BaseModel):
    status: Literal["answered", "unsupported", "no_evidence"]
    mode: Literal["offline", "online"]
    intent: str
    answer: str
    scope: Scope
    evidence: list[Evidence]
    limitations: list[str]
    suggestions: list[Suggestion]


class SampleQuestion(BaseModel):
    id: str
    label: str
    question: str


class AdvisorConfig(BaseModel):
    defaultMode: Literal["offline"]
    onlineAvailable: bool
    onlineProvider: str | None
    maxQuestionLength: int
    disclosure: str
    supportedQuestions: list[SampleQuestion]


class AdvisorRunner:
    """A timeout never frees a slot while its underlying worker is still running.

    A worker owns and closes its own DB snapshot, so timeout/disconnect cannot
    close a connection underneath a running query. No unbounded executor queue.
    """
    def __init__(self, workers=2, timeout=15.0):
        self.timeout = timeout
        self.slots = threading.BoundedSemaphore(workers)
        self.pool = ThreadPoolExecutor(max_workers=workers, thread_name_prefix="advisor")
        self.limiter = WindowLimiter()

    async def run(self, work, *, cancelled=None):
        if not self.slots.acquire(blocking=False):
            raise ApiError(429, "ADVISOR_BUSY", "参谋正在处理其他问题，请稍后重试")
        try:
            future = self.pool.submit(work)
        except Exception:
            self.slots.release()
            raise
        future.add_done_callback(lambda _: self.slots.release())
        wrapped = asyncio.wrap_future(future)
        # Retrieve late worker exceptions after a caller timeout/disconnect.
        wrapped.add_done_callback(lambda done: None if done.cancelled() else done.exception())
        try:
            return await asyncio.wait_for(asyncio.shield(wrapped), self.timeout)
        except asyncio.TimeoutError:
            if cancelled is not None:
                cancelled.set()
            raise ApiError(504, "ADVISOR_TIMEOUT", "参谋回答超时，请稍后重试；已发出的在线请求可能仍在收尾") from None
        except asyncio.CancelledError:
            if cancelled is not None:
                cancelled.set()
            raise

    def close(self):
        self.pool.shutdown(wait=False, cancel_futures=True)


def register(application, provider):
    runner = AdvisorRunner()
    application.state.advisor_runner = runner
    errors = {code: {"model": Envelope[dict]} for code in (400, 403, 404, 409, 413, 415, 422, 429, 502, 503, 504)}

    @application.get(PATH, response_model=Envelope[AdvisorConfig], operation_id="advisorConfig")
    def advisor_config(request: Request):
        settings = config.online_settings()
        return envelope(request, dict(defaultMode="offline", onlineAvailable=settings is not None,
            onlineProvider=settings.provider if settings else None, maxQuestionLength=300,
            disclosure=service.DISCLOSURE, supportedQuestions=service.QUESTIONS))

    @application.post(PATH, response_model=Envelope[AdvisorAnswer], responses=errors, operation_id="advisorAsk",
        openapi_extra={"requestBody": {"required": True, "content": {
            "application/json": {"schema": AdvisorRequest.model_json_schema()}}}})
    async def advisor_ask(request: Request):
        origin = request.headers.get("origin")
        allowed = {str(request.base_url).rstrip("/")}
        for middleware in application.user_middleware:
            allowed.update(middleware.kwargs.get("allow_origins", []))
        if origin and origin not in allowed:
            raise ApiError(403, "ORIGIN_DENIED", "请求来源不被允许")
        if request.headers.get("content-type", "").split(";")[0].strip().lower() != "application/json":
            raise ApiError(415, "INVALID_CONTENT_TYPE", "请使用 application/json")
        raw = bytearray()
        async for chunk in request.stream():
            if len(raw) + len(chunk) > 8192:
                raise ApiError(413, "REQUEST_TOO_LARGE", "提问内容超过允许大小")
            raw.extend(chunk)
        try:
            body = AdvisorRequest.model_validate(json.loads(raw))
        except (ValueError, UnicodeError) as exc:
            if isinstance(exc, ValidationError):
                errors_ = [{**item, "loc": ("body", *item["loc"])} for item in exc.errors()]
                raise RequestValidationError(errors_) from None
            raise ApiError(422, "INVALID_ARGUMENT", "请求必须是合法的 JSON 对象") from None
        settings = None
        if body.mode == "online":
            if body.consent is not True:
                raise ApiError(422, "CONSENT_REQUIRED", "在线辅助需为本次聚合指标外发明确勾选同意")
            settings = config.online_settings()
            if settings is None:
                raise ApiError(503, "ONLINE_NOT_CONFIGURED", "服务器尚未配置在线模型；离线问答仍可使用")
        try:
            runner.limiter.check(("advisor", request.client.host if request.client else "local"), 20)
        except DomainError:
            raise ApiError(429, "RATE_LIMITED", "参谋请求过于频繁，请稍后重试") from None
        deadline = time.monotonic() + runner.timeout
        cancelled = threading.Event()

        def work():
            with open_snapshot(application.state.database_path, mysql_settings=application.state.mysql_settings) as snapshot:
                request.state.publication = snapshot.metadata
                if cancelled.is_set() or time.monotonic() >= deadline:
                    raise ApiError(504, "ADVISOR_TIMEOUT", "参谋查询超时")
                result = service.answer(snapshot, provider, body, settings=settings, deadline=deadline, cancelled=cancelled)
                return envelope(request, result)

        return await runner.run(work, cancelled=cancelled)

    return runner
