"""Deterministic operational answers over the same published services as the UI.

This module neither reads raw datasets nor deserializes models. Evidence is
copied from bounded public rollups/provider responses, with a reproducible URL.
"""
import math
import re
from urllib.parse import urlencode

from data_analysis.backend import advanced, service
from data_analysis.backend.errors import ApiError

QUESTIONS = [
    {"id": "bottlenecks", "label": "先改哪里", "question": "分析当前范围的服务瓶颈，按优先级给出两项改进建议和验证指标。"},
    {"id": "stations", "label": "站点差异", "question": "哪些站需要优先关注？与对照站比较，说明差异和待核实原因。"},
    {"id": "behavior", "label": "人群策略", "question": "不同用户类型的补能行为有何差异？结合样本给出分群运营建议。"},
    {"id": "models", "label": "模型说明", "question": "目前有哪些模型，它们能说明什么？"},
]
DISCLOSURE = ("离线问答在本服务内计算。在线对话会将本次问题、最近最多6条对话历史、当前页面筛选说明、"
              "检索到的项目知识片段及有限聚合指标发送给已配置的在线模型，由模型生成回答。"
              "不会读取或外发用户与会话明细、密钥或完整工件；请勿在问题中填写个人信息或凭据。"
              "业务数据均为模拟数据；选择在线模式并点击发送即使用上述在线处理。")
TARGETS = {"overview": "查看运营总览", "advanced": "查看多维分析", "models": "查看模型分析", "anomalies": "查看用户与异常"}
MIN_COMPARISON_ATTEMPTS = 30
SITE_LABELS = {"OFFICE": "办公园区", "SHOPPING": "商业中心", "MALL": "商业中心", "RESIDENTIAL": "居民社区",
               "TRANSIT": "交通枢纽", "CAMPUS": "校园", "HIGHWAY": "高速服务区", "TRANSPORT_HUB": "交通枢纽", "PUBLIC": "公共电站"}
SESSION_RE = re.compile(r"(?<![A-Za-z0-9_-])SES-[A-Za-z0-9_-]{1,80}(?![A-Za-z0-9_-])", re.I)
STATION_RE = re.compile(r"(?<![A-Za-z0-9_-])ST-[A-Z0-9]+-[A-Z0-9]+(?![A-Za-z0-9_-])", re.I)
DATE_RE = re.compile(r"(?<![0-9])(?:[0-9]{4}[-/][0-9]{1,2}[-/][0-9]{1,2}|(?:[0-9]{4}年)?[0-9]{1,2}月[0-9]{1,2}日?)(?![0-9])")
# This is a bounded question interface, not a geographic named-entity model.
# Unrecognized Chinese scope wording fails closed instead of guessing a region.
SCOPE_WORDS = (
    "当前范围内 当前范围 所选范围 页面筛选 页面 当前 所选 全部城市 各个城市 城市 全国 全网 "
    "运营概况 运营总览 充电电量 单次电量 电量 净收款 充电利用率 利用率 充电服务 服务瓶颈 瓶颈 "
    "失败原因 未成功开始 未成功 成功开始 开始充电 成功率 尝试 排队 等待 平均时间 时间 拥堵 "
    "优先关注 优先复核 关注 复核 比较 对比 各站 哪些电站 哪些站 哪个站 电站 站点 "
    "不同用户类型 用户类型 用户 家庭 通勤 网约车 营运车队 补能间隔 充电间隔 回访间隔 补能行为 补能 "
    "平均 单次 多少 怎样 如何 什么 为什么 是多少 是什么 有何 差异 情况 原因 记录 指标 "
    "需要 应该 请问 请 查询 查看 解释 看看 统计 分析 总共 分别 哪些 哪个 这个 所有 "
    "按 优先级 两项 改进 建议 验证 对照站 说明 待核实 结合 样本 分群 运营 给出"
).split()


def classify(question):
    q = question.strip()
    if re.search(r"删除|修改|清空|密码|令牌|密钥|今天|昨天|明天|后天|本周|上周|本月|上月|今年|去年|实时|此刻|股票|光伏|保证|诊断故障|预测.*营收", q):
        return "unsupported"
    if re.search(r"SES-[A-Za-z0-9_-]+|异常会话|告警清单|异常筛查|会话异常", q, re.I):
        return "anomalies"
    if re.search(r"模型|盲评|精度|召回|误报|F1|MAE", q, re.I):
        return "models"
    if re.search(r"补能|充电间隔|回访间隔|单次电量|用户类型", q):
        return "behavior"
    if re.search(r"比较|对比|各站|哪个站|哪些站|哪些电站|优先关注|优先复核", q) and re.search(r"站|电站", q):
        return "stations"
    if re.search(r"瓶颈|失败原因|未成功|成功率|排队|等待|拥堵", q):
        return "bottlenecks"
    if re.search(r"运营概况|运营总览|电量|利用率|净收款", q):
        return "overview"
    return "unsupported"


