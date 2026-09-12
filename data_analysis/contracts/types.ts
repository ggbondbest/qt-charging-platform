// Generated from contracts/openapi.json. Regenerate; do not hand-edit.
// Amounts use integer CNY cents, energy Wh; date ranges are [startDate,endDate).

export type Chart = { "chart": string; "endDate": string; "granularity": string; "items": Array<(DailyPoint | HourlyPoint | CohortPoint | ServicePoint | LoadPoint)>; "limit": number; "startDate": string; "truncated": boolean };
export type City = { "cityId": string; "cityName": string; "latitude": number; "longitude": number; "timezone": string };
export type CohortPoint = { "queueAbandonedCount": (number | number | null); "queueCallExpiredCount": (number | number | null); "queueOtherStatusCount": (number | number | null); "queueServedCount": (number | number | null); "queueWaitingCount": (number | number | null); "queuesJoinedCount": (number | number | null); "reservationCancelledCount": (number | number | null); "reservationConfirmedCount": (number | number | null); "reservationExpiredCount": (number | number | null); "reservationOtherStatusCount": (number | number | null); "reservationsCreatedCount": (number | number | null); "time": string };
export type DailyPoint = { "availableSamples": (number | number | null); "chargingSamples": (number | number | null); "chargingUtilizationRate": (number | number | null); "completeChargingSamples": (number | number | null); "completeHours": (number | number | null); "completeSampleCount": (number | number | null); "completedSessions": (number | number | null); "energyWh": (number | number | null); "expectedSampleCount": (number | number | null); "gridCostCents": (number | number | null); "incompleteHours": (number | number | null); "maintenanceCostCents": (number | number | null); "maintenanceSamples": (number | number | null); "missingSampleCount": (number | number | null); "netPaidCents": (number | number | null); "observedHours": (number | number | null); "occupiedSamples": (number | number | null); "offlineSamples": (number | number | null); "operatingCostCents": (number | number | null); "paidCents": (number | number | null); "refundCents": (number | number | null); "reservedSamples": (number | number | null); "sampleCount": (number | number | null); "startedSessions": (number | number | null); "time": string };
export type Dataset = { "datasetId": string; "endDate": string; "generatedAt": string; "pipelineRunId": string; "publishedBatchId": string; "schemaVersion": string; "source": "SIMULATED"; "sourceManifestSha256": string; "startDate": string };
export type Envelope_Chart_ = { "code": string; "data": (Chart | null); "message": string; "meta": Meta };
export type Envelope_Health_ = { "code": string; "data": (Health | null); "message": string; "meta": Meta };
export type Envelope_ModelCapabilities_ = { "code": string; "data": (ModelCapabilities | null); "message": string; "meta": Meta };
export type Envelope_Overview_ = { "code": string; "data": (Overview | null); "message": string; "meta": Meta };
export type Envelope_Page_City__ = { "code": string; "data": (Page_City_ | null); "message": string; "meta": Meta };
export type Envelope_Page_Dataset__ = { "code": string; "data": (Page_Dataset_ | null); "message": string; "meta": Meta };
export type Envelope_Page_PipelineRun__ = { "code": string; "data": (Page_PipelineRun_ | null); "message": string; "meta": Meta };
export type Envelope_Page_Station__ = { "code": string; "data": (Page_Station_ | null); "message": string; "meta": Meta };
export type Envelope_PredictionResult_ = { "code": string; "data": (PredictionResult | null); "message": string; "meta": Meta };
export type Envelope_dict_ = { "code": string; "data": (Record<string, unknown> | null); "message": string; "meta": Meta };
export type Health = { "dataReady": boolean; "source": "SIMULATED"; "status": "ready" };
export type HourlyPoint = { "availableSamples": (number | number | null); "chargingSamples": (number | number | null); "chargingUtilizationRate": (number | number | null); "completeChargingSamples": (number | number | null); "completeSampleCount": (number | number | null); "energyWh": (number | number | null); "expectedSampleCount": (number | number | null); "maintenanceSamples": (number | number | null); "missingSampleCount": (number | number | null); "occupiedSamples": (number | number | null); "offlineSamples": (number | number | null); "reservedSamples": (number | number | null); "sampleCount": (number | number | null); "time": string };
export type LoadPoint = { "capacity": number; "completeStationCount": number; "expectedSampleCount": number; "incompleteStationCount": number; "isComplete": boolean; "meanPowerKw": (number | null); "missingSampleCount": number; "observedStationCount": number; "sampleCount": number; "stationCount": number; "time": string };
export type Meta = { "datasetId"?: (string | null); "generatedAt"?: (string | null); "pipelineRunId"?: (string | null); "publishedBatchId"?: (string | null); "requestId": string; "schemaVersion"?: string };
export type Metrics = { "activeUsers": (number | number | null); "availableSamples": (number | number | null); "cashContributionCents": (number | number | null); "chargerCount": (number | number | null); "chargingAndOccupiedRate": (number | number | null); "chargingSamples": (number | number | null); "chargingUtilizationRate": (number | number | null); "completeChargingSamples": (number | number | null); "completeHours": (number | number | null); "completeOccupiedSamples": (number | number | null); "completeSampleCount": (number | number | null); "completedSessions": (number | number | null); "energyWh": (number | number | null); "expectedSampleCount": (number | number | null); "gridCostCents": (number | number | null); "incompleteHours": (number | number | null); "invalidRatingCount": (number | number | null); "maintenanceCostCents": (number | number | null); "maintenanceSamples": (number | number | null); "meanRating": (number | number | null); "meanRepairResolutionSeconds": (number | number | null); "meanRepairWorkSeconds": (number | number | null); "missingSampleCount": (number | number | null); "netPaidCents": (number | number | null); "observedHours": (number | number | null); "occupiedSamples": (number | number | null); "offlineSamples": (number | number | null); "operatingCostCents": (number | number | null); "paidCents": (number | number | null); "queueAbandonedCount": (number | number | null); "queueCallExpiredCount": (number | number | null); "queueInvalidTimingCount": (number | number | null); "queueMeanSojournSeconds": (number | number | null); "queueMeanWaitSeconds": (number | number | null); "queueOtherStatusCount": (number | number | null); "queueServedCount": (number | number | null); "queueServedRate": (number | number | null); "queueSojournSecondsSum": (number | number | null); "queueWaitCount": (number | number | null); "queueWaitSecondsSum": (number | number | null); "queueWaitingCount": (number | number | null); "queuesJoinedCount": (number | number | null); "queuesResolvedCount": (number | number | null); "ratingCount": (number | number | null); "ratingSum": (number | number | null); "refundCents": (number | number | null); "repairInvalidTimingCount": (number | number | null); "repairResolutionCount": (number | number | null); "repairResolutionSecondsSum": (number | number | null); "repairWorkCount": (number | number | null); "repairWorkSecondsSum": (number | number | null); "repairsRestoredCount": (number | number | null); "repeatUserRate": (number | number | null); "repeatUsers": (number | number | null); "reservationCancelledCount": (number | number | null); "reservationConfirmedCount": (number | number | null); "reservationExpiredCount": (number | number | null); "reservationOtherStatusCount": (number | number | null); "reservationsCreatedCount": (number | number | null); "reservedSamples": (number | number | null); "sampleCount": (number | number | null); "startedSessions": (number | number | null); "stationCount": (number | number | null) };
export type ModelCapabilities = { "implementedPrediction"?: boolean; "models"?: Array<PublishedModel>; "status"?: string; "supportedTargets"?: Array<string> };
export type Overview = { "endDate": string; "metrics": Metrics; "source": string; "startDate": string; "units": Record<string, string> };
export type Page_City_ = { "hasNext": boolean; "items": Array<City>; "page": number; "pageSize": number; "total": number };
export type Page_Dataset_ = { "hasNext": boolean; "items": Array<Dataset>; "page": number; "pageSize": number; "total": number };
export type Page_PipelineRun_ = { "hasNext": boolean; "items": Array<PipelineRun>; "page": number; "pageSize": number; "total": number };
export type Page_Station_ = { "hasNext": boolean; "items": Array<Station>; "page": number; "pageSize": number; "total": number };
export type PeriodMetrics = { "chargingUtilizationRate": (number | null); "endDate": string; "energyWh": (number | null); "netPaidCents": number; "startDate": string };
export type PipelineRun = { "generatedAt": string; "pipelineRunId": string; "publishedBatchId": string; "quality": QualitySummary; "status": "PUBLISHED" };
export type PredictionPoint = { "timestamp": string; "value": number };
export type PredictionRequest = { "cityId"?: (string | null); "datasetId"?: (string | null); "horizonHours"?: 1 | 6 | 24; "modelId": string; "publishedBatchId"?: (string | null); "referenceTime": string; "stationId": string };
export type PredictionResult = { "featureVersion": "history24-v1"; "modelId": string; "modelVersion": string; "points": Array<PredictionPoint>; "schemaVersion": "1.0.0"; "unit": "kW" | "chargers" };
export type PublishedModel = { "modelId": string; "modelVersion": string; "target": "load" | "availability" };
export type QualitySummary = { "cleanRows": (number | null); "cleanSessionRows": (number | null); "normalizationSemantics": string; "normalizedByTable": Array<TableNormalizationCount>; "normalizedRows": (number | null); "rawRows": (number | null); "rejectedRows": (number | null); "rejectionReasons": Array<RejectionReason>; "rejectionSampleLimit": number; "rejectionSamples": Array<RejectionSample>; "tables": Array<TableRowCount> };
export type RejectionReason = { "reason": string; "rowCount": number };
export type RejectionSample = { "rejectionReason": string; "sessionId": string };
export type ServicePoint = { "invalidRatingCount": (number | number | null); "meanRating": (number | number | null); "queueInvalidTimingCount": (number | number | null); "queueMeanWaitSeconds": (number | number | null); "queueSojournSecondsSum": (number | number | null); "queueWaitCount": (number | number | null); "queueWaitSecondsSum": (number | number | null); "queuesResolvedCount": (number | number | null); "ratingCount": (number | number | null); "ratingSum": (number | number | null); "repairInvalidTimingCount": (number | number | null); "repairResolutionCount": (number | number | null); "repairResolutionSecondsSum": (number | number | null); "repairWorkCount": (number | number | null); "repairWorkSecondsSum": (number | number | null); "repairsRestoredCount": (number | number | null); "time": string };
export type Station = { "availableCount": number; "capacity": number; "chargingCount": number; "cityId": string; "cityLatitude": number; "cityLongitude": number; "cityName": string; "dataAsOf": (string | null); "isComplete": boolean; "isCurrent": boolean; "latitude": number; "longitude": number; "maintenanceCount": number; "observedPileCount": number; "occupiedCount": number; "offlineCount": number; "periodMetrics": PeriodMetrics; "ratedCapacityKw": number; "reservedCount": number; "siteType": string; "snapshotAt": (string | null); "snapshotSemantics": "LATEST_IN_BATCH"; "stationId": string; "stationName": string; "transformerKw": number; "unknownCount": number };
export type TableNormalizationCount = { "normalizedRows": number; "tableName": string };
export type TableRowCount = { "rawRows": number; "tableName": string };

