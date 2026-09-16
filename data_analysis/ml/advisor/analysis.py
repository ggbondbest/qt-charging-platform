"""Deterministic, bounded comparisons over the published dashboard aggregates.

Callers supply validated page filters. No model-selected SQL, entity lookup,
raw business row, or new prediction is used here. Period comparisons always
keep the same city/station and use a complete, immediately preceding window.
"""
from datetime import date
import math
import time
from urllib.parse import urlencode

from data_analysis.backend import advanced, service
from data_analysis.backend.errors import ApiError

MAX_EVIDENCE = 48
MIN_COMPARISON_ATTEMPTS = 30
TOPICS = ("overview", "bottlenecks", "stations", "behavior")
SITE_LABELS = {"OFFICE": "办公园区", "SHOPPING": "商业中心", "MALL": "商业中心", "RESIDENTIAL": "居民社区",
               "TRANSIT": "交通枢纽", "CAMPUS": "校园", "HIGHWAY": "高速服务区", "PUBLIC": "公共电站"}
SEGMENT_LABELS = {"COMMUTER": "通勤用户", "RIDE_HAILING": "网约车用户", "FLEET": "营运车队",
                  "FAMILY": "家庭用户", "PRIVATE": "私家用户"}


def _number(value):
    return value if type(value) in (int, float) and math.isfinite(value) else None


def _scaled(value, scale=1):
    value = _number(value)
    return value * scale if value is not None else None


def _difference(current, previous):
    return current - previous if current is not None and previous is not None else None


def _remaining(deadline, cancelled):
    if (cancelled is not None and cancelled.is_set()) or (deadline is not None and time.monotonic() >= deadline):
        raise ApiError(504, "ADVISOR_TIMEOUT", "参谋分析超时，请稍后重试")


def _source(metadata, filters, path, field):
    values = dict(datasetId=metadata["datasetId"], publishedBatchId=metadata["publishedBatchId"],
                  startDate=filters["start"], endDate=filters["end"])
    for name, key in (("cityId", "city"), ("stationId", "station")):
        if filters.get(key) is not None:
            values[name] = filters[key]
    return dict(endpoint=path + "?" + urlencode(values), field=field)


class _Evidence:
    def __init__(self, metadata, filters, topics):
        self.metadata = metadata
        self.filters = filters
        self.groups = {topic: [] for topic in topics}
        self.limitations = []

    def row(self, key, label, value, unit, field, *, path="/api/v1/dashboard/advanced", filters=None):
        return dict(id=key, label=label, value=_number(value) if not isinstance(value, str) else value,
                    unit=unit, source=_source(self.metadata, filters or self.filters, path, field))

    def group(self, topic, rows, *, requires=()):
        if rows:
            self.groups[topic].append(dict(rows=rows, requires=set(requires)))

    def finish(self):
        """Equal topic shares first; then spend spare capacity on whole groups."""
        result, selected, chosen = [], set(), set()
        topics = list(self.groups)
        share = MAX_EVIDENCE // max(1, len(topics))

        def include(topic, index, budget):
            group = self.groups[topic][index]
            if ((topic, index) in chosen or len(group["rows"]) > budget
                    or not group["requires"].issubset(selected)):
                return 0
            result.extend(group["rows"])
            selected.update(row["id"] for row in group["rows"])
            chosen.add((topic, index))
            return len(group["rows"])

        for topic in topics:
            remaining = share
            for index in range(len(self.groups[topic])):
                remaining -= include(topic, index, remaining)
        while len(result) < MAX_EVIDENCE:
            before = len(result)
            for topic in topics:
                for index in range(len(self.groups[topic])):
                    include(topic, index, MAX_EVIDENCE - len(result))
            if len(result) == before:
                break
        if len(chosen) < sum(len(groups) for groups in self.groups.values()):
            self.limitations.append("本次按主题分配最多48条证据，仅展示完整的比较组；未展示的站点、入口或群体不能被理解为没有记录。")
        return result


