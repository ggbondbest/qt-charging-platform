"""参谋工具箱:一组只读查询,每个返回"带 source 的 JSON 字符串";外加一个
page_action 页面引导工具(不读写业务数据,只校验并排队白名单界面动作)。

红线(代码层面强制,不靠提示词自觉):
- 绝不查 anomaly_labels——真值标签只允许进离线评测,参谋看不到逐会话答案;
  分型召回从冻结评测 json 的 testRecallByType 读(那是模型输出的汇总,不是答案泄露)。
- datasets/ 与 outputs/ 冻结工件只读;告警清单等派生工件由 artifacts.prepare() 生成到
  outputs/ml_advisor/,本模块只读它。
- 返回体量做截断(列表 ≤20 行/字符串 ≤200 字),防上下文爆炸。
"""
from __future__ import annotations

import json
import re
import sqlite3
import sys
from functools import lru_cache

import numpy as np
import pandas as pd

from . import actions, config

MAX_ROWS = 20


def _dump(obj: object) -> str:
    return json.dumps(_shrink(obj), ensure_ascii=False, default=str)


def _shrink(obj):
    if isinstance(obj, dict):
        return {k: _shrink(v) for k, v in obj.items()}
    if isinstance(obj, list):
        out = [_shrink(x) for x in obj[:MAX_ROWS]]
        if len(obj) > MAX_ROWS:
            out.append(f"…截断,共 {len(obj)} 项")
        return out
    if isinstance(obj, str) and len(obj) > 200:
        return obj[:200] + "…"
    return obj


def _err(msg: str) -> str:
    # 给模型的硬指令:拿不到据,就如实说"无依据",不许编
    return _dump({"error": msg, "advice": "无数据可引;回答必须声明『无依据』或建议先跑 prepare"})


@lru_cache(maxsize=None)
def _clean_raw(name: str) -> pd.DataFrame:
    parts = sorted(str(p) for p in (config.CLEAN_DIR / name).glob("*.parquet"))
    return pd.concat([pd.read_parquet(p) for p in parts], ignore_index=True)


def _clean(name: str) -> pd.DataFrame:
    # 每次给副本:调用方要加派生列(queue["d"]=...),不能污染缓存里的原始表
    return _clean_raw(name).copy()


# ---------- 模型卡与评测件 ----------

def list_models() -> str:
    """三条已交付模型线 + v1→v5 盲评链成绩,来源是登记文档原文(不重新解读)。"""
    rows = []
    reg = config.DA_ROOT / "ml" / "README.md"
    if reg.exists():
        txt = reg.read_text(encoding="utf-8")
        sec = txt.split("## 已交付模型登记", 1)[-1]
        for line in sec.splitlines():
            if line.strip().startswith("|") and "---" not in line:
                cells = [c.strip() for c in line.strip().strip("|").split("|")]
                if len(cells) >= 4 and cells[0] not in ("线", ""):
                    rows.append({"线": cells[0], "模型": cells[2], "状态": cells[3][:160],
                                 "source": "data_analysis/ml/README.md"})
    lineage = {}
    v5 = config.ANOMALY_OUT / "test_metrics_context-weather-fixedthr-v5.json"
    if v5.exists():
        lineage = json.loads(v5.read_text(encoding="utf-8")).get("lineage", {})
    return _dump({"models": rows, "anomalyLineage": lineage,
                  "sources": ["data_analysis/ml/README.md", "test_metrics_context-weather-fixedthr-v5.json"]})


def _find_metrics(model_id: str):
    pats = [config.ANOMALY_OUT, config.CHURN_OUT, config.LOAD_OUT]
    hits = []
    for d in pats:
        if d.exists():
            hits += [p for p in sorted(d.glob("test_metrics_*.json")) if model_id in p.name]
    return hits


def read_test_metrics(model_id: str) -> str:
    """读某模型线的一次性考试(盲评)冻结件原文。model_id 支持子串,如 v5/churn/hgb-q50。"""
    alias = {"v5": "context-weather-fixedthr-v5", "v4": "context-baseline-rankfuse-v4",
             "v1": "iforest-session-battery-v1", "v2": "iforest-pointmax-battery-v2",
             "v3": "iforest-session-rankfuse-v3", "churn": "gbdt-churn-user-v2",
             "load": "hgb-q50-history24-v1"}
    key = alias.get(model_id.lower().strip(), model_id.strip())
    hits = _find_metrics(key)
    if not hits:
        # 错误文本会被模型看到并可能原样复述,只给别名,不给文件名清单
        return _err(f"找不到该模型的成绩记录(model_id={model_id});"
                    "可用别名:v1、v2、v3、v4、v5、churn、load")
    out = []
    for p in hits[:3]:
        j = json.loads(p.read_text(encoding="utf-8"))
        keep = {k: j[k] for k in ("modelId", "modelVersion", "datasetId", "note", "splits",
                                  "chosenSignals", "chosenPercentile", "validation", "test",
                                  "testRecallByType", "randomRef", "lineage", "model",
                                  "recencyRule", "metricCaveats", "horizons") if k in j}
        keep["source"] = str(p.relative_to(config.DA_ROOT)).replace("\\", "/")
        out.append(keep)
    return _dump(out if len(out) > 1 else out[0])