def number(value):
    return value if type(value) in (int, float) and math.isfinite(value) else None


def formatted(item):
    value, unit = item["value"], item["unit"]
    if value is None:
        return item["label"] + "：无足够观测"
    text = f"{value:,.2f}".rstrip("0").rstrip(".") if type(value) in (int, float) else str(value)
    return f"{item['label']}：{text}{unit}"


def concise_answer(intent, evidence, intro):
    """A short program-rendered conclusion; the full audit trail stays in cards."""
    items = {item["id"]: item for item in evidence}

    def value(key):
        item = items.get(key)
        if not item or item["value"] is None:
            return "暂无观测"
        n = item["value"]
        return f"{n:,.2f}".rstrip("0").rstrip(".") if type(n) in (int, float) else str(n)

    if intent == "overview":
        return "所选范围的观测电量为" + value("energy") + " kWh，完整小时充电利用率为" + value("utilization") + "%；已解决排队平均等待" + value("queue_wait") + "分钟。缺测不当作零，这些不是实时状态。"
    if intent == "bottlenecks":
        text = f"{value('attempts')}次充电尝试中，未成功开始{value('failed')}次。"
        if "failure_count" in items:
            reason = items["failure_count"]["label"].removeprefix("最多的失败原因（").removesuffix("）")
            text += f"主要原因为“{reason}”：{value('failure_count')}次，占未成功尝试{value('failure_share')}%。"
        if "cell_attempts" in items:
            title = items["cell_attempts"]["label"].rsplit(" · ", 1)[0]
            text += f"可先复核{title}：{value('cell_attempts')}次尝试、成功率{value('cell_success')}%，平均等待{value('cell_wait')}分钟。等待等指标为并列关联，不能据此认定失败原因。"
        else:
            text += "尚无至少30次尝试且有失败的站型×小时组合，不给出时段优先级。"
        return text
    if intent == "stations":
        if "station_0_success" not in items:
            return "所选范围有" + value("station_count") + "个电站出现尝试，但均少于30次，样本太少，暂不做优先排序。可扩大日期范围再比较。"
        name = items["station_0_attempts"]["label"].rsplit(" · ", 1)[0]
        return f"仅比较至少30次尝试的站点；成功率最低、可先复核的是{name}：{value('station_0_attempts')}次尝试，成功率{value('station_0_success')}%，平均等待{value('station_0_wait')}分钟。其余候选见证据；这不是设备故障判断或综合排名。"
    if intent == "behavior":
        intervals = [item for item in evidence if item["id"].startswith("segment_") and item["id"].endswith("_interval") and number(item["value"]) is not None]
        intervals.sort(key=lambda item: (item["value"], item["id"]))
        chosen = [intervals[0]] if intervals else []
        if len(intervals) > 1:
            chosen.append(intervals[-1])
        descriptions = []
        for item in chosen:
            name = item["label"].rsplit(" · ", 1)[0]
            key = item["id"].removesuffix("_interval")
            descriptions.append(f"{name}平均补能间隔{value(item['id'])}天，单次电量{value(key + '_energy')} kWh")
        if not descriptions:
            return "所选范围有会话记录，但尚无可观察的相邻补能间隔，不能比较补能频率。首次观察不当作零间隔。"
        return "平台会话中，" + "；".join(descriptions) + "。仅列间隔两端的群体；首次观察不计零间隔，也不涵盖站外补能，不能据此判断流失。"
    if intent == "models":
        missing = [item["label"] for item in evidence if item["id"].endswith("_status") and item["value"] != "就绪"]
        text = "负荷模型描述小时平均用电，空闲模型描述小时末桩状态，到站模型辅助到站后的服务判断；用户与异常模型用于固定历史样本复核。"
        return text + (("其中“" + "、".join(missing) + "”尚未就绪。") if missing else "当前这些能力均已就绪。") + "评测和可用状态见证据，不提供实时安全诊断。"
    if intent == "anomalies":
        if "anomaly_0_score" not in items:
            return f"历史TEST已复核{value('inspected_count')}次会话，没有会话超过当前阈值；不能据此推断不存在异常。"
        sid = items["anomaly_0_score"]["label"].rsplit(" · ", 1)[0]
        prefix = f"历史TEST中有{value('anomaly_count')}次告警。" if "anomaly_count" in items else ""
        return prefix + f"{sid}的异常分数为{value('anomaly_0_score')}，阈值为{value('anomaly_0_threshold')}，结果为“{value('anomaly_0_status')}”。这是已结束会话的辅助筛查，不是故障概率或安全诊断。"
    return intro


