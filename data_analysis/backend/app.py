"""Run: uvicorn data_analysis.backend.app:app --host 127.0.0.1 --port 8000.

ANALYTICS_DB selects one immutable published SQLite snapshot. Every response
identifies that snapshot; unavailable models never fabricate predictions.
"""

import argparse
import json
import os
from pathlib import Path
from typing import Annotated
import uuid

from fastapi import Depends, FastAPI, Query, Request
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from starlette.exceptions import HTTPException

from .database import META_FIELDS, SCHEMA_VERSION, open_snapshot
from .errors import ApiError
from .models import (BatchQuery, Chart, ChartQuery, City, Dataset, Envelope, FilterQuery, Health,
                     ModelCapabilities, Overview, Page, PageQuery, PipelineRun, PredictionRequest, PredictionResult, Station)
from . import service


def envelope(request, data=None, code="OK", message="成功"):
    published = getattr(request.state, "publication", {})
    meta = {key: published.get(key) for key in META_FIELDS}
    meta.update(requestId=getattr(request.state, "request_id", uuid.uuid4().hex),
                schemaVersion=published.get("schemaVersion", SCHEMA_VERSION))
    return dict(code=code, message=message, data=data, meta=meta)


def create_app(database_path=None):
    application = FastAPI(title="Charging Analytics API", version=SCHEMA_VERSION,
        description="只读访问已发布的统计批次。金额单位分、电量 Wh；日期 Asia/Shanghai，endDate exclusive。预测尚未实现。",
        docs_url="/docs", redoc_url=None)
    application.state.database_path = database_path if database_path is not None else os.environ.get("ANALYTICS_DB")
    origins = os.environ.get("ANALYTICS_CORS_ORIGINS", "http://localhost:5173,http://127.0.0.1:5173")
    application.add_middleware(CORSMiddleware, allow_origins=[value.strip() for value in origins.split(",") if value.strip()],
                               allow_credentials=False, allow_methods=["GET", "POST"], allow_headers=["Content-Type"],
                               expose_headers=["X-Request-ID"])

    @application.middleware("http")
    async def trace_request(request, call_next):
        request.state.request_id = uuid.uuid4().hex
        request.state.publication = {}
        try:
            response = await call_next(request)
        except Exception:
            response = JSONResponse(status_code=500,
                content=envelope(request, code="INTERNAL_ERROR", message="服务内部错误，请提供请求编号以便排查"))
        response.headers["X-Request-ID"] = request.state.request_id
        response.headers["Cache-Control"] = "no-store"
        return response

    @application.exception_handler(ApiError)
    async def public_error(request, exc):
        return JSONResponse(status_code=exc.status, content=envelope(request, exc.data, exc.code, exc.message))

    @application.exception_handler(RequestValidationError)
    async def invalid_request(request, exc):
        # Never echo input values, body content, stack traces or physical paths.
        fields = sorted({".".join(str(part) for part in item["loc"])[:120] for item in exc.errors()})
        return JSONResponse(status_code=422, content=envelope(request, {"fields": fields}, "INVALID_ARGUMENT", "请求参数格式或取值不正确"))

    @application.exception_handler(HTTPException)
    async def http_error(request, exc):
        codes = {404: ("NOT_FOUND", "接口不存在"), 405: ("METHOD_NOT_ALLOWED", "不支持该请求方式")}
        code, message = codes.get(exc.status_code, ("HTTP_ERROR", "请求无法处理"))
        return JSONResponse(status_code=exc.status_code, content=envelope(request, code=code, message=message))

    def database(request: Request):
        with open_snapshot(application.state.database_path) as snapshot:
            request.state.publication = snapshot.metadata
            yield snapshot

    # Apply one error envelope schema to all public routes. Endpoint payloads
    # have specific response models rather than arbitrary untyped dictionaries.
    errors = {status: {"model": Envelope[dict], "description": description} for status, description in
              [(400, "Inconsistent filters"), (404, "Unknown dataset/city/station"), (409, "Published batch mismatch"),
               (422, "Invalid query/body"), (500, "Unexpected internal error"), (503, "Published data or model unavailable")]}

    @application.get("/api/v1/health", response_model=Envelope[Health], responses=errors, operation_id="health")
    def health(request: Request, query: Annotated[BatchQuery, Query()], snapshot=Depends(database)):
        service.check_batch(snapshot, query)
        return envelope(request, dict(status="ready", dataReady=True, source=snapshot.metadata["source"]))

    @application.get("/api/v1/datasets", response_model=Envelope[Page[Dataset]], responses=errors, operation_id="listDatasets")
    def datasets(request: Request, query: Annotated[PageQuery, Query()], snapshot=Depends(database)):
        service.check_filters(snapshot, query)
        if query.sortBy not in {"id", "generatedAt"}:
            raise ApiError(422, "INVALID_SORT", "数据集排序仅支持 id、generatedAt")
        fields = ("datasetId", "publishedBatchId", "pipelineRunId", "schemaVersion", "source", "startDate", "endDate", "generatedAt", "sourceManifestSha256")
        item = {key: snapshot.metadata[key] for key in fields}
        return envelope(request, service.page([item] if query.page == 1 else [], 1, query))

    @application.get("/api/v1/cities", response_model=Envelope[Page[City]], responses=errors, operation_id="listCities")
    def cities(request: Request, query: Annotated[PageQuery, Query()], snapshot=Depends(database)):
        filters = service.check_filters(snapshot, query)
        return envelope(request, service.city_list(snapshot, filters, query))

    @application.get("/api/v1/stations", response_model=Envelope[Page[Station]], responses=errors, operation_id="listStations")
    def stations(request: Request, query: Annotated[PageQuery, Query()], snapshot=Depends(database)):
        filters = service.check_filters(snapshot, query)
        return envelope(request, service.station_list(snapshot, filters, query))

    @application.get("/api/v1/pipeline/runs", response_model=Envelope[Page[PipelineRun]], responses=errors, operation_id="listPipelineRuns")
    def pipeline_runs(request: Request, query: Annotated[PageQuery, Query()], snapshot=Depends(database)):
        service.check_filters(snapshot, query)
        if query.sortBy not in {"id", "generatedAt"}:
            raise ApiError(422, "INVALID_SORT", "批次排序仅支持 id、generatedAt")
        source = snapshot.metadata["qualityReport"]
        item = dict(pipelineRunId=snapshot.metadata["pipelineRunId"], publishedBatchId=snapshot.metadata["publishedBatchId"],
            status="PUBLISHED", generatedAt=snapshot.metadata["generatedAt"],
            quality=service.quality_summary(source))
        return envelope(request, service.page([item] if query.page == 1 else [], 1, query))

    @application.get("/api/v1/dashboard/overview", response_model=Envelope[Overview], responses=errors, operation_id="dashboardOverview")
    def overview(request: Request, query: Annotated[FilterQuery, Query()], snapshot=Depends(database)):
        return envelope(request, service.overview(snapshot, service.check_filters(snapshot, query)))

    @application.get("/api/v1/dashboard/charts", response_model=Envelope[Chart], responses=errors, operation_id="dashboardCharts")
    def charts(request: Request, query: Annotated[ChartQuery, Query()], snapshot=Depends(database)):
        return envelope(request, service.charts(snapshot, service.check_filters(snapshot, query), query))

    @application.get("/api/v1/models", response_model=Envelope[ModelCapabilities], responses=errors, operation_id="modelCapabilities")
    def models(request: Request, query: Annotated[BatchQuery, Query()], snapshot=Depends(database)):
        service.check_batch(snapshot, query)
        return envelope(request, ModelCapabilities().model_dump())

    prediction_errors = errors.copy()
    prediction_errors[200] = {"model": Envelope[PredictionResult],
        "description": "Future model-adapter success format. Not implemented: current valid requests only return 503 MODEL_NOT_READY."}
    prediction_errors[503] = {"model": Envelope[ModelCapabilities], "description": "MODEL_NOT_READY: no trained model published"}

    @application.post("/api/v1/predict/load", status_code=503, response_model=Envelope[ModelCapabilities], responses=prediction_errors, operation_id="predictLoadNotReady")
    def predict_load(request: Request, body: PredictionRequest, snapshot=Depends(database)):
        service.validate_prediction(snapshot, body)

    @application.post("/api/v1/predict/availability", status_code=503, response_model=Envelope[ModelCapabilities], responses=prediction_errors, operation_id="predictAvailabilityNotReady")
    def predict_availability(request: Request, body: PredictionRequest, snapshot=Depends(database)):
        service.validate_prediction(snapshot, body)

    return application


app = create_app()


def export_openapi(output):
    """Exclusive write; schema generation needs no database or Spark runtime."""
    output = Path(output)
    if output.exists() or output.is_symlink():
        raise FileExistsError("Refusing to overwrite OpenAPI output")
    output.parent.mkdir(parents=True, exist_ok=True)
    schema = create_app().openapi()
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(schema, stream, ensure_ascii=False, indent=2, sort_keys=True)
        stream.write("\n")
    return schema


def main(argv=None):
    parser = argparse.ArgumentParser(description="Export API contract without accessing data")
    parser.add_argument("--export-openapi", required=True, type=Path)
    args = parser.parse_args(argv)
    export_openapi(args.export_openapi)


if __name__ == "__main__":
    main()
