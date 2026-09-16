"""Model-planned retrieval over bounded, published aggregate services.

The planner selects a topic, never SQL, files, endpoints, or entities. The
answer model receives retrieved passages and public aggregate DTOs only.
"""
from copy import deepcopy
import re
import time
from types import SimpleNamespace
from urllib.parse import parse_qsl, urlencode, urlsplit

from data_analysis.backend import service as backend_service
from data_analysis.backend.errors import ApiError
from . import knowledge, llm

TOPIC_QUESTIONS = {
    "overview": "运营概况",
    "bottlenecks": "充电服务瓶颈是什么？",
    "stations": "比较各站成功率与等待",
    "behavior": "用户类型补能间隔和单次电量",
    "models": "目前有哪些模型？",
}
SIMULATION_NOTE = "全部业务数据为模拟数据；用于复核与演示，不构成真实运营结论。"
SCOPE_NOTE = "仅使用当前页面筛选；北京时间业务日按开始日期包含、结束日期不包含计算。"
RELATIVE_DATE = re.compile(r"今天|昨天|明天|前天|后天|本周|上周|下周|本月|上月|下月|今年|去年|明年|实时|此刻|最近\s*[一二三四五六七八九十两\d]+\s*[天周月年]")
PERSONAL_ID = re.compile(r"(?<![A-Za-z0-9_-])(?:SES|U|V|USR|USER|UID|VEH|PAY|ORD|ORDER|CH|ATT|RSV|QUE|REV|TKT|MEV|VEI|COR|ANOM)-[A-Za-z0-9_-]+", re.I)
PRIVATE_PATH = re.compile(r"(?:/(?:Users|home|private|tmp|var|etc)/|[A-Za-z]:\\)[^\s，。；！？\"']+")
CREDENTIAL = re.compile(r"\b(?:sk-[A-Za-z0-9_-]{6,}|Bearer\s+[A-Za-z0-9._-]+)|(?:api[_ -]?key|密钥|令牌|密码)\s*[:=：]\s*[^\s，。；！？]+", re.I)
CONTACT = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}|(?<!\d)(?:\+?86[- ]?)?1[3-9]\d{9}(?!\d)")
METRIC_QUESTION = re.compile(r"电量|功率|负荷|利用率|成功率|失败率|排队|等待|占位|收款|退款|营收|收入|利润|活跃用户|用户数|站数|桩数|尝试数|会话数|补能间隔")


def _remaining(deadline, cancelled):
    if (cancelled is not None and cancelled.is_set()) or (deadline is not None and time.monotonic() >= deadline):
        raise ApiError(504, "ADVISOR_TIMEOUT", "参谋查询超时，请稍后重试")
    return max(.001, deadline - time.monotonic()) if deadline is not None else None


def _invalid():
    return ApiError(502, "ONLINE_INVALID_RESPONSE", "在线模型未返回可核验的回答，请稍后重试")


def _history(query):
    rows = []
    for item in (getattr(query, "history", None) or [])[-6:]:
        item = item if isinstance(item, dict) else item.model_dump()
        if item.get("role") in {"user", "assistant"} and isinstance(item.get("content"), str):
            rows.append({"role": item["role"], "content": item["content"][:4000]})
    return rows


