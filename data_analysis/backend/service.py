"""Queries over fixed serving tables; no caller-controlled SQL identifiers."""

from datetime import timedelta
import re

from data_analysis.contracts.model import PredictionContext

from .database import business_date
from .errors import ApiError

STATE_COLUMNS = [f"{state}_samples" for state in ("available", "charging", "reserved", "occupied", "maintenance", "offline")]
DAILY_COLUMNS = ["energy_wh", "grid_cost_cents", "completed_sessions", "paid_cents", "refund_cents",
    "operating_cost_cents", "maintenance_cost_cents", "net_paid_cents", "started_sessions"] + STATE_COLUMNS + [
    "sample_count", "expected_sample_count", "missing_sample_count", "observed_hours", "complete_hours",
    "incomplete_hours", "complete_charging_samples", "complete_sample_count"]
COHORT_COLUMNS = ["reservations_created_count", "reservation_confirmed_count", "reservation_cancelled_count",
    "reservation_expired_count", "reservation_other_status_count", "queues_joined_count", "queue_served_count",
    "queue_abandoned_count", "queue_call_expired_count", "queue_waiting_count", "queue_other_status_count"]
SERVICE_COLUMNS = ["queues_resolved_count", "queue_wait_count", "queue_wait_seconds_sum", "queue_sojourn_seconds_sum",
    "queue_invalid_timing_count", "repairs_restored_count", "repair_resolution_count", "repair_resolution_seconds_sum",
    "repair_work_count", "repair_work_seconds_sum", "repair_invalid_timing_count", "rating_count", "rating_sum", "invalid_rating_count"]
COHORT_SEMANTICS = "FINAL_OUTCOME_IN_BATCH_BY_CREATED_OR_JOINED_DATE"


def camel(name):
    first, *rest = name.split("_")
    return first + "".join(part.title() for part in rest)


def ratio(numerator, denominator):
    return numerator / denominator if denominator else None


def check_batch(snapshot, query):
    meta = snapshot.metadata
    if query.datasetId is not None and query.datasetId != meta["datasetId"]:
        raise ApiError(404, "DATASET_NOT_FOUND", "指定的数据集尚未发布")
    if query.publishedBatchId is not None and query.publishedBatchId != meta["publishedBatchId"]:
        raise ApiError(409, "BATCH_MISMATCH", "发布批次已变化，请重新选择数据批次")


def check_filters(snapshot, query):
    check_batch(snapshot, query)
    meta = snapshot.metadata
    start = query.startDate or meta["startDate"]
    end = query.endDate or meta["endDate"]
    try:
        first, last = business_date(start), business_date(end)
    except ValueError as exc:
        raise ApiError(422, "INVALID_ARGUMENT", "日期必须是有效的 YYYY-MM-DD") from exc
    if first >= last:
        raise ApiError(422, "INVALID_DATE_RANGE", "startDate 必须早于 endDate，endDate 不包含当天")
    if first < business_date(meta["startDate"]) or last > business_date(meta["endDate"]):
        raise ApiError(422, "DATE_OUT_OF_RANGE", "查询日期超出已发布数据范围")
    if query.cityId is not None and not snapshot.one("SELECT city_id FROM cities WHERE city_id = ?", (query.cityId,)):
        raise ApiError(404, "CITY_NOT_FOUND", "指定城市不存在")
    if query.stationId is not None:
        station = snapshot.one("SELECT city_id FROM station_snapshot WHERE station_id = ?", (query.stationId,))
        if station is None:
            raise ApiError(404, "STATION_NOT_FOUND", "指定电站不存在")
        if query.cityId is not None and station["city_id"] != query.cityId:
            raise ApiError(400, "FILTER_MISMATCH", "电站不属于所选城市")
    return dict(start=start, end=end, city=query.cityId, station=query.stationId)


def where(filters, dates=True, alias=""):
    prefix = alias + "." if alias else ""
    clauses, values = [], []
    if dates:
        clauses += [f"{prefix}business_date >= ?", f"{prefix}business_date < ?"]
        values += [filters["start"], filters["end"]]
    for key, field in (("city", "city_id"), ("station", "station_id")):
        if filters[key] is not None:
            clauses.append(f"{prefix}{field} = ?")
            values.append(filters[key])
    return " AND ".join(clauses) or "1=1", values


