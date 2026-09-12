"""Shared public request and response shapes, camelCase on the wire."""

from typing import Generic, Literal, TypeVar

from pydantic import BaseModel, ConfigDict, Field, create_model, field_validator

from data_analysis.contracts import CONTRACT_VERSION, FEATURE_VERSION
from data_analysis.contracts.serving import parse_utc

from .database import SCHEMA_VERSION
from .service import COHORT_COLUMNS, DAILY_COLUMNS, SERVICE_COLUMNS, STATE_COLUMNS, camel

Payload = TypeVar("Payload")


class Meta(BaseModel):
    requestId: str
    datasetId: str | None = None
    schemaVersion: str = SCHEMA_VERSION
    pipelineRunId: str | None = None
    publishedBatchId: str | None = None
    generatedAt: str | None = None


class Envelope(BaseModel, Generic[Payload]):
    code: str
    message: str
    data: Payload | None
    meta: Meta


class BatchQuery(BaseModel):
    model_config = ConfigDict(extra="forbid")
    datasetId: str | None = Field(None, min_length=1, max_length=128)
    publishedBatchId: str | None = Field(None, min_length=1, max_length=128)


class FilterQuery(BatchQuery):
    cityId: str | None = Field(None, min_length=1, max_length=128)
    stationId: str | None = Field(None, min_length=1, max_length=128)
    startDate: str | None = Field(None, pattern=r"^\d{4}-\d{2}-\d{2}$", description="Asia/Shanghai business date, inclusive")
    endDate: str | None = Field(None, pattern=r"^\d{4}-\d{2}-\d{2}$", description="Asia/Shanghai business date, exclusive")


class PageQuery(FilterQuery):
    page: int = Field(1, ge=1, le=1000000)
    pageSize: int = Field(20, ge=1, le=100)
    sortBy: str = Field("id", min_length=1, max_length=32)
    sortOrder: Literal["asc", "desc"] = "asc"


class ChartQuery(FilterQuery):
    chart: Literal["energy", "revenue", "utilization", "states", "service", "cohorts", "load"] = "energy"
    granularity: Literal["day", "hour"] = "day"
    limit: int = Field(400, ge=1, le=1000)


class PredictionRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    datasetId: str | None = Field(None, min_length=1, max_length=128)
    publishedBatchId: str | None = Field(None, min_length=1, max_length=128)
    cityId: str | None = Field(None, min_length=1, max_length=128)
    stationId: str = Field(min_length=1, max_length=128)
    referenceTime: str = Field(pattern=r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:00:00(?:\.0+)?Z$",
                              max_length=40, description="Exact UTC hour boundary ending in Z; first forecast interval starts here")
    modelId: str = Field(min_length=1, max_length=128)
    horizonHours: Literal[1, 6, 24] = 1

    @field_validator("horizonHours", mode="before")
    @classmethod
    def integer_horizon(cls, value):
        # Literal equality otherwise treats True == 1 and 6.0 == 6.
        if type(value) is not int:
            raise ValueError("horizonHours must be an integer, not a boolean or float")
        return value

    @field_validator("datasetId", "publishedBatchId", "cityId", "stationId", "modelId")
    @classmethod
    def nonblank_identifiers(cls, value):
        if value is not None and not value.strip():
            raise ValueError("Identifiers cannot be blank")
        return value


class Page(BaseModel, Generic[Payload]):
    items: list[Payload]
    page: int
    pageSize: int
    total: int
    hasNext: bool


class PublishedModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    modelId: str = Field(min_length=1, max_length=128)
    modelVersion: str = Field(min_length=1)
    target: Literal["load", "availability"]


class ModelCapabilities(BaseModel):
    models: list[PublishedModel] = Field(default_factory=list)
    implementedPrediction: bool = False
    supportedTargets: list[str] = Field(default_factory=lambda: ["load", "availability"])
    status: str = "MODEL_NOT_READY"


class PredictionPoint(BaseModel):
    model_config = ConfigDict(extra="forbid")
    timestamp: str = Field(json_schema_extra={"format": "date-time"})
    value: float = Field(ge=0, strict=True, allow_inf_nan=False)

    @field_validator("timestamp")
    @classmethod
    def utc_timestamp(cls, value):
        parse_utc(value)
        return value


class PredictionResult(BaseModel):
    """Future successful model adapter output; no endpoint fabricates it today."""
    model_config = ConfigDict(extra="forbid")
    schemaVersion: Literal[CONTRACT_VERSION]
    featureVersion: Literal[FEATURE_VERSION]
    modelId: str = Field(min_length=1)
    modelVersion: str = Field(min_length=1)
    unit: Literal["kW", "chargers"]
    points: list[PredictionPoint] = Field(min_length=1, max_length=24)


