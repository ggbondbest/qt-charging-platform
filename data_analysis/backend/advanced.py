"""Bounded rollups of immutable Spark aggregates, pinned to the MySQL batch.

No raw business rows, user identifiers or model labels are loaded by a request.
Ratios are formed AFTER summing their numerators/denominators. A missing or
incompatible bundle fails closed; this endpoint never supplies mock fallbacks.
"""

from collections import defaultdict
from datetime import date
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import threading

from .errors import ApiError
from .service import ratio

DEFAULT_BUNDLE = Path(__file__).resolve().parents[1] / "datasets/advanced_analytics_v2"
TABLES = ("station_day", "station_hour", "attempt_flow", "session_segments", "retention", "user_behavior", "service_hour")
MAX_ROWS = 250_000
MAX_EXPANDED_BYTES = 300_000_000
_lock = threading.Lock()
_cache = {}


def _unavailable():
    return ApiError(503, "DATA_NOT_READY", "多维分析成果未就绪或校验失败，请运行 advanced_analysis 并发布匹配批次")


def load_bundle(metadata, directory=None):
    root = Path(directory or os.environ.get("ANALYTICS_ADVANCED_BUNDLE", DEFAULT_BUNDLE)).resolve()
    try:
        manifest_path = root / "advanced_manifest.json"
        marker = root / "_SUCCESS"
        if not marker.is_file() or marker.is_symlink() or (root / "_RUNNING").exists():
            raise ValueError("Incomplete publication")
        if manifest_path.is_symlink() or manifest_path.stat().st_size > 4_000_000:
            raise ValueError("Invalid manifest")
        raw = manifest_path.read_bytes()
        manifest = json.loads(raw)
        for key in ("datasetId", "publishedBatchId", "pipelineRunId", "sourceManifestSha256"):
            if not manifest.get(key) or manifest[key] != metadata.get(key):
                raise ApiError(409, "BATCH_MISMATCH", "多维分析与当前数据库发布批次不一致，请重新发布分析成果")
        if (manifest.get("schemaVersion") != "1.0.0" or manifest.get("analysisVersion") != "2.0.0"
                or set(manifest["tables"]) != set(TABLES)):
            raise ValueError("Unknown aggregate schema")
        if (manifest.get("engine") != "PySpark" or manifest.get("rawFactsCollected") is not False
                or manifest.get("referenceAggregatesUsedAsInput") is not False
                or manifest.get("authenticatedCleanInventoryVerified") is not True
                or not manifest.get("invariants") or any(value is not True for value in manifest["invariants"].values())):
            raise ValueError("Missing computation evidence")
        paths = []
        for name in TABLES:
            item = manifest["tables"][name]
            path = root / item["file"]
            if path.parent != root or path.is_symlink() or path.suffix != ".gz":
                raise ValueError("Invalid aggregate path")
            stat = path.stat()
            if not 0 <= item["rows"] <= MAX_ROWS or stat.st_size > MAX_EXPANDED_BYTES:
                raise ValueError("Aggregate exceeds serving bounds")
            paths.append((name, path, stat.st_mtime_ns, stat.st_ctime_ns, stat.st_size))
        fingerprint = (str(root), hashlib.sha256(raw).hexdigest(), tuple((n, m, c, s) for n, _, m, c, s in paths))
        with _lock:
            if fingerprint in _cache:
                return _cache[fingerprint]
            tables = {}
            for name, path, *_ in paths:
                compressed = path.read_bytes()
                if hashlib.sha256(compressed).hexdigest() != manifest["tables"][name]["sha256"]:
                    raise ValueError("Aggregate hash mismatch")
                with gzip.open(path, "rb") as stream:
                    data = stream.read(MAX_EXPANDED_BYTES + 1)
                if len(data) > MAX_EXPANDED_BYTES:
                    raise ValueError("Expanded aggregate exceeds serving bounds")
                rows = json.loads(data)
                if not isinstance(rows, list) or len(rows) != manifest["tables"][name]["rows"]:
                    raise ValueError("Aggregate count mismatch")
                if any(not isinstance(row, dict) or "user_id" in row for row in rows):
                    raise ValueError("Invalid aggregate row")
                tables[name] = rows
            result = (manifest, tables)
            _cache.clear()  # One immutable publication per process, bounded memory.
            _cache[fingerprint] = result
            return result
    except ApiError:
        raise
    except (OSError, ValueError, TypeError, KeyError, EOFError):
        raise _unavailable() from None