def sums(snapshot, table, columns, filters):
    # Only module-owned constants call this function; identifiers never come
    # from a request, even for ORDER BY or chart selection.
    clause, values = where(filters)
    fields = ", ".join(f"COALESCE(SUM({column}), 0) AS {column}" for column in columns)
    return snapshot.one(f"SELECT {fields} FROM {table} WHERE {clause}", values)


def user_counts(snapshot, filters):
    clause, values = where(filters)
    return snapshot.one(f"""SELECT COUNT(*) AS active_users,
        COALESCE(SUM(CASE WHEN session_total >= 2 THEN 1 ELSE 0 END), 0) AS repeat_users
        FROM (SELECT user_id, SUM(session_count) AS session_total FROM user_activity_daily
              WHERE {clause} GROUP BY user_id)""", values)


def overview(snapshot, filters):
    daily = sums(snapshot, "station_metrics_daily", DAILY_COLUMNS, filters)
    cohort = sums(snapshot, "station_cohorts_daily", COHORT_COLUMNS, filters)
    service = sums(snapshot, "station_service_daily", SERVICE_COLUMNS, filters)
    users = user_counts(snapshot, filters)
    clause, values = where(filters, dates=False)
    dimensions = snapshot.one(f"SELECT COUNT(*) AS station_count, COALESCE(SUM(capacity), 0) AS charger_count FROM station_snapshot WHERE {clause}", values)
    hourly_clause, hourly_values = where(filters)
    occupied = snapshot.one(f"""SELECT COALESCE(SUM(CASE WHEN is_complete = 1 THEN occupied_samples ELSE 0 END), 0)
        AS complete_occupied_samples FROM station_hourly_metrics WHERE {hourly_clause}""", hourly_values)
    metrics = {camel(key): value for item in (daily, cohort, service, users, dimensions) for key, value in item.items()}
    metrics.update(chargingUtilizationRate=ratio(daily["complete_charging_samples"], daily["complete_sample_count"]),
        chargingAndOccupiedRate=ratio(daily["complete_charging_samples"]+occupied["complete_occupied_samples"], daily["complete_sample_count"]),
        completeOccupiedSamples=occupied["complete_occupied_samples"],
        repeatUserRate=ratio(users["repeat_users"], users["active_users"]),
        queueServedRate=ratio(cohort["queue_served_count"], cohort["queues_joined_count"]),
        queueMeanWaitSeconds=ratio(service["queue_wait_seconds_sum"], service["queue_wait_count"]),
        queueMeanSojournSeconds=ratio(service["queue_sojourn_seconds_sum"], service["queue_wait_count"]),
        meanRepairResolutionSeconds=ratio(service["repair_resolution_seconds_sum"], service["repair_resolution_count"]),
        meanRepairWorkSeconds=ratio(service["repair_work_seconds_sum"], service["repair_work_count"]),
        meanRating=ratio(service["rating_sum"], service["rating_count"]),
        cashContributionCents=daily["net_paid_cents"]-daily["grid_cost_cents"]-daily["operating_cost_cents"]-daily["maintenance_cost_cents"])
    if not daily["observed_hours"]:
        metrics["energyWh"] = None
    return dict(startDate=filters["start"], endDate=filters["end"], source=snapshot.metadata["source"], metrics=metrics,
        units={"energyWh": "Wh", "*Cents": "CNY cents", "*Seconds": "seconds", "*Rate": "fraction 0..1; null for empty denominator",
               "cashContributionCents": "modeled cash contribution, excludes capex/tax/depreciation/demand fees",
               "chargingUtilizationRate": "complete-hour CHARGING samples / all pile samples in complete hours",
               "cohortOutcomeSemantics": COHORT_SEMANTICS,
               "queueMeanWaitSeconds": "joined→first called, or joined→resolved if never called; resolved-event cohort"})


