"""mock 后端:规则式"脚本化模型",让 agent 全链路(计划→并行工具→下钻→引用作答)
在**没有任何网络与 key** 的情况下跑通——既是测试替身,也是答辩断网兜底的演示形态。

诚实约束:mock 的答案模板只允许引用工具返回 JSON 里的字段值,绝不内联任何业务数字;
工具没给的东西一律回答"无依据"。它验证的是"管线正确",不宣称模型能力——真实
多步推理请切 openai/anthropic 后端。
"""
from __future__ import annotations

import json
import re
import uuid

from . import actions as page_actions


def _tool_results(transcript: list[dict]) -> dict[str, list[dict]]:
    out: dict[str, list[dict]] = {}
    for t in transcript:
        if t["role"] == "tool":
            try:
                out.setdefault(t["name"], []).append(json.loads(t["content"]))
            except json.JSONDecodeError:
                out.setdefault(t["name"], []).append({"error": "非 JSON 工具返回"})
    return out


def _called_names(transcript: list[dict]) -> list[str]:
    return [c["name"] for t in transcript if t["role"] == "assistant" for c in t.get("tool_calls", [])]


# 页面名别名按长度倒序拼正则(保证"负荷与空闲预测"优先于短词);动词后允许
# "页名链"(智能分析·用户与异常/智能分析的用户与异常):贪婪重复吃已知页名+分隔符,
# 最后一段才是真目标——重复体必须以已知页名开头,"打开后顺便看负荷"这类泛词接不上,
# 不会误劫持数据问句。
_PAGE_ALT = "|".join(sorted(page_actions.NAVIGATE_ALIASES, key=len, reverse=True))
_NAV_ALIAS_RE = re.compile(r"(?:打开|切换到|切换为|跳转?到|前往)[\s“\"「]*"
                           r"(?:(?:" + _PAGE_ALT + r")[的和·、\s”\"「]*)*"
                           r"(" + _PAGE_ALT + r")")
_FILL_ALIAS_RE = re.compile(r"把\s*[\"“']?([^\"“'\n]{1,60}?)[\"”']?\s*(?:填入|填进|输入到|输进|搜索)"
                            r"[\s“\"「]*(" + "|".join(sorted(page_actions.FILL_ALIASES, key=len, reverse=True)) + r")")


def _plan(question: str) -> list[dict]:
    """规则策略器:命中关键词 → 工具调用(含参数抽取),一次最多两路并行。"""
    q = question
    calls: list[dict] = []

    def add(name, **args):
        if len(calls) < 2:
            calls.append({"name": name, "arguments": args})

    # 页面操作是排他强意图,必须放最前:"打开负荷与空闲预测页"含"负荷",
    # 晚一步就被数据分支劫持(批评家没看到的顺序坑);动词语序紧邻页名才算,
    # "我前往的站还要等多久"这类数据问句不会被"前往"误伤。
    mn = _NAV_ALIAS_RE.search(q)
    if mn:
        add("page_action", kind="navigate",
            target=page_actions.NAVIGATE_ALIASES[mn.group(1)])
        return calls
    mf = _FILL_ALIAS_RE.search(q)
    if mf:
        add("page_action", kind="fill",
            target=page_actions.FILL_ALIASES[mf.group(2)], value=mf.group(1).strip())
        return calls

    # 真实站点号是分段的(ST-BJ-01),旧式连写(ST01)也兼容;SES-/C 开头是别的实体,不误抓
    m = re.search(r"\b(?:ST|STAT)[-_][A-Z0-9]{1,4}(?:[-_][A-Z0-9]{1,4})*\b"
                  r"|(ST|STAT)_?[A-Z0-9]*\d+|\b[A-Z]{1,4}\d{2,}\b", q)
    station = m.group(0) if m else None
    d = re.search(r"\d{4}-\d{2}-\d{2}", q)
    date = d.group(0) if d else None
    mc = re.search(r"(CITY|C)\d+|\b[A-Z]{2,}\d*\b(?=城)", q)

    if re.search(r"流失|churn", q, re.I):
        add("read_test_metrics", model_id="gbdt-churn-user-v2")
    elif re.search(r"负荷|hgb|预测精度|MAE", q, re.I):
        add("read_test_metrics", model_id="hgb-q50-history24-v1")
    elif re.search(r"v5|异常|盲评|考试|误报|精度|告警.*(成绩|多少|几条)|F1", q, re.I):
        add("read_test_metrics", model_id="v5")
    if re.search(r"清单|哪些.*(站|会话|告警)|下钻|为什么.*(告|警)", q, re.I):
        add("query_alerts", split="TEST", station_id=station)
    if (station and re.search(r"排队|等待|拥堵", q)):
        calls.clear(); add("queue_summary", station_id=station, date=date)
    if (station and re.search(r"维修|工单|坏", q)):
        calls.clear(); add("lookup_maintenance", station_id=station)
    if (station and re.search(r"评论|抱怨|评价|用户说", q)):
        calls.clear(); add("review_feedback", station_id=station)
    if (station and re.search(r"档案|几把枪|容量|功率", q)):
        calls.clear(); add("station_profile", station_id=station)
    if (date and mc and re.search(r"天气|气温|下雨|周末|活动", q)):
        calls.clear(); add("lookup_weather_calendar", city_id=mc.group(0).split("城")[0], date=date)
    if re.search(r"哪些模型|模型卡|能干什么|几条线|登记", q):
        calls.clear(); add("list_models")
    if not calls:
        add("search_knowledge", query=q)
    return calls