def numeric(row, key):
    value = row.get(key)
    return value if isinstance(value, (int, float)) and math.isfinite(value) else 0


def grouped(rows, fields):
    result = defaultdict(list)
    for row in rows:
        result[tuple(row[field] for field in fields)].append(row)
    return result


def total(rows, field):
    return sum(numeric(row, field) for row in rows)


def hour_metrics(rows):
    observed = [row for row in rows if row.get("has_observation")]
    complete = [row for row in rows if row.get("is_complete")]
    return dict(energyKwh=total(observed, "energy_wh") / 1000 if observed else None,
                meanPowerKw=ratio(total(complete, "energy_wh") / 1000, len(complete)),
                chargingUtilization=ratio(total(complete, "charging_samples"), total(complete, "sample_count")),
                sampleHours=len(complete))


PATH_LABELS = {"DIRECT": "直接尝试", "QUEUE": "排队入口", "RESERVATION": "预约入口"}
FAILURE_LABELS = {
    "NO_AVAILABLE_CHARGER": "无可用电桩", "QUEUE_PATIENCE": "等候超出耐心",
    "CALL_TIMEOUT": "叫号未确认", "AUTH_FAILED": "身份认证失败",
    "CONNECTOR_HANDSHAKE": "电桩握手失败", "APP_TIMEOUT": "应用请求超时",
    "CANCELLED": "预约取消", "EXPIRED": "预约过期", "PERIOD_CLOSING": "观察期结束未开始",
    "UNKNOWN": "未说明原因",
}
GAP_BUCKETS = ("LT1D", "1_TO_3D", "3_TO_7D", "7_TO_14D", "GE14D")
ENERGY_BUCKETS = ("LT10", "10_TO_20", "20_TO_40", "GE40")


def failure_reason(row):
    reason = row.get("failure_reason")
    if reason not in FAILURE_LABELS:
        reason = {"RESERVATION_CANCELLED": "CANCELLED", "RESERVATION_EXPIRED": "EXPIRED"}.get(row.get("outcome"), "UNKNOWN")
    return reason


def failure_summary(rows):
    counts = defaultdict(int)
    for row in rows:
        if row["outcome"] != "STARTED":
            counts[failure_reason(row)] += row["attempt_count"]
    failures, attempts = sum(counts.values()), total(rows, "attempt_count")
    return [dict(reason=key, label=FAILURE_LABELS[key], count=n,
                 shareOfFailures=ratio(n, failures), shareOfAttempts=ratio(n, attempts))
            for key, n in sorted(counts.items(), key=lambda item: (-item[1], item[0])) if n]


def flow_analysis(rows):
    """One cohort, three stages; do not collapse distinct losses into abandonment."""
    links = defaultdict(int)
    for row in rows:
        n = row["attempt_count"]
        path = "入口 · " + PATH_LABELS.get(row["access_path"], "其他入口")
        outcome = ("成功 · 开始充电" if row["outcome"] == "STARTED"
                   else "未成功 · " + FAILURE_LABELS[failure_reason(row)])
        links[("全部充电尝试", path)] += n
        links[(path, outcome)] += n
    return dict(attemptCount=total(rows, "attempt_count"),
                nodes=[{"name": name} for name in sorted({name for pair in links for name in pair})],
                links=[dict(source=a, target=b, value=n) for (a, b), n in sorted(links.items()) if n])