def page(items, total, query):
    return dict(items=items, page=query.page, pageSize=query.pageSize, total=total,
                hasNext=query.page*query.pageSize < total)


def station_list(snapshot, filters, query):
    sorting = {"id": "s.station_id", "name": "s.station_name", "capacity": "s.capacity", "city": "s.city_id",
               "energy": "period_energy_wh", "netPaid": "period_net_paid_cents", "utilization": "period_utilization"}
    if query.sortBy not in sorting:
        raise ApiError(422, "INVALID_SORT", "电站排序仅支持 id、name、capacity、city、energy、netPaid、utilization")
    clause, values = where(filters, dates=False, alias="s")
    daily_clause, daily_values = where(filters)
    total = snapshot.one(f"SELECT COUNT(*) AS n FROM station_snapshot s WHERE {clause}", values)["n"]
    sort = sorting[query.sortBy]
    # Rank the whole filtered set before pagination. Null observations always
    # sort last (both directions), and station ID resolves equal metric values.
    rows = snapshot.rows(f"""WITH period AS (
            SELECT station_id,
                CASE WHEN SUM(observed_hours) > 0 THEN SUM(energy_wh) ELSE NULL END AS period_energy_wh,
                SUM(net_paid_cents) AS period_net_paid_cents,
                1.0*SUM(complete_charging_samples)/NULLIF(SUM(complete_sample_count), 0) AS period_utilization
            FROM station_metrics_daily WHERE {daily_clause} GROUP BY station_id)
        SELECT s.*, p.period_energy_wh, COALESCE(p.period_net_paid_cents, 0) AS period_net_paid_cents,
               p.period_utilization
        FROM station_snapshot s LEFT JOIN period p ON p.station_id = s.station_id
        WHERE {clause} ORDER BY ({sort} IS NULL) ASC, {sort} {query.sortOrder.upper()}, s.station_id ASC LIMIT ? OFFSET ?""",
        daily_values+values+[query.pageSize, (query.page-1)*query.pageSize])
    # Snapshot is a batch-end observation, NOT fabricated historical realtime.
    items = []
    for row in rows:
        energy = row.pop("period_energy_wh")
        net_paid = row.pop("period_net_paid_cents")
        utilization = row.pop("period_utilization")
        result = {camel(key): bool(value) if key in {"is_current", "is_complete"} else value for key, value in row.items()}
        result["periodMetrics"] = dict(startDate=filters["start"], endDate=filters["end"],
            energyWh=energy, netPaidCents=net_paid, chargingUtilizationRate=utilization)
        result["snapshotSemantics"] = "LATEST_IN_BATCH"
        items.append(result)
    return page(items, total, query)


def city_list(snapshot, filters, query):
    sorting = {"id": "city_id", "name": "city_name"}
    if query.sortBy not in sorting:
        raise ApiError(422, "INVALID_SORT", "城市排序仅支持 id、name")
    # A station filter selects its parent city, using the verified relation.
    city_id = filters["city"]
    if filters["station"]:
        city_id = snapshot.one("SELECT city_id FROM station_snapshot WHERE station_id = ?", (filters["station"],))["city_id"]
    clause, values = ("city_id = ?", [city_id]) if city_id is not None else ("1=1", [])
    total = snapshot.one(f"SELECT COUNT(*) AS n FROM cities WHERE {clause}", values)["n"]
    rows = snapshot.rows(f"SELECT city_id, city_name, latitude, longitude, timezone FROM cities WHERE {clause} ORDER BY {sorting[query.sortBy]} {query.sortOrder.upper()}, city_id ASC LIMIT ? OFFSET ?",
                         values+[query.pageSize, (query.page-1)*query.pageSize])
    return page([{camel(key): value for key, value in row.items()} for row in rows], total, query)


def _chart_row(row, columns):
    result = {"time": row["time"], **{camel(key): row[key] for key in columns}}
    if "complete_sample_count" in row:
        result["chargingUtilizationRate"] = ratio(row["complete_charging_samples"], row["complete_sample_count"])
    if "queue_wait_count" in row:
        result["queueMeanWaitSeconds"] = ratio(row["queue_wait_seconds_sum"], row["queue_wait_count"])
        result["meanRating"] = ratio(row["rating_sum"], row["rating_count"])
    return result