_NUMERIC_FIELDS = set(DAILY_COLUMNS + COHORT_COLUMNS + SERVICE_COLUMNS)
_DERIVED_FIELDS = ["activeUsers", "repeatUsers", "stationCount", "chargerCount", "chargingUtilizationRate",
    "chargingAndOccupiedRate", "completeOccupiedSamples", "repeatUserRate", "queueServedRate",
    "queueMeanWaitSeconds", "queueMeanSojournSeconds", "meanRepairResolutionSeconds", "meanRepairWorkSeconds",
    "meanRating", "cashContributionCents"]
Metrics = create_model("Metrics", __config__=ConfigDict(extra="forbid"),
    **{key: (int | float | None, ...) for key in sorted({camel(field) for field in _NUMERIC_FIELDS} | set(_DERIVED_FIELDS))})


def _point_model(name, columns, derived=()):
    return create_model(name, __config__=ConfigDict(extra="forbid"), time=(str, ...),
        **{camel(key): (int | float | None, ...) for key in columns},
        **{key: (int | float | None, ...) for key in derived})


DailyPoint = _point_model("DailyPoint", DAILY_COLUMNS, ("chargingUtilizationRate",))
HourlyPoint = _point_model("HourlyPoint", ["energy_wh", "sample_count", "expected_sample_count", "missing_sample_count"]
                          + STATE_COLUMNS + ["complete_charging_samples", "complete_sample_count"], ("chargingUtilizationRate",))
CohortPoint = _point_model("CohortPoint", COHORT_COLUMNS)
ServicePoint = _point_model("ServicePoint", SERVICE_COLUMNS, ("queueMeanWaitSeconds", "meanRating"))


class LoadPoint(BaseModel):
    model_config = ConfigDict(extra="forbid")
    time: str
    meanPowerKw: float | None
    capacity: int
    stationCount: int
    observedStationCount: int
    completeStationCount: int
    incompleteStationCount: int
    sampleCount: int
    expectedSampleCount: int
    missingSampleCount: int
    isComplete: bool


class Chart(BaseModel):
    chart: str
    granularity: str
    startDate: str
    endDate: str
    items: list[DailyPoint | HourlyPoint | CohortPoint | ServicePoint | LoadPoint]
    limit: int
    truncated: bool


class Overview(BaseModel):
    startDate: str
    endDate: str
    source: str
    metrics: Metrics
    units: dict[str, str]


class City(BaseModel):
    cityId: str
    cityName: str
    latitude: float
    longitude: float
    timezone: str


class PeriodMetrics(BaseModel):
    startDate: str
    endDate: str
    energyWh: int | None
    netPaidCents: int
    chargingUtilizationRate: float | None


class Station(BaseModel):
    stationId: str
    cityId: str
    stationName: str
    cityName: str
    siteType: str
    latitude: float
    longitude: float
    cityLatitude: float
    cityLongitude: float
    capacity: int
    ratedCapacityKw: float
    transformerKw: float
    snapshotAt: str | None
    dataAsOf: str | None
    observedPileCount: int
    availableCount: int
    chargingCount: int
    reservedCount: int
    occupiedCount: int
    maintenanceCount: int
    offlineCount: int
    unknownCount: int
    isCurrent: bool
    isComplete: bool
    snapshotSemantics: Literal["LATEST_IN_BATCH"]
    periodMetrics: PeriodMetrics


class Dataset(BaseModel):
    datasetId: str
    publishedBatchId: str
    pipelineRunId: str
    schemaVersion: str
    source: Literal["SIMULATED"]
    startDate: str
    endDate: str
    generatedAt: str
    sourceManifestSha256: str


class Health(BaseModel):
    status: Literal["ready"]
    dataReady: bool
    source: Literal["SIMULATED"]


class TableRowCount(BaseModel):
    model_config = ConfigDict(extra="forbid")
    tableName: str
    rawRows: int


class TableNormalizationCount(BaseModel):
    model_config = ConfigDict(extra="forbid")
    tableName: str
    normalizedRows: int


class RejectionReason(BaseModel):
    model_config = ConfigDict(extra="forbid")
    reason: str
    rowCount: int


class RejectionSample(BaseModel):
    model_config = ConfigDict(extra="forbid")
    sessionId: str
    rejectionReason: str


class QualitySummary(BaseModel):
    model_config = ConfigDict(extra="forbid")
    rawRows: int | None
    cleanRows: int | None
    rejectedRows: int | None
    normalizedRows: int | None
    cleanSessionRows: int | None
    tables: list[TableRowCount]
    normalizedByTable: list[TableNormalizationCount]
    rejectionReasons: list[RejectionReason]
    rejectionSamples: list[RejectionSample]
    rejectionSampleLimit: int
    normalizationSemantics: str


class PipelineRun(BaseModel):
    pipelineRunId: str
    publishedBatchId: str
    status: Literal["PUBLISHED"]
    generatedAt: str
    quality: QualitySummary