def recharge_behavior(rows):
    intervals, energy, segments = [], [], []
    for (segment,), values in sorted(grouped(rows, ["user_segment"]).items()):
        sessions, valid = total(values, "session_count"), total(values, "interval_count")
        segments.append(dict(userSegment=segment, sessionCount=sessions, intervalCount=valid,
            firstObservedCount=total(values, "first_observed_count"),
            meanIntervalDays=ratio(total(values, "interval_seconds_sum") / 86400, valid),
            meanEnergyKwh=ratio(total(values, "energy_wh") / 1000, sessions)))
        for field, buckets, target, denominator in (("gap_bucket", GAP_BUCKETS, intervals, valid),
                                                    ("energy_bucket", ENERGY_BUCKETS, energy, sessions)):
            counts = defaultdict(int)
            for row in values:
                counts[row[field]] += row["session_count"]
            target.extend(dict(userSegment=segment, bucket=bucket, count=counts[bucket],
                               share=ratio(counts[bucket], denominator)) for bucket in buckets)
    return dict(definition="按用户类型比较会话分布，不是新客留存或去重用户占比。间隔为同一用户全网相邻两次开始充电的时间差；"
        "先回看全批次历史，再按本次会话的电站、站型和开始日期筛选。首次观察没有前序，单独计数、不当作零间隔；"
        "间隔占比以可观测间隔次数为分母，电量占比以会话数为分母，高频用户会贡献更多记录。"
        "仅统计本平台记录，生成器另有站外补能，因此该间隔不代表车辆全部充电之间的间隔。"
        "只观察到一次或观察期末未再出现的用户，不能据此判为流失。",
        sessionCount=total(rows, "session_count"), intervalCount=total(rows, "interval_count"),
        firstObservedCount=total(rows, "first_observed_count"), intervals=intervals, energy=energy, segments=segments)


def service_bottlenecks(attempts, contexts, hours, dimensions):
    """Side-by-side, same-hour cohorts; association is not individual causation."""
    attempts_by_cell, context_by_cell, hours_by_cell = defaultdict(list), defaultdict(list), defaultdict(list)
    for source, target in ((attempts, attempts_by_cell), (contexts, context_by_cell), (hours, hours_by_cell)):
        for row in source:
            target[(dimensions[row["station_id"]]["site_type"], row["local_hour"])].append(row)
    by_type = grouped(list(dimensions.values()), ["site_type"])
    cells = []
    for (site,), stations in sorted(by_type.items()):
        inventory = defaultdict(lambda: [0, 0.0])
        for station in stations:
            for interface in station.get("interfaces", []):
                values = inventory[interface["connector_type"]]
                values[0] += interface["charger_count"]
                values[1] += interface["rated_power_kw"]
        interfaces = [dict(connectorType=k, chargerCount=v[0], ratedPowerKw=v[1]) for k, v in sorted(inventory.items())]
        for hour in range(24):
            group, context = attempts_by_cell[(site, hour)], context_by_cell[(site, hour)]
            n = total(group, "attempt_count")
            started = total([r for r in group if r["outcome"] == "STARTED"], "attempt_count")
            metrics = hour_metrics(hours_by_cell[(site, hour)])
            cells.append(dict(siteType=site, hour=hour, stationCount=len(stations), attemptCount=n,
                successfulAttempts=started, successRate=ratio(started, n),
                meanWaitMinutes=ratio(total(context, "queue_wait_seconds_sum") / 60, total(context, "queue_wait_count")),
                queueWaitCount=total(context, "queue_wait_count"),
                overstayShare=ratio(total(context, "occupied_seconds_sum"), total(context, "connected_seconds_sum")),
                sessionCount=total(context, "session_count"), chargingUtilization=metrics["chargingUtilization"],
                completeStationHours=metrics["sampleHours"], interfaces=interfaces, failures=failure_summary(group)))
    access = []
    for path, label in PATH_LABELS.items():
        values = [r for r in attempts if r["access_path"] == path]
        n = total(values, "attempt_count")
        started = total([r for r in values if r["outcome"] == "STARTED"], "attempt_count")
        access.append(dict(path=path, label=label, attemptCount=n, successfulAttempts=started, successRate=ratio(started, n)))
    count = total(attempts, "attempt_count")
    successful = total([r for r in attempts if r["outcome"] == "STARTED"], "attempt_count")
    return dict(definition="成功率按尝试创建日期和北京时间小时分组，成功仅指最终开始充电。点击格子联动同站型、同小时的相关资源指标："
        "等待按排队加入小时归属，加入到首次叫号、未叫号则到离队；充后占位按该小时开始的完整会话归属；"
        "利用率来自该小时的完整遥测；接口为所选电站的静态配置，不随天数累加。"
        "这些是不同事件口径的并列关联，不是每次失败发生瞬间的资源快照，不能据此证明某接口或占位导致失败。"
        "各入口并非随机分组，预约与直接尝试成功率的差异不等于预约策略的因果效果。",
        attemptCount=count, successfulAttempts=successful, failedAttempts=count-successful,
        failures=failure_summary(attempts), accessPaths=access, cells=cells)