def charts(snapshot, filters, query):
    if query.chart == "load" and query.granularity != "hour":
        raise ApiError(422, "INVALID_ARGUMENT", "负荷趋势仅支持小时粒度")
    if query.granularity == "hour" and query.chart not in {"energy", "utilization", "states", "load"}:
        raise ApiError(422, "INVALID_ARGUMENT", "该图表只支持按日统计")
    clause, values = where(filters)
    if query.chart == "load":
        dimension_clause, dimension_values = where(filters, dates=False)
        dimension = snapshot.one(f"SELECT COUNT(*) AS station_count, COALESCE(SUM(capacity),0) AS capacity FROM station_snapshot WHERE {dimension_clause}", dimension_values)
        rows = snapshot.rows(f"""SELECT recorded_at AS time, SUM(mean_power_kw) AS total_power,
            COUNT(*) AS row_count, COUNT(mean_power_kw) AS observed_station_count,
            SUM(CASE WHEN is_complete=1 AND mean_power_kw IS NOT NULL THEN 1 ELSE 0 END) AS complete_station_count,
            SUM(sample_count) AS sample_count, SUM(expected_sample_count) AS expected_sample_count,
            SUM(missing_sample_count) AS missing_sample_count
            FROM station_hourly_metrics WHERE {clause} GROUP BY recorded_at ORDER BY recorded_at LIMIT ?""", values+[query.limit+1])
        items = []
        for row in rows[:query.limit]:
            complete = row["row_count"] == dimension["station_count"] == row["complete_station_count"]
            items.append(dict(time=row["time"], meanPowerKw=row["total_power"] if complete else None,
                capacity=dimension["capacity"], stationCount=dimension["station_count"],
                observedStationCount=row["observed_station_count"], completeStationCount=row["complete_station_count"],
                incompleteStationCount=dimension["station_count"]-row["complete_station_count"],
                sampleCount=row["sample_count"], expectedSampleCount=row["expected_sample_count"],
                missingSampleCount=row["missing_sample_count"], isComplete=complete))
    elif query.granularity == "hour":
        columns = ["energy_wh", "sample_count", "expected_sample_count", "missing_sample_count"]+STATE_COLUMNS
        projection = ", ".join(f"SUM({key}) AS {key}" for key in columns)
        projection += ", SUM(CASE WHEN is_complete = 1 THEN charging_samples ELSE 0 END) AS complete_charging_samples, SUM(CASE WHEN is_complete = 1 THEN sample_count ELSE 0 END) AS complete_sample_count"
        rows = snapshot.rows(f"SELECT recorded_at AS time, {projection} FROM station_hourly_metrics WHERE {clause} GROUP BY recorded_at ORDER BY recorded_at ASC LIMIT ?",
                             values+[query.limit+1])
        columns += ["complete_charging_samples", "complete_sample_count"]
        items = [_chart_row(row, columns) for row in rows[:query.limit]]
    else:
        if query.chart == "cohorts":
            table, columns = "station_cohorts_daily", COHORT_COLUMNS
        elif query.chart == "service":
            table, columns = "station_service_daily", SERVICE_COLUMNS
        else:
            table, columns = "station_metrics_daily", DAILY_COLUMNS
        projection = ", ".join(f"SUM({key}) AS {key}" for key in columns)
        rows = snapshot.rows(f"SELECT business_date AS time, {projection} FROM {table} WHERE {clause} GROUP BY business_date ORDER BY business_date ASC LIMIT ?",
                             values+[query.limit+1])
        if query.chart in {"cohorts", "service"}:
            # These are sparse event tables: no row really means no events,
            # unlike missing telemetry, which must never become zero load.
            by_date = {row["time"]: row for row in rows}
            count = (business_date(filters["end"])-business_date(filters["start"])).days
            dates = [(business_date(filters["start"])+timedelta(days=i)).isoformat() for i in range(min(count, query.limit))]
            items = [_chart_row(by_date.get(day, dict(time=day, **dict.fromkeys(columns, 0))), columns) for day in dates]
            truncated = count > query.limit
        else:
            items = []
            for row in rows[:query.limit]:
                if not row["observed_hours"]:
                    row["energy_wh"] = None
                items.append(_chart_row(row, columns))
            truncated = len(rows) > query.limit
    if query.granularity == "hour":
        truncated = len(rows) > query.limit
    return dict(chart=query.chart, granularity=query.granularity, startDate=filters["start"], endDate=filters["end"],
        items=items, limit=query.limit, truncated=truncated)