// GET /api/v1/cities
export type listCitiesQuery = { "datasetId"?: (string | null); "publishedBatchId"?: (string | null); "cityId"?: (string | null); "stationId"?: (string | null); "startDate"?: (string | null); "endDate"?: (string | null); "page"?: number; "pageSize"?: number; "sortBy"?: string; "sortOrder"?: "asc" | "desc" };
export type listCitiesResponse200 = Envelope_Page_City__;
export type listCitiesResponse400 = Envelope_dict_;
export type listCitiesResponse404 = Envelope_dict_;
export type listCitiesResponse409 = Envelope_dict_;
export type listCitiesResponse422 = Envelope_dict_;
export type listCitiesResponse500 = Envelope_dict_;
export type listCitiesResponse503 = Envelope_dict_;

// GET /api/v1/dashboard/charts
export type dashboardChartsQuery = { "datasetId"?: (string | null); "publishedBatchId"?: (string | null); "cityId"?: (string | null); "stationId"?: (string | null); "startDate"?: (string | null); "endDate"?: (string | null); "chart"?: "energy" | "revenue" | "utilization" | "states" | "service" | "cohorts" | "load"; "granularity"?: "day" | "hour"; "limit"?: number };
export type dashboardChartsResponse200 = Envelope_Chart_;
export type dashboardChartsResponse400 = Envelope_dict_;
export type dashboardChartsResponse404 = Envelope_dict_;
export type dashboardChartsResponse409 = Envelope_dict_;
export type dashboardChartsResponse422 = Envelope_dict_;
export type dashboardChartsResponse500 = Envelope_dict_;
export type dashboardChartsResponse503 = Envelope_dict_;