def month_index(value):
    parsed = date.fromisoformat(value)
    return parsed.year * 12 + parsed.month - 1


def retention_analysis(rows, filters, manifest, station_dimensions, site_type):
    scope_type, scope_id, scope_label = "ALL", "ALL", "全部城市（跨站去重）"
    if filters["station"]:
        scope_type, scope_id = "STATION", filters["station"]
        scope_label = station_dimensions[scope_id]["station_name"] + "（站内回访）"
    elif filters["city"]:
        scope_type, scope_id = "CITY", filters["city"]
        scope_label = next(row["city_name"] for row in station_dimensions.values() if row["city_id"] == scope_id) + "（城市内去重）"
    # Current month is incomplete unless the exclusive end is next-month day 1.
    source_end_index = month_index(manifest["completeMonthsThrough"]) + 1
    source_end = f"{source_end_index // 12:04d}-{source_end_index % 12 + 1:02d}-01"
    observation_end = min(filters["end"][:7] + "-01", source_end)
    definition = ("首次观察到充电的月份分组；不是注册新增。按城市/电站去重，回看全历史，不受起始日期限制；"
                  "只使用截止结束日期前完整自然月，未观察到的未来单元格留空。")
    result = dict(scopeLabel=scope_label, definition=definition, observationEnd=observation_end, cohorts=[])
    if site_type:
        result["definition"] += "站型筛选下暂停留存展示，避免相加重复用户产生错误分母。"
        return result
    relevant = [r for r in rows if r["scope_type"] == scope_type and r["scope_id"] == scope_id
                and r["cohort_month"] < observation_end]
    groups = grouped(relevant, ["cohort_month"])
    max_columns = max((month_index(observation_end) - month_index(key[0]) for key in groups), default=0)
    for (month,), entries in sorted(groups.items()):
        size = entries[0]["cohort_size"]
        by_offset = {r["month_offset"]: r["n"] for r in entries}
        cells = []
        for offset in range(max_columns):
            observed = month_index(month) + offset < month_index(observation_end)
            users = by_offset.get(offset, 0) if observed else None
            cells.append(dict(offset=offset, users=users, rate=ratio(users, size) if users is not None else None))
        result["cohorts"].append(dict(month=month, size=size, cells=cells))
    return result


def pearson(pairs):
    n = len(pairs)
    if n < 3:
        return None
    mx, my = sum(x for x, _ in pairs) / n, sum(y for _, y in pairs) / n
    vx, vy = sum((x-mx)**2 for x, _ in pairs), sum((y-my)**2 for _, y in pairs)
    if vx <= 1e-20 or vy <= 1e-20:
        return None
    return max(-1.0, min(1.0, sum((x-mx)*(y-my) for x, y in pairs) / math.sqrt(vx*vy)))