def location_matches(snapshot, question, filters, intent):
    """Only published aliases and reviewed domain wording are supported."""
    cities = snapshot.rows("SELECT city_id, city_name FROM cities")
    mentioned = set()
    known_names = []
    for city in cities:
        name = str(city["city_name"])
        aliases = {name, name.removesuffix("市")}
        known_names.extend(aliases)
        if any(len(alias) >= 2 and alias.casefold() in question.casefold() for alias in aliases):
            mentioned.add(city["city_id"])
    remaining = question
    for name in sorted(set(known_names), key=len, reverse=True):
        remaining = re.sub(re.escape(name), "", remaining, flags=re.I)
    if intent in {"overview", "stations", "bottlenecks", "behavior"}:
        remaining = STATION_RE.sub("", remaining)
        for word in sorted(SCOPE_WORDS, key=len, reverse=True):
            remaining = remaining.replace(word, "")
        remaining = re.sub(r"[的和与及在对把为中内这那有是了吗呢吧么一下]", "", remaining)
        if re.search(r"[\u4e00-\u9fff]", remaining):
            return False
    elif re.search(r"[\u4e00-\u9fff]{2,12}市", remaining.replace("城市", "")):
        return False
    effective_city = filters["city"]
    if effective_city is None and filters["station"]:
        effective_city = snapshot.one("SELECT city_id FROM station_snapshot WHERE station_id = ?", (filters["station"],))["city_id"]
    return not mentioned or mentioned == {effective_city}