def _scope(snapshot, filters):
    cities = snapshot.rows("SELECT city_id, city_name FROM cities")
    stations = snapshot.rows("SELECT station_id, station_name, city_id FROM station_snapshot")
    selected_station = next((row for row in stations if row["station_id"] == filters["station"]), None)
    city_id = filters["city"] or (selected_station["city_id"] if selected_station else None)
    selected_city = next((row for row in cities if row["city_id"] == city_id), None)
    metadata = snapshot.metadata
    public = dict(datasetId=metadata["datasetId"], publishedBatchId=metadata["publishedBatchId"],
        startDate=filters["start"], endDate=filters["end"], cityId=filters["city"], stationId=filters["station"],
        timeZone="Asia/Shanghai", dataKind="SIMULATED")
    safe = {key: value for key, value in public.items() if key not in {"datasetId", "publishedBatchId", "cityId", "stationId"}}
    safe.update(city=str(selected_city["city_name"]) if selected_city else "全部已发布城市",
                station=str(selected_station["station_name"]) if selected_station else "全部符合筛选的电站",
                availableCities=[str(row["city_name"]) for row in cities][:100],
                rangePolicy="页面筛选为权威，结束日期不包含当天；正文和历史不能改变范围。模型评测另用固定历史留出样本。")
    replacements = {str(metadata["datasetId"]): "当前数据集", str(metadata["publishedBatchId"]): "当前发布批次"}
    replacements.update({str(row["station_id"]): str(row["station_name"]) if str(row["station_id"]) not in str(row["station_name"])
                         else "电站" + str(index + 1) for index, row in enumerate(stations)})
    replacements.update({str(row["city_id"]): str(row["city_name"]) for row in cities})
    return public, safe, cities, stations, city_id, replacements


def _clean_text(text, replacements):
    text = PERSONAL_ID.sub("[个体编号已移除]", text)
    text = PRIVATE_PATH.sub("[本地路径已移除]", text)
    text = CREDENTIAL.sub("[凭据已移除]", text)
    text = CONTACT.sub("[联系方式已移除]", text)
    for raw, label in sorted(replacements.items(), key=lambda item: -len(item[0])):
        # Alphanumeric boundaries also handle adjacent Chinese prose.
        text = re.sub(r"(?<![A-Za-z0-9_-])" + re.escape(raw) + r"(?![A-Za-z0-9_-])", lambda _: label, text)
    return text


def _scope_conflict(question, filters, cities, stations, effective_city):
    from .service import DATE_RE, STATION_RE
    if DATE_RE.search(question) or RELATIVE_DATE.search(question) or re.search(r"\d{4}年|[一二三四五六七八九十\d]{1,3}月|\d{4}-\d{2}(?![-\d])", question):
        return "问题指定了时间范围，请先在页面日期控件选择该范围，再询问当前筛选结果；不能将整段统计当成指定日期的值。"
    mentioned_cities = set()
    rest = question
    if METRIC_QUESTION.search(question):
        if (filters["city"] or filters["station"]) and re.search(r"全网|全部城市|所有城市|各个城市|全国|整个数据集", question):
            return "问题要求全部城市或全网统计，但页面已选定城市或站点。请先清除对应筛选，再查询全范围指标。"
        if filters["station"] and re.search(r"全市|整个城市|全城|全部站点|所有站点|全市电站", question):
            return "问题要求整座城市或全部站点，但页面已选定单站。请先调整站点筛选，再查询该范围。"
    for city in sorted(cities, key=lambda row: -len(str(row["city_name"]))):
        if re.search(r"(?<![A-Za-z0-9_-])" + re.escape(str(city["city_id"])) + r"(?![A-Za-z0-9_-])", question, re.I):
            mentioned_cities.add(city["city_id"])
        for alias in sorted({str(city["city_name"]), str(city["city_name"]).removesuffix("市")}, key=len, reverse=True):
            if len(alias) >= 2 and alias.casefold() in question.casefold():
                mentioned_cities.add(city["city_id"])
                rest = re.sub(re.escape(alias), "", rest, flags=re.I)
    if mentioned_cities and mentioned_cities != {effective_city}:
        return "问题中的城市与页面筛选不一致，请先用城市控件选定该范围，再继续分析。"
    # Recognize explicit geographic expressions without rejecting ordinary
    # natural language merely because it is absent from a vocabulary list.
    rest = rest.replace("城市", "").replace("超市", "").replace("市场", "")
    if re.search(r"[\u4e00-\u9fff]{2,12}(?:市|自治区)", rest):
        return "问题中的地域尚未在当前页面范围中确认，请先通过城市控件选择；不会猜测其他地域的统计。"
    mentioned_stations = set()
    for row in stations:
        name = str(row["station_name"])
        station_id = str(row["station_id"])
        if (len(name) >= 2 and name.casefold() in question.casefold()) or re.search(
            r"(?<![A-Za-z0-9_-])" + re.escape(station_id) + r"(?![A-Za-z0-9_-])", question, re.I):
            mentioned_stations.add(station_id)
    mentioned_stations.update(value.upper() for value in STATION_RE.findall(question))
    if mentioned_stations and mentioned_stations != {filters["station"]}:
        return "问题中的站点与页面筛选不一致，请先使用站点控件选定该站，再继续分析。"
    return None