def _period_group(packet, topic, key, label, current, previous, unit, field, prior_filters,
                  *, path="/api/v1/dashboard/overview", percentage=False, growth=False):
    current_id = topic + "." + key
    rows = [packet.row(current_id, "当前范围 · " + label, current, unit, field, path=path)]
    if prior_filters is not None:
        previous_id = current_id + "_previous"
        rows.append(packet.row(previous_id, "紧邻等长前期 · " + label, previous, unit, field,
                               path=path, filters=prior_filters))
        delta = _difference(current, previous)
        rows.append(packet.row(current_id + "_change", label + ("变化（百分点）" if percentage else "绝对变化"),
            delta, " 个百分点" if percentage else unit,
            "derived: " + current_id + " - " + previous_id + "; 两期日期分别见输入证据source.endpoint", path=path))
        if growth:
            rows.append(packet.row(current_id + "_change_pct", label + "相对变化", delta / abs(previous) * 100
                if delta is not None and previous not in (None, 0) else None, "%",
                "derived: (" + current_id + " - " + previous_id + ") / abs(" + previous_id + ") × 100; 前期为0时不可计算", path=path))
    packet.group(topic, rows)


def _overview(packet, current, previous, current_advanced, previous_advanced, prior_filters):
    metrics = current.get("metrics", {})
    past = previous.get("metrics", {}) if previous else {}
    for key, label, field, unit, scale, percentage, growth in (
        ("energy", "观测充电电量", "energyWh", " kWh", .001, False, True),
        ("net_paid", "净收款", "netPaidCents", " 元", .01, False, True),
        ("utilization", "完整小时充电利用率", "chargingUtilizationRate", "%", 100, True, False)):
        _period_group(packet, "overview", key, label, _scaled(metrics.get(field), scale),
            _scaled(past.get(field), scale), unit, "metrics." + field + (" × " + str(scale) if scale != 1 else ""),
            prior_filters, percentage=percentage, growth=growth)
    if current_advanced is not None:
        now = current_advanced.get("service", {})
        old = previous_advanced.get("service", {}) if previous_advanced else {}
        _period_group(packet, "overview", "success_rate", "尝试最终开始充电成功率",
            _success(now), _success(old), "%", "derived: service.successfulAttempts / service.attemptCount × 100",
            prior_filters, path="/api/v1/dashboard/advanced", percentage=True)
    for specifications in (
        (("active_users", "窗口内去重活跃用户", "activeUsers", " 人"), ("sessions", "开始充电会话", "startedSessions", " 次")),
        (("observed_hours", "有观测站点小时", "observedHours", " 站点小时"), ("complete_hours", "完整站点小时", "completeHours", " 站点小时")),
        (("station_count", "范围内静态电站", "stationCount", " 站"), ("charger_count", "范围内静态充电资源", "chargerCount", " 个"))):
        packet.group("overview", [packet.row("overview." + key, label, metrics.get(field), unit,
            "metrics." + field, path="/api/v1/dashboard/overview") for key, label, field, unit in specifications])
    packet.group("overview", [packet.row("overview." + key, label, _scaled(metrics.get(field), .01), " 元",
        "metrics." + field + " / 100", path="/api/v1/dashboard/overview") for key, label, field in
        (("paid", "成功收款", "paidCents"), ("refund", "成功退款", "refundCents"))])
    packet.limitations.append("净收款按现金发生日统计，等于成功收款减成功退款，不是利润；相对变化以abs(前期)为分母，前期为0不计算。活跃用户在所选窗口去重；站数和充电资源是静态快照，不随日期累计。")


def _success(data):
    total, started = _number(data.get("attemptCount")), _number(data.get("successfulAttempts"))
    return started / total * 100 if total and started is not None else None


