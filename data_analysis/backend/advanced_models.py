"""Typed multidimensional analytics, distinct from prediction endpoints."""

from pydantic import BaseModel, Field

from .models import FilterQuery


class AdvancedQuery(FilterQuery):
    siteType: str | None = Field(None, min_length=1, max_length=40)


class AdvancedProvenance(BaseModel):
    sourceLabel: str
    analysisId: str
    engine: str
    generatedAt: str
    completeMonthsThrough: str
    notes: list[str]


class AdvancedScope(BaseModel):
    startDate: str
    endDate: str
    cityId: str | None
    stationId: str | None
    siteType: str | None


class AdvancedSummary(BaseModel):
    stationCount: int
    sessionCount: int
    attemptCount: int
    observedStationHours: int
    completeStationHours: int


class DemandCell(BaseModel):
    weekday: int
    hour: int
    energyKwh: float | None
    meanPowerKw: float | None
    chargingUtilization: float | None
    sampleHours: int


class StationEfficiency(BaseModel):
    stationId: str
    stationName: str
    cityId: str
    cityName: str
    siteType: str
    energyKwh: float | None
    chargingUtilization: float | None
    meanWaitMinutes: float | None
    overstayShare: float | None
    netCashYuan: float
    sessionCount: int
    attemptCount: int
    successRate: float | None


class WeatherCell(BaseModel):
    temperatureBin: int
    hour: int
    chargingUtilization: float | None
    meanPowerKw: float | None
    sampleHours: int


class FlowNode(BaseModel):
    name: str


class FlowLink(BaseModel):
    source: str
    target: str
    value: int


class AttemptFlow(BaseModel):
    attemptCount: int
    nodes: list[FlowNode]
    links: list[FlowLink]


class FailureReason(BaseModel):
    reason: str
    label: str
    count: int
    shareOfFailures: float | None
    shareOfAttempts: float | None


class AccessPathSummary(BaseModel):
    path: str
    label: str
    attemptCount: int
    successfulAttempts: int
    successRate: float | None


class BehaviorCell(BaseModel):
    userSegment: str
    bucket: str
    count: int
    share: float | None


class BehaviorSegment(BaseModel):
    userSegment: str
    sessionCount: int
    intervalCount: int
    firstObservedCount: int
    meanIntervalDays: float | None
    meanEnergyKwh: float | None


class RechargeBehavior(BaseModel):
    definition: str
    sessionCount: int
    intervalCount: int
    firstObservedCount: int
    intervals: list[BehaviorCell]
    energy: list[BehaviorCell]
    segments: list[BehaviorSegment]


class ConnectorInventory(BaseModel):
    connectorType: str
    chargerCount: int
    ratedPowerKw: float


class ServiceCell(BaseModel):
    siteType: str
    hour: int
    stationCount: int
    attemptCount: int
    successfulAttempts: int
    successRate: float | None
    meanWaitMinutes: float | None
    queueWaitCount: int
    overstayShare: float | None
    sessionCount: int
    chargingUtilization: float | None
    completeStationHours: int
    interfaces: list[ConnectorInventory]
    failures: list[FailureReason]


class ServiceBottlenecks(BaseModel):
    definition: str
    attemptCount: int
    successfulAttempts: int
    failedAttempts: int
    failures: list[FailureReason]
    accessPaths: list[AccessPathSummary]
    cells: list[ServiceCell]


class RetentionCell(BaseModel):
    offset: int
    users: int | None
    rate: float | None


class RetentionCohort(BaseModel):
    month: str
    size: int
    cells: list[RetentionCell]


class RetentionAnalysis(BaseModel):
    scopeLabel: str
    definition: str
    observationEnd: str
    cohorts: list[RetentionCohort]


class SessionSegment(BaseModel):
    siteType: str
    userSegment: str
    batteryCapacityBand: str
    connectorType: str
    sessionCount: int
    energyKwh: float
    meanChargeMinutes: float | None
    meanOverstayMinutes: float | None


class CorrelationCell(BaseModel):
    x: str
    y: str
    value: float | None
    n: int


class AdvancedAnalytics(BaseModel):
    provenance: AdvancedProvenance
    scope: AdvancedScope
    stationTypes: list[str]
    summary: AdvancedSummary
    heatmap: list[DemandCell]
    stations: list[StationEfficiency]
    weather: list[WeatherCell]
    flow: AttemptFlow
    behavior: RechargeBehavior
    service: ServiceBottlenecks
    retention: RetentionAnalysis
    segments: list[SessionSegment]
    correlations: list[CorrelationCell]
    insights: list[str]