// GET /api/v1/dashboard/overview
export type dashboardOverviewQuery = { "datasetId"?: (string | null); "publishedBatchId"?: (string | null); "cityId"?: (string | null); "stationId"?: (string | null); "startDate"?: (string | null); "endDate"?: (string | null) };
export type dashboardOverviewResponse200 = Envelope_Overview_;
export type dashboardOverviewResponse400 = Envelope_dict_;
export type dashboardOverviewResponse404 = Envelope_dict_;
export type dashboardOverviewResponse409 = Envelope_dict_;
export type dashboardOverviewResponse422 = Envelope_dict_;
export type dashboardOverviewResponse500 = Envelope_dict_;
export type dashboardOverviewResponse503 = Envelope_dict_;

// GET /api/v1/datasets
export type listDatasetsQuery = { "datasetId"?: (string | null); "publishedBatchId"?: (string | null); "cityId"?: (string | null); "stationId"?: (string | null); "startDate"?: (string | null); "endDate"?: (string | null); "page"?: number; "pageSize"?: number; "sortBy"?: string; "sortOrder"?: "asc" | "desc" };
export type listDatasetsResponse200 = Envelope_Page_Dataset__;
export type listDatasetsResponse400 = Envelope_dict_;
export type listDatasetsResponse404 = Envelope_dict_;
export type listDatasetsResponse409 = Envelope_dict_;
export type listDatasetsResponse422 = Envelope_dict_;
export type listDatasetsResponse500 = Envelope_dict_;
export type listDatasetsResponse503 = Envelope_dict_;