def _bottlenecks(packet, current, previous, prior_filters):
    data = current.get("service", {})
    old = previous.get("service", {}) if previous else {}
    packet.group("bottlenecks", [packet.row("bottlenecks." + key, label, data.get(field), " 次", "service." + field)
        for key, label, field in (("attempts", "充电尝试", "attemptCount"), ("successful", "成功开始", "successfulAttempts"),
                                  ("failed", "未成功开始", "failedAttempts"))])
    _period_group(packet, "bottlenecks", "success_rate", "尝试最终开始充电成功率", _success(data), _success(old), "%",
        "derived: service.successfulAttempts / service.attemptCount × 100", prior_filters,
        path="/api/v1/dashboard/advanced", percentage=True)
    failures = sorted(data.get("failures", []), key=lambda row: (-(_number(row.get("count")) or 0), str(row.get("reason"))))[:3]
    access = sorted((row for row in data.get("accessPaths", []) if (_number(row.get("attemptCount")) or 0) > 0),
                   key=lambda row: (-row["attemptCount"], str(row.get("path"))))[:3]
    cells = [row for row in data.get("cells", []) if (_number(row.get("attemptCount")) or 0) >= MIN_COMPARISON_ATTEMPTS]
    if cells:
        peak = min(cells, key=lambda row: (-row["attemptCount"], str(row.get("siteType")), row.get("hour", 0)))
        title = SITE_LABELS.get(peak.get("siteType"), "其他站型") + f" · {peak['hour']:02d}:00"
        field = f"service.cells[siteType={peak['siteType']},hour={peak['hour']}]"
        packet.group("bottlenecks", [packet.row("bottlenecks.peak_" + key, "最高尝试量组合 · " + title + " · " + label,
            _scaled(peak.get(source), scale), unit, field + "." + source + (" × 100" if scale == 100 else ""))
            for key, label, source, unit, scale in (
                ("attempts", "尝试数", "attemptCount", " 次", 1), ("success", "成功率", "successRate", "%", 100),
                ("wait", "平均等待", "meanWaitMinutes", " 分钟", 1), ("wait_count", "有效等待样本", "queueWaitCount", " 次", 1),
                ("utilization", "充电利用率", "chargingUtilization", "%", 100),
                ("hours", "完整站点小时", "completeStationHours", " 站点小时", 1))])
    else:
        packet.limitations.append("没有至少30次尝试的站型×小时组合，不能给出高需求时段优先级。")
    # Interleave reason and entry groups so a combined-topic budget retains
    # at least one of each before adding second/third-ranked entries.
    for index in range(max(len(failures), len(access))):
        if index < len(failures):
            row = failures[index]
            label = str(row.get("label") or advanced.FAILURE_LABELS.get(row.get("reason"), "未说明原因"))
            field = "service.failures[reason=" + str(row["reason"]) + "]"
            packet.group("bottlenecks", [
                packet.row(f"bottlenecks.failure_{index}_count", label + " · 未成功次数", row.get("count"), " 次", field + ".count"),
                packet.row(f"bottlenecks.failure_{index}_share", label + " · 占全部未成功尝试", _scaled(row.get("shareOfFailures"), 100), "%", field + ".shareOfFailures × 100")])
        if index < len(access):
            row = access[index]
            label = str(row.get("label") or row["path"])
            field = "service.accessPaths[path=" + str(row["path"]) + "]"
            packet.group("bottlenecks", [
                packet.row(f"bottlenecks.entry_{index}_attempts", label + "入口 · 尝试样本", row.get("attemptCount"), " 次", field + ".attemptCount"),
                packet.row(f"bottlenecks.entry_{index}_success", label + "入口 · 最终成功率", _scaled(row.get("successRate"), 100), "%", field + ".successRate × 100")])
    if not data.get("attemptCount"):
        packet.limitations.append("所选范围没有充电尝试，失败原因与入口成功率均无有效分母。")
    packet.limitations.append("失败原因占比以未成功尝试为分母，入口成功率以该入口尝试为分母；入口并非随机分组。最高需求组合按尝试总数选择，受站数和日期长度影响，不是每站到达率；样本30次只是展示门槛。等待、占位、利用率属于不同事件口径，并列关联不能证明失败原因。")