def validate_prediction(snapshot, body):
    check_batch(snapshot, body)
    station = snapshot.one("SELECT city_id FROM station_snapshot WHERE station_id = ?", (body.stationId,))
    if station is None:
        raise ApiError(404, "STATION_NOT_FOUND", "指定电站不存在")
    if body.cityId is not None and body.cityId != station["city_id"]:
        raise ApiError(400, "FILTER_MISMATCH", "电站不属于所选城市")
    try:
        PredictionContext(dataset_id=snapshot.metadata["datasetId"],
            published_batch_id=snapshot.metadata["publishedBatchId"], station_id=body.stationId,
            reference_time=body.referenceTime, horizon_hours=body.horizonHours, model_id=body.modelId)
    except ValueError as exc:
        raise ApiError(422, "INVALID_ARGUMENT", "referenceTime 必须是以 Z 结尾的 UTC 整点，modelId 不可为空白") from exc
    # Do not enforce history-only bounds on a forecasting instant. Capability
    # remains explicit until a trained, versioned model adapter is integrated.
    raise ApiError(503, "MODEL_NOT_READY", "模型尚未训练或发布，当前不提供预测结果",
                   {"implementedPrediction": False, "models": [], "supportedTargets": ["load", "availability"], "status": "MODEL_NOT_READY"})


def quality_summary(source):
    """Allow-list public counts only; never expose source/output paths or raw JSON."""
    def number(value):
        return value if type(value) is int and value >= 0 else None

    def count_map(value):
        if not isinstance(value, dict):
            return {}
        return {key: count for key, count in sorted(value.items())
                if isinstance(key, str) and re.fullmatch(r"[A-Za-z0-9_]{1,128}", key) and number(count) is not None}

    counts = count_map(source.get("input_rows"))
    normalized = count_map(source.get("normalized_enum_rows"))
    reasons = count_map(source.get("rejection_reasons"))
    raw, rejected = sum(counts.values()) if counts else None, number(source.get("rejected_session_rows"))
    samples_source = source.get("rejection_samples", [])
    if not samples_source and isinstance(source.get("sanitizedQuality"), dict):
        samples_source = source["sanitizedQuality"].get("rejection_samples", [])
    samples = []
    for row in samples_source[:100] if isinstance(samples_source, list) else []:
        if not isinstance(row, dict):
            continue
        sid, reason = row.get("session_id"), row.get("rejection_reason")
        if (isinstance(sid, str) and re.fullmatch(r"[A-Za-z0-9_-]{1,128}", sid)
                and isinstance(reason, str) and re.fullmatch(r"[A-Za-z0-9_]{1,128}", reason)):
            samples.append(dict(sessionId=sid, rejectionReason=reason))
    return dict(rawRows=raw, cleanRows=raw-rejected if raw is not None and rejected is not None and raw >= rejected else None,
        rejectedRows=rejected, normalizedRows=sum(normalized.values()) if normalized else None,
        cleanSessionRows=number(source.get("clean_session_rows")),
        tables=[dict(tableName=key, rawRows=count) for key, count in counts.items()],
        normalizedByTable=[dict(tableName=key, normalizedRows=count) for key, count in normalized.items()],
        rejectionReasons=[dict(reason=key, rowCount=count) for key, count in reasons.items()],
        rejectionSamples=samples, rejectionSampleLimit=100,
        normalizationSemantics="Rows with at least one enum trim/uppercase/empty-to-NULL change; counted per table, not cells or necessarily corrupt rows")