def answer(snapshot, provider, query, *, settings=None, deadline=None, cancelled=None):
    if settings is not None:
        from . import rag
        return rag.answer(snapshot, provider, query, settings=settings, deadline=deadline, cancelled=cancelled)
    filters = service.check_filters(snapshot, query)
    metadata = snapshot.metadata
    if metadata.get("source") != "SIMULATED":
        raise ApiError(503, "DATA_NOT_READY", "参谋当前仅支持已发布的模拟业务数据")
    intent = classify(query.question)
    location_ok = location_matches(snapshot, query.question, filters, intent)
    if not location_ok:
        intent = "unsupported"
    # Explicit dates inside prose are not silently reinterpreted as page filters.
    dates = DATE_RE.findall(query.question)
    if dates:
        intent = "unsupported"
    station_ids = STATION_RE.findall(query.question)
    if station_ids and any(value.upper() != filters["station"] for value in station_ids):
        intent = "unsupported"
    sessions = SESSION_RE.findall(query.question)
    if "SES-" in query.question.upper() and len(sessions) != 1:
        intent = "unsupported"
    if "ST-" in query.question.upper() and not station_ids:
        intent = "unsupported"
    scope = dict(datasetId=metadata["datasetId"], publishedBatchId=metadata["publishedBatchId"],
        startDate=filters["start"], endDate=filters["end"], cityId=filters["city"], stationId=filters["station"],
        timeZone="Asia/Shanghai", dataKind="SIMULATED")
    result = dict(status="answered", mode=query.mode, intent=intent, answer="", scope=scope,
                  evidence=[], limitations=["全部业务数据为模拟数据；用于复核与演示，不构成真实运营结论。"], suggestions=[])
    ev = result["evidence"]
    selected = {key: value for key, value in scope.items()
                if key in ("datasetId", "publishedBatchId", "cityId", "stationId", "startDate", "endDate") and value is not None}
    bound = {key: selected[key] for key in ("datasetId", "publishedBatchId")}

    def url(path, *, model=False):
        return path + "?" + urlencode(bound if model else selected)

    def evidence(key, label, value, unit, endpoint, field, *, model=False):
        if not isinstance(value, str):
            value = number(value)
        ev.append(dict(id=key, label=label, value=value, unit=unit,
                       source=dict(endpoint=url(endpoint, model=model), field=field)))

    def nav(target):
        result["suggestions"].append(dict(target=target, label=TARGETS[target]))

    def percent(value):
        return number(value) * 100 if number(value) is not None else None

    intro = "按页面所选范围查询，结束日期不包含当天。"
    if intent == "unsupported":
        text = "暂不支持这个问题。"
        if not location_ok:
            text = "问题中的城市与页面筛选不一致，或地域/问句表述暂不支持。只识别本批次城市和示例中的运营问句；请用城市控件选择范围，不猜测其他地域。"
        if dates:
            text = "正文日期暂不解析，请先在页面日期控件选择范围，再提问；不会把单日问题当成整段统计。"
        result.update(status="unsupported", answer=text + "可以查询运营瓶颈、站点比较、补能行为、运营概况、现有模型或历史异常会话。日期和站点请通过页面筛选；不会推测实时数据或执行修改。")
        nav("advanced")
        return result

    if intent == "overview":
        metrics = service.overview(snapshot, filters)["metrics"]
        if not any(metrics.get(key) for key in ("observedHours", "startedSessions", "activeUsers", "paidCents", "refundCents")):
            result["status"] = "no_evidence"
        path = "/api/v1/dashboard/overview"
        for key, label, source, unit, scale in [
            ("energy", "观测充电电量", "energyWh", " kWh", .001),
            ("utilization", "完整小时充电利用率", "chargingUtilizationRate", "%", 100),
            ("queue_wait", "已解决排队平均等待", "queueMeanWaitSeconds", " 分钟", 1/60),
            ("sessions", "开始充电会话", "startedSessions", " 次", 1),
            ("net_paid", "按支付发生日统计的净收款", "netPaidCents", " 元", .01),
            ("paid", "按支付发生日统计的成功收款", "paidCents", " 元", .01),
            ("refund", "按退款发生日统计的成功退款", "refundCents", " 元", .01),
            ("active_users", "窗口内去重活跃用户", "activeUsers", " 人", 1),
            ("observed_hours", "有遥测观测的站点小时", "observedHours", " 站点小时", 1),
            ("complete_hours", "完整遥测站点小时", "completeHours", " 站点小时", 1),
            ("station_count", "范围内电站快照数量", "stationCount", " 站", 1),
            ("charger_count", "范围内充电资源快照数量", "chargerCount", " 个", 1),
        ]:
            value = number(metrics.get(source))
            evidence(key, label, value * scale if value is not None else None, unit, path, "metrics." + source)
        result["limitations"].append("利用率仅使用完整遥测小时；排队等待按解决日期归属，不能解释当前实时队长。")
        result["limitations"].append("净收款为成功收款减成功退款，不是利润；活跃用户在整个所选窗口去重，不能相加每日人数。站数与充电资源数为范围内静态快照，不按日期累计，也不是实时可用数量。")
        nav("overview")

    elif intent in ("bottlenecks", "stations", "behavior"):
        data = advanced.analyze(metadata, filters, snapshot=snapshot)
        path = "/api/v1/dashboard/advanced"
        if intent == "bottlenecks":
            item = data["service"]
            for key, label, field in [("attempts", "充电尝试", "attemptCount"),
                                       ("successful", "成功开始", "successfulAttempts"), ("failed", "未成功开始", "failedAttempts")]:
                evidence(key, label, item[field], " 次", path, "service." + field)
            if not item["attemptCount"]:
                result["status"] = "no_evidence"
            if item["failures"]:
                top = item["failures"][0]
                label = advanced.FAILURE_LABELS.get(top["reason"], "未说明原因")
                evidence("failure_count", "最多的失败原因（" + label + "）", top["count"], " 次", path, "service.failures[0].count")
                evidence("failure_share", "该原因占全部未成功尝试", percent(top["shareOfFailures"]), "%", path, "service.failures[0].shareOfFailures × 100")
                intro += "先复核出现最多的失败原因，再在多维分析中对照站型与小时。"
            else:
                intro += "所选记录未报告失败原因，不能据此推断所有资源都充足。"
            cells = [row for row in item["cells"] if row["attemptCount"] >= MIN_COMPARISON_ATTEMPTS
                     and row["attemptCount"] > row["successfulAttempts"]]
            if cells:
                cell = max(cells, key=lambda row: (row["attemptCount"] - row["successfulAttempts"],
                                                    row["attemptCount"], row["siteType"], -row["hour"]))
                title = SITE_LABELS.get(cell["siteType"], "其他站型") + f" · {cell['hour']:02d}:00"
                intro += "达到至少30次尝试展示门槛的组合中，未成功数量最多的是“" + title + "”，可优先复核该时段的服务过程。"
                prefix = f"service.cells[siteType={cell['siteType']},hour={cell['hour']}]"
                for key, label, value, unit, field in [
                    ("cell_attempts", "尝试数", cell["attemptCount"], " 次", "attemptCount"),
                    ("cell_failed", "未成功数", cell["attemptCount"]-cell["successfulAttempts"], " 次", "attemptCount - successfulAttempts"),
                    ("cell_success", "开始充电成功率", percent(cell["successRate"]), "%", "successRate × 100"),
                    ("cell_wait", "平均等待", cell["meanWaitMinutes"], " 分钟", "meanWaitMinutes"),
                    ("cell_wait_count", "等待记录数", cell["queueWaitCount"], " 次", "queueWaitCount"),
                    ("cell_overstay", "充后占位占连接时间", percent(cell["overstayShare"]), "%", "overstayShare × 100"),
                    ("cell_utilization", "完整小时充电利用率", percent(cell["chargingUtilization"]), "%", "chargingUtilization × 100"),
                    ("cell_hours", "完整观测站点小时", cell["completeStationHours"], " 个", "completeStationHours")]:
                    evidence(key, title + " · " + label, value, unit, path, prefix + "." + field)
            else:
                result["limitations"].append("没有至少30次尝试且存在失败的站型×小时组合，不据此给出时段优先级。30次仅为展示门槛，不是显著性检验。")
            result["limitations"].append("按尝试创建日期归属，成功指批次最终开始充电。等待、占位和利用率为不同事件口径，关联不等于失败因果。")
        elif intent == "stations":
            observed = [row for row in data["stations"] if row["attemptCount"] > 0]
            rows = [row for row in observed if row["attemptCount"] >= MIN_COMPARISON_ATTEMPTS]
            rows.sort(key=lambda row: (row["successRate"] if row["successRate"] is not None else 1, row["stationId"]))
            evidence("station_count", "有充电尝试的电站", len(observed), " 站", path, "stations[attemptCount > 0].length")
            evidence("comparable_station_count", "至少30次尝试的电站", len(rows), " 站", path, "stations[attemptCount >= 30].length")
            if not observed:
                result["status"] = "no_evidence"
            if observed and not rows:
                intro += "所选电站均少于30次尝试，样本太少，暂不给出优先关注排序。请扩大日期范围后再比较。"
            else:
                intro += "在至少30次尝试的电站中，按开始充电成功率从低到高列出至多三个待复核站；这不是综合优劣排名。"
            for index, row in enumerate(rows[:3]):
                name = str(row["stationName"])[:80]
                field = f"stations[stationId={row['stationId']}]"
                for suffix, label, value, unit, source in [
                    ("attempts", "尝试数", row["attemptCount"], " 次", "attemptCount"),
                    ("success", "成功率", percent(row["successRate"]), "%", "successRate × 100"),
                    ("wait", "平均等待", row["meanWaitMinutes"], " 分钟", "meanWaitMinutes"),
                    ("util", "充电利用率", percent(row["chargingUtilization"]), "%", "chargingUtilization × 100")]:
                    evidence(f"station_{index}_{suffix}", name + " · " + label, value, unit, path, field + "." + source)
            result["limitations"].append("少于30次尝试的站不进入优先排序；30次仅为展示门槛，不保证统计可靠性。样本量、站型与供给配置不同；成功率低不等于设备故障，缺测不记作零。")
        else:
            item = data["behavior"]
            for key, label, field in [("sessions", "会话数", "sessionCount"), ("intervals", "可观察相邻间隔", "intervalCount"),
                                       ("first_observed", "首次观察会话", "firstObservedCount")]:
                evidence(key, label, item[field], " 次", path, "behavior." + field)
            if not item["sessionCount"]:
                result["status"] = "no_evidence"
            labels = {"COMMUTER": "通勤用户", "RIDE_HAILING": "网约车用户", "FLEET": "营运车队", "FAMILY": "家庭用户", "PRIVATE": "私家用户"}
            for index, row in enumerate(item["segments"][:6]):
                name = labels.get(row["userSegment"], "其他用户类型")
                field = f"behavior.segments[userSegment={row['userSegment']}]"
                evidence(f"segment_{index}_interval", name + " · 平均补能间隔", row["meanIntervalDays"], " 天", path, field + ".meanIntervalDays")
                evidence(f"segment_{index}_energy", name + " · 单次平均电量", row["meanEnergyKwh"], " kWh", path, field + ".meanEnergyKwh")
            result["limitations"].append("按会话统计，并非去重用户占比。间隔回看全批次历史，首次观察不计零间隔；仅含本平台补能，未再次出现不能直接判为流失。")
        nav("advanced")

    else:
        if provider is None:
            raise ApiError(503, "MODEL_NOT_READY", "当前交付模型尚未就绪")
        registry = provider.registry(metadata)  # Includes current source/batch verification.
        result["limitations"].append("页面日期与站点筛选不改变模型训练和评测范围；模型证据来自该批次固定历史留出样本，不代表当前实时风险。")
        intro = "以下是当前发布批次的模型记录，使用模型自己的历史评测范围。"
        path = "/api/v1/intelligence/models"
        if intent == "models":
            intro += "负荷模型描述未来小时平均用电；空闲模型描述小时末桩状态；到站模型辅助行驶到站后的服务判断；用户与异常模型用于固定历史样本复核。"
            explanations = {"load": "小时平均负荷", "availability": "小时末空闲桩", "insights": "用户流失与会话异常", "arrival": "分钟到站与等待"}
            for key, label in explanations.items():
                state = registry.get(key, {}).get("status", "NOT_READY")
                evidence(key + "_status", label, "就绪" if state == "READY" else "未就绪", "", path, key + ".status", model=True)
            report = registry.get("insights", {}).get("anomaly", {}).get("test", {})
            for key, label in [("precision", "异常筛查 TEST 精确率"), ("recall", "异常筛查 TEST 召回率")]:
                if number(report.get(key)) is not None:
                    evidence(key, label, percent(report[key]), "%", path, "insights.anomaly.test." + key + " × 100", model=True)
            result["limitations"].append("小时末空闲桩不等于分钟到站库存；异常分数不是故障概率，流失分数不是校准概率。")
            nav("models")
        else:
            entity = sessions[0].upper() if sessions else None
            info = provider.insight("anomalies", metadata, entity_id=entity, limit=3)
            path = "/api/v1/intelligence/insights/anomalies" + ("/" + entity if entity else "")
            if entity:
                rows = [info]
            else:
                rows = info["items"]
                evidence("anomaly_count", "历史 TEST 告警会话", info["total"], " 次", path, "total", model=True)
                evidence("inspected_count", "历史 TEST 复核会话", info["inspectedSessions"], " 次", path, "inspectedSessions", model=True)
            if not rows:
                if info.get("inspectedSessions", 0) > 0:
                    intro += "已复核样本没有会话超过当前告警阈值，不能据此推断不存在异常。"
                else:
                    result["status"] = "no_evidence"
            for index, row in enumerate(rows[:3]):
                prefix = str(row["sessionId"])[:90]
                source_prefix = "" if entity else f"items[{index}]."
                for suffix, label, value, field in [
                    ("score", "异常分数", row["anomalyScore"], "anomalyScore"),
                    ("threshold", "筛查阈值", row["threshold"], "threshold"),
                    ("status", "筛查结果", "需人工复核" if row["flagged"] else "未触发告警", "flagged")]:
                    evidence(f"anomaly_{index}_{suffix}", prefix + " · " + label, value, "", path, source_prefix + field, model=True)
                for field, label, unit in [("maxTemperatureC", "最高温度", " ℃"),
                                           ("temperatureAboveContextC", "超出历史工况温度", " ℃"),
                                           ("durationMinutes", "会话时长", " 分钟")]:
                    value = row.get("features", {}).get(field)
                    if number(value) is not None:
                        evidence(f"anomaly_{index}_{field}", prefix + " · " + label, value, unit,
                                 path, source_prefix + "features." + field, model=True)
            result["limitations"].append("只复核已结束的 TEST 会话，不提供实时安全诊断；告警需要人工查看原始业务和设备情况。")
            nav("anomalies")

    if result["status"] == "no_evidence":
        result["answer"] = "当前范围没有足够的可用记录支持该比较或判断；请调整页面筛选。" + ("模型的固定历史范围不随筛选改变。" if intent == "anomalies" else "")
        return result
    result["answer"] = concise_answer(intent, ev, intro)
    return result