# ---------- 告警与下钻(模型输出侧,无真值) ----------

def query_alerts(split: str = "TEST", station_id: str | None = None, limit: int = 20) -> str:
    if not config.ALERTS_CSV.exists():
        return _err("告警清单未生成:先运行 python -m data_analysis.ml.advisor.driver --prepare")
    df = pd.read_csv(config.ALERTS_CSV)
    if split and split != "ALL":
        df = df[df["split"] == split.upper()]
    if station_id:
        df = df[df["station_id"] == station_id]
    n = len(df)
    cols = ["session_id", "station_id", "charger_id", "city_id", "date", "split", "score", "threshold"]
    return _dump({"split": split, "station_id": station_id, "total_flagged": int(n),
                  "rows": df[cols].head(min(int(limit), MAX_ROWS)).to_dict("records"),
                  "note": "模型告警输出,不含真值标签;命中情况见评测件",
                  "source": "outputs/ml_advisor/alerts_v5.csv"})


def explain_session(session_id: str) -> str:
    """单条会话为什么(没)触发:v5 固定阈值决策函数逐信号复算 + 曲线摘要。只读重算。

    信号定义走 advisor 自带的 signals_v3(队友重构删了原 detect_v3 后,归因链
    不能再追他们包内的符号);异常文本一律消毒,原始堆栈只进服务端日志。
    """
    import sys

    try:
        import joblib

        from . import signals_v3
        b = joblib.load(config.ANOMALY_OUT / "context-weather-fixedthr-v5.joblib")
        ctx = pd.read_pickle(config.ANOMALY_OUT / "context_signals_v5.pkl")
        v3 = signals_v3.session_signals()
        allsig = {k: ctx[k] for k in ("tempW", "cellW", "curDeficit") if k in ctx.columns}
        allsig.update({k: v3[k] for k in ("thermalSpike", "currentJump", "v1Mean")})
        zs = {}
        contrib = 0.0
        for name, (med, mad) in b["z_med_mad"].items():
            s = allsig.get(name)
            if s is None or session_id not in s.index:
                zs[name] = {"value": None, "note": "无此会话信号"}
                continue
            raw = float(s.loc[session_id])
            z = max(-10.0, min(10.0, (raw - med) / (1.4826 * mad + 1e-9)))
            if not np.isfinite(z):  # 缺信号会话按"最不异常"钉底,与告警清单口径一致
                z = 0.0
            zs[name] = {"raw": round(raw, 3), "z": round(z, 3), "used": name in b["signals"]}
            if name in b["signals"]:
                contrib += z
        comp = contrib / len(b["signals"])
        pts = _clean("charging_sessions")
        ses = pts[pts["session_id"] == session_id]
        summary = {} if ses.empty else ses.iloc[0].to_dict()
        return _dump({"session_id": session_id, "chosen_signals": b["signals"],
                      "threshold": b["threshold"], "score": round(comp, 3),
                      "alerted": bool(comp > b["threshold"]), "per_signal": zs,
                      "session_fields": {k: summary.get(k) for k in
                                         ("station_id", "charger_id", "started_at", "ended_at",
                                          "energy_wh", "stop_reason", "status")},
                      "model_id": b["model_id"],
                      "sources": ["outputs/ml_anomaly/context-weather-fixedthr-v5.joblib",
                                  "clean/charging_sessions(只读)"]})
    except Exception as e:
        print(f"[tools] explain_session 失败:{type(e).__name__}: {e}", file=sys.stderr)
        return _err("explain_session 暂不可用(归因链依赖异常,详情在服务端日志)")


# ---------- 语境:天气、日历、站、工单、排队、评价 ----------