def _stations(packet, data):
    rows = [row for row in data.get("stations", []) if (_number(row.get("attemptCount")) or 0) >= MIN_COMPARISON_ATTEMPTS
            and _number(row.get("successRate")) is not None]
    rows.sort(key=lambda row: (row["successRate"], str(row["stationId"])))
    counts = [
        packet.row("stations.observed_count", "有充电尝试的电站", sum((_number(row.get("attemptCount")) or 0) > 0
            for row in data.get("stations", [])), " 站", "derived: stations[attemptCount > 0].length"),
        packet.row("stations.comparable_count", "达到30次尝试且成功率有效的电站", len(rows), " 站",
                   "derived: stations[attemptCount >= 30 and successRate != null].length")]
    if not rows:
        packet.group("stations", counts)
        packet.limitations.append("所选范围没有至少30次尝试且成功率有效的电站，不给出站点优先级或对照结论。")
        return
    first = rows[0]
    controls = [row for row in rows[1:] if row.get("siteType") == first.get("siteType")]
    control = max(controls or rows[1:], key=lambda row: (row["successRate"], str(row["stationId"]))) if len(rows) > 1 else None
    focus = [row for row in rows if control is None or row["stationId"] != control["stationId"]][:3]
    order = [("station_0", focus[0], "关注站1")]
    if control is not None:
        role = "同站型对照" if control.get("siteType") == first.get("siteType") else "跨站型对照"
        order.append(("control_0", control, role))
    else:
        packet.limitations.append("仅一站达到比较门槛，没有独立对照站，不能作站间差异判断。")
    order.extend((f"station_{index}", row, f"关注站{index + 1}") for index, row in enumerate(focus[1:], 1))
    for prefix, row, role in order:
        field = "stations[stationId=" + str(row["stationId"]) + "]"
        name = str(row.get("stationName") or "未命名电站")[:80]
        packet.group("stations", [packet.row("stations." + prefix + "_" + key, role + " · " + name + " · " + label,
            _scaled(row.get(source), scale), unit, field + "." + source + (" × 100" if scale == 100 else ""))
            for key, label, source, unit, scale in (
                ("attempts", "尝试数", "attemptCount", " 次", 1), ("success", "成功率", "successRate", "%", 100),
                ("wait", "平均等待", "meanWaitMinutes", " 分钟", 1), ("utilization", "充电利用率", "chargingUtilization", "%", 100))])
    if control is not None:
        inputs = ("stations.station_0_success", "stations.control_0_success")
        packet.group("stations", [packet.row("stations.focus_control_success_gap", "关注站1减对照站 · 成功率差异",
            (first["successRate"] - control["successRate"]) * 100, " 个百分点", "derived: " + inputs[0] + " - " + inputs[1])], requires=inputs)
    packet.group("stations", counts)
    packet.limitations.append("关注站按达到30次尝试的电站成功率从低到高选择，最多三站；对照站优先选与首个关注站同站型且成功率较高的另一站。对照未随机匹配，供给和样本不同，不能作因果解释、设备故障诊断或综合优劣排名。")


def _behavior(packet, data):
    behavior = data.get("behavior", {})
    rows = [row for row in behavior.get("segments", []) if (_number(row.get("sessionCount")) or 0) > 0]
    eligible = [row for row in rows if (_number(row.get("intervalCount")) or 0) >= MIN_COMPARISON_ATTEMPTS
                and _number(row.get("meanIntervalDays")) is not None]
    eligible.sort(key=lambda row: (row["meanIntervalDays"], str(row["userSegment"])))
    endpoints = [eligible[0], eligible[-1]] if len(eligible) >= 2 else eligible[:1]
    selected = endpoints + sorted((row for row in rows if row not in endpoints),
                                 key=lambda row: (-row["sessionCount"], str(row["userSegment"])))
    selected = selected[:4]
    for index, row in enumerate(selected):
        label = SEGMENT_LABELS.get(row["userSegment"], "其他用户类型")
        prefix = "behavior.segment_" + str(index)
        field = "behavior.segments[userSegment=" + str(row["userSegment"]) + "]"
        packet.group("behavior", [packet.row(prefix + "_" + key, label + " · " + name,
            row.get(source), unit, field + "." + source) for key, name, source, unit in (
                ("sessions", "会话样本", "sessionCount", " 次"), ("interval_count", "可观察间隔样本", "intervalCount", " 次"),
                ("interval", "平均补能间隔", "meanIntervalDays", " 天"), ("energy", "单次平均电量", "meanEnergyKwh", " kWh"))])
    packet.group("behavior", [packet.row("behavior." + key, label, behavior.get(field), " 次", "behavior." + field)
        for key, label, field in (("sessions", "会话样本", "sessionCount"), ("intervals", "可观察相邻间隔", "intervalCount"),
                                  ("first_observed", "首次观察会话", "firstObservedCount"))])
    if len(endpoints) == 2:
        ids = ["behavior.segment_" + str(number) + suffix for number in (0, 1) for suffix in ("_interval", "_energy")]
        packet.group("behavior", [
            packet.row("behavior.interval_gap", "间隔最长群体减最短群体 · 平均间隔差异",
                endpoints[1]["meanIntervalDays"] - endpoints[0]["meanIntervalDays"], " 天",
                "derived: behavior.segment_1_interval - behavior.segment_0_interval"),
            packet.row("behavior.energy_gap", "同两群体 · 单次平均电量差异",
                _difference(_number(endpoints[1].get("meanEnergyKwh")), _number(endpoints[0].get("meanEnergyKwh"))), " kWh",
                "derived: behavior.segment_1_energy - behavior.segment_0_energy")], requires=ids)
    if len(eligible) < 2:
        packet.limitations.append("不足两个群体具有至少30个可观察间隔，仅展示群体样本与均值，不做补能频率的端点差异排序。")
    if not behavior.get("sessionCount"):
        packet.limitations.append("所选范围没有平台会话，不能比较用户群体的电量与补能间隔。")
    packet.limitations.append("补能频繁程度仅用本平台全网相邻开始时间间隔描述，不取其倒数伪造人均频次；会话数和间隔数不是去重用户数。首次观察不计零间隔，站外补能不在观察内，期末未再次出现不能判为流失。群体差异为描述性比较，30个间隔只是展示门槛。")