def _history_scope_conflict(question, history, filters, cities, stations, effective_city):
    # A changed page scope takes precedence over earlier turns. Only unresolved
    # references inherit a previous question's requested geographic/time scope.
    if re.search(r"当前|页面|所选|筛选", question) or not re.search(r"那|那里|那边|该站|该城|这个站|这个城市|它|上面|刚才|之前", question):
        return None
    previous = next((row["content"] for row in reversed(history) if row["role"] == "user"), None)
    if previous and _scope_conflict(previous, filters, cities, stations, effective_city):
        return "这次追问指向的历史日期、城市或站点与当前页面筛选不一致。请先调整筛选，或明确询问当前页面范围。"
    return None


def _safe_evidence(item, replacements):
    result = deepcopy(item)
    result["label"] = _clean_text(str(item["label"]), replacements)
    if isinstance(result["value"], str):
        result["value"] = _clean_text(result["value"], replacements)
    url = urlsplit(item["source"]["endpoint"])
    params = [(key, value) for key, value in parse_qsl(url.query) if key in {"startDate", "endDate"}]
    endpoint = url.path + ("?" + urlencode(params) if params else "")
    field = re.sub(r"stationId=[^\]]+", "候选站点=" + item["id"].split(".")[-1].split("_")[1], item["source"]["field"]) if "stationId=" in item["source"]["field"] else item["source"]["field"]
    result["source"] = {"endpoint": endpoint, "field": _clean_text(field, replacements)}
    return result