def lookup_weather_calendar(city_id: str, date: str) -> str:
    w = _clean("weather_hourly")
    w["d"] = pd.to_datetime(w["recorded_at"]).dt.strftime("%Y-%m-%d")
    day = w[(w["city_id"] == city_id) & (w["d"] == date)]
    cal = _clean("calendar")
    cal["d"] = pd.to_datetime(cal["business_date"].astype(str)).dt.strftime("%Y-%m-%d")
    crow = cal[(cal["city_id"] == city_id) & (cal["d"] == date)]
    if day.empty:
        # 错误文本会被 mock/真模型逐字转述进正文,裸表名不许出现在这里(scrub 不遮表名)
        return _err(f"天气观测记录里没有 {city_id} {date} 的数据(数据窗 2025-12-01~2026-05-30 内才有)")
    out = {"city_id": city_id, "date": date,
           "weather": {"temp_min": round(float(day["temperature_c"].min()), 1),
                       "temp_max": round(float(day["temperature_c"].max()), 1),
                       "temp_mean": round(float(day["temperature_c"].mean()), 1),
                       "rainfall_mm": round(float(day["rainfall_mm"].sum()), 1),
                       "conditions": sorted(day["weather"].astype(str).unique())[:6]},
           "calendar": crow[["is_weekend", "scenario_event", "demand_multiplier"]].to_dict("records"),
           "sources": ["clean/weather_hourly", "clean/calendar"]}
    return _dump(out)


def lookup_maintenance(charger_id: str | None = None, station_id: str | None = None) -> str:
    t = _clean("maintenance_tickets")
    if charger_id:
        t = t[t["charger_id"] == charger_id]
    if station_id:
        t = t[t["station_id"] == station_id]
    if t.empty:
        return _err(f"无维修工单:{charger_id or station_id}")
    cols = ["ticket_id", "charger_id", "fault_type", "severity", "reported_at", "restored_at", "status"]
    return _dump({"count": int(len(t)), "tickets": t[cols].head(MAX_ROWS).to_dict("records"),
                  "source": "clean/maintenance_tickets"})


def queue_summary(station_id: str, date: str | None = None) -> str:
    q = _clean("queue_entries")
    q["d"] = pd.to_datetime(q["joined_at"]).dt.strftime("%Y-%m-%d")
    sel = q[q["station_id"] == station_id]
    if date:
        sel = sel[sel["d"] == date]
    if sel.empty:
        return _err(f"排队记录里没有 {station_id} {date or ''} 的记录")
    waited = sel.dropna(subset=["called_at"])
    wait_min = (pd.to_datetime(waited["called_at"]) - pd.to_datetime(waited["joined_at"])).dt.total_seconds() / 60
    return _dump({"station_id": station_id, "date": date or "全部",
                  "entries": int(len(sel)),
                  "avg_wait_min": round(float(wait_min.mean()), 1) if len(wait_min) else None,
                  "max_wait_min": round(float(wait_min.max()), 1) if len(wait_min) else None,
                  "avg_position_at_join": round(float(sel["position_at_join"].mean()), 1),
                  "status_counts": sel["status"].value_counts().to_dict(),
                  "source": "clean/queue_entries"})


def station_profile(station_id: str) -> str:
    st = _clean("stations")
    row = st[st["station_id"] == station_id]
    if row.empty:
        return _err(f"无此站点 {station_id}")
    ch = _clean("chargers")
    chs = ch[ch["station_id"] == station_id]
    return _dump({"station": row.iloc[0].to_dict(),
                  "chargers": {"count": int(len(chs)),
                               "rated_kw": chs["rated_power_kw"].round(1).tolist()[:30]},
                  "sources": ["clean/stations", "clean/chargers"]})


def review_feedback(station_id: str, limit: int = 10) -> str:
    r = _clean("reviews")
    sel = r[r["station_id"] == station_id]
    if sel.empty:
        return _err(f"用户评价里没有 {station_id} 的记录")
    return _dump({"station_id": station_id, "n": int(len(sel)),
                  "rating_mean": round(float(sel["rating"].mean()), 2),
                  "issue_types": sel["issue_type"].value_counts().head(8).to_dict(),
                  "samples": sel.dropna(subset=["comment"]).tail(int(limit))[
                      ["rating", "issue_type", "comment"]].to_dict("records"),
                  "source": "clean/reviews"})


# ---------- 文本检索(FTS5/BM25;CJK 逐字切分) ----------

_CJK = re.compile(r"([㐀-鿿぀-ヿ가-힯])")


def _seg(text: str) -> str:
    """中文逐字加空格(FTS5 unicode61 不切 CJK,单字 token 是最省事的等效分词)。"""
    return _CJK.sub(r" \1 ", str(text))