def build_analysis(snapshot, filters, topics, *, deadline=None, cancelled=None):
    """Return at most 48 grouped evidence rows, plus observed topic signals.

    Empty valid aggregates become limitations. Publication integrity and
    transport errors remain explicit ApiErrors; they are not empty datasets.
    """
    _remaining(deadline, cancelled)
    requested = list(dict.fromkeys(topic for topic in topics if topic in TOPICS))
    packet = _Evidence(snapshot.metadata, dict(filters), requested)
    if not requested:
        return dict(evidence=[], limitations=[], suggestions=[], observed=False, observedTopics=[])
    try:
        start, end = date.fromisoformat(filters["start"]), date.fromisoformat(filters["end"])
        batch_start, batch_end = date.fromisoformat(snapshot.metadata["startDate"]), date.fromisoformat(snapshot.metadata["endDate"])
    except (ValueError, KeyError, TypeError):
        raise ApiError(422, "INVALID_DATE_RANGE", "分析日期必须是有效业务日期") from None
    if not batch_start <= start < end <= batch_end:
        raise ApiError(422, "DATE_OUT_OF_RANGE", "分析日期超出已发布范围")
    prior_start = start - (end - start)
    prior_filters = {**filters, "start": prior_start.isoformat(), "end": start.isoformat()} if prior_start >= batch_start else None
    compare = bool(set(requested).intersection({"overview", "bottlenecks"}))
    if compare:
        if prior_filters is None:
            packet.limitations.append("批次内没有完整的紧邻等长前期；本次不做跨期比较，也不截短前期凑同比。")
        else:
            packet.limitations.append(f"比较当前[{filters['start']}, {filters['end']})与紧邻等长前期[{prior_filters['start']}, {prior_filters['end']})，保留相同城市与站点筛选。比例变化使用百分点，不保证两期构成或日类型相同。")

    def checked(call, *args, **kwargs):
        _remaining(deadline, cancelled)
        value = call(*args, **kwargs)
        _remaining(deadline, cancelled)
        return value

    current_overview = checked(service.overview, snapshot, filters) if "overview" in requested else None
    previous_overview = checked(service.overview, snapshot, prior_filters) if current_overview is not None and prior_filters else None
    current = checked(advanced.analyze, snapshot.metadata, filters, snapshot=snapshot)
    previous = checked(advanced.analyze, snapshot.metadata, prior_filters, snapshot=snapshot) if compare and prior_filters else None
    observed = {}
    if "overview" in requested:
        _overview(packet, current_overview, previous_overview, current, previous, prior_filters)
        metrics = current_overview.get("metrics", {})
        observed["overview"] = any(metrics.get(key) for key in ("observedHours", "startedSessions", "activeUsers", "paidCents", "refundCents"))
    if "bottlenecks" in requested:
        _bottlenecks(packet, current, previous, prior_filters)
        observed["bottlenecks"] = bool(current.get("service", {}).get("attemptCount"))
    if "stations" in requested:
        _stations(packet, current)
        observed["stations"] = any(row.get("attemptCount", 0) > 0 for row in current.get("stations", []))
    if "behavior" in requested:
        _behavior(packet, current)
        observed["behavior"] = bool(current.get("behavior", {}).get("sessionCount"))
    _remaining(deadline, cancelled)
    evidence = packet.finish()
    limitations = ["全部充电业务为模拟数据；比较只描述已发布聚合，不代表实时状态、真实经营结论或因果效果。", *packet.limitations]
    if not any(observed.values()):
        limitations.append("当前所选主题没有可用业务观察；历史记录或静态库存不能补作本期观测。")
    suggestions = []
    if "overview" in requested:
        suggestions.append(dict(target="overview", label="查看运营总览"))
    if set(requested).intersection({"bottlenecks", "stations", "behavior"}):
        suggestions.append(dict(target="advanced", label="查看多维分析"))
    return dict(evidence=evidence, limitations=limitations, suggestions=suggestions,
                observed=any(observed.values()), observedTopics=[topic for topic, value in observed.items() if value])