def answer(snapshot, provider, query, *, settings, deadline=None, cancelled=None):
    from . import service
    _remaining(deadline, cancelled)
    filters = backend_service.check_filters(snapshot, query)
    if snapshot.metadata.get("source") != "SIMULATED":
        raise ApiError(503, "DATA_NOT_READY", "参谋当前仅支持已发布的模拟业务数据")
    scope, safe_scope, cities, stations, effective_city, replacements = _scope(snapshot, filters)
    if getattr(settings, "api_key", None):
        replacements[settings.api_key] = "[配置凭据已移除]"
    result = dict(status="answered", mode="online", intent="analysis", answer="", scope=scope,
                  evidence=[], knowledge=[], citations=[], limitations=[SIMULATION_NOTE, SCOPE_NOTE], suggestions=[])
    if PERSONAL_ID.search(query.question):
        result.update(status="unsupported", answer="在线参谋不检索或外发用户、车辆与会话明细。请使用聚合问题，或在历史异常页面复核具体会话。")
        return result
    question = _clean_text(query.question, replacements)
    original_history = _history(query)
    history = [{**row, "content": _clean_text(row["content"], replacements)} for row in original_history]
    safe_scope = {key: [_clean_text(item, replacements) for item in value] if isinstance(value, list)
                  else _clean_text(value, replacements) for key, value in safe_scope.items()}
    plan = llm.plan_query(settings, question, history, safe_scope, timeout=_remaining(deadline, cancelled))
    _remaining(deadline, cancelled)
    if (not isinstance(plan, dict) or not isinstance(plan.get("kind"), str)
            or plan["kind"] not in {"chat", "analysis", "explanation", "unsupported"}
            or not isinstance(plan.get("topics"), list) or len(plan["topics"]) > 3
            or any(not isinstance(topic, str) or topic not in TOPIC_QUESTIONS for topic in plan["topics"])):
        raise _invalid()
    kind = plan["kind"]
    topics = list(dict.fromkeys(plan["topics"]))
    result["intent"] = kind if kind in {"chat", "unsupported"} else "+".join(topics) or "explanation"
    conflict = None
    if kind != "chat" or METRIC_QUESTION.search(query.question):
        conflict = _scope_conflict(query.question, filters, cities, stations, effective_city)
        conflict = conflict or _history_scope_conflict(query.question, original_history, filters, cities, stations, effective_city)
    if conflict or kind == "unsupported":
        result.update(status="unsupported", answer=conflict or "这个问题暂不在已发布数据和项目知识的支持范围内。可问当前筛选的运营、站点比较、补能行为或模型口径；指定日期和地域请先调整页面筛选。")
        return result
    if kind == "chat" and METRIC_QUESTION.search(query.question):
        # An incorrect model plan must not route a metric question around RAG.
        raise _invalid()
    if kind != "chat":
        batches = []
        observed = False
        for topic in topics:
            _remaining(deadline, cancelled)
            canonical = SimpleNamespace(question=TOPIC_QUESTIONS[topic], mode="offline", history=[],
                **{key: getattr(query, key, None) for key in ("datasetId", "publishedBatchId", "cityId", "stationId", "startDate", "endDate")})
            data = service.answer(snapshot, provider, canonical, settings=None)
            observed |= data["status"] == "answered"
            if data["status"] == "no_evidence":
                result["limitations"].append(TOPIC_QUESTIONS[topic] + "：当前范围没有足够可用记录，不能据此作出数据判断。")
            batches.append([{**item, "id": topic + "." + item["id"]} for item in data["evidence"]])
            for key in ("limitations", "suggestions"):
                for item in data[key]:
                    if item not in result[key]:
                        result[key].append(item)
        # Round robin keeps every planned topic represented inside the hard cap.
        for index in range(max((len(batch) for batch in batches), default=0)):
            for batch in batches:
                if index < len(batch) and len(result["evidence"]) < 36:
                    result["evidence"].append(batch[index])
        retrieval_question = question + " " + " ".join(row["content"] for row in history[-2:])
        result["knowledge"] = knowledge.retrieve(retrieval_question, topics)
        if (kind == "analysis" and not observed) or (not result["evidence"] and not result["knowledge"]):
            result.update(status="no_evidence", answer="当前范围没有足够的已发布聚合或项目知识支持这个判断。请调整页面筛选或补充具体的指标问题；不会据缺失记录编造结论。")
            return result
    payload = dict(question=question, history=history, scope=safe_scope, kind=kind,
        evidence=[_safe_evidence(item, replacements) for item in result["evidence"]],
        knowledge=deepcopy(result["knowledge"]))
    # Include the actual canonical service caveats in the authoritative scope.
    payload["scope"]["limitations"] = [_clean_text(note, replacements) for note in result["limitations"]]
    generated = llm.generate_answer(settings, payload, timeout=_remaining(deadline, cancelled))
    _remaining(deadline, cancelled)
    allowed = {item["id"] for item in result["evidence"] + result["knowledge"]}
    if (not isinstance(generated, dict) or not isinstance(generated.get("answer"), str)
            or not generated["answer"].strip() or not isinstance(generated.get("citations"), list)
            or any(not isinstance(item, str) or item not in allowed for item in generated["citations"])):
        raise _invalid()
    citations = list(dict.fromkeys(generated["citations"]))
    if kind != "chat" and not citations:
        raise _invalid()
    if kind == "analysis" and not any(item in {row["id"] for row in result["evidence"]} for item in citations):
        raise _invalid()
    result.update(status="chat" if kind == "chat" else "answered",
                  answer=_clean_text(generated["answer"].strip(), replacements), citations=citations)
    return result