def correlations(rows):
    features = {
        "充电利用率": lambda r: ratio(numeric(r, "complete_charging_samples"), numeric(r, "complete_sample_count")),
        "平均等待分钟": lambda r: ratio(numeric(r, "queue_wait_seconds_sum") / 60, numeric(r, "queue_wait_count")),
        "充后占位占比": lambda r: ratio(numeric(r, "occupied_seconds_sum"), numeric(r, "connected_seconds_sum")),
        "尝试成功率": lambda r: ratio(numeric(r, "successful_attempt_count"), numeric(r, "attempt_count")),
        "日均温度": lambda r: ratio(numeric(r, "weather_temperature_sum"), numeric(r, "weather_hour_count")),
    }
    vectors = [{name: fn(row) for name, fn in features.items()} for row in rows]
    result = []
    for x in features:
        for y in features:
            pairs = [(v[x], v[y]) for v in vectors if v[x] is not None and v[y] is not None]
            result.append(dict(x=x, y=y, value=pearson(pairs), n=len(pairs)))
    return result


def analyze(metadata, filters, site_type=None, directory=None):
    manifest, tables = load_bundle(metadata, directory)
    all_dimensions = {row["station_id"]: row for row in manifest["stations"]}
    types = sorted({r["site_type"] for r in all_dimensions.values()})
    if site_type is not None and site_type not in types:
        raise ApiError(422, "INVALID_ARGUMENT", "指定站型不存在")
    dimensions = {key: row for key, row in all_dimensions.items()
                  if (not filters["city"] or row["city_id"] == filters["city"])
                  and (not filters["station"] or key == filters["station"])
                  and (not site_type or row["site_type"] == site_type)}

    def selected(name):
        return [row for row in tables[name] if row["station_id"] in dimensions
                and filters["start"] <= row["business_date"] < filters["end"]]

    days, hours, attempts, segments = (selected(name) for name in TABLES[:4])
    hours_by_week = defaultdict(list)
    weather_by_hour = defaultdict(list)
    for row in hours:
        hours_by_week[(date.fromisoformat(row["business_date"]).weekday(), row["local_hour"])].append(row)
        temp = row.get("temperature_c")
        if isinstance(temp, (int, float)) and math.isfinite(temp):
            weather_by_hour[(int(math.floor(temp / 5) * 5), row["local_hour"])].append(row)
    heatmap = [dict(weekday=weekday, hour=hour, **hour_metrics(hours_by_week[(weekday, hour)]))
               for weekday in range(7) for hour in range(24)]
    weather = []
    for (temp, hour), values in sorted(weather_by_hour.items()):
        metrics = hour_metrics(values)
        metrics.pop("energyKwh")
        weather.append(dict(temperatureBin=temp, hour=hour, **metrics))
    stations = []
    station_days = grouped(days, ["station_id"])
    for key, dimension in sorted(dimensions.items()):
        values = station_days.get((key,), [])
        stations.append(dict(stationId=key, stationName=dimension["station_name"], cityId=dimension["city_id"],
            cityName=dimension["city_name"], siteType=dimension["site_type"],
            energyKwh=total(values, "energy_wh") / 1000 if total(values, "observed_hours") else None,
            chargingUtilization=ratio(total(values, "complete_charging_samples"), total(values, "complete_sample_count")),
            meanWaitMinutes=ratio(total(values, "queue_wait_seconds_sum") / 60, total(values, "queue_wait_count")),
            overstayShare=ratio(total(values, "occupied_seconds_sum"), total(values, "connected_seconds_sum")),
            netCashYuan=total(values, "net_paid_cents") / 100,
            sessionCount=total(values, "session_count"), attemptCount=total(values, "attempt_count"),
            successRate=ratio(total(values, "successful_attempt_count"), total(values, "attempt_count"))))
    segment_result = []
    for (site, user, capacity, connector), values in sorted(grouped(segments,
            ["site_type", "user_segment", "battery_capacity_band", "connector_type"]).items()):
        n = total(values, "session_count")
        segment_result.append(dict(siteType=site, userSegment=user, batteryCapacityBand=capacity, connectorType=connector,
            sessionCount=n, energyKwh=total(values, "energy_wh") / 1000,
            meanChargeMinutes=ratio(total(values, "active_seconds_sum") / 60, total(values, "active_duration_count")),
            meanOverstayMinutes=ratio(total(values, "occupied_seconds_sum") / 60, total(values, "occupied_duration_count"))))
    summary = dict(stationCount=len(dimensions), sessionCount=total(days, "session_count"),
                   attemptCount=total(attempts, "attempt_count"), observedStationHours=sum(bool(r.get("has_observation")) for r in hours),
                   completeStationHours=sum(bool(r.get("is_complete")) for r in hours))
    insights = []
    populated = [cell for cell in heatmap if cell["meanPowerKw"] is not None]
    if populated:
        peak = max(populated, key=lambda row: row["meanPowerKw"])
        insights.append(f"平均单站小时负荷最高为周{'一二三四五六日'[peak['weekday']]} {peak['hour']:02d}:00，"
                        f"{peak['meanPowerKw']:.1f} kW，来自 {peak['sampleHours']} 个完整站点小时；不是全网总负荷峰值。")
    occupied = [row for row in stations if row["overstayShare"] is not None and row["sessionCount"] >= 10]
    if occupied:
        worst = max(occupied, key=lambda row: row["overstayShare"])
        insights.append(f"{worst['stationName']} 的充后占位占连接时长 {worst['overstayShare']:.1%}"
                        f"（{worst['sessionCount']} 次会话）；可优先复核车位周转，不等同于设备故障。")
    started = total(days, "successful_attempt_count")
    if summary["attemptCount"]:
        insights.append(f"所选范围 {summary['attemptCount']:,} 次尝试中 {started:,} 次成功开始，"
                        f"成功率 {started/summary['attemptCount']:.1%}；服务流图按尝试创建日期归属、使用批次最终结果。")
    behavior = recharge_behavior(selected("user_behavior"))
    service = service_bottlenecks(attempts, selected("service_hour"), hours, dimensions)
    if service["failures"]:
        cause = service["failures"][0]
        insights.append(f"未成功开始的 {service['failedAttempts']:,} 次尝试中，最多为“{cause['label']}”："
                        f"{cause['count']:,} 次，占未成功尝试 {cause['shareOfFailures']:.1%}；"
                        "按站型和小时进一步核对，不把模拟原因分布当作真实经营结论。")
    return dict(provenance=dict(sourceLabel="行为校准模拟充电业务 + ERA5 真实历史气象；未使用 ACN 实测充电记录",
        analysisId=manifest["analysisId"], engine=str(manifest["engine"]) + " " + manifest.get("sparkVersion", ""), generatedAt=manifest["generatedAt"],
        completeMonthsThrough=manifest["completeMonthsThrough"], notes=[
            "PySpark 多表关联、小时覆盖网格、分层聚合、全历史用户间隔窗口；在线仅合并已发布的聚合结果。",
            "电量来自遥测积分；平均负荷和利用率只取完整小时，缺测不当作零。",
            "气象格子为站点小时样本数，同城天气会重复匹配不同站；不是独立气象观测数。",
            "相关矩阵按站点日描述 Pearson 关联（至少3个有效配对），无因果或显著性推断；注意站点异质性与重复观测。",
            "净收款是收款减退款，不是利润；站点日汇总的排队等待按解决日期归属，瓶颈联动按排队加入小时归属。"]),
        scope=dict(startDate=filters["start"], endDate=filters["end"], cityId=filters["city"],
                   stationId=filters["station"], siteType=site_type), stationTypes=types, summary=summary,
        heatmap=heatmap, stations=stations, weather=weather, flow=flow_analysis(attempts),
        behavior=behavior, service=service,
        retention=retention_analysis(tables["retention"], filters, manifest, all_dimensions, site_type),
        segments=segment_result, correlations=correlations(days), insights=insights)