def search_knowledge(query: str, k: int = 5) -> str:
    if not config.KNOWLEDGE_DB.exists():
        return _err("知识索引未建:先运行 python -m data_analysis.ml.advisor.driver --prepare")
    toks = _seg(query).split()[:10]
    if not toks:
        return _err("空查询")
    match = " AND ".join('"' + t.replace('"', "") + '"' for t in toks)
    con = sqlite3.connect(config.KNOWLEDGE_DB)
    try:
        rows = con.execute(
            "SELECT d.source, bm25(docs_fts) AS score, substr(d.text,1,160) "
            "FROM docs_fts JOIN docs d ON d.id=docs_fts.rowid "
            "WHERE docs_fts MATCH ? ORDER BY score LIMIT ?", (match, int(k))).fetchall()
    except sqlite3.OperationalError as e:
        return _err(f"检索失败:{e}")
    finally:
        con.close()
    if not rows:
        return _err(f"无命中:{query}")
    return _dump({"query": query, "hits": [{"source": s, "bm25": round(float(sc), 2), "snippet": t}
                                           for s, sc, t in rows]})


# ---------- 注册表(名字/描述/JSON schema 三件套,协议层各自渲染) ----------

REGISTRY = {
    "list_models": {"h": list_models, "description": "列已交付模型线与盲评链成绩(读登记表与血缘,勿改述)",
                    "parameters": {"type": "object", "properties": {}}},
    "read_test_metrics": {"h": read_test_metrics,
                          "description": "读某模型一次性考试冻结件原文(精度/召回/告警数/分型召回/随机对照)。model_id 如 v5、gbdt-churn-user-v2、hgb-q50",
                          "parameters": {"type": "object", "properties": {
                              "model_id": {"type": "string"}}, "required": ["model_id"]}},
    "query_alerts": {"h": query_alerts, "description": "查 v5 告警清单(模型输出侧,无真值标签)",
                     "parameters": {"type": "object", "properties": {
                         "split": {"type": "string", "enum": ["TRAIN", "VALIDATION", "TEST", "ALL"], "default": "TEST"},
                         "station_id": {"type": "string"}, "limit": {"type": "integer", "default": 20}}}},
    "explain_session": {"h": explain_session, "description": "单会话告警归因:固定阈值逐信号 z 复算+曲线字段",
                        "parameters": {"type": "object", "properties": {
                            "session_id": {"type": "string"}}, "required": ["session_id"]}},
    "lookup_weather_calendar": {"h": lookup_weather_calendar, "description": "某城某日已观测天气与日历事件(已发生事实)",
                                "parameters": {"type": "object", "properties": {
                                    "city_id": {"type": "string"}, "date": {"type": "string", "description": "YYYY-MM-DD"}},
                                    "required": ["city_id", "date"]}},
    "lookup_maintenance": {"h": lookup_maintenance, "description": "维修工单(按枪或站)",
                           "parameters": {"type": "object", "properties": {
                               "charger_id": {"type": "string"}, "station_id": {"type": "string"}}}},
    "queue_summary": {"h": queue_summary, "description": "站点排队统计:等待时长/位置/状态。date 留空=统计全部数据窗,别自己编日期",
                      "parameters": {"type": "object", "properties": {
                          "station_id": {"type": "string"}, "date": {"type": "string"}}}},
    "station_profile": {"h": station_profile, "description": "站点档案+桩配置",
                        "parameters": {"type": "object", "properties": {
                            "station_id": {"type": "string"}}, "required": ["station_id"]}},
    "review_feedback": {"h": review_feedback, "description": "用户评价:评分分布/问题类型/评论样例",
                        "parameters": {"type": "object", "properties": {
                            "station_id": {"type": "string"}}, "required": ["station_id"]}},
    "search_knowledge": {"h": search_knowledge, "description": "全文检索文档/评论/维修备注(BM25)",
                         "parameters": {"type": "object", "properties": {
                             "query": {"type": "string"}, "k": {"type": "integer", "default": 5}},
                             "required": ["query"]}},
    # 页面引导:白名单在 actions.ai_actions.json,校验在 actions.normalize(本表只做注册)
    "page_action": {"h": actions.page_action, **actions.SPEC},
}


def execute(name: str, args: dict) -> str:
    """统一入口:参数按 schema 过滤(防模型瞎塞字段),异常一律转 error JSON。"""
    spec = REGISTRY.get(name)
    if spec is None:
        return _err(f"未知工具 {name};可用:{sorted(REGISTRY)}")
    props = set(spec["parameters"].get("properties", {}))
    clean_args = {k: v for k, v in (args or {}).items() if k in props}
    try:
        return spec["h"](**clean_args)
    except Exception as e:  # 工具崩了也不许让模型编:回 error+advice
        # 原始异常(常含绝对路径/依赖细节)只进服务端日志,出口给消毒文本
        print(f"[tools] {name} 执行异常:{type(e).__name__}: {e}", file=sys.stderr)
        return _err(f"工具 {name} 执行异常:{type(e).__name__}(详情在服务端日志)")


SPECS = [{"name": n, "description": s["description"], "parameters": s["parameters"]}
         for n, s in REGISTRY.items()]