// GET /api/v1/health
export type healthQuery = { "datasetId"?: (string | null); "publishedBatchId"?: (string | null) };
export type healthResponse200 = Envelope_Health_;
export type healthResponse400 = Envelope_dict_;
export type healthResponse404 = Envelope_dict_;
export type healthResponse409 = Envelope_dict_;
export type healthResponse422 = Envelope_dict_;
export type healthResponse500 = Envelope_dict_;
export type healthResponse503 = Envelope_dict_;

// GET /api/v1/models
export type modelCapabilitiesQuery = { "datasetId"?: (string | null); "publishedBatchId"?: (string | null) };
export type modelCapabilitiesResponse200 = Envelope_ModelCapabilities_;
export type modelCapabilitiesResponse400 = Envelope_dict_;
export type modelCapabilitiesResponse404 = Envelope_dict_;
export type modelCapabilitiesResponse409 = Envelope_dict_;
export type modelCapabilitiesResponse422 = Envelope_dict_;
export type modelCapabilitiesResponse500 = Envelope_dict_;
export type modelCapabilitiesResponse503 = Envelope_dict_;

// GET /api/v1/pipeline/runs
export type listPipelineRunsQuery = { "datasetId"?: (string | null); "publishedBatchId"?: (string | null); "cityId"?: (string | null); "stationId"?: (string | null); "startDate"?: (string | null); "endDate"?: (string | null); "page"?: number; "pageSize"?: number; "sortBy"?: string; "sortOrder"?: "asc" | "desc" };
export type listPipelineRunsResponse200 = Envelope_Page_PipelineRun__;
export type listPipelineRunsResponse400 = Envelope_dict_;
export type listPipelineRunsResponse404 = Envelope_dict_;
export type listPipelineRunsResponse409 = Envelope_dict_;
export type listPipelineRunsResponse422 = Envelope_dict_;
export type listPipelineRunsResponse500 = Envelope_dict_;
export type listPipelineRunsResponse503 = Envelope_dict_;

// POST /api/v1/predict/availability
export type predictAvailabilityNotReadyQuery = Record<string, unknown>;
export type predictAvailabilityNotReadyBody = PredictionRequest;
export type predictAvailabilityNotReadyResponse200 = Envelope_PredictionResult_;
export type predictAvailabilityNotReadyResponse400 = Envelope_dict_;
export type predictAvailabilityNotReadyResponse404 = Envelope_dict_;
export type predictAvailabilityNotReadyResponse409 = Envelope_dict_;
export type predictAvailabilityNotReadyResponse422 = Envelope_dict_;
export type predictAvailabilityNotReadyResponse500 = Envelope_dict_;
export type predictAvailabilityNotReadyResponse503 = Envelope_ModelCapabilities_;

// POST /api/v1/predict/load
export type predictLoadNotReadyQuery = Record<string, unknown>;
export type predictLoadNotReadyBody = PredictionRequest;
export type predictLoadNotReadyResponse200 = Envelope_PredictionResult_;
export type predictLoadNotReadyResponse400 = Envelope_dict_;
export type predictLoadNotReadyResponse404 = Envelope_dict_;
export type predictLoadNotReadyResponse409 = Envelope_dict_;
export type predictLoadNotReadyResponse422 = Envelope_dict_;
export type predictLoadNotReadyResponse500 = Envelope_dict_;
export type predictLoadNotReadyResponse503 = Envelope_ModelCapabilities_;

// GET /api/v1/stations
export type listStationsQuery = { "datasetId"?: (string | null); "publishedBatchId"?: (string | null); "cityId"?: (string | null); "stationId"?: (string | null); "startDate"?: (string | null); "endDate"?: (string | null); "page"?: number; "pageSize"?: number; "sortBy"?: string; "sortOrder"?: "asc" | "desc" };
export type listStationsResponse200 = Envelope_Page_Station__;
export type listStationsResponse400 = Envelope_dict_;
export type listStationsResponse404 = Envelope_dict_;
export type listStationsResponse409 = Envelope_dict_;
export type listStationsResponse422 = Envelope_dict_;
export type listStationsResponse500 = Envelope_dict_;
export type listStationsResponse503 = Envelope_dict_;