class MockBackend:
    name = "mock"

    def generate(self, system: str, transcript: list[dict], specs: list[dict]) -> dict:
        question = next(t["content"] for t in transcript if t["role"] == "user")
        done = _called_names(transcript)
        results = _tool_results(transcript)

        if not done:
            return {"content": "mock 策略:按关键词规划第一轮工具。",
                    "tool_calls": [{"id": f"mock_{uuid.uuid4().hex[:8]}",
                                    "name": c["name"], "arguments": c["arguments"]}
                                   for c in _plan(question)]}

        # 有告警清单且还没下钻 → 追一条 explain(演示真正的多步链)
        qa = results.get("query_alerts", [{}])[0]
        if "query_alerts" in done and "explain_session" not in done and qa.get("rows"):
            sid = qa["rows"][0]["session_id"]
            return {"content": "mock 策略:清单非空,下钻首个会话的触发归因。",
                    "tool_calls": [{"id": "mock_explain", "name": "explain_session",
                                    "arguments": {"session_id": sid}}]}

        return {"content": self._compose(question, results), "tool_calls": []}

    @staticmethod
    def _compose(question: str, results: dict) -> str:
        lines: list[str] = []
        cites: list[str] = []

        def src(obj):
            for k in ("source", "sources"):
                v = obj.get(k)
                if isinstance(v, str):
                    cites.append(v)
                elif isinstance(v, list):
                    cites.extend(map(str, v))

        for name, objs in results.items():
            for o in objs:
                if not isinstance(o, dict):
                    # 个别冻结件顶层就是 JSON 数组(如负荷线考试记录):原样摘录,不崩
                    lines.append(f"{name}:" + json.dumps(o, ensure_ascii=False)[:280] + "…")
                    continue
                src(o)
                if "error" in o:
                    continue
                if name == "page_action":
                    # 专属分支:不 JSON dump、不拼 [来源];label 已在服务端由注册表 resolve
                    for a in o.get("ui_actions", []):
                        if a.get("kind") == "navigate":
                            lines.append(f"已为你跳转到「{a['label']}」页。")
                        else:
                            lines.append(f"已在「{a['label']}」填入:{a['value']}(未提交,请你亲手点查询)。")
                elif name == "read_test_metrics":
                    t = o.get("test") or {}
                    if t:
                        lines.append(f"{o.get('modelId', '?')}:TEST 段 告警 {t.get('flagged')} 条、"
                                     f"误报 {t.get('fp')}、漏报 {t.get('fn')},"
                                     f"precision={t.get('precision')} recall={t.get('recall')} f1={t.get('f1')}")
                    if o.get("testRecallByType"):
                        lines.append("分型召回:" + json.dumps(o["testRecallByType"], ensure_ascii=False))
                    if o.get("randomRef"):
                        lines.append(f"随机对照(同预算):{json.dumps(o['randomRef'], ensure_ascii=False)}")
                    if o.get("model"):  # churn 线的汇总块结构不同
                        lines.append(f"{o.get('modelId', '?')}:TEST {json.dumps(o['model'], ensure_ascii=False)}")
                elif name == "query_alerts":
                    lines.append(f"{o.get('split')} 段模型告警共 {o.get('total_flagged')} 条"
                                 + (f",样例:{[r.get('session_id') for r in o.get('rows', [])[:3]]}" if o.get("rows") else ""))
                elif name == "explain_session":
                    lines.append(f"会话 {o.get('session_id')}:融合分 {o.get('score')} vs 阈值 "
                                 f"{round(float(o.get('threshold', 0)), 3)} → "
                                 f"{'告警' if o.get('alerted') else '未告警'};"
                                 f"入选信号 {o.get('chosen_signals')},逐信号 z:"
                                 + json.dumps({k: v.get('z') if isinstance(v, dict) else v
                                                for k, v in (o.get('per_signal') or {}).items()}, ensure_ascii=False))
                elif name == "search_knowledge":
                    for h in o.get("hits", [])[:3]:
                        lines.append(f"资料命中[{h['source']}]:{h['snippet'][:60]}…")
                elif name in ("queue_summary", "lookup_maintenance", "review_feedback",
                              "station_profile", "lookup_weather_calendar", "list_models"):
                    lines.append(f"{name}:{json.dumps(o, ensure_ascii=False)[:280]}…")

        errs = [o.get("error") for objs in results.values() for o in objs if "error" in o]
        if not lines:
            return "无依据:" + ";".join(map(str, errs)) + "。(工具未返回可引用数据,拒绝作答)"
        out = "\n".join(lines)
        if errs:
            out += "\n(另有查询无依据:" + ";".join(map(str, errs))[:120] + ")"
        if cites:  # 纯页面操作回合没有数据引用,不输出空 [来源] 行
            out += "\n[来源]" + " | ".join(dict.fromkeys(cites))
            out += "\n※ 依据仅来自本次检索到的平台工件;成绩为模拟数据测试结果,不构成真实运营结论。"
        return out
